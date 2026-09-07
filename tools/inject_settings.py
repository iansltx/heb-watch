#!/usr/bin/env python3
"""Inject Clay settings into the pypkjs (emulator) localStorage for this app.

Used to test the app in the QEMU emulator, where the phone-side settings UI
is not available. Run the mock gateway first (tools/mock-heb-graphql.py), then:

    python3 tools/inject_settings.py basalt "http://localhost:8917/graphql/<uuid>"
    pebble install --emulator basalt

Usage: inject_settings.py <platform> <list_url> [hide_checked] [sort_order]
"""

import dbm.dumb
import glob
import json
import os
import sys

UUID = "7f4d2c88-9b1e-4d6a-a3f2-58c07d4e91ab"
PERSIST = os.path.expanduser("~/Library/Application Support/Pebble SDK")

platform = sys.argv[1] if len(sys.argv) > 1 else "basalt"
url = sys.argv[2] if len(sys.argv) > 2 else ""
hide = (sys.argv[3] if len(sys.argv) > 3 else "false") == "true"
sort_order = sys.argv[4] if len(sys.argv) > 4 else "category"
reset = (sys.argv[5] if len(sys.argv) > 5 else "false") == "true"

sdk_dirs = sorted(glob.glob(os.path.join(PERSIST, "[0-9]*.[0-9]*.[0-9]*")), reverse=True)
if not sdk_dirs:
    raise SystemExit("no Pebble SDK found in %s" % PERSIST)

path = os.path.join(sdk_dirs[0], platform, "localstorage", UUID)
os.makedirs(os.path.dirname(path), exist_ok=True)
db = dbm.dumb.open(path, "c")
settings = {"ListUrl": url, "HideChecked": hide, "SortOrder": sort_order}
if reset:
    settings["ResetChecks"] = True
db["clay-settings"] = json.dumps(settings)
db.close()
print("injected settings for %s (%s): %s sort=%s reset=%s" %
      (platform, os.path.basename(sdk_dirs[0]), url, sort_order, reset))