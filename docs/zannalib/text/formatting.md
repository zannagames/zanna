---
status: active
audience: public
last-verified: 2026-07-26
---

# Formatting & Generation
> Template, StringBuilder, TextWrapper, InvariantNumberFormat, Pluralize, Version, Html, Markdown

**Part of [Zanna Runtime Library](../README.md) › [Text Processing](README.md)**

---

## Zanna.Text.Template

Simple string templating with placeholder substitution.

**Type:** Static utility class

### Methods

| Method                                    | Signature                           | Description                                       |
|-------------------------------------------|-------------------------------------|---------------------------------------------------|
| `Render(template, values)`                | `String(String, Map)`               | Replace `{{key}}` placeholders with Map values    |
| `RenderSeq(template, values)`             | `String(String, Seq)`               | Replace `{{0}}`, `{{1}}` with Seq values          |
| `RenderWith(template, values, pre, suf)`  | `String(String, Map, String, String)` | Use custom delimiters instead of `{{` `}}`      |
| `Has(template, key)`                      | `Boolean(String, String)`           | Check if template contains placeholder for key   |
| `Keys(template)`                          | Registry: `Seq<String>(String)`; runtime: `StringSet` | Extract all unique placeholder keys      |
| `Escape(text)`                            | `String(String)`                    | Escape `{{` and `}}` for literal output           |

### Placeholder Syntax

- Default delimiters: `{{` and `}}`
- Whitespace inside placeholders is trimmed: `{{ name }}` = `{{name}}`
- Missing keys are left as-is: `{{unknown}}` outputs `{{unknown}}`
- Empty keys are left as-is: `{{}}` outputs `{{}}`
- Unclosed placeholders are left as-is: `{{name` outputs `{{name`
- Doubling delimiters escapes them: `{{{{name}}}}` renders as literal `{{name}}`; `Has()` and `Keys()` ignore escaped placeholders

### Notes

- Map and Seq substitutions accept runtime String values directly or strings wrapped with
  `Zanna.Core.Box.Str`
- Other value types in the Map/Seq are ignored (the placeholder is left as-is)
- Placeholder indices that overflow integer range are ignored (placeholder left as-is)
- Template rendering and escaping use runtime string byte length, so embedded `NUL` bytes in values are preserved
- `Escape()` always returns a new string result, even when the input contains no template delimiters
- `Keys()` returns an owned `Zanna.Collections.StringSet` (and is registered as such), so key
  order is unspecified, duplicate placeholders produce one key, and natural member access such as
  `.Count`/`.Has(...)` resolves against StringSet.
- Thread safe: all functions are stateless

### Traps

- `Render`: Traps if template or values is null
- `RenderSeq`: Traps if template or values is null
- `RenderWith`: Traps if template, values, prefix, or suffix is null; traps if prefix or suffix is empty

### Zia Example

```zia
module TemplateDemo;

bind Zanna.Terminal;
bind Zanna.Data.Json as Json;

func start() {
    var data = Json.Parse("{\"name\":\"Zia\",\"version\":\"1.0\"}");
    var result = Zanna.Text.Template.Render("Hello {{name}} v{{version}}!", data);
    Say(result);  // Hello Zia v1.0!
}
```

### BASIC Example

```basic
' Use Json.Parse to create a map for template variables
DIM data AS OBJECT = Zanna.Data.Json.Parse("{""name"":""Zia"",""version"":""1.0""}")
DIM result AS STRING = Zanna.Text.Template.Render("Hello {{name}} v{{version}}!", data)
PRINT result  ' Output: Hello Zia v1.0!
```

### Additional BASIC Example

```basic
' Create a Map with values
DIM values AS Zanna.Collections.Map = Zanna.Collections.Map.New()
values.Set("name", Zanna.Core.Box.Str("Alice"))
values.Set("count", Zanna.Core.Box.Str("5"))

' Render template
DIM result AS STRING = Zanna.Text.Template.Render("Hello {{name}}, you have {{count}} messages.", values)
PRINT result  ' Output: "Hello Alice, you have 5 messages."

' Whitespace in placeholders is ignored
result = Zanna.Text.Template.Render("Hello {{ name }}!", values)
PRINT result  ' Output: "Hello Alice!"

' Missing keys are left as-is
result = Zanna.Text.Template.Render("Hello {{unknown}}!", values)
PRINT result  ' Output: "Hello {{unknown}}!"
```

### Positional Substitution Example

```basic
' Create a Seq with positional values
DIM values AS Zanna.Collections.Seq = Zanna.Collections.Seq.New()
values.Push(Zanna.Core.Box.Str("Alice"))
values.Push(Zanna.Core.Box.Str("Bob"))
values.Push(Zanna.Core.Box.Str("Charlie"))

' Use numeric indices
DIM result AS STRING = Zanna.Text.Template.RenderSeq("{{0}} and {{1}} meet {{2}}", values)
PRINT result  ' Output: "Alice and Bob meet Charlie"

' Out of range indices are left as-is
result = Zanna.Text.Template.RenderSeq("{{0}} and {{99}}", values)
PRINT result  ' Output: "Alice and {{99}}"
```

### Custom Delimiters Example

```basic
DIM values AS Zanna.Collections.Map = Zanna.Collections.Map.New()
values.Set("name", Zanna.Core.Box.Str("Alice"))
values.Set("count", Zanna.Core.Box.Str("5"))

' Use dollar signs as delimiters
DIM result AS STRING = Zanna.Text.Template.RenderWith("Hello $name$!", values, "$", "$")
PRINT result  ' Output: "Hello Alice!"

' Use ERB-style delimiters
result = Zanna.Text.Template.RenderWith("<%= name %> has <%= count %> items", values, "<%=", "%>")
PRINT result  ' Output: "Alice has 5 items"

' Use percent delimiters
result = Zanna.Text.Template.RenderWith("Hello %name%!", values, "%", "%")
PRINT result  ' Output: "Hello Alice!"
```

