#include <pebble.h>

// ---------------------------------------------------------------------------
// HEB List — displays a shared H-E-B shopping list and lets the user check
// items off. Data is fetched by PebbleKit JS (src/pkjs/index.js) and pushed
// over AppMessage; check state is toggled locally on the watch and recorded
// by the JS side (HEB's public API has no check-off mutation for guests).
// ---------------------------------------------------------------------------

#define MAX_ITEMS 300
#define MAX_SECTIONS 64

#define NAME_LEN 64
#define LOC_LEN 32
#define GROUP_LEN 28

#define BOX_SIZE 11
#define BOX_X 6
#define TEXT_X (BOX_X + BOX_SIZE + 6)
#define CELL_PAD 4

// Wrap limits — measurement and drawing must use the same boxes so cell height
// always matches what is drawn.
#define NAME_LINES 3
#define SUB_LINES 2

// Status codes (AppStatus)
enum {
  ST_LOADING = 0,
  ST_READY = 1,
  ST_CACHED = 2, // ready, but served from the phone's cache
  ST_ERROR = 3,
  ST_NO_URL = 4,
  ST_EMPTY = 5,
};

// ListFlags (AppListFlags)
#define FLAG_CACHED 0x01

typedef struct {
  char name[NAME_LEN];
  char location[LOC_LEN];
  char group[GROUP_LEN];
  uint8_t checked;
  uint8_t qty;
} Item;

typedef struct {
  uint16_t start; // index into s_items
  uint16_t count;
} Section;

static Window *s_window;
static MenuLayer *s_menu;

static Item *s_items = NULL;
static Section *s_sections = NULL;
static uint16_t s_item_count = 0;  // items received
static uint16_t s_item_total = 0;  // expected items (from begin message)
static uint16_t s_section_count = 0;

static char s_list_name[48];
static char s_status_message[96];
static uint8_t s_status = ST_LOADING;
static uint8_t s_list_flags = 0;
static time_t s_updated_at = 0;

static GFont s_font_name;
static GFont s_font_sub;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void safe_copy(char *dst, size_t cap, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Read an integer tuple regardless of the width the sender chose.
static uint32_t tuple_int(const Tuple *t) {
  if (!t) return 0;
  switch (t->type) {
    case TUPLE_UINT:
      switch (t->length) {
        case 1: return t->value->uint8;
        case 2: return t->value->uint16;
        case 4: return t->value->uint32;
      }
      break;
    case TUPLE_INT:
      switch (t->length) {
        case 1: return (uint32_t)t->value->int8;
        case 2: return (uint32_t)t->value->int16;
        case 4: return (uint32_t)t->value->int32;
      }
      break;
    default:
      break;
  }
  return 0;
}

static uint16_t count_unchecked(void) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < s_item_count; i++) {
    if (!s_items[i].checked) n++;
  }
  return n;
}

// Build the section list: contiguous runs of identical group strings.
// Menu section N+1 maps to s_sections[N].
static void rebuild_sections(void) {
  s_section_count = 0;
  if (s_item_count == 0) return;
  uint16_t start = 0;
  for (uint16_t i = 1; i <= s_item_count; i++) {
    bool boundary = (i == s_item_count) ||
                    (strncmp(s_items[i].group, s_items[i - 1].group, GROUP_LEN) != 0);
    if (boundary) {
      if (s_section_count >= MAX_SECTIONS) {
        // Too many groups: fold the remainder into the last section rather
        // than dropping those items from the list.
        s_sections[MAX_SECTIONS - 1].count = s_item_count - s_sections[MAX_SECTIONS - 1].start;
        break;
      }
      s_sections[s_section_count].start = start;
      s_sections[s_section_count].count = i - start;
      s_section_count++;
      start = i;
    }
  }
}

