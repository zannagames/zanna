---
status: active
audience: public
last-verified: 2026-08-17
---

# Formatting

**Part of [Zanna Runtime Library](../README.md) › [Localization](README.md)**

Number, date, relative-time, and list formatters that consume a `Locale`'s data to produce user-facing strings.

---

## Zanna.Localization.NumberFormat

Locale-aware number formatting **and parsing** with configurable fraction digits, grouping, strict-mode ambiguity handling, and seven rounding modes.

### Methods (format)

| Method | Signature | Description |
|---|---|---|
| `New()` | `NumberFormat()` | Construct for the current locale. |
| `ForLocale(loc)` | `NumberFormat(Locale)` | Construct bound to the given locale. |
| `Decimal(n)` | `String(Float)` | Locale-formatted decimal. |
| `DecimalN(n, digits)` | `String(Float, Int)` | Force exactly N fraction digits; `digits` clamps to `0..20`. |
| `Integer(n)` | `String(Int)` | Integer with locale grouping. |
| `Percent(n)` | `String(Float)` | Multiplies by 100; appends percent symbol. |
| `Currency(n)` | `String(Float)` | Uses locale's default currency. |
| `CurrencyOf(n, code)` | `String(Float, String)` | Override ISO-4217 code. |
| `Scientific(n, digits)` | `String(Float, Int)` | Scientific notation; `digits` clamps to `0..20`. |
| `Ordinal(n)` | `String(Int)` | `"1st" / "2nd"` (English rules; other locales pending). |

### Methods (parse)

| Method | Signature | Description |
|---|---|---|
| `ParseDecimal(s)` | `Float(String)` | Strict-or-lenient decimal parse; traps on failure. |
| `TryParseDecimal(s)` | `Option[Float](String)` | Returns `None` on failure, no trap. |
| `ParseInteger(s)` | `Int(String)` | Rejects fractional input. |
| `TryParseInteger(s)` | `Option[Int](String)` | Returns `None` on failure, no trap. |
| `ParseCurrency(s)` | `Float(String)` | Recognizes locale positive/negative framing, symbol, or default code; traps on failure. |
| `TryParseCurrency(s)` | `Option[Float](String)` | Returns `None` on failure, no trap. |

### Properties

| Property | Type | Description |
|---|---|---|
| `Locale` | `Locale` | Read-only. |
| `MinFractionDigits` | `Int` | Min digits after decimal (clamped to `0..20`). Raising it also raises `MaxFractionDigits` when necessary. |
| `MaxFractionDigits` | `Int` | Max digits after decimal (clamped to `0..20`). Lowering it also lowers `MinFractionDigits` when necessary. |
| `UseGrouping` | `Bool` | Enable/disable the locale's thousands separator. |
| `Strict` | `Bool` | Strict parse mode; rejects ambiguous groupings. |
| `RoundingMode` | `String` | `"halfEven"` (default) / `"halfUp"` / `"halfDown"` / `"up"` / `"down"` / `"ceiling"` / `"floor"`. |

### Notes

- Format output uses the locale's `numbers.decimal_sep`, `numbers.group_sep`, `numbers.percent`, `currency.symbol`, and `currency.pattern_*` templates.
- Grouping supports both `group_size` and `secondary_group_size`; locales such as Hindi can emit `1,23,45,678`.
- Integer formatting/parsing is exact across the full signed 64-bit range, including `-9223372036854775808`.
- A valid `numbers.digits` table is honored for both formatting and parsing, so configured
  non-Latin digit sets round-trip. Locale files currently receive only weak UTF-8 span validation;
  see VDOC-069 in the [review findings](../../../misc/reviews/documentation-review-findings.md#vdoc-069--locale-digit-validation-does-not-validate-utf-8).
- Decimal parsing and decimal/scientific formatting use C-locale numeric conversion internally, then apply locale separators, so the host process locale does not change results. There is no scientific-notation parse method.
- Decimal and currency parsing accept the locale's exact `NaN` and infinity tokens (with an
  optional sign), so formatted non-finite values round-trip through `TryParseDecimal` and
  `TryParseCurrency`. Integer parsing rejects the tokens.
- `CurrencyOf` requires a 3-letter uppercase ISO-style code. Non-default valid codes are rendered literally as the symbol placeholder unless the locale has a dedicated symbol table in a future release.
- Currency parsing recognizes the configured symbol, the default code, and any standalone
  3-uppercase-letter ISO code, so non-default `CurrencyOf` output (e.g. en-US
  `CurrencyOf(100, "EUR")` → `"EUR100.00"`) round-trips through `TryParseCurrency`.