### Query Placeholder Keys Example

```basic
DIM template AS STRING = "Hello {{name}}, you have {{count}} messages from {{sender}}."

' Check if template has a specific placeholder
IF Zanna.Text.Template.Has(template, "name") THEN
    PRINT "Template uses 'name' placeholder"
END IF

' Get all placeholder keys as a StringSet of unique strings
DIM keys AS Zanna.Collections.StringSet = Zanna.Text.Template.Keys(template)
PRINT keys.Count  ' Output: 3

' Iterate over keys (order not guaranteed)
' Contains: "name", "count", "sender"
```

### Escape Example

```basic
' Escape braces to output them literally
DIM text AS STRING = "Use {{name}} for placeholders"
DIM escaped AS STRING = Zanna.Text.Template.Escape(text)
PRINT escaped  ' Output: "Use {{{{name}}}} for placeholders"

' The escaped text, when rendered, produces the original braces
DIM values AS Zanna.Collections.Map = Zanna.Collections.Map.New()
DIM result AS STRING = Zanna.Text.Template.Render(escaped, values)
PRINT result  ' Output: "Use {{name}} for placeholders"
```

### Use Cases

- Generating emails with dynamic content
- Building SQL queries (use with caution - prefer parameterized queries)
- Templating configuration files
- Generating reports with placeholder substitution
- Simple string interpolation without full template engines

---

## Zanna.Text.StringBuilder

Mutable string builder for efficient string concatenation. Use when building strings incrementally to avoid creating
many intermediate string objects.

**Type:** Instance (opaque*)
**Constructor:** `NEW Zanna.Text.StringBuilder()`

### Properties

| Property   | Type    | Description                              |
|------------|---------|------------------------------------------|
| `Length`   | Integer | Current byte length of the accumulated string |
| `Capacity` | Integer | Allocated buffer capacity in bytes, including room for the terminating `NUL` |

### Methods

| Method             | Signature               | Description                                           |
|--------------------|-------------------------|-------------------------------------------------------|
| `Append(text)`     | `StringBuilder(String)` | Appends text and returns self for chaining            |
| `AppendLine(text)` | `StringBuilder(String)` | Appends text and then `\n`; returns self for chaining |
| `ToString()`       | `String()`              | Returns the accumulated string                        |
| `Clear()`          | `Void()`                | Clears the buffer                                     |

### Zia Example

```zia
module StringBuilderDemo;

bind Zanna.Terminal;
bind Zanna.Text.StringBuilder as SB;

func start() {
    var sb = SB.New();
    SB.Append(sb, "Hello");
    SB.Append(sb, ", ");
    SB.Append(sb, "World!");
    Say("Result: " + SB.ToString(sb));  // Hello, World!
}
```

### BASIC Example

```basic
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()

' Method chaining
sb.Append("Hello, ").Append("World!").Append(" How are you?")

PRINT sb.ToString()  ' Output: "Hello, World! How are you?"
PRINT sb.Length      ' Output: 28

' Append lines
sb.Clear()
sb.AppendLine("a").AppendLine("b")
PRINT sb.ToString()  ' Output: "a\nb\n"

sb.Clear()
PRINT sb.Length      ' Output: 0
```

### Performance Note

Use `StringBuilder` instead of repeated string concatenation in loops:

```basic
' Efficient: O(n) total
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
FOR i = 1 TO 1000
    sb.Append("item ")
NEXT i
result = sb.ToString()

' Inefficient: O(n^2) due to intermediate strings
DIM result AS STRING
result = ""
FOR i = 1 TO 1000
    result = result + "item "  ' Creates new string each iteration
NEXT i
```

### Notes

- `Append(NULL)` treats the input as empty; `AppendLine(NULL)` appends just `\n`.
- `Length` is a byte count, not a Unicode codepoint count.
- Embedded `NUL` bytes are preserved in appended text and in `ToString()`.
- `Clear()` resets `Length` to zero but retains the allocated buffer and its `Capacity` for reuse.
- Calling instance properties or methods with a null `StringBuilder` receiver traps with `InvalidOperation`.

---

## Zanna.Text.TextWrapper

Text wrapping, alignment, indentation, and truncation utilities for formatting text to specified widths.

**Type:** Static utility class

### Methods

| Method                            | Signature                        | Description                                              |
|-----------------------------------|----------------------------------|----------------------------------------------------------|
| `Wrap(text, width)`               | `String(String, Integer)`        | Wrap text to a byte width, inserting newlines when needed |
| `WrapLines(text, width)`          | `Seq(String, Integer)`           | Wrap text and return a Seq of line strings                 |
| `Fill(text, width)`               | `String(String, Integer)`        | Exact alias of `Wrap`                                      |
| `Indent(text, prefix)`            | `String(String, String)`         | Add prefix to the start of each line                     |
| `Dedent(text)`                    | `String(String)`                 | Remove common leading whitespace from all lines          |
| `Hang(text, prefix)`              | `String(String, String)`         | Indent all lines except the first (hanging indent)       |
| `Truncate(text, width)`           | `String(String, Integer)`        | Truncate to width with "..." suffix if needed            |
| `TruncateWith(text, width, suffix)` | `String(String, Integer, String)` | Truncate with custom suffix                           |
| `Shorten(text, width)`            | `String(String, Integer)`        | Shorten by replacing middle portion with "..."           |
| `AlignLeft(text, width)`          | `String(String, Integer)`        | Left-align text, padding with spaces to width            |
| `AlignRight(text, width)`         | `String(String, Integer)`        | Right-align text, padding with spaces to width           |
| `Center(text, width)`             | `String(String, Integer)`        | Center text, padding with spaces to width                |
| `LineCount(text)`                 | `Integer(String)`                | Count the number of lines in text                        |
| `MaxLineLength(text)`                | `Integer(String)`                | Get the byte length of the longest line                  |