// Map a menu cell (section >= 1) to an item index. Returns -1 if invalid.
static int16_t cell_to_item_index(const MenuIndex *ci) {
  if (ci->section == 0) return -1; // info row
  if (ci->section > s_section_count) return -1;
  const Section *sec = &s_sections[ci->section - 1];
  if (ci->row >= sec->count) return -1;
  return sec->start + ci->row;
}

// ---------------------------------------------------------------------------
// Menu callbacks
// ---------------------------------------------------------------------------

static void draw_checkbox(GContext *ctx, int16_t cell_y, bool highlighted, bool checked) {
  GColor fg = highlighted ? GColorBlack : GColorWhite;
  GColor bg = highlighted ? GColorWhite : GColorBlack;
  int16_t y = cell_y + 4;
  GRect box = GRect(BOX_X, y, BOX_SIZE, BOX_SIZE);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_fill_color(ctx, fg);
  if (checked) {
    graphics_fill_rect(ctx, box, 0, GCornerNone);
    // check mark inside the filled box, in the background color
    graphics_context_set_stroke_color(ctx, bg);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(BOX_X + 2, y + 6), GPoint(BOX_X + 4, y + 8));
    graphics_draw_line(ctx, GPoint(BOX_X + 4, y + 8), GPoint(BOX_X + 8, y + 3));
    graphics_context_set_stroke_width(ctx, 1);
    graphics_context_set_stroke_color(ctx, fg);
  } else {
    graphics_draw_rect(ctx, box);
  }
}

static void build_subtitle(const Item *it, char *buf, size_t cap) {
  if (it->qty > 1) {
    snprintf(buf, cap, "%s - Qty %d", it->location, it->qty);
  } else {
    snprintf(buf, cap, "%s", it->location);
  }
}

