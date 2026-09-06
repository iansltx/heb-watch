# HEB shared shopping list — reverse engineering notes

Findings from reverse engineering `https://www.heb.com/shopping-list/shared/<uuid>`
(captured 2026-09-06 via a real Chrome session).

## Surface

`www.heb.com` is a Next.js (pages router) app behind Imperva (Incapsula) bot protection.

* Plain HTTP clients (curl, python) **can** reach `https://www.heb.com/graphql` — the
  endpoint is not hard-gated on page-load WAF rules — but repeated non-browser traffic
  from one IP gets flagged with `502 Bad Gateway` + an injected `/_Incapsula_Resource`
  sensor script. Page routes (`/shopping-list/...`) are more aggressively protected
  (hard block pages for curl; `401 errorCode 15` for headless Chrome).
* Static assets (`cx.static.heb.com/_next/...`) are not protected and can be fetched freely.
* On-device behavior (iOS WebView / PebbleKit JS XHR) still needs to be verified — the
  app is designed to fail gracefully and surface the error.

## Data API

GraphQL gateway: `POST https://www.heb.com/graphql`

* Supports **arbitrary full query text** — APQ (`persistedQuery` sha256 hashes) is used
  by the site, but the gateway also executes ad-hoc queries, so we ship a minimal query
  instead of replaying the site's 4KB document.
* Introspection is disabled (`INTROSPECTION_DISABLED`), but validation errors leak full
  input type definitions (useful for probing).
* The browser sends `Content-Type: application/json`; no cookies needed for public
  shared lists.

### Query (used by this app)

```graphql
query getSharedList($input: GetShoppingListInputV2!) {
  getShoppingListV2(input: $input) {
    ... on ShoppingListV2 {
      id
      name
      totalItemCount
      itemPage {
        thisPage { totalCount page size sort sortDirection }
        items {
          id
          checked
          quantity
          groupHeader
          ... on ProductShoppingListItemV2 {
            product {
              fullDisplayName
              productLocation { location }
            }
          }
          ... on GenericShoppingListItemV2 { genericName }
        }
      }
    }
    ... on ShoppingListErrorV2 { code title message }
  }
}
```

Variables (schema: `GetShoppingListInputV2 { id: ID!, page: ShoppingListItemPageInputV2 }`,
`ShoppingListItemPageInputV2 { page=0, size=5000, sort=CATEGORY, sortDirection=ASC }`):

```json
{"input": {"id": "<list-uuid>", "page": {"page": 0, "size": 5000, "sort": "CATEGORY", "sortDirection": "ASC"}}}
```

`page` is optional; defaults request the full dataset.

### Response shape

```json
{"data": {"getShoppingListV2": {
  "id": "...", "name": "Allandale Shopping List", "totalItemCount": 121,
  "itemPage": {"thisPage": {...}, "items": [
    {"id": "<item-uuid>", "checked": true, "quantity": 1, "groupHeader": "Bakery & bread",
     "product": {"fullDisplayName": "H-E-B Bakery ...", "productLocation": {"location": "In Bakery"}}}
  ]}}}}
```

Notes:

* `itemPage.thisPage.totalCount` was 108 while `totalItemCount` was 121 for the test list
  (server-side quirk — some items are not returned in the page; treat `items` as truth).
* `location` strings look like `Aisle 4`, `In Dairy on the Back Wall`, `In Produce`, etc.
* Items are an interface: `ProductShoppingListItemV2` (has `product`) or
  `GenericShoppingListItemV2` (has `genericName`).

## Check-off (marking items purchased)

* Items carry a persistent `checked` boolean, but **no mutation on the web gateway sets
  it**: `UpdateShoppingListItemInputV2 = {listId, itemId, genericName, note,
  quantityOrWeight, page}` — no `checked`. Brute-forced candidate mutation names
  (`checkOffItem`, `markItemPurchased`, ...) all 404. The web bundle contains no
  check-off mutation. The state is presumably managed by the H-E-B mobile app via a
  separate (undiscovered) API.
* The shared list page is **view-only** for guests ("To edit, copy to your lists");
  the site redirects guests to login when they try to interact.

**Conclusion:** server-side check-off is not reachable with the public API. This app
implements check-off locally on the phone (persisted per item id in PebbleKit JS
`localStorage`), merged over live server state on each refresh.

## Settings

* App settings page is generated locally by Clay (`@rebble/clay`) as a `data:` URI —
  no hosting required. Users paste the shared-list URL; the list id (`uuid`) is
  extracted with a regex and everything else is ignored.