- Currency parsing accepts the locale's positive and negative patterns, including accounting parentheses such as `"($1,234.56)"`.
- **Strict mode parses**: strict rejects inputs where a group separator appears at a non-group-size position (e.g. `"1,00"` under en-US where `group_size=3`). Lenient ignores group widths but every separator must still sit between digits: `"1,,2"`, `"1,"`, and `",1"` are rejected in both modes.
- Unknown `RoundingMode` strings silently select `"halfEven"`. The property affects decimal, percent, and currency formatting; integer/ordinal output is exact and `Scientific` uses the C formatter's rounding.
- `halfEven` (banker's rounding) is the default. Under the normal round-to-nearest floating-point environment, halfway values round to the nearest even result.
- `Scientific` localizes digits, the decimal separator, the exponent marker, mantissa and
  exponent sign tokens, and non-finite values (the same locale NaN/infinity tokens as
  `Decimal`).
- The process-wide C numeric locale used by decimal/scientific conversion has a known
  unsynchronized first-use initialization race (VDOC-080). Avoid deliberately racing the first
  NumberFormat calls across threads until that runtime issue is fixed.
- Ordinal in v1 delegates to `Zanna.Text.InvariantNumberFormat.Ordinal` (English suffixes); locale-specific ordinal suffix tables are a future phase.

### Zia Example

```zia
module NumberFormatDemo;

bind Zanna.Terminal;
bind Zanna.Localization.Locale as Locale;
bind Zanna.Localization.NumberFormat as NumberFormat;

func start() {
    var fmt = NumberFormat.ForLocale(Locale.Parse("en-US"));
    Say(fmt.Decimal(1234.5));   // "1,234.5"
    Say(fmt.Currency(1234.56)); // "$1,234.56"
    Say(fmt.Percent(0.125));    // "12.5%"

    var parsed = fmt.ParseDecimal("1,234.5");
    Say(parsed);                // 1234.5
}
```

### BASIC Example

```basic
DIM fmt AS Zanna.Localization.NumberFormat
fmt = Zanna.Localization.NumberFormat.ForLocale(Zanna.Localization.Locale.Parse("en-US"))
PRINT fmt.Currency(99.99)   ' $99.99
```

---

## Zanna.Localization.DateFormat

CLDR-pattern-letter date and time formatting.

### Methods

| Method | Signature | Description |
|---|---|---|
| `New()` | `DateFormat()` | Construct for the current locale. |
| `ForLocale(loc)` | `DateFormat(Locale)` | |
| `Short(ts)` | `String(Int)` | `"3/15/27"` |
| `Medium(ts)` | `String(Int)` | `"Mar 15, 2027"` |
| `Long(ts)` | `String(Int)` | `"March 15, 2027"` |
| `Full(ts)` | `String(Int)` | `"Monday, March 15, 2027"` |
| `Custom(ts, pattern)` | `String(Int, String)` | CLDR pattern letters. |
| `TimeShort(ts)` | `String(Int)` | `"2:30 PM"` |
| `TimeMedium(ts)` | `String(Int)` | `"2:30:05 PM"` |
| `DateTimeShort(ts)` | `String(Int)` | Date + time. |
| `DateTimeMedium(ts)` | `String(Int)` | |
| `DateOnly(d, style)` | `String(DateOnly, String)` | Format a DateOnly by named style (`"short"/"medium"/"long"/"full"`). |
| `MonthName(m, abbr)` | `String(Int, Bool)` | `1..12`. |
| `DayName(dow, abbr)` | `String(Int, Bool)` | `0..6`, 0 = Sunday. |
| `AmPm(isPm)` | `String(Bool)` | |

### Supported CLDR pattern letters

| Letter | Meaning | Rep counts |
|---|---|---|
| `y` | Year | 2 emits two digits; 4+ pads to that width; other counts emit the full year |
| `M` | Month | 1 (numeric), 2 (zero-padded), 3 (abbreviated), 4 (wide), 5+ (first UTF-8 codepoint of the wide name) |
| `d` | Day of month | 1 (numeric), 2+ (zero-padded to two digits) |
| `E` | Weekday | 1-3 (abbreviated), 4 (wide), 5+ (first UTF-8 codepoint of the wide name) |
| `H` | Hour 0-23 | 1 (numeric), 2+ (zero-padded) |
| `h` | Hour 1-12 | 1 (numeric), 2+ (zero-padded) |
| `m` | Minute | 1 (numeric), 2+ (zero-padded) |
| `s` | Second | 1 (numeric), 2+ (zero-padded) |
| `a` | AM/PM | Any positive repeat count emits the same marker |
| `'...'` | Quoted literal | `''` inside = literal apostrophe |

### Notes

- Timestamp inputs are Unix seconds. Date/time components are interpreted in the process's local time zone, matching `Zanna.Time.DateTime` component accessors; the selected `Locale` changes formatting, not the time zone.
- Numeric pattern output uses the locale's `numbers.digits`; date styles and custom patterns can emit non-Latin digits.
- `DateTimeShort` and `DateTimeMedium` use locale `datetime_short` / `datetime_medium` composition patterns when provided, falling back to date + space + time.
- Only the letters listed above are supported. Any other ASCII letter traps with `"unsupported pattern letter"`.
- Unterminated quoted literals trap instead of silently treating the rest of the pattern as literal text.
- Pattern length is capped at 256 bytes.
- The 5+ `M`/`E` form is a narrow-name approximation; locale files do not carry separate CLDR
  narrow-name tables, and the emitter takes one UTF-8 codepoint rather than one grapheme cluster.
- `DateOnly` defaults to `"medium"` when its style is null; any non-null style outside `"short"`, `"medium"`, `"long"`, and `"full"` traps.

### Zia Example

```zia
module DateFormatDemo;

bind Zanna.Terminal;
bind Zanna.Time.DateTime as DateTime;
bind Zanna.Localization.DateFormat as DateFormat;
bind Zanna.Localization.Locale as Locale;

func start() {
    var fmt = DateFormat.ForLocale(Locale.Parse("en-US"));
    var ts = DateTime.FromParts(2027, 3, 15, 14, 30, 5);

    Say(fmt.Long(ts));                       // "March 15, 2027"
    Say(fmt.Custom(ts, "yyyy-MM-dd HH:mm")); // "2027-03-15 14:30"
    Say(fmt.Custom(ts, "EEEE, MMM d"));      // "Monday, Mar 15"
}
```

---

## Zanna.Localization.RelativeTimeFormat

Human-readable "N units ago" / "in N units" strings.

### Methods

| Method | Signature | Description |
|---|---|---|
| `New()` | `RelativeTimeFormat()` | Construct for the current locale with `Style = "long"`. |
| `ForLocale(loc)` | `RelativeTimeFormat(Locale)` | |
| `Format(duration)` | `String(Int)` | Duration in milliseconds. Positive = past. |
| `FormatFrom(then, now)` | `String(Int, Int)` | Format relative delta between two Unix-second timestamps. |
| `Short(duration)` | `String(Int)` | Short-style form. |
| `Long(duration)` | `String(Int)` | Long-style form (default). |
| `Numeric(value, unit)` | `String(Int, String)` | Explicit unit: `"second"/"minute"/"hour"/"day"/"week"/"month"/"year"`; positive is past, negative is future, and zero is `now`. |

### Properties

| Property | Type | Description |
|---|---|---|
| `Locale` | `Locale` | |
| `Style` | `String` | `"long"` / `"short"`. |

### Notes

- Unit thresholds use fixed approximations: `>= 365d` is a year, `>= 30d` a month, then `>= 7d`, `>= 1d`, `>= 1h`, `>= 1m`, else seconds. Counts truncate toward zero after choosing the unit.
- Durations whose absolute value is less than one second format with the locale's `relative_time.now` string.
- Plural form selected via the bound locale's `PluralRules` cardinal table.
- `Style` accepts only `"long"` and `"short"`; unknown values trap. `Short` and `Long` force their named style without changing the property, while `Format`, `FormatFrom`, and `Numeric` use the current property. Short style uses `short_past` / `short_future` and `short_units` when available.
- Relative-time numbers are localized with the locale digit set.
- `FormatFrom` traps if subtracting the timestamps or converting their seconds delta to
  milliseconds would overflow. `Format` and `Numeric` accept the full signed range;
  magnitudes are tracked unsigned, so `INT64_MIN` renders its exact absolute value.

---

## Zanna.Localization.ListFormat

Locale-correct list joining ("A, B, and C").

### Methods

| Method | Signature | Description |
|---|---|---|
| `New()` | `ListFormat()` | Construct for the current locale. |
| `ForLocale(loc)` | `ListFormat(Locale)` | |
| `And(items)` | `String(Object)` | Conjunction join; object must be a runtime `List` of raw strings. |
| `Or(items)` | `String(Object)` | Disjunction join; same carrier requirement. |
| `Unit(items)` | `String(Object)` | Plain unit join (no conjunction); same carrier requirement. |
| `Short(items)` | `String(Object)` | Exact `And` alias in v1; same carrier requirement. |

### Notes

- 0 items → `""`; 1 item → the item verbatim; 2 → pair template; 3+ → start/middle/end recursive combine per CLDR.
- The runtime expects a `Zanna.Collections.List` whose elements are raw runtime strings. A typed Zia `List[String]` currently stores boxed elements and is not compatible with this API. A string `Split()` result contains raw strings and can be converted with `ToList()`, as below.
- Joins are reference-balanced: every element reference retained during formatting is released
  before the call returns.

### Zia Example

```zia
module ListFormatDemo;

bind Zanna.Terminal;
bind Zanna.Localization.ListFormat as ListFormat;
bind Zanna.Localization.Locale as Locale;

func start() {
    var fruit = "apples|bananas|cherries".Split("|").ToList();

    var fmt = ListFormat.ForLocale(Locale.Parse("en-US"));
    Say(fmt.And(fruit)); // "apples, bananas, and cherries"

    var colors = "red|blue".Split("|").ToList();
    Say(fmt.Or(colors)); // "red or blue"
}
```

### See Also

- [Messages](messages.md) — `MessageBundle.Plural` and `PluralRules` for locale-aware plural selection.
- [Collation and text direction](collation.md) — public `TextDirection` utilities and internal collation notes.
- [Data files](data-files.md) — JSON schema for loaded locales' format templates.
