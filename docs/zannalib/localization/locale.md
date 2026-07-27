---
status: active
audience: public
last-verified: 2026-07-26
---

# Locale, LocaleInfo, LocaleManager

**Part of [Zanna Runtime Library](../README.md) › [Localization](README.md)**

---

## Zanna.Localization.Locale

Immutable handle representing the runtime's BCP-47-shaped locale tag. Parsed from strings like
`"en-US"`, `"zh-Hans-CN"`, and `"fr-FR"`; the current parser implements a constrained subset
of RFC 5646 rather than a complete language-tag validator.

**Type:** Reference class (immutable, GC-managed).

### Methods

| Method | Signature | Description |
|---|---|---|
| `Parse(tag)` | `Locale(String)` | Canonicalize + validate; **traps** on invalid input. |
| `TryParse(tag)` | `Option[Locale](String)` | Returns `Some(Locale)` on success or `None` on failure, instead of trapping. |
| `FromParts(lang, script, region)` | `Locale(String, String, String)` | Concatenate the supplied parts and parse the result. Empty script/region strings mean "absent". |
| `Invariant()` | `Locale()` | Returns the `root` locale (universal fallback). |
| `Equals(other)` | `Bool(Locale)` | Canonical-tag equality for non-null handles. |
| `Fallbacks()` | `Object()` (runtime `List` of `Locale`) | Returns the walk-order fallback chain. |
| `ToString()` | `String()` | Canonical tag string. |

`TryParse` returns an Option, so a rejected tag is reported as `None` rather than a null handle.

### Properties

| Property | Type | Description |
|---|---|---|
| `Language` | `String` | Lowercased primary subtag (e.g. `"en"`). |
| `Script` | `String` | Title-case script subtag (e.g. `"Latn"`) or `""`. |
| `Region` | `String` | Uppercase region subtag (e.g. `"US"`) or `""`. |
| `Tag` | `String` | Canonical BCP-47 tag (e.g. `"en-Latn-US"`). |

### Notes

- Input canonicalization lowercases the language, Title-cases the script, uppercases an alphabetic region, normalizes underscores to dashes, and lowercases extension/private-use subtags. `Locale.Parse` does **not** accept POSIX encoding or modifier suffixes such as `.UTF-8` or `@latin`; only the system-locale platform adapters strip those suffixes before parsing.
- Leading, trailing, and duplicate separators are rejected (`"-en"`, `"en-"`, `"en--US"`).
- The accepted shape is a 2–8-letter primary language followed by optional script, region,
  variants, extensions, and a private-use tail. Variants, accepted extensions, and accepted
  private-use tails are preserved in `Tag`. Fallbacks walk from the full tag to its base
  language/script/region chain, then `root`.
- Parsed-but-unloaded locales are still usable for `Tag` / `Fallbacks()`, but their info queries use the baked en-US invariant data. Loading the tag does not retroactively bind an already-created handle; parse it again, use the handle returned by `LocaleManager.Load`, or pass it to `SetCurrent` after loading.
- `Parse` traps on empty input, a primary language shorter than two characters, subtags longer than eight characters, malformed extension/private-use structure, or non-ASCII-alphanumeric subtag content.
- The parser follows the RFC 5646 well-formedness grammar: pure private-use tags
  (`x-private`, empty `Language`), extended language subtags (`zh-cmn-Hans-CN`, up to three
  3-letter subtags after a 2-3 letter language), and long tags (canonical buffer 127 bytes)
  are accepted, while empty extensions (`en-a-b-foo`, `en-a-x-foo`), script or region after a
  variant (`en-abcde-Latn`, `en-abcde-US`), and duplicate variants (`sl-rozaj-rozaj`) are
  rejected. Subtag validity against the IANA registry (including grandfathered tags) is not
  checked.
- `FromParts` requires one pre-split subtag per argument: values containing `-` or `_` trap,
  and each supplied part must classify as the field it was passed as (`FromParts("en", "US", "")`
  traps because `US` is a region shape, not a script).
- Null handles are invariant-shaped across the whole API, including equality:
  `Equals(null, Invariant())` returns `true`.
- The runtime registry currently exposes `Fallbacks()` as opaque `Object`, so Zia cannot infer that it is iterable. Traverse it through `Zanna.Collections.List` as shown below.

### Zia Example

```zia
module LocaleDemo;

bind Zanna.Terminal;
bind Zanna.Collections.List as RuntimeList;
bind Zanna.Localization.Locale as Locale;

func start() {
    var us = Locale.Parse("en-US");
    Say(us.Language); // "en"
    Say(us.Region);   // "US"
    Say(us.Tag);      // "en-US"

    var chain = us.Fallbacks();
    var i = 0;
    while i < RuntimeList.get_Count(chain) {
        Say(Locale.ToString(RuntimeList.Get(chain, i))); // en-US, en, root
        i += 1;
    }
}
```

### BASIC Example

```basic
DIM localeObj AS Zanna.Localization.Locale
localeObj = Zanna.Localization.Locale.Parse("en-US")
PRINT localeObj.ToString()
PRINT Zanna.Localization.Locale.get_Language(localeObj)
```

### See Also

