// ---------------------------------------------------------------------------
// HEB List — PebbleKit JS
//
// Fetches a shared H-E-B shopping list from www.heb.com/graphql and streams it
// to the watch. Check-off state is local: HEB's public API exposes no check
// mutation for shared lists (they are view-only to guests), so toggles are
// persisted per item id in localStorage and merged over server state.
// ---------------------------------------------------------------------------

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

// Status codes — must match src/c/main.c
var ST_LOADING = 0;
var ST_READY = 1;
var ST_CACHED = 2;
var ST_ERROR = 3;
var ST_NO_URL = 4;
var ST_EMPTY = 5;

var FLAG_CACHED = 0x01;

var GRAPHQL_URL = 'https://www.heb.com/graphql';

// Sort orders offered in settings, mapped to the gateway's ShoppingListItemPageInputV2
// enum values (mirrors heb.com's own shared-list UI: sort=CATEGORY | STORE_LOCATION |
// ALPHABETICAL, sortDirection=ASC | DESC).
var SORTS = {
  'category':   { sort: 'CATEGORY',       direction: 'ASC'  },
  'aisle':      { sort: 'STORE_LOCATION', direction: 'ASC'  },
  'aisle-desc': { sort: 'STORE_LOCATION', direction: 'DESC' },
  'az':         { sort: 'ALPHABETICAL',   direction: 'ASC'  },
  'za':         { sort: 'ALPHABETICAL',   direction: 'DESC' }
};

// Category headers are only meaningful when the server returns items grouped by
// category; other sorts interleave groups, so they are blanked (one section).
function sortConfig() {
  return SORTS[settings().SortOrder] || SORTS.category;
}

function useCategoryGroups() {
  return sortConfig().sort === 'CATEGORY';
}

// A settings URL pointing at heb.com uses the real gateway. Any other URL is
// used directly as a GraphQL endpoint (proxy/mock for testing, or a relay if
// HEB's bot protection ever blocks the phone). The list id is extracted via
// UUID from wherever it appears in the URL.
function endpointFor(url) {
  return /heb\.com/i.test(url) ? GRAPHQL_URL : String(url).trim();
}

// Keep in sync with src/c/main.c buffer sizes.
var MAX_ITEMS = 300;
var NAME_MAX = 63;
var LOC_MAX = 31;
var GROUP_MAX = 27;
var STATUS_MAX = 90;

var QUERY = [
  'query getSharedList($input: GetShoppingListInputV2!) {',
  '  getShoppingListV2(input: $input) {',
  '    ... on ShoppingListV2 {',
  '      id',
  '      name',
  '      totalItemCount',
  '      itemPage {',
  '        items {',
  '          id',
  '          checked',
  '          quantity',
  '          groupHeader',
  '          ... on ProductShoppingListItemV2 {',
  '            product {',
  '              fullDisplayName',
  '              productLocation { location }',
  '            }',
  '          }',
  '          ... on GenericShoppingListItemV2 { genericName }',
  '        }',
  '      }',
  '    }',
  '    ... on ShoppingListErrorV2 { code title message }',
  '  }',
  '}'
].join('\n');

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

var items = [];          // current snapshot sent to the watch
var fetching = false;
var streaming = false;   // a list stream to the watch is in progress
var pendingFetch = false;

function settings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

function localChecks() {
  try {
    return JSON.parse(localStorage.getItem('heb-checks')) || {};
  } catch (e) {
    return {};
  }
}

function saveLocalChecks(checks) {
  localStorage.setItem('heb-checks', JSON.stringify(checks));
}

function cachedList() {
  try {
    return JSON.parse(localStorage.getItem('heb-cache')) || null;
  } catch (e) {
    return null;
  }
}

function saveCachedList(payload, url) {
  try {
    localStorage.setItem('heb-cache', JSON.stringify({ payload: payload, url: url }));
  } catch (e) {
    // storage full — not fatal
  }
}

function extractListId(url) {
  if (!url) return null;
  var m = String(url).match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/i);
  return m ? m[0].toLowerCase() : null;
}

