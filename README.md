# HEB List (Pebble)

A Pebble watchapp (Core Devices SDK / `pebble-tool`) that shows an H-E-B shared
shopping list on your wrist and lets you check items off while you shop.

* Item name is the top line(s) — long names **wrap** (up to 3 lines) instead of truncating.
* Aisle / store location is shown as the subtitle.
* Items are grouped by category with inverted section headers.
* Select toggles an item's check (box fills + strikethrough); long-press select
  refreshes from H-E-B; select on the info row also refreshes.
* Sort order is configurable in settings, matching H-E-B's own options:
  category (default), most efficient route (store-location order, asc/desc),
  A-Z, Z-A — passed through to H-E-B's query; on any non-category sort the
  category headers are hidden.
* Touch (Core Time 2 / Core 2 Duo): swipe to scroll the list, tap an item to
  check it off, tap the info row to refresh. Requires *Touch navigation*
  enabled in the watch's system settings; buttons keep working either way.
* Check-off state is **local** (per item, persisted on the phone) — H-E-B's public
  API does not expose a check-off mutation for shared lists, which are view-only
  to guests. See `docs/api-notes.md` for details.
* The list is cached on the phone, so the app opens instantly with the last known
  list and refreshes in the background.
* The header row shows the remaining count in the title — `(5) List name`, or
  `(5*)` when displaying the cached list — and shrinks to one line; a second
  status line appears only while loading or on errors.

## Setup

Requirements: [uv](https://docs.astral.sh/uv/getting-started/installation/), then:

```sh
uv tool install pebble-tool
pebble sdk install   # first run, downloads toolchain + QEMU
pebble package install   # installs @rebble/clay from package.json deps
```

## Build

```sh
pebble build           # produces build/heb-watch.pbw
pebble install --emulator basalt   # run in QEMU
pebble logs --emulator basalt
```

Gotcha: when changing `messageKeys` in `package.json`, run `pebble clean` before
the next build — the SDK does not regenerate the C-side message-key constants
(`build/include/message_keys.auto.*`) on an incremental build, leaving the watch
and phone halves disagreeing on key indices.

## Install on your watch

The bundle targets every platform (aplite … emery/gabbro), so the same `.pbw`
runs on a Pebble Time 2 (emery) via the Core Devices app. Two ways to get it
there:

1. Copy `build/heb-watch.pbw` to your phone (AirDrop / file transfer) and open
   it with the Core Devices app (share sheet on iOS, file manager on Android).
2. Or enable the Developer Connection in the Core Devices app (phone and
   computer on the same Wi-Fi) and run `pebble install --phone <phone-ip>`.

Then open the app's **Settings** (gear) in the Core Devices app and paste your
shared list URL, e.g. `https://www.heb.com/shopping-list/shared/<uuid>`.

Verified live (Sept 2026): real list from `www.heb.com/graphql` through the
Core Devices app on a Pebble Time 2 — fetch, display, and check-off all work;
the phone-side request is not blocked by H-E-B's bot protection.

## How it works

```
HEB (www.heb.com/graphql)  <-minimal GraphQL query-  PebbleKit JS (src/pkjs)
                              |  localStorage: settings, local checks, cache
                              v  AppMessage (item name/location/checked...)
                          Watch UI (src/c) — MenuLayer, wrapped text, checkbox
```

## Emulator testing (no real list needed)

The settings UI can't be driven in QEMU, so settings are injected directly into
the emulator JS runtime's localStorage:

```sh
python3 tools/mock-heb-graphql.py &            # mock HEB gateway on :8917
python3 tools/inject_settings.py basalt \
  "http://localhost:8917/graphql/12345678-1234-4321-8765-432109876543"
pebble install --emulator basalt
```

The injector also accepts `[hide_checked] [sort_order]` (one of `category`,
`aisle`, `aisle-desc`, `az`, `za` — `aisle` is H-E-B's "most efficient route")
for testing sorts; the mock honors the requested sort, and the real gateway's
sort enum is documented in `docs/api-notes.md`.

Any URL containing a UUID that is **not** heb.com is used verbatim as the GraphQL
endpoint (that's how the mock works; it also doubles as a relay escape hatch if
H-E-B's bot protection ever blocks the phone). See `docs/api-notes.md` for the
reverse-engineered H-E-B API.