// Line height of a font, derived by measuring reference glyphs.
static int16_t font_line_height(GFont font) {
  GRect box = GRect(0, 0, 100, 300);
  return (int16_t)graphics_text_layout_get_content_size(
      "Mg", font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
}

// Content size of text wrapped in a box `lines` tall — clamped to that height,
// so callers can lay out and draw with identical boxes.
static GSize measure_text(const char *text, GFont font, int16_t w, int16_t lines) {
  GRect box = GRect(0, 0, w, lines * font_line_height(font));
  return graphics_text_layout_get_content_size(
      text, font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft);
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index,
                     void *callback_context) {
  GRect cell = layer_get_bounds(cell_layer);
  bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
  GColor fg = highlighted ? GColorBlack : GColorWhite;
  graphics_context_set_text_color(ctx, fg);
  graphics_context_set_antialiased(ctx, true);

  if (cell_index->section == 0) {
    // Info row: list name + status line
    const char *line2 = (s_status_message[0] != '\0') ? s_status_message : "";
    GRect box1 = GRect(6, cell.origin.y + 2, cell.size.w - 10, 22);
    graphics_draw_text(ctx, s_list_name[0] ? s_list_name : "HEB List", s_font_name, box1,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    GRect box2 = GRect(6, cell.origin.y + 24, cell.size.w - 10, 18);
    graphics_draw_text(ctx, line2, s_font_sub, box2,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  int16_t idx = cell_to_item_index(cell_index);
  if (idx < 0) return;
  const Item *it = &s_items[idx];

  draw_checkbox(ctx, cell.origin.y, highlighted, it->checked);

  int16_t w = cell.size.w - TEXT_X - 4;
  int16_t name_h = NAME_LINES * font_line_height(s_font_name);
  int16_t sub_h = SUB_LINES * font_line_height(s_font_sub);
  GRect name_box = GRect(TEXT_X, cell.origin.y + 1, w, name_h);
  GSize name_size = graphics_text_layout_get_content_size(
      it->name, s_font_name, name_box, GTextOverflowModeWordWrap, GTextAlignmentLeft);

  graphics_draw_text(ctx, it->name, s_font_name, name_box, GTextOverflowModeWordWrap,
                     GTextAlignmentLeft, NULL);

  if (it->checked && name_size.h > 0) {
    // strikethrough across the name block
    int16_t mid_y = cell.origin.y + 1 + name_size.h / 2;
    graphics_draw_line(ctx, GPoint(TEXT_X, mid_y), GPoint(cell.size.w - 4, mid_y));
  }

  char sub[LOC_LEN + 16];
  build_subtitle(it, sub, sizeof(sub));
  GRect sub_box = GRect(TEXT_X, cell.origin.y + 1 + name_size.h, w, sub_h);
  graphics_draw_text(ctx, sub, s_font_sub, sub_box, GTextOverflowModeWordWrap,
                     GTextAlignmentLeft, NULL);
}

// Sections with an empty group (e.g. non-category sorts) render without a header bar.
static bool section_has_header(uint16_t section_index) {
  if (section_index == 0 || section_index > s_section_count) return false;
  const Section *sec = &s_sections[section_index - 1];
  return sec->count > 0 && s_items[sec->start].group[0] != '\0';
}

static void draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index,
                        void *callback_context) {
  if (!section_has_header(section_index)) return;
  const Section *sec = &s_sections[section_index - 1];
  // Inverted header bar: white background, black text (menu_cell_basic_header_draw
  // alone renders black text and is invisible on this menu's black background).
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, s_items[sec->start].group,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(bounds.origin.x + 3, bounds.origin.y,
                           bounds.size.w - 6, bounds.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static int16_t get_cell_height(struct MenuLayer *menu_layer, MenuIndex *cell_index,
                               void *callback_context) {
  GRect bounds = layer_get_bounds(menu_layer_get_layer(menu_layer));
  if (cell_index->section == 0) {
    return 44; // info row
  }
  int16_t idx = cell_to_item_index(cell_index);
  if (idx < 0) return 44;
  const Item *it = &s_items[idx];
  int16_t w = bounds.size.w - TEXT_X - 4;
  GSize name_size = measure_text(it->name, s_font_name, w, NAME_LINES);
  char sub[LOC_LEN + 16];
  build_subtitle(it, sub, sizeof(sub));
  GSize sub_size = measure_text(sub, s_font_sub, w, SUB_LINES);
  int16_t h = 1 + name_size.h + sub_size.h + 2 * CELL_PAD;
  if (h < BOX_SIZE + 8) h = BOX_SIZE + 8;
  return h;
}

static int16_t get_header_height(struct MenuLayer *menu_layer, uint16_t section_index,
                                 void *callback_context) {
  return section_has_header(section_index) ? MENU_CELL_BASIC_HEADER_HEIGHT : 0;
}

static uint16_t get_num_sections(struct MenuLayer *menu_layer, void *callback_context) {
  return 1 + s_section_count;
}

static uint16_t get_num_rows(struct MenuLayer *menu_layer, uint16_t section_index,
                             void *callback_context) {
  if (section_index == 0) return 1; // info row
  if (section_index > s_section_count) return 0;
  return s_sections[section_index - 1].count;
}

static void request_refresh(MenuLayer *menu_layer) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_AppRefresh, 1);
    app_message_outbox_send();
    s_status = ST_LOADING;
    safe_copy(s_status_message, sizeof(s_status_message), "Refreshing...");
    layer_mark_dirty(menu_layer_get_layer(menu_layer));
  }
}

static void select_click(struct MenuLayer *menu_layer, MenuIndex *cell_index,
                         void *callback_context) {
  if (cell_index->section == 0) {
    // info row: select = refresh
    request_refresh(menu_layer);
    return;
  }

  int16_t idx = cell_to_item_index(cell_index);
  if (idx < 0) return;
  Item *it = &s_items[idx];
  it->checked = !it->checked;

  // Record locally; JS persists it.
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint16(out, MESSAGE_KEY_AppToggleItem, (uint16_t)idx);
    dict_write_uint8(out, MESSAGE_KEY_AppItemChecked, it->checked);
    app_message_outbox_send();
  }
  layer_mark_dirty(menu_layer_get_layer(menu_layer));
}

static void select_long_click(struct MenuLayer *menu_layer, MenuIndex *cell_index,
                              void *callback_context) {
  request_refresh(menu_layer);
}

// ---------------------------------------------------------------------------
// Status / list assembly
// ---------------------------------------------------------------------------

static void update_status_line(void) {
  char buf[96];
  switch (s_status) {
    case ST_LOADING:
      safe_copy(s_status_message, sizeof(s_status_message), "Loading...");
      break;
    case ST_NO_URL:
      safe_copy(s_status_message, sizeof(s_status_message),
                "Set list URL in phone settings");
      break;
    case ST_ERROR:
      // s_status_message already holds the error text
      break;
    case ST_EMPTY:
      safe_copy(s_status_message, sizeof(s_status_message), "List is empty");
      break;
    default: {
      char left[16];
      snprintf(left, sizeof(left), "%d left", count_unchecked());
      char when[16];
      if (s_updated_at > 0) {
        struct tm *tm = localtime(&s_updated_at);
        if (tm) {
          strftime(when, sizeof(when), "%I:%M%p", tm);
        } else {
          when[0] = '\0';
        }
      } else {
        when[0] = '\0';
      }
      snprintf(buf, sizeof(buf), "%s%s%s%s",
               left,
               (s_list_flags & FLAG_CACHED) ? " (cached)" : "",
               when[0] ? " " : "",
               when);
      safe_copy(s_status_message, sizeof(s_status_message), buf);
      break;
    }
  }
}

static void menu_reload(void) {
  update_status_line();
  menu_layer_reload_data(s_menu);
}

static void free_items(void) {
  if (s_items) {
    free(s_items);
    s_items = NULL;
  }
  if (s_sections) {
    free(s_sections);
    s_sections = NULL;
  }
  s_item_count = 0;
  s_item_total = 0;
  s_section_count = 0;
}

static bool alloc_items(uint16_t n) {
  free_items();
  if (n == 0) return true;
  s_items = malloc(sizeof(Item) * n);
  if (!s_items) return false;
  s_sections = malloc(sizeof(Section) * (MAX_SECTIONS < n ? MAX_SECTIONS : n));
  if (!s_sections) {
    free(s_items);
    s_items = NULL;
    return false;
  }
  memset(s_items, 0, sizeof(Item) * n);
  return true;
}

// ---------------------------------------------------------------------------
// AppMessage
// ---------------------------------------------------------------------------

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;

  // Standalone status update (error, no-url, loading, ...)
  if ((t = dict_find(iter, MESSAGE_KEY_AppStatus)) &&
      !dict_find(iter, MESSAGE_KEY_AppEndOfList)) {
    s_status = (uint8_t)tuple_int(t);
    Tuple *msg = dict_find(iter, MESSAGE_KEY_AppStatusMessage);
    safe_copy(s_status_message, sizeof(s_status_message),
              msg ? msg->value->cstring : "");
    if (s_status == ST_ERROR) {
      vibes_short_pulse();
    }
    menu_reload();
    return;
  }

  // Start of a fresh list
  if ((t = dict_find(iter, MESSAGE_KEY_AppListBegin))) {
    uint16_t total = (uint16_t)tuple_int(dict_find(iter, MESSAGE_KEY_AppItemCount));
    APP_LOG(APP_LOG_LEVEL_INFO, "begin: total=%u", total);
    Tuple *name = dict_find(iter, MESSAGE_KEY_AppListName);
    safe_copy(s_list_name, sizeof(s_list_name),
              name ? name->value->cstring : "HEB List");
    s_list_flags = 0;
    // alloc_items() resets the counters, so set s_item_total afterwards.
    if (!alloc_items(total)) {
      s_status = ST_ERROR;
      safe_copy(s_status_message, sizeof(s_status_message),
                "List too large for watch");
      vibes_short_pulse();
      menu_reload();
      return;
    }
    s_item_total = total;
    s_status = ST_LOADING;
    safe_copy(s_status_message, sizeof(s_status_message), "Loading...");
    menu_reload();
    return;
  }

  // One item
  if ((t = dict_find(iter, MESSAGE_KEY_AppItemName))) {
    uint16_t idx = (uint16_t)tuple_int(dict_find(iter, MESSAGE_KEY_AppItemIndex));
    if (!s_items || idx >= s_item_total || idx >= MAX_ITEMS) return;
    Item *it = &s_items[idx];
    safe_copy(it->name, NAME_LEN, t->value->cstring);
    Tuple *loc = dict_find(iter, MESSAGE_KEY_AppItemLocation);
    safe_copy(it->location, LOC_LEN, loc ? loc->value->cstring : "");
    Tuple *grp = dict_find(iter, MESSAGE_KEY_AppItemGroup);
    safe_copy(it->group, GROUP_LEN, grp ? grp->value->cstring : "");
    Tuple *chk = dict_find(iter, MESSAGE_KEY_AppItemChecked);
    it->checked = chk ? (tuple_int(chk) ? 1 : 0) : 0;
    Tuple *qty = dict_find(iter, MESSAGE_KEY_AppItemQty);
    it->qty = (uint8_t)tuple_int(qty);
    if (idx >= s_item_count) {
      s_item_count = idx + 1;
    }
    return;
  }

  // End of list
  if ((t = dict_find(iter, MESSAGE_KEY_AppEndOfList))) {
    s_list_flags = (uint8_t)tuple_int(dict_find(iter, MESSAGE_KEY_AppListFlags));
    Tuple *st = dict_find(iter, MESSAGE_KEY_AppStatus);
    s_status = st ? (uint8_t)tuple_int(st) : ST_READY;
    Tuple *msg = dict_find(iter, MESSAGE_KEY_AppStatusMessage);
    safe_copy(s_status_message, sizeof(s_status_message),
              msg ? msg->value->cstring : "");
    s_updated_at = time(NULL);
    if (s_item_count == 0 && s_status != ST_ERROR) {
      s_status = ST_EMPTY;
    }
    rebuild_sections();
    APP_LOG(APP_LOG_LEVEL_INFO, "end: count=%u total=%u secs=%u",
            s_item_count, s_item_total, s_section_count);
    menu_reload();
    return;
  }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason,
                                  void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_menu = menu_layer_create(bounds);
  menu_layer_set_normal_colors(s_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_menu, GColorWhite, GColorBlack);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
      .get_num_sections = get_num_sections,
      .get_num_rows = get_num_rows,
      .get_cell_height = get_cell_height,
      .get_header_height = get_header_height,
      .draw_row = draw_row,
      .draw_header = draw_header,
      .select_click = select_click,
      .select_long_click = select_long_click,
  });
  menu_layer_set_click_config_onto_window(s_menu, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu));
}

static void window_unload(Window *window) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
}

static void init(void) {
  s_font_name = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_font_sub = fonts_get_system_font(FONT_KEY_GOTHIC_14);

  // Opt into touch navigation (Core Time 2 / Core 2 Duo): the firmware's gesture
  // bridge maps swipes/taps on the MenuLayer to up/down/select button presses
  // and the menu scrolls by touch natively. Buttons keep working either way;
  // takes effect once "Touch navigation" is enabled in the watch's settings.
  app_touch_navigation_enable(true);

  safe_copy(s_list_name, sizeof(s_list_name), "HEB List");
  safe_copy(s_status_message, sizeof(s_status_message), "Connecting...");

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load,
      .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(512, 64);
}

static void deinit(void) {
  window_destroy(s_window);
  free_items();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}