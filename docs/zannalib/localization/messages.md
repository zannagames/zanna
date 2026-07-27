---
status: active
audience: public
last-verified: 2026-07-26
---

# Messages & Plural Rules

**Part of [Zanna Runtime Library](../README.md) › [Localization](README.md)**

Translation catalogs with fallback chains and CLDR-style plural category selection.

---

## Zanna.Localization.MessageBundle

Translation catalog keyed by message ID. Supports placeholder interpolation, fallback stacks, and plural-aware lookup.

### Methods

| Method | Signature | Description |
|---|---|---|
| `New()` | `MessageBundle()` | Empty bundle bound to the current locale. |
| `LoadFromJson(loc, path)` | `MessageBundle(Locale, String)` | Load from filesystem JSON. |
| `LoadFromAsset(loc, name)` | `MessageBundle(Locale, String)` | Load from ZPAK asset. |
| `FromMap(loc, map)` | `MessageBundle(Locale, runtime Map of String)` | Use an existing raw-string map. |
| `Get(key)` | `String(String)` | Traps when no bundle in the chain has `key`. |
| `TryGet(key)` | `Option[String](String)` | Returns `Some(value)` when present, including empty translations; `None` when missing. |
| `GetOr(key, default)` | `String(String, String)` | Returns the resolved value or the provided fallback string. |
| `Has(key)` | `Bool(String)` | |
| `Format(key, vars)` | `String(String, runtime Map of String)` | `{name}`-style placeholders. |
| `FormatWith(key, values)` | `String(String, runtime List of String)` | Positional `{0}`, `{1}`, … |
| `Plural(key, n, vars)` | `String(String, Int, runtime Map of String)` | Plural-aware lookup (see Notes). |
| `Fallback(other)` | `MessageBundle(MessageBundle)` | Attach a fallback and return this bundle; traps when the proposed chain reaches this bundle. |
| `Keys()` | `Object()` (runtime `List` of `String`) | Snapshot of keys defined by this bundle (excludes fallback); order is unspecified. |

`TryGet` returns an Option, so an intentional empty translation (`Some("")`) is distinguishable from a missing key (`None`). Use `GetOr` when a plain fallback string is enough.

### Properties

| Property | Type | Description |
|---|---|---|
| `Locale` | `Locale` | |
| `Count` | `Int` | Entries in this bundle (excludes fallback). |

### Notes

- **Fallback chain.** Each bundle lookup checks the raw key, then locale-qualified keys using `<locale-tag>:<key>` across that bundle's locale fallback chain, then repeats on its explicit fallback. A lookup examines at most 16 bundles. `Fallback` validates the entire proposed chain and traps on any cycle, regardless of chain length.
- **Plural.** `Plural(key, n, vars)` evaluates the bound locale's cardinal plural category for `n`, then looks up `<key>.<category>` (falling through to `<key>.other` if missing). `{n}` is added to a temporary copy of `vars`; the caller's map is not mutated. The injected value is rendered with the bound locale's `numbers.digits` set, matching `RelativeTimeFormat`.
- **Value types.** `FromMap`, JSON loaders, `Format(vars)`, and `Plural(vars)` require raw runtime string values. In Zia, create the runtime map with `Zanna.Collections.Map.New()` and populate it with `Map.SetStr`; a typed `Map[String, String]` currently boxes its values and is not compatible. `FormatWith(values)` likewise requires a runtime list containing raw strings; a string `Split(...).ToList()` result is one compatible source. Oversized positional indices are preserved literally.
- **Missing placeholders.** Placeholders referencing absent keys in `vars` are preserved literally, e.g. `{name}` stays `{name}`.
- **Escaping.** Use `{{` and `}}` for literal braces.
- JSON loaders expect a flat `{"key": "value", ...}` object and reject non-string values.
- `FromMap` retains the supplied runtime map rather than cloning it. Mutating that map later
  changes subsequent bundle lookups; `Keys()` itself returns a new snapshot list.
- `Keys()` is registered as opaque `Object`, so direct `.Count` chaining is inferred against
  `MessageBundle` and fails. Use `Zanna.Collections.List.get_Count(bundle.Keys())` and
  `Zanna.Collections.List.Get(...)` explicitly.

### Message JSON shape

Message files are independent of the locale-data schema. Their root is one flat JSON object:

```json
{
  "greet": "Hello, {name}!",
  "items.one": "1 item",
  "items.other": "{n} items"
}
```

Keys and values must be strings. Nested objects, arrays, numbers, booleans, and JSON null values
are rejected as message values. The loader does not impose the locale file's 256 KB or 256-byte
field caps.

### Zia Example