- [LocaleInfo](#zannalocalizationlocaleinfo) — queries about a Locale.
- [LocaleManager](#zannalocalizationlocalemanager) — registry of loaded locales.

---

## Zanna.Localization.LocaleInfo

Static utility: queries about a Locale's display properties.

**Type:** Static utility.

### Methods

| Method | Signature | Description |
|---|---|---|
| `DisplayName(loc, inLoc)` | `String(Locale, Locale)` | Combined display name (e.g. `"English (United States)"`). |
| `LanguageName(loc, inLoc)` | `String(Locale, Locale)` | Native language name. |
| `RegionName(loc, inLoc)` | `String(Locale, Locale)` | Native region name. |
| `TextDirection(loc)` | `String(Locale)` | `"ltr"` or `"rtl"`. |
| `FirstDayOfWeek(loc)` | `Int(Locale)` | 0 = Sunday .. 6 = Saturday. |
| `IsRightToLeft(loc)` | `Bool(Locale)` | Convenience for `TextDirection == "rtl"`. |
| `MeasurementSystem(loc)` | `String(Locale)` | `"metric"` / `"us"` / `"uk"`. |
| `Currency(loc)` | `String(Locale)` | Default ISO-4217 code (e.g. `"USD"`). |

### Notes

- `inLocale` is accepted but ignored in v1 (only en-US is baked); display names are always emitted in the target locale's native language.
- NULL locale falls through to the invariant's values (equal to en-US in v1).

---

## Zanna.Localization.LocaleManager

Static process-global registry of loaded locales + current/system-locale state.

**Type:** Static utility.

### Methods

| Method | Signature | Description |
|---|---|---|
| `Current()` | `Locale()` | Currently active locale. |
| `SetCurrent(loc)` | `Void(Locale)` | Set current; **traps** if locale isn't loaded. |
| `System()` | `Locale()` | System locale detected at first access (may be unloaded); falls back to en-US when detection fails. |
| `Available()` | `Object()` (runtime `List` of `String`) | Registered locale tags in registration order. |
| `IsLoaded(loc)` | `Bool(Locale)` | Whether the locale is in the registry. |
| `LoadFromJson(path)` | `Void(String)` | Load from filesystem; traps on error. |
| `TryLoadFromJson(path)` | `Bool(String)` | Returns `false` on failure instead of trapping. |
| `LoadFromAsset(name)` | `Void(String)` | Load through the asset system (embedded or mounted ZPAK asset). |
| `TryLoadFromAsset(name)` | `Bool(String)` | Returns `false` on failure. |
| `LoadBuiltin(tag)` | `Void(String)` | Load one of the C-baked locales (`"en-US"` only in v1). |
| `Load(tag)` | `Locale(String)` | Canonicalize the tag, then try the registry and filesystem search paths; returns null if invalid or unavailable. |
| `SearchPath()` | `String()` | Current search paths joined with the platform path-list separator. |
| `AddSearchPath(path)` | `Void(String)` | Append a filesystem search dir. |
| `Unload(loc)` | `Bool(Locale)` | Remove a locale; refuses when in use. |
| `Reset()` | `Void()` | Clear search paths and remove unused loaded locales; baked en-US remains. |

### Notes

- First access triggers lazy initialization: it registers baked en-US and detects the system locale. Current uses that locale only when its exact tag is already loaded; otherwise current starts as en-US.
- Registry/current/system operations are synchronized through a process-global rwlock. Formatters
  capture the locale's data pointer at construction and do not re-lock; concurrently mutating one
  formatter's writable properties is not a supported synchronization mechanism.
- `LoadFromJson(path)` and `LoadFromAsset(name)` parse locale JSON and register the canonical tag. `Try*` variants return `false` on missing/malformed input instead of trapping.
- `Load(tag)` looks for `<canonical-tag>.json` in directories added with `AddSearchPath`.
- Baked en-US is authoritative. Loading JSON whose canonical tag is `en-US` reports success but
  leaves the baked record in place.
- `LoadFromJson` / `LoadFromAsset` refuse to replace a loaded locale while any live locale or formatter object still retains that locale data. `Try*` returns `false`; non-try loaders trap after releasing the registry lock.
- `Unload` returns `false` (does not trap) when the locale is the current or system locale, or when other live handles/formatters still hold its data pointer. Passing the only remaining `Locale` handle for that loaded record unbinds that handle and unloads the record.
- `Reset` changes both current and system handles to en-US, clears search paths, and leaves in-use
  loaded records registered so existing locale/formatter objects cannot dangle; unload them after
  those objects are released. It does not re-run system detection.
- On Windows, detection first uses `GetUserDefaultLocaleName`; its environment fallback checks
  `LC_ALL`, `LANG`, then `LC_MESSAGES`. Linux/BSD check `LC_ALL`, `LC_MESSAGES`, then `LANG`.
  macOS deliberately uses only `LC_ALL`, `LANG`, and `LC_MESSAGES`—not CoreFoundation—so a
  GUI-launched process without those variables commonly falls back to en-US.
- Like `Fallbacks()`, `Available()` is registered as opaque `Object`. Call
  `Zanna.Collections.List.get_Count(...)` / `Get(...)` explicitly rather than chaining `.Count`.

### See Also

- [Formatting](formatting.md), [Messages](messages.md), [Collation](collation.md) — consumers of registered locales.
- [Data files](data-files.md) — JSON schema for loaded locales.