// Normalize a raw GraphQL list item into the shape sent to the watch.
// Items are an interface: ProductShoppingListItemV2 (product record, has an
// aisle/location) or GenericShoppingListItemV2 (free text, no location).
function mapItem(raw) {
  var name = (raw.product && raw.product.fullDisplayName) || raw.genericName || 'Item';
  var location = (raw.product && raw.product.productLocation &&
                  raw.product.productLocation.location) || '';
  return {
    id: raw.id,
    name: String(name),
    location: String(location),
    group: useCategoryGroups() ? String(raw.groupHeader || '') : '',
    qty: raw.quantity || 1,
    checked: !!raw.checked
  };
}

// ---------------------------------------------------------------------------
// Sending to the watch
// ---------------------------------------------------------------------------

function sendDict(dict, onDone) {
  Pebble.sendAppMessage(dict, function () {
    if (onDone) onDone(true);
  }, function (e) {
    console.log('sendAppMessage failed: ' + JSON.stringify(e));
    if (onDone) onDone(false);
  });
}

function sendStatus(status, message) {
  var dict = { AppStatus: status };
  if (message) {
    dict.AppStatusMessage = String(message).substring(0, STATUS_MAX);
  }
  sendDict(dict);
}

function sendItem(i, onDone) {
  var it = items[i];
  var dict = {
    AppItemIndex: i,
    AppItemName: String(it.name).substring(0, NAME_MAX),
    AppItemChecked: it.checked ? 1 : 0,
    AppItemQty: it.qty || 1
  };
  dict.AppItemLocation = String(it.location || '').substring(0, LOC_MAX);
  dict.AppItemGroup = String(it.group || '').substring(0, GROUP_MAX);
  sendDict(dict, onDone);
}

