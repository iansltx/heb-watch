#!/usr/bin/env python3
"""Mock of HEB's shared-list GraphQL gateway, for emulator/offline testing.

Serves the same response shape as POST https://www.heb.com/graphql for the
getSharedList operation, with permissive CORS so the phone-side JS can reach it.
Run it, then set the app's "Shared list URL" to:

    http://<host>:8917/graphql/<any-uuid>

If HEB's bot protection ever blocks the watch app's phone traffic, the same
script can be adapted as a relay to the real gateway.
"""

import json
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

ITEMS = [
    {"id": "11111111-1111-4111-8111-111111111111", "checked": False, "quantity": 1,
     "groupHeader": "Bakery & bread",
     "product": {"fullDisplayName": "H-E-B Bakery Double Chocolate Chip Muffins, 4 ct",
                 "productLocation": {"location": "In Bakery"}}},
    {"id": "22222222-2222-4222-8222-222222222222", "checked": True, "quantity": 1,
     "groupHeader": "Bakery & bread",
     "product": {"fullDisplayName": "H-E-B Essential Grains Oat & Nut Sliced Bread, 24 oz",
                 "productLocation": {"location": "Aisle 4"}}},
    {"id": "33333333-3333-4333-8333-333333333333", "checked": False, "quantity": 2,
     "groupHeader": "Beverages",
     "product": {"fullDisplayName": "H-E-B Dr. B Soda 12 pk Cans - Pure Cane Sugar, 12 oz",
                 "productLocation": {"location": "Aisle 11"}}},
    {"id": "44444444-4444-4444-8444-444444444444", "checked": False, "quantity": 1,
     "groupHeader": "Beverages",
     "product": {"fullDisplayName": "H-E-B Unsweetened Grapefruit Sparkling Water 12 pk Cans",
                 "productLocation": {"location": "Aisle 11"}}},
    {"id": "55555555-5555-4555-8555-555555555555", "checked": False, "quantity": 1,
     "groupHeader": "Dairy & eggs",
     "product": {"fullDisplayName": "H-E-B Heavy Whipping Cream, 32 oz",
                 "productLocation": {"location": "In Dairy on the Back Wall"}}},
    # generic (free-text) item, no product record
    {"id": "66666666-6666-4666-8666-666666666666", "checked": False, "quantity": 1,
     "groupHeader": "Dairy & eggs",
     "genericName": "Sour cream (whatever kind)"},
    {"id": "77777777-7777-4777-8777-777777777777", "checked": False, "quantity": 1,
     "groupHeader": "Everyday essentials",
     "product": {"fullDisplayName":
                 "Cascade Complete Dishwasher Detergent Powder, Fresh Scent, 75 oz (a very long "
                 "product name that definitely wraps across multiple lines on the watch screen)",
                 "productLocation": {"location": "Aisle 21"}}},
    {"id": "88888888-8888-4888-8888-888888888888", "checked": True, "quantity": 1,
     "groupHeader": "Everyday essentials",
     "product": {"fullDisplayName": "Dawn Ultra Original Scent Liquid Dish Soap, 67 oz",
                 "productLocation": {"location": "Aisle 21"}}},
]


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            body = json.loads(raw or b"{}")
        except (ValueError, json.JSONDecodeError):
            body = {}
        list_id = None
        try:
            list_id = body["variables"]["input"]["id"]
        except (KeyError, TypeError):
            pass
        if not list_id:
            list_id = "00000000-0000-4000-8000-000000000000"
        m = re.match(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$",
                     str(list_id), re.I)
        if not m:
            payload = {"errors": [{"message": "invalid list id"}]}
        else:
            payload = {
                "data": {
                    "getShoppingListV2": {
                        "id": list_id,
                        "name": "Allandale Shopping List",
                        "totalItemCount": len(ITEMS),
                        "itemPage": {"items": ITEMS},
                        "__typename": "ShoppingListV2",
                    }
                }
            }
        data = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Accept")
        self.end_headers()
        self.wfile.write(data)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Accept")
        self.end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8917
    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"mock HEB GraphQL on http://localhost:{port}/graphql/<uuid> (Ctrl+C to stop)")
    server.serve_forever()