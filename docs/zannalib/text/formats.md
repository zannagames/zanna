---
status: active
audience: public
last-verified: 2026-07-26
---

# Data Formats
> Json, JsonPath, JsonStream, Csv, Toml, Ini, Xml, Yaml, and serialization

**Part of [Zanna Runtime Library](../README.md) › [Text Processing](README.md)**

---

## Zanna.Data.Json

JSON value-tree parsing and formatting targeting ECMA-404/[RFC 8259](https://www.rfc-editor.org/rfc/rfc8259.html),
with the validation and runtime-value limitations called out below.

**Type:** Static utility class

### Methods

| Method              | Signature           | Description                                      |
|---------------------|---------------------|--------------------------------------------------|
| `Parse(text)`       | `Object(String)`    | Parse JSON string into a value tree              |
| `ParseObject(text)` | `Map(String)`       | Parse JSON, expecting an object (returns a typed `Map`) |
| `ParseArray(text)`  | `Object(String)`    | Parse JSON, expecting an array (Seq)             |
| `Format(value)`     | `String(Object)`    | Format value as compact JSON string              |
| `FormatPretty(v,n)` | `String(Object,Int)`| Format with indentation (n spaces)               |
| `IsValid(text)`     | `Boolean(String)`   | Check if string is valid JSON                    |
| `TypeOf(value)`     | `String(Object)`    | Get type: `"null"`, `"boolean"`, `"number"`, `"string"`, `"array"`, `"object"`, or `"unknown"` |
| `NewObject()`       | `Map()`             | Create an empty JSON object backed by `Map`      |
| `Has(obj,key)`      | `Boolean(Object,String)` | Check object key existence                 |
| `GetStr(obj,key)`   | `String(Object,String)` | Read a string object field                  |
| `GetInt(obj,key)`   | `Integer(Object,String)` | Read a numeric object field as an integer   |
| `GetBool(obj,key)`  | `Boolean(Object,String)` | Read a boolean object field                |
| `SetStr(obj,key,value)` | `Void(Object,String,String)` | Write a string object field          |
| `SetInt(obj,key,value)` | `Void(Object,String,Integer)` | Write an integer object field        |
| `SetBool(obj,key,value)` | `Void(Object,String,Boolean)` | Write a boolean object field       |

### Value Access

JSON values are returned as native Zanna types:

| JSON Type   | Zanna Type         | Access Method                      |
|-------------|--------------------|------------------------------------|
| null        | `NULL`             | Check with `value = NULL` |
| boolean     | boxed Boolean      | Use a typed map/path helper, or `Zanna.Core.Box.ToI1(value)` |
| number      | boxed Double       | Use `Zanna.Core.Box.ToF64(value)`; `GetInt` truncates to an integer |
| string      | runtime String     | Use `Map.GetStr`, `Seq.GetStr`, or `JsonPath.GetStr` |
| array       | `Seq`              | Use `Count` and `Get(index)` |
| object      | `Map`              | Use `Count`, `Get(key)`, and `Keys()` |

JSON numbers are always parsed as boxed IEEE-754 doubles, including integer-looking tokens such
as `1`. JSON strings are runtime string handles, not `Box.Str` values, so the strict
`Zanna.Core.Box.ToStr` unboxer is not the general accessor for parsed JSON strings.

### Zia Example

```zia
module JsonDemo;

bind Zanna.Terminal;
bind Zanna.Data.Json as Json;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var obj = Json.Parse("{\"name\":\"Zanna\",\"version\":1}");
    Say("Type: " + Json.TypeOf(obj));                       // object
    Say("Valid: " + Fmt.Bool(Json.IsValid("{\"ok\":true}"))); // true
    Say(Json.Format(obj));                                    // {"name":"Zanna","version":1}
}
```

### BASIC Example

```basic
' Parse JSON
DIM json AS STRING = "{""name"": ""Alice"", ""age"": 30, ""active"": true}"
DIM data AS OBJECT = Zanna.Data.Json.Parse(json)

' The Json class aliases resolve in BASIC and Zia alike
DIM name AS STRING = Zanna.Data.Json.GetStr(data, "name")
DIM age AS INTEGER = Zanna.Data.Json.GetInt(data, "age")
PRINT "Name: "; name   ' Output: Alice
PRINT "Age: "; age     ' Output: 30

' Check type
DIM valueType AS STRING = Zanna.Data.Json.TypeOf(Zanna.Collections.Map.Get(data, "active"))
PRINT valueType        ' Output: "boolean"

' Format with pretty printing
DIM config AS OBJECT = Zanna.Collections.Map.New()
config.Set("debug", Zanna.Core.Box.I1(true))
config.Set("port", Zanna.Core.Box.I64(8080))

DIM formattedJson AS STRING = Zanna.Data.Json.FormatPretty(config, 2)
PRINT formattedJson
' Output:
' {
'   "debug": true,
'   "port": 8080
' }

' Validate JSON
IF Zanna.Data.Json.IsValid(json) THEN
    DIM parsed AS OBJECT = Zanna.Data.Json.Parse(json)
END IF
```

### Use Cases

- **API responses:** Parse JSON from web services
- **Configuration:** Read/write JSON config files
- **Data interchange:** Standard format for data exchange
- **Storage:** Serialize application state

### Correctness Notes

- `Parse`, `ParseObject`, `ParseArray`, and `IsValid` use the runtime string byte length, not C-string truncation.
- Invalid escapes, malformed UTF-16 surrogate pairs, raw control characters inside strings, trailing content, leading-zero numbers, and out-of-range/non-finite numbers are rejected.
- `IsValid` and `JsonStream` mirror the parser's raw UTF-8 validation (overlong encodings,
  surrogates, bad continuation bytes, and code points above U+10FFFF are rejected), so
  `IsValid(text)` never accepts input that `Parse(text)` traps on. `Format` emits stored string
  bytes unchanged — output-side validation would break the binary-safe runtime String model.
- `Format` escapes embedded `NUL` and other control bytes as JSON escapes, and traps on cyclic
  `Seq`/`Map` object graphs instead of recursing indefinitely. Unsupported runtime objects and
  non-finite boxed doubles are emitted as JSON `null`.
- Converting a boxed double through `Json.GetInt`/`Map.GetInt` truncates toward zero with
  defined saturation: values above/below the signed 64-bit range clamp to the range limits and
  NaN converts to 0, identically on every platform.
- The `Get*`, `Set*`, and `Has` entries on the `Json` class surface are aliases for the
  registered `Zanna.Collections.Map` functions. Both Zia and BASIC resolve them by their `Json`
  names; calling the concrete `Map` functions directly is equivalent.

---

## Zanna.Data.JsonPath

JSONPath-like query expressions for navigating parsed JSON objects. Works with objects returned by `Zanna.Data.Json.Parse()`.

**Type:** Static utility class

### Methods

| Method                      | Signature                   | Description                                          |
|-----------------------------|-----------------------------|------------------------------------------------------|
| `Get(root, path)`           | `Object(Object, String)`    | Get value at path, or NULL if not found              |
| `GetOr(root, path, default)`| `Object(Object, String, Object)` | Get value at path, or default if not found      |
| `Has(root, path)`           | `Boolean(Object, String)`   | Check if path exists in the object                   |
| `Query(root, path)`         | `Object(Object, String)`    | Return a `Seq` of values matching a wildcard path    |
| `GetStr(root, path)`        | `String(Object, String)`    | Get scalar text at path, or empty string             |
| `GetInt(root, path)`        | `Integer(Object, String)`   | Convert scalar value at path to integer, or 0        |