### Notes

- Widths and line lengths are byte counts, not Unicode codepoint counts or terminal display widths.
  Wrapping, truncation, and shortening can split a multi-byte UTF-8 sequence and return malformed
  text; see [VDOC-046](../../../misc/reviews/documentation-review-findings.md#vdoc-046--textwrapper-can-split-utf-8-sequences).
- `Wrap` prefers the last ASCII space or tab before the limit, hard-splits longer words, preserves
  existing `LF` line breaks and other whitespace, and disables wrapping when `width <= 0`.
- `Fill` calls `Wrap` directly. It does not normalize whitespace or existing line breaks.
- `Truncate` appends "..." only when the text exceeds the specified width; the total byte length
  including "..." equals width.
- `TruncateWith` clips a suffix that is longer than the requested width so the returned string never exceeds width
- `Dedent` removes a common leading whitespace byte prefix; tabs and spaces are not treated as interchangeable
- `LineCount` does not count a final trailing newline as an extra empty line
- `Shorten` keeps the beginning and end of the text, replacing the middle with "..."; truncation
  and shortening return an empty string for nonempty text when `width <= 0`.
- `Wrap`, `Fill`, `WrapLines`, indentation, truncation, shortening, alignment, and line-metric helpers treat null text as empty text
- A null indent or hanging prefix is treated as an empty prefix; a null truncation suffix is treated as an empty suffix
- `Indent("", prefix)` returns an empty string instead of a standalone prefix. A trailing `LF`
  creates a trailing prefixed empty line; `Dedent` preserves blank lines while removing common
  indentation from non-blank lines.
- `WrapLines` returns an owned sequence of owned line strings. Empty or null input produces one
  empty line, not an empty sequence.

### Zia Example

```zia
module TextWrapDemo;

bind Zanna.Terminal;
bind Zanna.Text.TextWrapper as TW;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var text = "The quick brown fox jumps over the lazy dog near the riverbank";

    // Wrap text at 20 bytes
    Say(TW.Wrap(text, 20));

    // Center text
    Say("[" + TW.Center("hello", 20) + "]");

    // Truncate with ellipsis
    Say(TW.Truncate("This is a long sentence that needs truncating", 20));

    // Indent text
    var lines = "line one\nline two\nline three";
    Say(TW.Indent(lines, "  > "));

    // Line metrics
    Say("Lines: " + Fmt.Int(TW.LineCount(lines)));
    Say("MaxLen: " + Fmt.Int(TW.MaxLineLength(lines)));
}
```

### BASIC Example

```basic
' Wrap text at a specified width
DIM text AS STRING = "The quick brown fox jumps over the lazy dog near the riverbank"
DIM wrapped AS STRING = Zanna.Text.TextWrapper.Wrap(text, 20)
PRINT wrapped
' Output (lines broken at word boundaries):
' The quick brown fox
' jumps over the lazy
' dog near the
' riverbank

' Get wrapped lines as a Seq
DIM lines AS Zanna.Collections.Seq = Zanna.Text.TextWrapper.WrapLines(text, 20)
PRINT lines.Count  ' Output: 4

' Text alignment
PRINT "["; Zanna.Text.TextWrapper.AlignLeft("hello", 20); "]"   ' Output: [hello               ]
PRINT "["; Zanna.Text.TextWrapper.AlignRight("hello", 20); "]"  ' Output: [               hello]
PRINT "["; Zanna.Text.TextWrapper.Center("hello", 20); "]"  ' Output: [       hello        ]

' Truncation
DIM long AS STRING = "This is a very long sentence"
PRINT Zanna.Text.TextWrapper.Truncate(long, 15)              ' 15 bytes; ends in three periods
PRINT Zanna.Text.TextWrapper.TruncateWith(long, 15, "~")     ' Output: "This is a very~"
PRINT Zanna.Text.TextWrapper.Shorten(long, 20)               ' 20 bytes; middle replaced by three periods

' Indentation
DIM block AS STRING = "line one" + CHR$(10) + "line two" + CHR$(10) + "line three"
PRINT Zanna.Text.TextWrapper.Indent(block, "  ")
' Output:
'   line one
'   line two
'   line three

' Dedent (remove common whitespace)
DIM indented AS STRING = "    hello" + CHR$(10) + "    world"
PRINT Zanna.Text.TextWrapper.Dedent(indented)
' Output:
' hello
' world

' Hanging indent
PRINT Zanna.Text.TextWrapper.Hang(block, "    ")
' Output:
' line one
'     line two
'     line three

' Line metrics
PRINT Zanna.Text.TextWrapper.LineCount(block)    ' Output: 3
PRINT Zanna.Text.TextWrapper.MaxLineLength(block)   ' Output: 10
```

### Use Cases

- **ASCII console output:** Format text to a byte-oriented width
- **Reports:** Align columns and wrap long descriptions
- **Help text:** Format command-line help messages with indentation
- **Notifications:** Truncate long messages for display in limited space

---

## Zanna.Text.InvariantNumberFormat

Number formatting utilities for human-readable display of integers and floating-point values.

**Type:** Static utility class

### Methods

| Method                   | Signature              | Description                                         |
|--------------------------|------------------------|-----------------------------------------------------|
| `Bytes(n)`               | `String(Integer)`      | Format byte count as a binary-scaled size (B through EB) |
| `Currency(amount, sym)`  | `String(Double, String)` | Format as currency with symbol and two decimal places |
| `Decimals(n, places)`    | `String(Double, Integer)` | Format number with specified decimal places        |
| `Ordinal(n)`             | `String(Integer)`      | Format integer as ordinal (1st, 2nd, 3rd, 4th...)  |
| `Pad(n, width)`          | `String(Integer, Integer)` | Zero-pad integer to minimum width               |
| `Percent(n)`             | `String(Double)`       | Format fraction as percentage (0.756 -> "75.6%")   |
| `Thousands(n, sep)`      | `String(Integer, String)` | Format integer with thousands separator          |
| `ToWords(n)`             | `String(Integer)`      | Convert any signed 64-bit integer to English words          |

### Notes

- `Bytes` scales by 1024 through B, KB, MB, GB, TB, PB, and EB. Scaled magnitudes below 10 use two decimal places; larger scaled magnitudes use one.
- `Currency` adds thousands separators and always shows two decimal places (e.g., `$1,234.56`)
- A null currency symbol defaults to `$`; an empty symbol remains empty. The full runtime symbol,
  including embedded `NUL` bytes, is preserved.
- `Decimals`, `Currency`, and `Percent` use canonical `NaN`, `Infinity`, and `-Infinity` spellings for non-finite values; `Percent` appends `%`
- `Decimals` clamps the requested precision to 0 through 20 places; `Percent` rounds to at most one decimal place and drops a trailing `.0`
- `Ordinal` handles special cases: 11th, 12th, 13th (not 11st, 12nd, 13th)
- `Bytes` and `Pad` handle the full signed 64-bit integer range, including `INT64_MIN`; `Pad`
  clamps its requested minimum width to 1 through 64 bytes.
- `Thousands` preserves the full runtime string for the separator, including embedded `NUL` bytes; null or empty separators default to `","`
- `ToWords` handles the full signed 64-bit range and produces hyphenated compound numbers (e.g., "forty-two", "one hundred twenty-three")
- Despite the class name, finite floating-point output uses the process C numeric locale. The VM
  selects the `C` locale, but an embedding process can produce decimal-comma output; see
  [VDOC-041](../../../misc/reviews/documentation-review-findings.md#vdoc-041--toml-and-yaml-numeric-emission-is-locale-sensitive).
- Formatting helpers trap on internal builder allocation or overflow failure instead of returning partial output

### Zia Example

```zia
module NumFmtDemo;

bind Zanna.Terminal;
bind Zanna.Text.InvariantNumberFormat as NF;

func start() {
    Say(NF.Bytes(1048576));            // 1.00 MB
    Say(NF.Currency(29.99, "$"));      // $29.99
    Say(NF.Decimals(3.14159, 2));      // 3.14
    Say(NF.Ordinal(3));                // 3rd
    Say(NF.Pad(7, 3));                 // 007
    Say(NF.Percent(0.756));            // 75.6%
    Say(NF.Thousands(1234567, ","));   // 1,234,567
    Say(NF.ToWords(42));               // forty-two
}
```

### BASIC Example

```basic
' Format bytes as human-readable sizes
PRINT Zanna.Text.InvariantNumberFormat.Bytes(1048576)     ' Output: "1.00 MB"
PRINT Zanna.Text.InvariantNumberFormat.Bytes(1073741824)  ' Output: "1.00 GB"

' Currency formatting
PRINT Zanna.Text.InvariantNumberFormat.Currency(29.99, "$")   ' Output: "$29.99"
PRINT Zanna.Text.InvariantNumberFormat.Currency(1234.5, "€")  ' Output: "€1,234.50"

' Control decimal places
PRINT Zanna.Text.InvariantNumberFormat.Decimals(3.14159, 2)  ' Output: "3.14"
PRINT Zanna.Text.InvariantNumberFormat.Decimals(3.14159, 4)  ' Output: "3.1416"

' Ordinal numbers
PRINT Zanna.Text.InvariantNumberFormat.Ordinal(1)   ' Output: "1st"
PRINT Zanna.Text.InvariantNumberFormat.Ordinal(2)   ' Output: "2nd"
PRINT Zanna.Text.InvariantNumberFormat.Ordinal(3)   ' Output: "3rd"
PRINT Zanna.Text.InvariantNumberFormat.Ordinal(11)  ' Output: "11th"

' Zero-padding
PRINT Zanna.Text.InvariantNumberFormat.Pad(7, 3)    ' Output: "007"
PRINT Zanna.Text.InvariantNumberFormat.Pad(42, 5)   ' Output: "00042"

' Percentage
PRINT Zanna.Text.InvariantNumberFormat.Percent(0.756)  ' Output: "75.6%"

' Thousands separator
PRINT Zanna.Text.InvariantNumberFormat.Thousands(1234567, ",")  ' Output: "1,234,567"
PRINT Zanna.Text.InvariantNumberFormat.Thousands(1234567, ".")  ' Output: "1.234.567"

' Number to English words
PRINT Zanna.Text.InvariantNumberFormat.ToWords(42)   ' Output: "forty-two"
PRINT Zanna.Text.InvariantNumberFormat.ToWords(100)  ' Output: "one hundred"
```

### Use Cases

- **Reports:** Format numbers for display in tables and summaries
- **File sizes:** Show download sizes in human-readable units
- **Invoices:** Format currency amounts with proper symbols
- **Stable application output:** Use fixed formatting when the process numeric locale is `C`

---

## Zanna.Text.Pluralize

English noun pluralization and singularization. Handles common English rules, irregular forms, and uncountable nouns.

**Type:** Static utility class

### Methods

| Method              | Signature                | Description                                          |
|---------------------|--------------------------|------------------------------------------------------|
| `Plural(word)`      | `String(String)`         | Convert a singular noun to its plural form           |
| `Singular(word)`    | `String(String)`         | Convert a plural noun to its singular form           |
| `Count(n, word)`    | `String(Integer, String)` | Format a count with the correctly pluralized noun   |

### Notes

- Handles regular English pluralization rules (adding -s, -es, -ies, etc.)
- Knows common irregular forms: child/children, person/people, mouse/mice, ox/oxen, etc.
- `Count` uses the singular form for `1` and `-1`; every other count uses the plural form
- Irregular forms preserve input casing for title-case and all-uppercase words, e.g. `Child` -> `Children`, `CHILD` -> `CHILDREN`
- Regular suffix rules are case-insensitive for matching and preserve all-uppercase input where practical, e.g. `BOX` -> `BOXES`, `CITY` -> `CITIES`
- Runtime string byte length is used, so embedded `NUL` bytes are preserved in regular inflection and `Count`
- This is a compact rule table, not a full NLP engine; ambiguous and uncommon English inflections may be wrong

### Zia Example

```zia
module PluralDemo;

bind Zanna.Terminal;
bind Zanna.Text.Pluralize as P;

func start() {
    Say(P.Plural("box"));        // boxes
    Say(P.Plural("child"));      // children
    Say(P.Plural("cat"));        // cats
    Say(P.Singular("boxes"));    // box
    Say(P.Singular("children")); // child
    Say(P.Count(1, "item"));     // 1 item
    Say(P.Count(5, "item"));     // 5 items
    Say(P.Count(0, "item"));     // 0 items
}
```

### BASIC Example

```basic
' Pluralize nouns
PRINT Zanna.Text.Pluralize.Plural("box")    ' Output: "boxes"
PRINT Zanna.Text.Pluralize.Plural("child")  ' Output: "children"
PRINT Zanna.Text.Pluralize.Plural("cat")    ' Output: "cats"
PRINT Zanna.Text.Pluralize.Plural("city")   ' Output: "cities"

' Singularize nouns
PRINT Zanna.Text.Pluralize.Singular("boxes")    ' Output: "box"
PRINT Zanna.Text.Pluralize.Singular("children") ' Output: "child"
PRINT Zanna.Text.Pluralize.Singular("cats")     ' Output: "cat"

' Count with automatic pluralization
PRINT Zanna.Text.Pluralize.Count(0, "item")  ' Output: "0 items"
PRINT Zanna.Text.Pluralize.Count(1, "item")  ' Output: "1 item"
PRINT Zanna.Text.Pluralize.Count(5, "item")  ' Output: "5 items"
PRINT Zanna.Text.Pluralize.Count(1, "child") ' Output: "1 child"
PRINT Zanna.Text.Pluralize.Count(3, "child") ' Output: "3 children"
```

### Use Cases

- **User-facing messages:** "You have 3 new messages" vs "You have 1 new message"
- **Report generation:** Dynamically format counts with correct noun forms
- **Template rendering:** Combine with Template class for grammatically correct output

---

## Zanna.Text.Version

Semantic version parsing, comparison, and manipulation. The core syntax follows
[Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html), with an additional optional
leading `v` or `V` accepted for compatibility.

**Type:** Instance class
**Constructor:** `Parse(versionString)` -- parses a version string and returns a Version object (`NULL` if invalid)

### Properties

| Property     | Type    | Access    | Description                                    |
|--------------|---------|-----------|------------------------------------------------|
| `Major`      | Integer | Read-only | Major version number                           |
| `Minor`      | Integer | Read-only | Minor version number                           |
| `Patch`      | Integer | Read-only | Patch version number                           |
| `Prerelease` | String  | Read-only | Pre-release identifier (empty if none)         |
| `Build`      | String  | Read-only | Build metadata string (empty if none)          |

### Methods

| Method            | Signature            | Description                                                 |
|-------------------|----------------------|-------------------------------------------------------------|
| `BumpMajor()`     | `String()`           | Bump major version, reset minor and patch to 0              |
| `BumpMinor()`     | `String()`           | Bump minor version, reset patch to 0                        |
| `BumpPatch()`     | `String()`           | Bump patch version                                          |
| `CompareTo(other)`      | `Integer(Version)`   | Compare to another version: -1 (less), 0 (equal), 1 (greater) |
| `Compare(a, b)`   | `Integer(String, String)` | Parse and compare two version strings: -1, 0, or 1 (static) |
| `IsValid(str)`    | `Boolean(String)`    | Check if a string is a valid semantic version (static)      |
| `ParseMajor(str)` | `Integer(String)`    | Parse and return the major component (static)               |
| `ParseMinor(str)` | `Integer(String)`    | Parse and return the minor component (static)               |
| `ParsePatch(str)` | `Integer(String)`    | Parse and return the patch component (static)               |
| `Satisfies(constraint)` | `Boolean(String)` | Check if version satisfies a constraint (e.g., ">=1.0.0", "^1.2.3", "~1.2.3") |
| `ToString()`      | `String()`           | Format version object back to string                        |

### Notes

- Follows SemVer 2.0.0: `MAJOR.MINOR.PATCH[-prerelease][+buildmetadata]`
- `Parse` requires all three numeric components and returns NULL for invalid SemVer strings
- Numeric components are limited to the signed 64-bit range `0` through `9223372036854775807`
  (the class exposes them as integer properties). SemVer itself sets no component-size limit, so
  larger otherwise-valid version strings are rejected by design.
- Pre-release and build identifiers must be non-empty dot-separated ASCII alphanumeric/hyphen identifiers; numeric pre-release identifiers cannot have leading zeroes
- `Cmp` ignores build metadata per the SemVer specification; pre-release versions have lower precedence than the associated normal version
- `Parse` and `IsValid` also accept one optional leading `v` or `V`; this is a Zanna extension to
  strict SemVer 2.0.0, and `ToString()` omits that prefix.
- `Satisfies` supports constraint operators: `>=`, `<=`, `>`, `<`, `=`, `!=`, `^` (compatible), and `~` (same major/minor)
- `Satisfies` trims leading and trailing whitespace around the constraint and version operand
- Constraint operands must contain all three numeric components. An empty or whitespace-only constraint matches every valid receiver.
- Embedded `NUL` bytes in constraints are not treated as terminators; constraints containing them are parsed by full byte length and generally fail as invalid
- `Compare` orders an invalid parse below a valid one (and considers two invalid parses equal); `ParseMajor`, `ParseMinor`, and `ParsePatch` return `0` when parsing fails, so use `IsValid` when zero is ambiguous
- `BumpMajor`, `BumpMinor`, and `BumpPatch` return new numeric version strings, drop
  prerelease/build metadata, and do not modify the original object. They trap if the component to
  increment is already `9223372036854775807`.

### Zia Example

```zia
module VersionDemo;

bind Zanna.Terminal;
bind Zanna.Text.Version as Version;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var v = Version.Parse("1.2.3-beta+build42");

    Say("Major: " + Fmt.Int(v.Major));          // 1
    Say("Minor: " + Fmt.Int(v.Minor));          // 2
    Say("Patch: " + Fmt.Int(v.Patch));          // 3
    Say("Pre: " + v.Prerelease);                // beta
    Say("Build: " + v.Build);                   // build42
    Say("String: " + v.ToString());             // 1.2.3-beta+build42

    Say("BumpMinor: " + v.BumpMinor());         // 1.3.0
    Say("BumpMajor: " + v.BumpMajor());         // 2.0.0

    Say("Valid: " + Fmt.Bool(Version.IsValid("1.0.0")));    // true
    Say("Invalid: " + Fmt.Bool(Version.IsValid("not.a.version"))); // false

    var v2 = Version.Parse("2.0.0");
    Say("Cmp: " + Fmt.Int(v.CompareTo(v2)));         // -1
    Say("Satisfies: " + Fmt.Bool(v.Satisfies(">=1.0.0"))); // true
}
```

### BASIC Example

```basic
' Parse a semantic version string
DIM v AS Zanna.Text.Version = Zanna.Text.Version.Parse("1.2.3-beta+build42")

' Access version components
PRINT v.Major       ' Output: 1
PRINT v.Minor       ' Output: 2
PRINT v.Patch       ' Output: 3
DIM prerelease AS STRING = v.Prerelease
DIM buildMetadata AS STRING = v.Build
DIM versionText AS STRING = v.ToString()
PRINT prerelease    ' Output: "beta"
PRINT buildMetadata ' Output: "build42"
PRINT versionText   ' Output: "1.2.3-beta+build42"

' Bump versions
PRINT v.BumpMajor()  ' Output: "2.0.0"
PRINT v.BumpMinor()  ' Output: "1.3.0"
PRINT v.BumpPatch()  ' Output: "1.2.4"

' Compare versions
DIM v2 AS Zanna.Text.Version = Zanna.Text.Version.Parse("2.0.0")
DIM cmp AS INTEGER = v.CompareTo(v2)
PRINT cmp  ' Output: -1 (v < v2)

' Validate version strings
IF Zanna.Text.Version.IsValid("1.0.0") THEN
    PRINT "Valid version"
END IF

IF NOT Zanna.Text.Version.IsValid("not.a.version") THEN
    PRINT "Invalid version"
END IF

' Check version constraints
IF v.Satisfies(">=1.0.0") THEN
    PRINT "Meets minimum version"   ' Output: Meets minimum version
END IF

IF v.Satisfies("^1.2.0") THEN
    PRINT "Compatible with 1.2.x"   ' Output: Compatible with 1.2.x
END IF
```

### Use Cases

- **Dependency management:** Check if a library version meets requirements
- **Release automation:** Bump version numbers programmatically
- **Compatibility checks:** Verify version constraints before loading plugins
- **Version display:** Parse and format version strings for user-facing output

---

## Zanna.Text.Html

Tolerant HTML parser and utility functions for escaping, unescaping, tag stripping, link extraction, and text extraction.

**Type:** Static utility class

### Methods

| Method                       | Signature              | Description                                              |
|------------------------------|------------------------|----------------------------------------------------------|
| `Parse(html)`                | `Object(String)`       | Return a Map-backed parse tree                           |
| `ToText(html)`               | `String(String)`       | Strip tags and unescape entities to get plain text       |
| `Escape(text)`               | `String(String)`       | Escape HTML special characters (`<`, `>`, `&`, `"`, `'`) |
| `Unescape(text)`             | `String(String)`       | Unescape HTML entities (`&lt;`, `&gt;`, `&amp;`, etc.)   |
| `StripTags(html)`            | `String(String)`       | Remove all HTML tags (entities left as-is)               |
| `ExtractLinks(html)`         | `Object(String)`       | Return a Seq containing all `href` values from `<a>` tags |
| `ExtractText(html, tagName)` | `Object(String, String)` | Return a Seq containing text from matching tags        |

### Parse Tree Structure

`Parse()` returns a synthetic root Map whose `tag` and `text` are empty. Each Map node has the
following keys:

| Key        | Type           | Description                            |
|------------|----------------|----------------------------------------|
| `tag`      | String         | Tag name (e.g., `"div"`, `"p"`)        |
| `text`     | String         | Payload for a text node; empty on element nodes |
| `attrs`    | Map            | Attribute name-value pairs             |
| `children` | Seq of Maps    | Child element and text nodes            |

Text is represented by child nodes with an empty `tag`, an empty `attrs` Map and their source text
in `text`; it is not accumulated into the containing element's `text` field.

### Notes

- **Tolerant subset parser:** Unclosed tags, missing quotes, and other common malformed inputs do
  not trap, but this is not an HTML5 parser. It has no raw-text element model or browser-style tree
  correction, and a `>` inside a quoted attribute ends the tag.
- Closing tags are matched by tag name; unmatched closing tags are ignored instead of blindly popping the parse stack.
- Tag and attribute names retain their input spelling. Closing-tag and extraction matching is
  ASCII case-insensitive.
- **Entity support:** Handles the named entities `&lt;`, `&gt;`, `&amp;`, `&quot;`, `&apos;`, and
  `&nbsp;` (as an ordinary ASCII space). Decimal and hexadecimal numeric references decode only
  code points 1 through 127; other and unknown entities pass through unchanged. Parse-tree text and
  attributes are not entity-decoded.
- **StripTags vs ToText:** `StripTags` removes tags but leaves entities as-is. `ToText` removes tags AND unescapes entities.
- `StripTags` inserts separators for block-like tags such as paragraphs and line breaks, while inline tags such as `span`, `b`, and `i` do not invent spaces inside a word.
- Escaping, unescaping, stripping, and extraction use runtime string byte length, so embedded `NUL` bytes are preserved.
- `ExtractLinks` recognizes `href` attributes with whitespace around `=`, quoted or unquoted values, absolute paths such as `/about`, and self-closing tags; it ignores non-`href` names such as `data-href`.
- `Parse` is registered as returning a `Zanna.Collections.Map`, and `ExtractLinks`/`ExtractText`
  as string sequences, so chained member access resolves against the returned collections.
- `ExtractLinks` returns raw attribute text without entity decoding. `ExtractLinks` and
  `ExtractText` return owned sequences of owned strings.
- `ExtractText` tracks nesting depth for same-name elements: each top-level match yields one
  string containing the element's complete stripped text (nested matches fold into their parent).
- String-returning helpers return an empty string for `NULL`; extraction helpers return an empty `Seq`; `Parse(NULL)` returns an empty root Map node.

### Zia Example

```zia
module HtmlDemo;

bind Zanna.Terminal;
bind Zanna.Text.Html as Html;

func start() {
    // Escape HTML special characters
    var escaped = Html.Escape("<script>alert('xss')</script>");
    Say("Escaped: " + escaped);

    // Unescape HTML entities
    var unescaped = Html.Unescape("&lt;div&gt;Hello&lt;/div&gt;");
    Say("Unescaped: " + unescaped);

    // Strip HTML tags
    var stripped = Html.StripTags("<p>Hello <b>World</b></p>");
    Say("Stripped: " + stripped);

    // Convert HTML to plain text
    var plain = Html.ToText("<p>Price: &lt;$10&gt;</p>");
    Say("Plain: " + plain);
}
```

### BASIC Example

```basic
' Escape user input for safe HTML display
DIM userInput AS STRING = "<script>alert('xss')</script>"
DIM safe AS STRING = Zanna.Text.Html.Escape(userInput)
PRINT safe  ' Output: "&lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;"

' Unescape HTML entities
DIM text AS STRING = Zanna.Text.Html.Unescape("&lt;div&gt;Hello&lt;/div&gt;")
PRINT text  ' Output: "<div>Hello</div>"

' Strip tags to get raw text
DIM html AS STRING = "<p>Hello <b>World</b></p>"
DIM stripped AS STRING = Zanna.Text.Html.StripTags(html)
PRINT stripped  ' Output: "Hello World"

' Convert HTML to plain text (strips tags + unescapes)
DIM plain AS STRING = Zanna.Text.Html.ToText("<p>Price: &lt;$10&gt;</p>")
PRINT plain  ' Output: "Price: <$10>"

' Extract all links from HTML
DIM page AS STRING = "<a href=""https://example.com"">Link 1</a><a href=""/about"">About</a>"
DIM links AS Zanna.Collections.Seq = Zanna.Text.Html.ExtractLinks(page)
PRINT links.Count     ' Output: 2
PRINT Zanna.Collections.Seq.GetStr(links, 0)  ' Output: "https://example.com"
PRINT Zanna.Collections.Seq.GetStr(links, 1)  ' Output: "/about"

' Extract text from specific tags
DIM doc AS STRING = "<h1>Title</h1><p>Body</p><h1>Another</h1>"
DIM headings AS Zanna.Collections.Seq = Zanna.Text.Html.ExtractText(doc, "h1")
PRINT headings.Count     ' Output: 2
PRINT Zanna.Collections.Seq.GetStr(headings, 0)  ' Output: "Title"

' Parse HTML into a navigable tree
DIM tree AS Zanna.Collections.Map = Zanna.Text.Html.Parse("<div class=""main""><p>Hello</p></div>")
' tree is a Map with keys: tag, text, attrs, children
```

### Use Cases

- **Web scraping:** Extract links and content from downloaded HTML
- **Security:** Escape user input before including in HTML output
- **Email processing:** Convert HTML emails to plain text
- **Content extraction:** Pull text from specific HTML elements
- **HTML cleanup:** Strip tags from rich text content

---

## Zanna.Text.Markdown

Small Markdown-like HTML conversion and text extraction utility. It supports a practical subset of
headings, emphasis, links, code, lists, horizontal rules, and paragraph lines; it is not a
CommonMark parser.

**Type:** Static utility class

### Methods

| Method                  | Signature        | Description                                    |
|-------------------------|------------------|------------------------------------------------|
| `ToHtml(markdown)`      | `String(String)` | Convert Markdown text to HTML                  |
| `ToText(markdown)`      | `String(String)` | Strip Markdown formatting to get plain text    |
| `ExtractLinks(markdown)`| `Seq(String)`    | Extract all URLs from Markdown links           |
| `ExtractHeadings(markdown)` | `Seq(String)` | Extract all heading texts from Markdown        |

### Supported Markdown Syntax

| Syntax                | HTML Output          | Description            |
|-----------------------|----------------------|------------------------|
| `# Heading`           | `<h1>Heading</h1>`  | Headings (h1-h6, one to six `#` followed by a space) |
| `**bold**`            | `<strong>bold</strong>` | Bold text           |
| `*italic*`            | `<em>italic</em>`    | Italic text            |
| `` `code` ``          | `<code>code</code>`  | Inline code            |
| `[text](url)`         | `<a href="url">text</a>` | Links              |
| `- item` or `* item`  | `<ul><li>item</li></ul>` | Consecutive unordered list items |
| fenced code block     | `<pre><code>...</code></pre>` | Triple-backtick code blocks |
| `---`, `***`, or `___` | `<hr>`               | Three or more contiguous markers; `_ _ _` is also accepted |
| Ordinary source line   | `<p>...</p>`         | Each non-special line becomes its own paragraph |
| Blank line             | no output            | Closes an open list but emits no element |

### Zia Example

```zia
module MarkdownDemo;

bind Zanna.Terminal;
bind Zanna.Text.Markdown as Md;

func start() {
    var src = "# Hello\nThis is **bold** text.";

    // Convert Markdown to HTML
    var html = Md.ToHtml(src);
    Say("HTML: " + html);

    // Convert Markdown to plain text
    var plain = Md.ToText(src);
    Say("Text: " + plain);
}
```

### BASIC Example

```basic
' Convert Markdown to HTML
DIM md AS STRING = "# Hello" + CHR$(10) + "This is **bold** and *italic*."
DIM html AS STRING = Zanna.Text.Markdown.ToHtml(md)
PRINT html
' Output consists of the h1 and paragraph tags on separate lines.

' Convert to plain text
DIM plain AS STRING = Zanna.Text.Markdown.ToText(md)
PRINT plain
' Output:
' Hello
' This is bold and italic.

' Extract all links
DIM doc AS STRING = "Visit [Google](https://google.com) or [GitHub](https://github.com)"
DIM links AS Zanna.Collections.Seq = Zanna.Text.Markdown.ExtractLinks(doc)
PRINT links.Count     ' Output: 2
PRINT Zanna.Collections.Seq.GetStr(links, 0)  ' Output: "https://google.com"

' Extract headings for table of contents
DIM article AS STRING = "# Introduction" + CHR$(10) + "## Background" + CHR$(10) + "## Methods"
DIM headings AS Zanna.Collections.Seq = Zanna.Text.Markdown.ExtractHeadings(article)
PRINT headings.Count     ' Output: 3
PRINT Zanna.Collections.Seq.GetStr(headings, 0)  ' Output: "Introduction"
```

### Notes

- This is a basic Markdown converter, not a full CommonMark implementation
- Input is split only on `LF`. A preceding `CR` is retained in headings, paragraphs, and code, so
  normalize CRLF input before conversion; see
  [VDOC-050](../../../misc/reviews/documentation-review-findings.md#vdoc-050--markdown-retains-carriage-returns-from-crlf-input).
- Spaced `- - -` and `* * *` horizontal rules are consumed as list items, and a list is not closed
  before every following block type; see
  [VDOC-049](../../../misc/reviews/documentation-review-findings.md#vdoc-049--markdown-block-state-produces-wrong-or-invalid-html).
- Link URLs are escaped before being written to HTML attributes, and unsafe schemes are blocked even with leading whitespace/control bytes
- `ExtractLinks` applies the same unsafe-scheme policy and returns `"#"` in place of a blocked URL
- Unmatched `**`, `*`, and backtick markers are emitted as literal text rather than as unclosed formatting spans
- `ToText` preserves source line breaks but does not append an extra final newline; malformed link
  starts such as `[` stay literal. It removes every underscore byte, including literal underscores
  in names such as `snake_case`; see
  [VDOC-051](../../../misc/reviews/documentation-review-findings.md#vdoc-051--markdowntotext-deletes-literal-underscores).
- `ExtractHeadings` follows the same heading rule as rendering: one to six `#` characters followed by a space
- `ExtractHeadings` returns the raw heading contents, including any inline Markdown markers
- Extraction helpers return owned sequences of owned strings
- Null input produces an empty string or empty extraction sequence
- Nested formatting (e.g., bold within italic) may not render correctly

### Use Cases

- **Documentation rendering:** Convert Markdown docs to HTML
- **Content preview:** Generate plain text previews from Markdown
- **Link extraction:** Gather all URLs from documentation
- **Table of contents:** Build TOC from headings

---


## See Also

- [Encoding & Identity](encoding.md)
- [Data Formats](formats.md)
- [Pattern Matching](patterns.md)
- [Text Processing Overview](README.md)
- [Zanna Runtime Library](../README.md)
