---
status: active
audience: public
last-verified: 2026-09-13
---

# Zanna.Game.Config

`Config` wraps a parsed JSON value and resolves dotted/JSONPath-style paths. Create one with the
static `Load` or `FromString` factories, which return a `Zanna.Game.Config` or null.

## Construction

- `Zanna.Game.Config.Load(path)` returns null for a null or missing path, an empty file, invalid
  JSON, or allocation failure. Other open/read/stat failures can still trap.
- `Zanna.Game.Config.FromString(json)` returns null for null or invalid JSON.

## Queries

| Member | Return | Current behavior |
|---|---|---|
| `GetInt(path, default)` | `Integer` | Coerces numbers, booleans, and numeric strings. Returns `default` when the path is absent **or** the present value is not int-convertible (object, array, null, or non-numeric string) rather than a coerced `0` (VDOC-245). |
| `GetStr(path, default)` | `String` | Converts strings, numbers, and booleans to text. Returns the supplied `default` when the path is absent or the value is not string-convertible (object/array/null), rather than an empty string (VDOC-245). The registry types the return as `str`, so the result assigns to a `String` local and passes to `String` parameters directly (VDOC-237). |
| `GetBool(path, default)` | `Boolean` | Uses integer coercion and returns whether it is nonzero. Returns `default` when the path is absent or the value is not int-convertible, rather than `false` (VDOC-245). |
| `Has(path)` | `Boolean` | True when the path resolves to a non-null runtime value. |

Queries are allocation-free apart from the caller-owned string that `GetStr` returns: existence
checks route through the non-retaining `rt_jsonpath_has`, so repeated lookups no longer retain a
reference into the parsed JSON tree (VDOC-236).

## Example

```zia
module ConfigExample;

func start() {
    var config = Zanna.Game.Config.FromString(
        "{\"physics\":{\"gravity\":78},\"debug\":true}"
    );
    var gravity = config.GetInt("physics.gravity", 100);
    var debug = config.GetBool("debug", false);
    Zanna.Terminal.SayInt(gravity);
    Zanna.Terminal.SayBool(debug);
}
```