### Path Syntax

| Syntax           | Description                           | Example              |
|------------------|---------------------------------------|----------------------|
| `key`            | Access object property                | `"name"`             |
| `key1.key2`      | Nested property access                | `"user.name"`        |
| `key[0]`         | Array element by index                | `"items[0]"`         |
| `key1.key2[0].x` | Mixed object/array access             | `"users[0].name"`    |
| `key.*`          | Wildcard (all children)               | `"users.*.name"`     |
| `$` / `$.key`    | Optional root selector                | `"$.user.name"`      |
| `key[-1]`        | Negative index counted from the end   | `"items[-1]"`        |
| `["key"]`       | Quoted map key                         | `"[\"display.name\"]"` |

### Notes

- `GetStr()` retains string values and converts integer, double, and boolean boxes to text. It
  returns a new empty string for missing paths or containers/unsupported values.
- `GetOr()` returns a retained value when the path is found, and retains the supplied default when it is returned.
- `GetInt()` accepts integer/double/boolean boxes and integer strings. Representable doubles are
  truncated toward zero; missing or incompatible values produce `0`. An out-of-range double is
  currently converted with a C cast and can produce platform-dependent results; see
  [VDOC-037](../../../misc/reviews/documentation-review-findings.md#vdoc-037--json-derived-integer-accessors-have-undefined-out-of-range-conversion).
- `Get()`, `Has()`, `GetStr()`, `GetInt()`, and `Query()` also accept raw JSON source text as the
  root and parse it internally. A parsed JSON string scalar has the same runtime representation as
  source text and therefore cannot be used unambiguously as a root.
- `Has()` distinguishes a present JSON `null` member (true) from a missing path (false), and
  `GetOr()` substitutes its default only when the path is missing — a stored JSON `null` is
  returned as `NULL` rather than replaced.
- `Query()` supports one wildcard segment. It traverses arrays and object maps and safely skips
  scalar values that cannot contain the remaining path.
- `Query()` returns an owned sequence that retains the matched values it contains.
- The registry return is a typed sequence, so member access resolves directly on the result.
- Recursive descent (`..`), slices, filters, unions, and escaped quoted-key syntax are not
  implemented, despite broader syntax still named in the implementation header; see
  [VDOC-022](../../../misc/reviews/documentation-review-findings.md#vdoc-022--the-jsonpath-source-contract-overstates-the-implemented-syntax).
- Path parsing uses a C-string terminator rather than the runtime String length. An embedded `NUL`
  therefore discards the rest of the path and can select a different member; see
  [VDOC-038](../../../misc/reviews/documentation-review-findings.md#vdoc-038--jsonpath-ignores-path-bytes-after-an-embedded-nul).

### Zia Example

```zia
module JsonPathDemo;

bind Zanna.Terminal;
bind Zanna.Data.Json as Json;
bind Zanna.Data.JsonPath as JP;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var data = Json.Parse("{\"user\": {\"name\": \"Alice\", \"age\": 30}}");

    // Get a string value by path
    var name = JP.GetStr(data, "user.name");
    Say("Name: " + name);

    // Check if a path exists
    Say("Has name: " + Fmt.Bool(JP.Has(data, "user.name")));
    Say("Has email: " + Fmt.Bool(JP.Has(data, "user.email")));
}
```

### BASIC Example

```basic
' Parse a JSON document
DIM json AS STRING = "{""user"": {""name"": ""Alice"", ""scores"": [95, 87, 92]}}"
DIM data AS OBJECT = Zanna.Data.Json.Parse(json)

' Simple path access
DIM name AS STRING = Zanna.Data.JsonPath.GetStr(data, "user.name")
PRINT name  ' Output: "Alice"

' Array access
DIM first AS INTEGER = Zanna.Data.JsonPath.GetInt(data, "user.scores[0]")
PRINT first  ' Output: 95

' Check existence
IF Zanna.Data.JsonPath.Has(data, "user.email") THEN
    PRINT "Has email"
ELSE
    PRINT "No email field"  ' Output: "No email field"
END IF

' Default values (the missing path returns the boxed integer default)
DIM rankValue AS OBJECT = Zanna.Data.JsonPath.GetOr(data, "user.rank", Zanna.Core.Box.I64(-1))
PRINT Zanna.Core.Box.ToI64(rankValue)  ' Output: -1

' Wildcard queries
DIM api AS STRING = "{""users"": [{""name"": ""Alice""}, {""name"": ""Bob""}]}"
DIM apiData AS OBJECT = Zanna.Data.Json.Parse(api)
DIM names AS Zanna.Collections.Seq = Zanna.Data.JsonPath.Query(apiData, "users.*.name")
PRINT names.Count     ' Output: 2
```

### Use Cases

- **API responses:** Navigate deeply nested JSON without manual traversal
- **Configuration:** Access config values by dotted paths
- **Data extraction:** Query specific fields from complex JSON structures

---

## Zanna.Data.JsonStream

Pull-based JSON tokenizer for processing a complete JSON string without building the parsed
`Map`/`Seq` value tree.

**Type:** Instance class (requires `New(json)`)

### Constructor

| Method       | Signature             | Description                           |
|--------------|-----------------------|---------------------------------------|
| `New(json)`  | `JsonStream(String)`  | Create a tokenizer that retains the complete JSON string |

### Properties

| Property    | Type                  | Description                              |
|-------------|-----------------------|------------------------------------------|
| `Depth`     | `Integer` (read-only) | Current nesting depth (0 = top level)    |
| `TokenType` | `Integer` (read-only) | Current token type                       |

### Methods

| Method          | Signature        | Description                                      |
|-----------------|------------------|--------------------------------------------------|
| `Next()`        | `Integer()`      | Advance to next token; returns token type         |
| `NextResult()`  | `Result()`       | Advance to next token as `Ok(token)` or `Err(message)` |
| `HasNext()`     | `Boolean()`      | Check if more tokens remain                       |
| `StringValue()` | `String()`       | Get string value of current token (key/string)    |
| `NumberValue()` | `Double()`       | Get numeric value of current token                |
| `BoolValue()`   | `Boolean()`      | Get boolean value of current token                |
| `Skip()`        | `Void()`         | Skip current value (object, array, or primitive)  |

### Token Types

Token types are plain integers; pass and compare the values directly.

| Token          | Value | Description      |
|----------------|-------|------------------|
| `TOK_NONE`         | 0  | No token yet              |
| `TOK_OBJECT_START` | 1  | `{` — object begins      |
| `TOK_OBJECT_END`   | 2  | `}` — object ends        |
| `TOK_ARRAY_START`  | 3  | `[` — array begins       |
| `TOK_ARRAY_END`    | 4  | `]` — array ends         |
| `TOK_KEY`          | 5  | Object key string         |
| `TOK_STRING`       | 6  | String value              |
| `TOK_NUMBER`       | 7  | Numeric value             |
| `TOK_BOOL`         | 8  | Boolean value             |
| `TOK_NULL`         | 9  | Null value                |
| `TOK_ERROR`        | 10 | Parse error               |
| `TOK_END`          | 11 | End of input              |

### Notes

- Pull-based: call `NextResult()` to advance when parsing untrusted JSON — it reports a parse
  failure as `Err(message)`. `Next()` returns the raw token type and signals failure as
  `TOK_ERROR` (10).
- `Skip()` scans through the current container without allocating its value tree. Primitive tokens
  are already consumed, so `Skip()` is a no-op on them.
- `Depth` is the number of currently open objects/arrays after the current token is consumed.
- The tokenizer retains the entire input string and scratch storage for decoded strings. It uses
  less additional memory than `Json.Parse()` because it does not allocate a value tree, but it is
  not an incremental/feed parser.
- Tokens are consumed in order; there is no random access.
- `NextResult()` returns `Err(message)` on malformed JSON. Normal tokens, including `TOK_END`,
  are returned as `Ok(token)`.
- The parser enforces JSON separators and container state: missing commas/colons, mismatched closers, trailing commas, and multiple top-level values produce `TOK_ERROR`
- Number parsing rejects leading zeroes, incomplete fractions/exponents, NaN, Infinity, and overflow
- Strings reject raw control characters and invalid UTF-16 surrogate pairs

### Zia Example

```zia
module JsonStreamDemo;

bind Zanna.Terminal;
bind JS = Zanna.Data.JsonStream;

func start() {
    var s = JS.New("{\"name\":\"Alice\",\"age\":30,\"active\":true}");

    // Token-by-token parsing
    while JS.HasNext(s) {
        var next = JS.NextResult(s);
        if next.IsErr {
            Say("JSON error: " + next.UnwrapErrStr());
            return;
        }
        var tok = next.UnwrapI64();
        if tok == 5 { Say("Key: " + JS.StringValue(s)); }
        if tok == 6 { Say("String: " + JS.StringValue(s)); }
        if tok == 7 { SayNum(JS.NumberValue(s)); }
        if tok == 8 { SayBool(JS.BoolValue(s)); }
    }

    // Depth tracking
    var s2 = JS.New("{\"a\":{\"b\":1}}");
    JS.Next(s2);              // {
    SayInt(JS.Depth(s2));     // 1
    JS.Next(s2); JS.Next(s2); // key "a", {
    SayInt(JS.Depth(s2));     // 2

    // Skip containers
    var s3 = JS.New("{\"skip\":[1,2,3],\"keep\":\"yes\"}");
    JS.Next(s3);              // {
    JS.Next(s3);              // key "skip"
    JS.Next(s3);              // [ (start array)
    JS.Skip(s3);              // skip array contents
    JS.Next(s3);              // key "keep"
    JS.Next(s3);              // "yes"
    Say(JS.StringValue(s3));  // yes
}
```

### BASIC Example

```basic
' Stream through a JSON document
DIM json AS STRING = "{""name"": ""Alice"", ""scores"": [95, 87, 92]}"
DIM stream AS OBJECT = NEW Zanna.Data.JsonStream(json)

DO WHILE stream.HasNext()
    DIM nextToken AS OBJECT = stream.NextResult()
    IF nextToken.IsErr THEN
        PRINT "JSON error: "; nextToken.UnwrapErrStr()
        END
    END IF
    DIM tok AS INTEGER = nextToken.UnwrapI64()

    SELECT CASE tok
    CASE 5  ' TOK_KEY
        DIM key AS STRING = stream.StringValue()
        IF key = "scores" THEN
            ' Process the scores array
            DIM arrayStart AS OBJECT = stream.NextResult()  ' consume ARRAY_START
            DO WHILE TRUE
                DIM itemToken AS OBJECT = stream.NextResult()
                IF itemToken.IsErr THEN
                    PRINT "JSON error: "; itemToken.UnwrapErrStr()
                    EXIT DO
                END IF
                IF itemToken.UnwrapI64() = 4 THEN EXIT DO  ' TOK_ARRAY_END
                PRINT "Score: "; stream.NumberValue()
            LOOP
        END IF
    END SELECT
LOOP
```

### When to Use JsonStream vs Json

| Scenario                        | Recommendation              |
|---------------------------------|-----------------------------|
| Need a navigable value tree     | Use `Json.Parse()` |
| Want to avoid allocating a value tree | Use `JsonStream` |
| Need only selected containers   | Use `JsonStream` with `Skip()` |
| Need incremental input chunks   | Neither API supports feeds; buffer a complete value first |
| Processing JSON Lines           | Create a separate parser for each line/value |

---

## Zanna.Data.Csv

CSV parsing and formatting with [RFC 4180](https://www.rfc-editor.org/rfc/rfc4180.html)-style
quoting plus configurable-delimiter and line-ending extensions.

**Type:** Static utility class

### Methods

| Method                          | Signature             | Description                                                       |
|---------------------------------|-----------------------|-------------------------------------------------------------------|
| `ParseLine(line)`               | `Seq(String)`         | Parse a single CSV line into fields using comma delimiter         |
| `ParseLineWith(line, delim)`    | `Seq(String, String)` | Parse a single CSV line with custom delimiter                     |
| `Parse(text)`                   | `Object(String)`      | Parse CSV text into a `Seq` of row `Seq` values                   |
| `ParseWith(text, delim)`        | `Object(String, String)` | Parse CSV text with a custom delimiter into row `Seq` values   |
| `FormatLine(fields)`            | `String(Seq)`         | Format a Seq of fields into a CSV line                            |
| `FormatLineWith(fields, delim)` | `String(Seq, String)` | Format fields with custom delimiter                               |
| `Format(rows)`                  | `String(Seq)`         | Format a Seq of rows into multi-line CSV text                     |
| `FormatWith(rows, delim)`       | `String(Seq, String)` | Format rows with custom delimiter                                 |
| `IsValid(text)`                 | `Boolean(String)`     | Check CSV syntax without trapping                                 |

### Notes

- **Quoting rules:**
    - Fields containing delimiters, quotes, CR, or LF are automatically quoted.
    - Embedded quotes are escaped by doubling (`""`).
    - Newlines within quoted fields are preserved.
    - Leading/trailing whitespace in fields is preserved.
- Parsing accepts CRLF, LF, and CR record endings. Multi-row formatting emits LF after every row,
  including the last, so output is not strict RFC 4180 CRLF. Row widths are not required to match.
- A custom delimiter must contain exactly one byte and cannot be `"`, CR, LF, or NUL.
- A null or empty custom delimiter falls back to comma
- Empty fields are supported (adjacent delimiters create empty strings)
- Null fields passed to formatters are emitted as empty fields; other non-string fields use the
  runtime's generic object-to-string conversion
- Runtime string byte length is used, so embedded `NUL` bytes are preserved
- Parse functions return `Seq` objects (use `Count`, `Get(index)` to access)
- `Parse`/`ParseWith` are registered as `seq<obj>` (a sequence of row Seqs), so member access
  such as `rows.Count` resolves directly.
- `IsValid` uses the default comma delimiter and returns false for malformed quoting without trapping
- `ParseLine` accepts exactly one CSV record; use `Parse` / `ParseWith` for multi-record text
- Malformed quoted fields trap on unterminated quotes or non-delimiter characters after the closing quote
- Quotes inside unquoted fields are rejected as malformed CSV
- Custom delimiters must be exactly one byte; an empty delimiter selects the comma default and a
  longer delimiter traps.

### Zia Example

```zia
module CsvDemo;

bind Zanna.Terminal;
bind Zanna.Data.Csv as Csv;
bind Zanna.Text.Fmt as Fmt;
bind Zanna.Collections.Seq as Seq;

func start() {
    // Parse a CSV line into fields
    var fields = Csv.ParseLine("name,age,city");
    Say("Field count: " + Fmt.Int(fields.Count));

    // Format a CSV line with quoting
    var data = Seq.New();
    Seq.Push(data, "Hello, World");
    Seq.Push(data, "Simple");
    var line = Csv.FormatLine(data);
    Say("Formatted: " + line);
}
```

### BASIC Example

```basic
' Parse a simple CSV line
DIM fields AS Zanna.Collections.Seq = Zanna.Data.Csv.ParseLine("name,age,city")
PRINT fields.Count  ' Output: 3
PRINT Zanna.Collections.Seq.GetStr(fields, 0) ' Output: "name"

' Parse with quoted fields
DIM quote AS STRING = CHR$(34)
DIM row AS Zanna.Collections.Seq = Zanna.Data.Csv.ParseLine( _
    quote + "John Doe" + quote + ",30," + quote + "New York" + quote)
PRINT Zanna.Collections.Seq.GetStr(row, 0)    ' Output: John Doe
PRINT Zanna.Collections.Seq.GetStr(row, 2)    ' Output: New York

' Handle embedded quotes (doubled)
DIM quoted AS Zanna.Collections.Seq = Zanna.Data.Csv.ParseLine( _
    quote + "He said " + quote + quote + "Hello" + quote + quote + quote)
PRINT Zanna.Collections.Seq.GetStr(quoted, 0) ' Output: He said "Hello"

' Parse multi-line CSV
DIM csv AS STRING = "name,age" + CHR$(10) + "Alice,25" + CHR$(10) + "Bob,30"
DIM rows AS Zanna.Collections.Seq = Zanna.Data.Csv.Parse(csv)
PRINT rows.Count    ' Output: 3

' Format fields into CSV
DIM data AS Zanna.Collections.Seq = NEW Zanna.Collections.Seq()
data.Push("Hello, World")
data.Push("Simple")
DIM line AS STRING = Zanna.Data.Csv.FormatLine(data)
PRINT line          ' Output: "Hello, World",Simple

' Use tab delimiter
DIM tsv AS Zanna.Collections.Seq = Zanna.Data.Csv.ParseLineWith("a	b	c", CHR$(9))
PRINT tsv.Count     ' Output: 3
```

### Use Cases

- Importing/exporting spreadsheet data
- Configuration files with structured data
- Data interchange between systems
- Log file parsing

---

## Zanna.Data.Toml

Permissive parser and formatter for a practical subset of TOML (Tom's Obvious Minimal Language).
The current published specification is [TOML 1.1.0](https://toml.io/en/v1.1.0); this runtime's
older source contract says 1.0, but the accepted language is not a conformance implementation of
either version.

**Type:** Static utility class

### Methods

| Method                | Signature              | Description                                    |
|-----------------------|------------------------|------------------------------------------------|
| `Parse(text)`         | `Object(String)`       | Parse accepted TOML-like text into a Map       |
| `IsValid(text)`       | `Boolean(String)`      | Check whether the runtime parser accepts text  |
| `Format(map)`         | `String(Object)`       | Format a Map using the supported TOML subset   |
| `Get(root, keyPath)`  | `Object(Object, String)` | Get a value through nested Maps              |
| `GetStr(root, keyPath)` | `String(Object, String)` | Get a string value, or an empty string       |

### Notes

- **Parse output:** Keys and all scalar values—including numbers, booleans, and date/time-looking
  tokens—are runtime strings. Tables and inline tables become Maps; arrays become Seqs; arrays of
  tables become Seqs of Maps.
- **Dotted paths:** `Get()` and `GetStr()` split the lookup path on dots to navigate nested section
  Maps, such as `"server.host"`. There is no escaping for a literal dot in a key.
- Dotted section headers such as `[server.database.primary]` and dotted assignment keys such as
  `server.host = "localhost"` both create nested Maps, so the dotted getters reach them. Quoted
  keys (`"a.b" = ...`) keep their literal spelling, dots included; keys with a leading or trailing
  dot are rejected.
- **Format:** Formats top-level values and nested section Maps recursively (dotted `[a.b.c]`
  headers). It supports strings, boxed integers/doubles/booleans, nested arrays, and maps in
  value position as inline tables.
- Boxed doubles use `%.17g` with a float-token guard: whole-valued doubles emit as `1.0` (not the
  integer token `1`), so the value keeps its float type on reparse.
- `Format()` quotes keys and string values when needed so scalar values are not misinterpreted as Maps.
- `Format()` uses runtime string byte lengths and escapes embedded `NUL` and other control bytes as TOML basic-string escapes instead of truncating at the first `NUL`.
- `Get()` and `GetStr()` use the runtime byte length of the dotted path; embedded `NUL` bytes inside a path segment are part of that segment, not a terminator.
- Syntax errors recognized by the parser return NULL from `Parse()` rather than trapping. However,
  arbitrary bare values such as `x = alpha` are accepted even though TOML 1.0 does not permit them,
  so `IsValid()` is only an acceptance probe for this parser. See
  [VDOC-024](../../../misc/reviews/documentation-review-findings.md#vdoc-024--the-toml-parser-is-not-the-v10-parser-described-by-its-source-contract).
- Missing closing section or array brackets, trailing junk after section headers, duplicate keys,
  scalar/table conflicts, and section paths deeper than 200 components are rejected.
- `GetStr()` returns a retained string value when found, or a new empty string when the path is missing or not a string.
- Formatting emits nested tables recursively as dotted `[a.b.c]` section headers, and maps in
  value position (such as inside arrays) as inline tables, so `Format(Parse(text))` preserves
  nested configuration. Formatting depth is bounded (200 levels); exceeding it yields an empty
  string.
- Formatting depth is bounded (200 levels); cyclic or deeper containers fail closed with an empty
  string instead of recursing without bound.
- The parser validates the input is NUL-free, well-formed UTF-8 before parsing; malformed byte
  sequences make `Parse` return NULL and `IsValid` report false.
- TOML and YAML numeric formatting uses the process numeric locale rather than an isolated C
  locale when called outside the VM's locale initialization; see
  [VDOC-041](../../../misc/reviews/documentation-review-findings.md#vdoc-041--toml-and-yaml-numeric-emission-is-locale-sensitive).

### Zia Example

```zia
module TomlDemo;

bind Zanna.Terminal;
bind Zanna.Data.Toml as Toml;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Validate TOML strings
    Say("Valid: " + Fmt.Bool(Toml.IsValid("key = \"value\"")));
    Say("Invalid: " + Fmt.Bool(Toml.IsValid("= bad")));
}
```

### BASIC Example

```basic
' Parse TOML configuration
DIM config AS STRING = "[server]" + CHR$(10) + _
                       "host = ""localhost""" + CHR$(10) + _
                       "port = ""8080""" + CHR$(10) + _
                       "[database]" + CHR$(10) + _
                       "url = ""postgres://localhost/mydb"""

DIM data AS OBJECT = Zanna.Data.Toml.Parse(config)

' Access nested scalar values using the string getter
DIM host AS STRING = Zanna.Data.Toml.GetStr(data, "server.host")
PRINT host  ' Output: "localhost"

DIM port AS STRING = Zanna.Data.Toml.GetStr(data, "server.port")
PRINT port  ' Output: "8080"

' Validate before parsing
IF Zanna.Data.Toml.IsValid(config) THEN
    DIM parsed AS OBJECT = Zanna.Data.Toml.Parse(config)
END IF

' Format back to TOML
DIM formattedToml AS STRING = Zanna.Data.Toml.Format(data)
PRINT formattedToml
```

### Use Cases

- **Configuration files:** Parse application config in TOML format
- **Settings management:** Read/write user preferences
- **Build tools:** Parse project metadata files

---

## Zanna.Data.Ini

INI-style configuration parsing and manipulation. The accepted dialect uses `[section]` headers,
`key=value` pairs, and whole-line `;`/`#` comments.

**Type:** Static utility class

### Methods

| Method                          | Signature                          | Description                                              |
|---------------------------------|------------------------------------|----------------------------------------------------------|
| `Parse(text)`                   | `Object(String)`                    | Parse into a Map of section Maps                         |
| `Format(doc)`                   | `String(Object)`                    | Format a Map of Maps back to INI-style text              |
| `Get(doc, section, key)`        | `String(Object, String, String)`    | Get a value, or empty string if not found                |
| `Set(doc, section, key, value)` | `Void(Object, String, String, String)` | Set a value, creating the section if needed           |
| `HasSection(doc, section)`      | `Boolean(Object, String)`           | Check if a section exists                                |
| `Sections(doc)`                 | `Object(Object)`                    | Return a `Seq` containing all section names              |
| `Remove(doc, section, key)`     | `Boolean(Object, String, String)`   | Remove a key; return true if it was present              |

### Notes

- `Parse` returns a Map where each key is a case-sensitive section name and each value is a Map of
  case-sensitive key/value strings.
- The empty-string default section is created lazily: entries before the first named section are
  stored there, and `Sections()` includes its `""` name only when such entries exist. NULL input
  and an empty string both produce the same empty document.
- Leading/trailing whitespace around lines, section names, keys, and values is trimmed. Duplicate
  keys keep the last value.
- Only whole-line comments are recognized. Inline `;` and `#` bytes remain part of a value.
- Lines without `=` and malformed section lines are silently ignored; this API has no validation or
  parse-error result.
- `Set` creates the section automatically if it does not already exist.
- `Remove` returns true (1) if the key was found and removed, false (0) if not found.
- `Sections()` is registered as `seq<str>`, so member access resolves directly on the result.
- `Parse` and `Format` use runtime string byte lengths, so embedded `NUL` bytes in keys and values are preserved.
- `Format` writes values verbatim; INI has no escaping layer, so consumers that require text-only INI should avoid control bytes.

### Zia Example

```zia
module IniDemo;

bind Zanna.Terminal;
bind Zanna.Data.Ini as Ini;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var text = "[app]\nname=MyApp\nversion=1.0\n[db]\nhost=localhost";
    var doc = Ini.Parse(text);

    Say("Name: " + Ini.Get(doc, "app", "name"));          // MyApp
    Say("Host: " + Ini.Get(doc, "db", "host"));            // localhost
    Say("Has app: " + Fmt.Bool(Ini.HasSection(doc, "app"))); // true

    Ini.Set(doc, "db", "port", "5432");
    Say(Ini.Format(doc));
}
```

### BASIC Example

```basic
' Parse an INI configuration string
DIM text AS STRING = "[app]" + CHR$(10) + "name=MyApp" + CHR$(10) + "version=1.0" + CHR$(10)
text = text + "[db]" + CHR$(10) + "host=localhost"
DIM doc AS OBJECT = Zanna.Data.Ini.Parse(text)

' Read values
DIM name AS STRING = Zanna.Data.Ini.Get(doc, "app", "name")
PRINT name  ' Output: "MyApp"

DIM host AS STRING = Zanna.Data.Ini.Get(doc, "db", "host")
PRINT host  ' Output: "localhost"

' Missing keys return empty string
DIM missing AS STRING = Zanna.Data.Ini.Get(doc, "db", "port")
PRINT missing  ' Output: ""

' Check if section exists
IF Zanna.Data.Ini.HasSection(doc, "app") THEN
    PRINT "app section exists"
END IF

' List all sections
DIM sections AS Zanna.Collections.Seq = Zanna.Data.Ini.Sections(doc)
PRINT sections.Count  ' Output: 3 ("", "app", and "db")

' Modify and format back to INI text
Zanna.Data.Ini.Set(doc, "db", "port", "5432")
DIM formattedIni AS STRING = Zanna.Data.Ini.Format(doc)
PRINT formattedIni

' Remove a key
DIM removed AS INTEGER = Zanna.Data.Ini.Remove(doc, "db", "port")
PRINT removed  ' Output: 1 (true)
```

### Use Cases

- **Application config:** Read/write `.ini` or `.cfg` configuration files
- **Settings management:** Store and retrieve user preferences
- **Legacy interop:** Work with INI-format data from older systems

---

## Zanna.Data.Serialize

Unified facade for parsing, formatting, heuristically detecting, and projecting values among JSON,
XML, YAML, TOML, and CSV representations. Cross-format conversion is intentionally lossy for
formats with different data models.

**Type:** Static utility class

### Format Constants

Format selectors are plain integers; pass the values directly to the methods below.

| Format         | Value | Description      |
|----------------|-------|------------------|
| `FORMAT_JSON`  | 0     | JSON (RFC 8259)  |
| `FORMAT_XML`   | 1     | XML (subset)     |
| `FORMAT_YAML`  | 2     | YAML (1.2 subset)|
| `FORMAT_TOML`  | 3     | Runtime's permissive TOML subset |
| `FORMAT_CSV`   | 4     | CSV with RFC 4180-style quoting  |

### Methods

| Method                              | Signature                        | Description                                    |
|-------------------------------------|----------------------------------|------------------------------------------------|
| `Parse(text, format)`               | `Object(String, Integer)`        | Parse text in specified format into a value     |
| `ParseResult(text, format)`         | `Result(String, Integer)`        | Parse as `Ok(value)` or `Err(message)`          |
| `Format(value, format)`             | `String(Object, Integer)`        | Format value as compact text in given format    |
| `FormatPretty(value, format, indent)` | `String(Object, Integer, Integer)` | Format with indentation                    |
| `IsValid(text, format)`             | `Boolean(String, Integer)`       | Check if text is valid for given format         |
| `Detect(text)`                      | `Integer(String)`                | Auto-detect format from content (-1 if unknown) |
| `AutoParse(text)`                   | `Object(String)`                 | Parse by auto-detecting the format              |
| `AutoParseResult(text)`             | `Result(String)`                 | Auto-detect and parse as `Ok(value)` or `Err(message)` |
| `Convert(text, from, to)`           | `String(String, Integer, Integer)` | Convert between formats                      |
| `FormatName(format)`                | `String(Integer)`                | Get format name ("json", "xml", etc.)           |
| `MimeType(format)`                  | `String(Integer)`                | Get MIME type ("application/json", etc.)        |
| `FormatFromName(name)`              | `Integer(String)`                | Look up format by name (case-insensitive)       |

### Notes

- **Auto-detection heuristics:** after leading ASCII whitespace, a valid JSON object/array and then
  any JSON-looking `{`/`[` text → JSON; `<` → XML; a prefix of `---` → YAML; an accepted
  `[section]` or `key = value` first line → TOML; an accepted `key: value` first line → YAML; a
  first-line comma → CSV. Unknown plain text returns `-1`. Detection is a best-effort guess, not
  content-type proof.
- Prefer `ParseResult()` and `AutoParseResult()` for user-provided input; failures are returned as `Err(message)`
- `ParseResult()` returns `Ok(NULL)` for a valid JSON/YAML null value. `Parse()` and `AutoParse()`
  return NULL for both that success and a parse failure, so use the Result forms whenever the two
  must be told apart.
- `Convert()` is `Format(Parse(text, from), to)` plus generic projections. It preserves neither
  source spelling nor all source semantics: TOML scalar types currently parse as strings, XML
  declarations/DTDs are discarded, and CSV has no nested object model.
- XML-to-generic conversion groups repeated sibling tags into a Seq, preserves attributes under
  `@attrs`, and stores text under `@text` when attributes or child elements are also present.
  Generic-to-XML conversion wraps the value in `<root>`, turns Map keys into sanitized child tag
  names, and turns Seq elements into `<item>` children.
- TOML output wraps a non-Map value under `items` (Seq) or `value` (scalar). CSV output maps a Map
  to two-column key/value rows, a flat Seq to one-column rows, and a scalar to one cell.
- `FormatPretty()` applies indentation to JSON, XML, and YAML. TOML and CSV ignore the indent
  argument. Values below 1 select two spaces before dispatch.
- `FormatName()` returns lowercase `json`, `xml`, `yaml`, `toml`, or `csv`; unknown values return
  `unknown`. `FormatFromName()` is case-insensitive and also accepts `yml`.
- `FormatFromName()` currently compares a NUL-terminated view, so a runtime String such as
  `json\0suffix` is incorrectly accepted as `json`. Generic-to-XML projection likewise truncates
  Map keys at an embedded `NUL`; see
  [VDOC-043](../../../misc/reviews/documentation-review-findings.md#vdoc-043--serialize-name-and-xml-key-processing-truncates-at-embedded-nul).
- `MimeType()` returns `application/json`, `application/xml`, `application/yaml`,
  `application/toml`, or `text/csv`; unknown values return `application/octet-stream`.
- All returned strings are newly allocated. The facade dispatches to the format-specific runtime
  backends internally.

### BASIC Example

```basic
' Parse JSON data
DIM json AS STRING = "{""name"": ""Alice"", ""age"": 30}"
DIM parsed AS OBJECT = Zanna.Data.Serialize.ParseResult(json, 0)  ' FORMAT_JSON
IF parsed.IsErr THEN
    PRINT "Parse error: "; parsed.UnwrapErrStr()
    END
END IF
DIM data AS OBJECT = parsed.Unwrap()

' Convert JSON to TOML
DIM toml AS STRING = Zanna.Data.Serialize.Convert(json, 0, 3)  ' JSON → TOML
PRINT toml

' Auto-detect and parse
DIM unknown AS STRING = "{""key"": ""value""}"
DIM detected AS INTEGER = Zanna.Data.Serialize.Detect(unknown)
PRINT Zanna.Data.Serialize.FormatName(detected)  ' Output: "json"

DIM autoParsed AS OBJECT = Zanna.Data.Serialize.AutoParseResult(unknown)
IF autoParsed.IsErr THEN
    PRINT "AutoParse error: "; autoParsed.UnwrapErrStr()
    END
END IF

' Pretty-print as JSON
DIM pretty AS STRING = Zanna.Data.Serialize.FormatPretty(data, 0, 2)
PRINT pretty

' Validate before parsing
DIM userInput AS STRING = "{""enabled"": true}"
IF Zanna.Data.Serialize.IsValid(userInput, 0) THEN
    DIM safeResult AS OBJECT = Zanna.Data.Serialize.ParseResult(userInput, 0)
    IF safeResult.IsOk THEN
        DIM safe AS OBJECT = safeResult.Unwrap()
    END IF
END IF
```

### Use Cases

- **Format conversion:** Convert config files between JSON/YAML/TOML
- **API flexibility:** Accept data in any supported format
- **Auto-detection:** Process files without knowing their format in advance
- **Validation:** Check input format before processing
- **Projection:** Parse → modify → reformat data when the supported models overlap

---

## Zanna.Data.Xml

Mutable node tree for a practical subset of
[XML 1.0 Fifth Edition](https://www.w3.org/TR/xml/). It supports parsing, navigation, attributes,
child manipulation, and simple slash-path queries. All node values are opaque objects.

**Type:** Static utility class

### Parsing and Validation

| Method             | Signature                | Description                                     |
|--------------------|--------------------------|--------------------------------------------------|
| `Parse(xml)`       | `Object(String)`         | Parse an XML string; returns document node or NULL |
| `ParseResult(xml)` | `Result(String)`         | Parse as `Ok(document)` or `Err(message)`       |
| `IsValid(xml)`     | `Boolean(String)`        | True if the runtime's XML subset accepts the string |

### Node Creation

| Method              | Signature                | Description                              |
|---------------------|--------------------------|------------------------------------------|
| `Element(tag)`      | `Object(String)`         | Create a new element node with a tag     |
| `Text(content)`     | `Object(String)`         | Create a text node                       |
| `Comment(text)`     | `Object(String)`         | Create a comment node                    |
| `Cdata(text)`       | `Object(String)`         | Create a CDATA section node              |

### Node Information

| Method               | Signature                | Description                                         |
|----------------------|--------------------------|-----------------------------------------------------|
| `NodeType(n)`        | `Integer(Object)`        | Node type constant: element=1, text=2, comment=3, cdata=4, document=5 |
| `Tag(n)`             | `String(Object)`         | Tag name (element nodes only)                       |
| `Content(n)`         | `String(Object)`         | Raw text content of a text/comment/cdata node       |
| `TextContent(n)`     | `String(Object)`         | All descendant text concatenated (recursive)        |

### Attributes

| Method                    | Signature                         | Description                               |
|---------------------------|-----------------------------------|-------------------------------------------|
| `Attr(n, name)`           | `String(Object, String)`          | Attribute value (empty string if absent)  |
| `HasAttr(n, name)`        | `Boolean(Object, String)`         | True if attribute exists                  |
| `SetAttr(n, name, value)` | `Void(Object, String, String)`    | Set or add attribute                      |
| `RemoveAttr(n, name)`     | `Boolean(Object, String)`         | Remove attribute; true if present         |
| `AttrNames(n)`            | `Object(Object)`                  | Return a `Seq` of attribute name strings  |

### Navigation

| Method                  | Signature                    | Description                                        |
|-------------------------|------------------------------|----------------------------------------------------|
| `Children(n)`           | `Object(Object)`             | Return a `Seq` containing all child nodes          |
| `ChildCount(n)`         | `Integer(Object)`            | Number of child nodes                              |
| `ChildAt(n, i)`         | `Object(Object, Integer)`    | Child at index i (0-based)                         |
| `Child(n, tag)`         | `Object(Object, String)`     | First child element with given tag, or NULL        |
| `ChildrenByTag(n, tag)` | `Object(Object, String)`     | Return a `Seq` of child elements with given tag    |
| `Parent(n)`             | `Object(Object)`             | Parent node, or NULL for a detached/document node  |
| `Root(n)`               | `Object(Object)`             | Document element starting from any node            |

### Search

| Method            | Signature                | Description                                             |
|-------------------|--------------------------|---------------------------------------------------------|
| `Find(n, path)`   | `Object(Object, String)` | Find first node matching a simple path (e.g. "a/b/c")  |
| `FindAll(n, path)`| `Object(Object, String)` | Return a `Seq` of all nodes matching the path            |

### Mutation

| Method                    | Signature                        | Description                       |
|---------------------------|----------------------------------|-----------------------------------|
| `Append(n, child)`        | `Void(Object, Object)`           | Append child to node              |
| `Insert(n, i, child)`     | `Void(Object, Integer, Object)`  | Insert child at index i           |
| `Remove(n, child)`        | `Boolean(Object, Object)`        | Remove a child; true if it was present |
| `RemoveAt(n, i)`          | `Void(Object, Integer)`          | Remove child at index i           |
| `SetText(n, text)`        | `Void(Object, String)`           | Replace all children with a single text node |

### Serialization

| Method                    | Signature                         | Description                                     |
|---------------------------|-----------------------------------|-------------------------------------------------|
| `Format(n)`               | `String(Object)`                  | Compact XML string                              |
| `FormatPretty(n, indent)` | `String(Object, Integer)`         | Indented XML string (spaces per level)          |
| `Escape(s)`               | `String(String)`                  | Escape XML special characters (`<>&"'`)         |
| `Unescape(s)`             | `String(String)`                  | Unescape XML entities                           |

### Notes

- Prefer `ParseResult` for user-provided XML; it returns `Ok(document)` or `Err(message)`.
- `Parse` returns a document node on success or NULL on error, with no accompanying message. Use
  `Root(doc)` to get the document element.
- `Find` returns NULL when no node matches the path.
- Parsing supports elements, attributes, text, comments, CDATA, processing instructions, and
  DOCTYPE declarations. Processing instructions and DOCTYPE contents are validated only for a
  terminator and are discarded rather than represented in the node tree.
- XML parsing enforces one document element, matching closing tags, its byte-oriented XML-name
  rules, unique attributes, legal control characters, and recognized entity references.
- Only the five predefined named entities and numeric character references are decoded. General
  entities declared by a DTD are not expanded, so some well-formed XML 1.0 documents are rejected.
  The parser also does not validate DTDs, namespaces, or UTF-8 byte sequences as a full XML
  processor would; `IsValid` therefore checks acceptance by this practical subset, not XML 1.0
  conformance.
- Formatting a parsed tree cannot reproduce discarded declarations, processing instructions, or
  DOCTYPE content.
- Element, attribute, comment, and CDATA creation/mutation validate the runtime's name/content
  rules and report errors instead of creating malformed trees for ordinary NUL-free inputs.
  Embedded NUL in a programmatically supplied name is an open exception; see
  [VDOC-032](../../../misc/reviews/documentation-review-findings.md#vdoc-032--xml-name-validation-stops-at-an-embedded-nul).
- Path syntax for `Find`/`FindAll`: slash-separated tag names from the given node or its direct children (e.g. `"books/book/title"`). A path without `/` remains a recursive tag search.
- Attribute and child mutations are performed in-place on the node object.
- `Append` and `Insert` reject non-node children, document children, cycles, and children that already have a parent.
- `Append` and `Insert` retain the child; callers may keep using the child handle or release their own reference after insertion.
- `TextContent` is useful for extracting all readable text from a subtree.
- `AttrNames`, `Children`, `ChildrenByTag`, and `FindAll` are registered as typed sequences
  (`seq<str>` for names, `seq<obj>` for nodes), so member access resolves directly.

### Zia Example

```zia
module XmlDemo;

bind Zanna.Terminal;
bind Zanna.Data.Xml as Xml;

func start() {
    var src = "<catalog><book id=\"1\"><title>Zanna Primer</title><price>29.99</price></book></catalog>";
    var parsed = Xml.ParseResult(src);
    if parsed.IsErr {
        Say("Parse error: " + parsed.UnwrapErrStr());
        return;
    }
    var doc = parsed.Unwrap();
    var root = Xml.Root(doc);

    var book = Xml.Child(root, "book");
    Say("ID: " + Xml.Attr(book, "id"));                     // 1
    Say("Title: " + Xml.TextContent(Xml.Child(book, "title")));  // Zanna Primer

    // Build a new node and append it
    var book2 = Xml.Element("book");
    Xml.SetAttr(book2, "id", "2");
    var t2 = Xml.Element("title");
    Xml.SetText(t2, "Advanced Zanna");
    Xml.Append(book2, t2);
    Xml.Append(root, book2);

    Say("Children: " + Xml.Format(root));
    Say("Pretty:\n" + Xml.FormatPretty(root, 2));

    // Query all books
    var books = Xml.ChildrenByTag(root, "book");
    Say("Count: " + Xml.ChildCount(root));  // 2
}
```

### BASIC Example

```basic
DIM src AS STRING = "<config><host>localhost</host><port>8080</port></config>"
DIM parsed AS OBJECT = Zanna.Data.Xml.ParseResult(src)

IF parsed.IsErr THEN
    PRINT "Parse error: "; parsed.UnwrapErrStr()
    END
END IF

DIM doc AS OBJECT = parsed.Unwrap()
DIM root AS OBJECT = Zanna.Data.Xml.Root(doc)
DIM host AS STRING = Zanna.Data.Xml.TextContent(Zanna.Data.Xml.Child(root, "host"))
DIM port AS STRING = Zanna.Data.Xml.TextContent(Zanna.Data.Xml.Child(root, "port"))
PRINT "Host: "; host   ' localhost
PRINT "Port: "; port   ' 8080

' Add a new element
DIM debug AS OBJECT = Zanna.Data.Xml.Element("debug")
Zanna.Data.Xml.SetText(debug, "true")
Zanna.Data.Xml.Append(root, debug)

PRINT Zanna.Data.Xml.FormatPretty(root, 2)
' <config>
'   <host>localhost</host>
'   <port>8080</port>
'   <debug>true</debug>
' </config>

' Attribute access
DIM items AS STRING = "<items><item key=""a"" val=""1""/><item key=""b"" val=""2""/></items>"
DIM irResult AS OBJECT = Zanna.Data.Xml.ParseResult(items)
DIM ir AS OBJECT = irResult.Unwrap()
DIM allItems AS Zanna.Collections.Seq = Zanna.Data.Xml.ChildrenByTag(Zanna.Data.Xml.Root(ir), "item")
FOR i = 0 TO allItems.Count - 1
    DIM it AS OBJECT = allItems.Get(i)
    PRINT Zanna.Data.Xml.Attr(it, "key"); "="; Zanna.Data.Xml.Attr(it, "val")
NEXT
```

---

## Zanna.Data.Yaml

Parser and formatter for a practical subset of the
[YAML 1.2.2](https://yaml.org/spec/1.2.2/) core schema. It converts supported YAML text to/from
Zanna strings, integers, doubles, booleans, NULL, Maps, and Seqs.

**Type:** Static utility class

### Methods

| Method                    | Signature                        | Description                                           |
|---------------------------|----------------------------------|-------------------------------------------------------|
| `Parse(yaml)`             | `Object(String)`                 | Parse YAML string; returns value or NULL on error     |
| `ParseResult(yaml)`       | `Result(String)`                 | Parse as `Ok(value)` or `Err(message)`                |
| `IsValid(yaml)`           | `Boolean(String)`                | True if the runtime's YAML subset accepts the string  |
| `Format(obj)`             | `String(Object)`                 | Format a value as block YAML with two-space indentation |
| `FormatIndent(obj, spaces)` | `String(Object, Integer)`      | Format with given indentation level                   |
| `TypeOf(obj)`             | `String(Object)`                 | Value type string (see below)                         |

### TypeOf Return Values

| String        | Description                                   |
|---------------|-----------------------------------------------|
| `"null"`      | YAML null / missing value                     |
| `"bool"`      | Boolean (true/false)                          |
| `"int"`       | Integer scalar                                |
| `"float"`     | Floating-point scalar                         |
| `"string"`    | String scalar                                 |
| `"sequence"`  | Ordered list (YAML sequence)                  |
| `"mapping"`   | Key-value pairs (YAML mapping)                |
| `"unknown"`   | Unsupported runtime object type               |

### Notes

- Prefer `ParseResult` for user-provided YAML; it distinguishes valid YAML null (`Ok(NULL)`) from parse failure (`Err(message)`).
- `Parse` returns NULL both for valid YAML null and for parse failure, so use `ParseResult` when NULL is ambiguous.
- The returned object uses the same representation as `Zanna.Data.Json.Parse` — use `Map`, `Seq`, and scalar
  accessors to traverse the parsed value.
- Explicit multi-document YAML streams separated by `---` parse as a sequence of documents.
- Empty input is valid and maps to YAML null.
- Anchors, aliases, custom tags, merge keys, directives, and the full YAML schema system are not
  implemented. Anchor/alias syntax (`&name`, `*name` at the start of an unquoted token) is
  rejected with an explicit parse error rather than misparsed; quoted or mid-token `&`/`*`
  characters remain ordinary string content.
- Block and flow collections (`[]` and `{}`), quoted keys, whole-line/after-scalar comments,
  literal/folded block scalars, and common quoted-string escapes are supported.
- `Format` uses block-style collections and quotes ambiguous string scalars. `FormatIndent` uses
  the requested width from 1 through 8, changes values below 1 to 2, and clamps values above 8.
- The formatter is not lossless for every runtime scalar: `%g` formatting can round doubles, and
  string/key formatting stops at an embedded `NUL`. See
  [VDOC-027](../../../misc/reviews/documentation-review-findings.md#vdoc-027--yamlformat-can-lose-double-precision-and-string-bytes).
- A plain scalar with an embedded `NUL` can be classified from only its numeric prefix while the
  remaining bytes are ignored—for example, `1\0garbage` is accepted as integer `1`. See
  [VDOC-039](../../../misc/reviews/documentation-review-findings.md#vdoc-039--yaml-numeric-scalar-parsing-ignores-bytes-after-an-embedded-nul).
- The parser validates the input is well-formed UTF-8 before parsing; malformed byte sequences
  make `Parse` return NULL and `IsValid` report false.
- Finite double formatting depends on the process numeric locale when the runtime is embedded
  without the VM's locale initialization; see
  [VDOC-041](../../../misc/reviews/documentation-review-findings.md#vdoc-041--toml-and-yaml-numeric-emission-is-locale-sensitive).
- Formatting depth is bounded (200 levels, matching the parser); cyclic or deeper containers fail
  closed with an empty string instead of recursing without bound.
- YAML scalars with no explicit type tag are auto-typed for null, numbers, and YAML 1.2 booleans (`true`/`false`); legacy YAML 1.1 words such as `yes`, `no`, `on`, and `off` remain strings.
- Decimal, hexadecimal (`0x`), and octal (`0o`) integers are recognized. `.inf`, `-.inf`, and
  `.nan` are floating-point values; bare C spellings such as `inf` and `nan` remain strings.
- Tabs in indentation, unexpected over-indentation, invalid quoted-string escapes, and malformed flow collections are rejected.

### Zia Example

```zia
module YamlDemo;

bind Zanna.Terminal;
bind Zanna.Data.Yaml as Yaml;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var src = "name: Alice\nage: 30\nscores:\n  - 95\n  - 87\n  - 92\n";
    var parsed = Yaml.ParseResult(src);
    if parsed.IsErr {
        Say("YAML error: " + parsed.UnwrapErrStr());
        return;
    }
    var obj = parsed.Unwrap();

    Say("Type: " + Yaml.TypeOf(obj));  // mapping

    // Navigate using Map accessors (same as Json object)
    // Re-format as YAML
    Say(Yaml.FormatIndent(obj, 2));

    // Check validity
    Say("Valid: " + Fmt.Bool(Yaml.IsValid(src)));  // true
    Say("Invalid: " + Fmt.Bool(Yaml.IsValid("{")));  // false
}
```

### BASIC Example

```basic
DIM src AS STRING = "server:" & Chr(10) & _
                    "  host: localhost" & Chr(10) & _
                    "  port: 9090" & Chr(10) & _
                    "  tls: false" & Chr(10)

DIM parsed AS OBJECT = Zanna.Data.Yaml.ParseResult(src)

IF parsed.IsErr THEN
    PRINT "YAML error: "; parsed.UnwrapErrStr()
    END
END IF

DIM cfg AS OBJECT = parsed.Unwrap()
PRINT "Type: "; Zanna.Data.Yaml.TypeOf(cfg)   ' mapping

' Re-format as compact YAML
PRINT Zanna.Data.Yaml.Format(cfg)

' Re-format with 4-space indent
PRINT Zanna.Data.Yaml.FormatIndent(cfg, 4)

' Validate before processing user input
DIM userYaml AS STRING = "key: value"
IF Zanna.Data.Yaml.IsValid(userYaml) THEN
    DIM userParsed AS OBJECT = Zanna.Data.Yaml.ParseResult(userYaml)
    IF userParsed.IsOk THEN
        DIM value AS OBJECT = userParsed.Unwrap()
        PRINT Zanna.Data.Yaml.TypeOf(value)  ' mapping
    END IF
END IF
```

---


## See Also

- [Encoding & Identity](encoding.md)
- [Pattern Matching](patterns.md)
- [Formatting & Generation](formatting.md)
- [Text Processing Overview](README.md)
- [Zanna Runtime Library](../README.md)