// Serialize the current snapshot to the watch, item by item (each item fits in
// a single AppMessage). Only one stream may run at a time — interleaved streams
// would mix stale and fresh item content on the watch.
function sendList(listName, cached, onDone) {
  streaming = true;
  var count = items.length;
  sendDict({
    AppListBegin: 1,
    AppItemCount: count,
    AppListName: String(listName || 'HEB List').substring(0, 47)
  }, function (ok) {
    if (!ok) {
      streaming = false;
      if (onDone) onDone(false);
      return;
    }
    var i = 0;

    function next() {
      if (i >= count) {
        var status = count === 0 ? ST_EMPTY : (cached ? ST_CACHED : ST_READY);
        sendDict({ AppEndOfList: 1, AppListFlags: cached ? FLAG_CACHED : 0, AppStatus: status },
                 function () {
                   streaming = false;
                   if (onDone) onDone(true);
                 });
        return;
      }
      sendItem(i, function (okItem) {
        if (!okItem) {
          // Connection dropped mid-list; tell the watch so it is not stuck
          // at "Loading..." (this send will usually fail too, but try).
          sendStatus(ST_ERROR, 'Connection lost while syncing list');
          streaming = false;
          if (onDone) onDone(false);
          return;
        }
        i++;
        next();
      });
    }
    next();
  });
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

function applyLocalChecks(listId, rawItems) {
  var checks = localChecks();
  var listChecks = checks[listId] || {};
  return rawItems.map(function (it) {
    if (listChecks.hasOwnProperty(it.id)) {
      it.checked = !!listChecks[it.id];
    }
    return it;
  });
}

function xhrPost(url, body, onDone) {
  var xhr = new XMLHttpRequest();
  xhr.open('POST', url, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.setRequestHeader('Accept', 'application/json');
  xhr.onload = function () {
    try {
      onDone(xhr.status, xhr.responseText);
    } catch (e) {
      console.log('list handler threw: ' + (e && e.message ? e.message : String(e)));
    }
  };
  xhr.onerror = function () {
    console.log('xhr onerror');
    onDone(0, '');
  };
  xhr.timeout = 20000;
  xhr.ontimeout = function () {
    onDone(0, '');
  };
  xhr.send(body);
}

// A refresh requested while a stream is running is deferred until it finishes.
function listStreamDone() {
  if (pendingFetch && !streaming && !fetching) {
    pendingFetch = false;
    fetchList();
  }
}

function fetchList() {
  if (fetching) return;
  if (streaming) {
    pendingFetch = true;
    return;
  }
  var url = settings().ListUrl;
  var listId = extractListId(url);
  if (!listId) {
    sendStatus(ST_NO_URL, 'Set list URL in phone settings');
    return;
  }
  var endpoint = endpointFor(url);
  if (!/^https?:\/\//i.test(endpoint)) {
    sendStatus(ST_NO_URL, 'Set list URL in phone settings');
    return;
  }
  fetching = true;
  sendStatus(ST_LOADING, 'Loading...');

  var body = JSON.stringify({
    operationName: 'getSharedList',
    query: QUERY,
    variables: {
      input: (function () {
        var input = { id: listId };
        var sort = sortConfig();
        input.page = { page: 0, size: 5000, sort: sort.sort, sortDirection: sort.direction };
        return input;
      })()
    }
  });

  xhrPost(endpoint, body, function (status, text) {
    fetching = false;
    var parsed = null;
    if (status === 200 && text) {
      try {
        parsed = JSON.parse(text);
      } catch (e) {
        parsed = null;
      }
    }

    if (parsed && parsed.data && parsed.data.getShoppingListV2 &&
        parsed.data.getShoppingListV2.itemPage) {
      var list = parsed.data.getShoppingListV2;
      var rawItems = (list.itemPage.items || []).slice(0, MAX_ITEMS).map(mapItem);
      saveCachedList({ name: list.name, items: rawItems }, listId);
      presentList(list.name, rawItems, listId, false);
      return;
    }

    // Failure: fall back to cache if it is for the same list.
    console.log('fetch failed: status=' + status + ' bodyLen=' + (text ? text.length : -1));
    var cache = cachedList();
    if (cache && cache.url === listId && cache.payload && cache.payload.items) {
      presentList(cache.payload.name, cache.payload.items, listId, true);
      return;
    }

    var message = 'Could not reach HEB';
    if (status === 0) {
      message = 'No network connection';
    } else if (status === 401 || status === 403) {
      message = 'Blocked by HEB (' + status + ')';
    } else if (status >= 500) {
      message = 'HEB blocked request (' + status + ')';
    } else if (parsed && parsed.errors && parsed.errors.length) {
      var err = parsed.errors[0];
      if (err && err.message && /Could not resolve|not found/i.test(err.message)) {
        message = 'List not found';
      } else {
        message = 'HEB error: ' + String(err && err.message ? err.message : status).substring(0, 60);
      }
    } else if (parsed && parsed.data && parsed.data.getShoppingListV2 &&
               parsed.data.getShoppingListV2.message) {
      message = parsed.data.getShoppingListV2.message;
    } else if (status) {
      message = 'HTTP ' + status;
    }
    sendStatus(ST_ERROR, message);
  });
}

function presentList(name, rawItems, listId, cached, onDone) {
  var hideChecked = !!settings().HideChecked;
  var all = applyLocalChecks(listId, rawItems);
  items = hideChecked ? all.filter(function (it) { return !it.checked; }) : all;
  sendList(name, cached, onDone ? onDone : listStreamDone);
}

// ---------------------------------------------------------------------------
// Watch -> phone
// ---------------------------------------------------------------------------

Pebble.addEventListener('appmessage', function (e) {
  var dict = e.payload;
  if (dict.AppRefresh) {
    fetchList();
    return;
  }
  if (typeof dict.AppToggleItem !== 'undefined') {
    var idx = dict.AppToggleItem;
    var checked = !!dict.AppItemChecked;
    if (idx >= 0 && idx < items.length) {
      var item = items[idx];
      var url = extractListId(settings().ListUrl);
      if (url) {
        var checks = localChecks();
        if (!checks[url]) checks[url] = {};
        checks[url][item.id] = checked;
        saveLocalChecks(checks);
      }
    }
  }
});

// React to settings saves. Clay's own webviewclosed handler runs first
// (registered in the Clay constructor), persisting to localStorage; ours then
// re-reads them and refreshes the list.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  fetchList();
});

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  var listId = extractListId(settings().ListUrl);
  if (!listId) {
    sendStatus(ST_NO_URL, 'Set list URL in phone settings');
    return;
  }
  // Serve cache immediately (fast startup), then refresh in the background —
  // sequentially, so the two list streams never interleave.
  var cache = cachedList();
  if (cache && cache.url === listId && cache.payload && cache.payload.items) {
    presentList(cache.payload.name, cache.payload.items, listId, true, function () {
      fetchList();
    });
    return;
  }
  fetchList();
});