```zia
module MessageBundleDemo;

bind Zanna.Terminal;
bind Zanna.Collections.Map as Map;
bind Zanna.Localization.Locale as Locale;
bind Zanna.Localization.MessageBundle as MessageBundle;

func start() {
    var entries = Map.New();
    Map.SetStr(entries, "greet", "Hello, {name}!");
    Map.SetStr(entries, "items.one", "1 item");
    Map.SetStr(entries, "items.other", "{n} items");
    var msgs = MessageBundle.FromMap(Locale.Parse("en-US"), entries);

    var vars = Map.New();
    Map.SetStr(vars, "name", "Alice");
    Say(msgs.Format("greet", vars)); // "Hello, Alice!"

    var empty = Map.New();
    Say(msgs.Plural("items", 1, empty)); // "1 item"
    Say(msgs.Plural("items", 5, empty)); // "5 items"
}
```

Locale-qualified fallback example:

```zia
module QualifiedMessages;

bind Zanna.Terminal;
bind Zanna.Collections.Map as Map;
bind Zanna.Localization.Locale as Locale;
bind Zanna.Localization.MessageBundle as MessageBundle;

func start() {
    var entries = Map.New();
    Map.SetStr(entries, "fr:greet", "Bonjour");
    Map.SetStr(entries, "root:greet", "Hello");
    var msgs = MessageBundle.FromMap(Locale.Parse("fr-CA"), entries);

    Say(msgs.Get("greet")); // "Bonjour"
}
```

### See Also

- [PluralRules](#zannalocalizationpluralrules) — underlying category evaluator.
- [Data files](data-files.md) — locale records and the plural-rule DSL used to select message forms.

---

## Zanna.Localization.PluralRules

CLDR plural category selection for cardinal and ordinal numeric forms.

### Methods

| Method | Signature | Description |
|---|---|---|
| `ForLocale(loc)` | `PluralRules(Locale)` | |
| `Cardinal(n)` | `String(Float)` | Returns `"zero"/"one"/"two"/"few"/"many"/"other"`. |
| `CardinalInt(n)` | `String(Int)` | Integer fast path. |
| `Ordinal(n)` | `String(Int)` | English: `"one"/"two"/"few"/"other"` (1st/2nd/3rd/4th). |
| `Categories()` | `Object()` (runtime `List` of `String`) | Distinct categories appearing across this locale's cardinal and ordinal rule chains, preserving first occurrence. |

### Notes

- **Operands.** CLDR plural operands (`n`, `i`, `v`, `f`, `t`) are computed from the numeric input:
  - `n` = absolute value
  - `i` = integer part
  - `v` = visible fraction digit count (incl. trailing zeros)
  - `f` = visible fraction digits as integer (with trailing zeros)
  - `t` = visible fraction digits without trailing zeros
- A `Float` does not retain source spelling, so lexical trailing zeros cannot survive the call (`1.20` has the same value as `1.2`). Non-integer floats are rendered with C-locale `%.15g` before deriving `v`, `f`, and `t`; integer-valued doubles use `v=f=t=0`. The current derivation does not apply an exponent from that spelling, so values such as `0.0000001` incorrectly receive `v=f=t=0` (VDOC-076).
- Loaded locale plural rules support CLDR-style range/list predicates: `in`, `not in`, `within`, `not within`, comma-separated lists, and `a..b` ranges. `in` matches integral values only; `within` also matches fractional values.
- Despite the separate `CardinalInt` entry point, rule evaluation converts operands and literals
  to binary64. Equality, ranges, and `mod` can therefore be wrong above 2^53 (VDOC-083).
- Nonfinite `Cardinal` inputs return `"other"`.
- **en-US** cardinal: `one` for `i=1 && v=0`; else `other`.
- **en-US** ordinal: `one` for `n mod 10 = 1 && n mod 100 != 11`; `two` for `n mod 10 = 2 && n mod 100 != 12`; `few` for `n mod 10 = 3 && n mod 100 != 13`; else `other`.
- `Categories()` is registered as opaque `Object`; use the explicit
  `Zanna.Collections.List` accessors. Each call also currently leaks one retained string reference
  per category in its returned list (VDOC-077).

### Zia Example

```zia
module PluralRulesDemo;

bind Zanna.Terminal;
bind Zanna.Localization.Locale as Locale;
bind Zanna.Localization.PluralRules as PluralRules;

func start() {
    var pr = PluralRules.ForLocale(Locale.Parse("en-US"));
    Say(pr.CardinalInt(1)); // "one"
    Say(pr.CardinalInt(5)); // "other"
    Say(pr.Ordinal(11));    // "other" (11th)
    Say(pr.Ordinal(21));    // "one" (21st)
}
```

### See Also

- [Formatting › RelativeTimeFormat](formatting.md#zannalocalizationrelativetimeformat) — the primary consumer of `PluralRules` inside the runtime.
