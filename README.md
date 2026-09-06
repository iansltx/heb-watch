# HEB List (Pebble)

A Pebble watchapp (Core Devices SDK / `pebble-tool`) that shows an H-E-B shared
shopping list on your wrist and lets you check items off while you shop.

* Item name is the top line(s) — long names **wrap** instead of truncating.
* Aisle / store location is shown as the subtitle.
* Select toggles an item's check; long-press select refreshes from H-E-B.
* Check-off state is **local** (per item, persisted on the phone) — H-E-B's public
  API does not expose a check-off mutation for shared lists, which are view-only
  to guests. See `docs/api-notes.md` for details.

## Setup

Requirements: [uv](https://docs.astral.sh/uv/getting-started/installation/), then:

```sh
uv tool install pebble-tool
pebble sdk install   # first run, downloads toolchain + QEMU
pebble package install   # installs @rebble/clay from package.json deps
```

## Build

```sh
pebble build           # produces build/HEB-List.pbw
pebble install --emulator basalt   # run in QEMU
pebble logs --emulator basalt
```

Install on a real watch with the Core Devices app (sideload the `.pbw`), then open
the app's **Settings** (gear) on your phone and paste your shared list URL, e.g.
`https://www.heb.com/shopping-list/shared/<uuid>`.

## How it works

```
HEB (www.heb.com/graphql)  <-minimal GraphQL query-  PebbleKit JS (src/pkjs)
                              |  localStorage: settings, local checks, cache
                              v  AppMessage (item name/location/checked...)
                          Watch UI (src/c) — MenuLayer, wrapped text, checkbox
```

See `docs/api-notes.md` for the reverse-engineered H-E-B API.