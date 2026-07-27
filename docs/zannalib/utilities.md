---
status: active
audience: public
last-verified: 2026-07-26
---

# Utilities

> Conversion, parsing, formatting, and logging utilities.

**Part of the [Zanna Runtime Library](README.md)**

## Contents

- [Zanna.Core.Convert](#zannacoreconvert)
- [Zanna.Text.Fmt](#zannatextfmt)
- [Zanna.Diagnostics.Log](#zannadiagnosticslog)
- [Zanna.Core.Parse](#zannacoreparse)

---

## Zanna.Core.Convert

Type conversion utilities.

**Type:** Static utility class
**Runtime namespace:** `Zanna.Core.Convert`

### Methods

| Method                   | Signature         | Description                                      |
|--------------------------|-------------------|--------------------------------------------------|
| `ToInt64(text)`          | `Integer(String)` | Parses a string as a 64-bit integer              |
| `ToDouble(text)`         | `Double(String)`  | Parses a string as a double-precision float      |
| `NumToInt(value)`        | `Integer(Double)` | Converts a floating-point value to integer (truncates) |
| `ToStringInt(value)`     | `String(Integer)` | Converts an integer to its string representation |
| `ToStringDouble(value)`  | `String(Double)`  | Converts a double to its string representation   |

### Conversion Rules

- `ToInt64` parses signed decimal integer text with optional leading/trailing ASCII whitespace.
- `ToDouble` parses locale-independent decimal floating-point text with `.` as the decimal separator. It also accepts case-insensitive `NaN` and `Inf` with an optional sign. C-style hexadecimal floats such as `0x1p4`, overflow, embedded NUL bytes, and non-whitespace trailing characters are rejected.
- `ToStringDouble` uses locale-independent round-trip precision for finite values, and emits `NaN`, `Inf`, or `-Inf` for non-finite values, so text it emits can be parsed back through `ToDouble`.
- `ToString_Int` and `ToString_Double` remain available as compatibility aliases.
- Embedded NUL bytes and non-whitespace trailing characters are rejected.
- `NumToInt` truncates finite values toward zero. `NaN` converts to `0`; values outside the signed 64-bit range clamp to the nearest endpoint.

### Zia Example

```zia
module ConvertDemo;

bind Zanna.Terminal;
bind Zanna.Core.Convert as Convert;

func start() {
    // Parse string to integer
    var n = Convert.ToInt64("12345");
    Say("Parsed: " + Convert.ToStringInt(n + 1));   // Output: Parsed: 12346

    // Parse string to double
    var d = Convert.ToDouble("3.14159");
    Say("Pi: " + Convert.ToStringDouble(d));         // Output: Pi: 3.14159

    // Convert back to string
    Say(Convert.ToStringInt(42));                    // Output: 42
    Say(Convert.ToStringDouble(2.5));                // Output: 2.5
}
```

### BASIC Example

```basic
DIM s AS STRING
s = "12345"

DIM n AS INTEGER
n = Zanna.Core.Convert.ToInt64(s)
PRINT n + 1  ' Output: 12346

DIM d AS DOUBLE
d = Zanna.Core.Convert.ToDouble("3.14159")
PRINT d * 2  ' Output: 6.28318

' Convert back to string
PRINT Zanna.Core.Convert.ToStringInt(42)       ' Output: "42"
PRINT Zanna.Core.Convert.ToStringDouble(2.5)   ' Output: "2.5"
```

---

## Zanna.Text.Fmt

String formatting utilities for converting values to formatted strings.

**Type:** Static utility class

### Methods

| Method                      | Signature                          | Description                              |
|-----------------------------|------------------------------------|------------------------------------------|
| `Int(value)`                | `String(Integer)`                  | Format integer as decimal string         |
| `IntRadix(value, radix)`    | `String(Integer, Integer)`         | Format integer in specified radix (2-36) |
| `IntPad(value, width, pad)` | `String(Integer, Integer, String)` | Format integer with padding              |
| `Num(value)`                | `String(Double)`                   | Format number for display (15 significant digits) |
| `NumFixed(value, decimals)` | `String(Double, Integer)`          | Format number with fixed decimal places  |
| `Scientific(value, decimals)` | `String(Double, Integer)`        | Format number in scientific notation     |
| `Percent(value, decimals)`    | `String(Double, Integer)`        | Format number as percentage              |
| `Bool(value)`               | `String(Boolean)`                  | Format as "true" or "false"              |
| `YesNo(value)`              | `String(Boolean)`                  | Format as "yes" or "no"                  |
| `SizeBytes(bytes)`          | `String(Integer)`                  | Format byte count as human-readable size |
| `Hex(value)`                | `String(Integer)`                  | Format as lowercase hex                  |
| `HexPad(value, width)`      | `String(Integer, Integer)`         | Format as hex with zero-padding          |
| `Bin(value)`                | `String(Integer)`                  | Format as binary                         |
| `Oct(value)`                | `String(Integer)`                  | Format as octal                          |
| `IntGrouped(value, sep)`    | `String(Integer, String)`          | Format integer with thousands separator  |
| `Currency(value, dec, sym)` | `String(Double, Integer, String)`  | Format as currency with decimals and symbol |
| `ToWords(value)`            | `String(Integer)`                  | Convert integer to English words         |
| `Ordinal(value)`            | `String(Integer)`                  | Convert integer to ordinal (1st, 2nd, 3rd...) |

### Formatting Rules

- Numeric formatting is locale-stable: decimal output uses `.` even when the process locale uses another separator.
- `Num` is the historical display formatter: it uses 15 significant digits and is not guaranteed
  to preserve the exact IEEE-754 value when reparsed. Use `Zanna.Core.Convert.ToStringDouble()`
  when exact round-trip text is required.
- Signed zero formats as zero in `Num`, `NumFixed`, `Scientific`, `Percent`, and `Currency`.
- `Scientific`, `Percent`, and `YesNo` remain available as compatibility aliases.
- `NumFixed`, `Scientific`, `Percent`, and `Currency` clamp requested decimal places to 0-20.
- `IntRadix` returns an empty string outside radix 2-36. Decimal negatives use a minus sign;
  non-decimal radices, `Hex`, `HexPad`, `Bin`, and `Oct` format the unsigned 64-bit bit pattern.
- `HexPad` clamps width to 1-16 digits.
- `IntPad` defaults to space padding when the pad string is null, empty, or does not start with a valid UTF-8 character. It uses the first full UTF-8 character from valid pad strings and supports widths beyond 255 subject to allocation limits.
- `Size` chooses units using integer byte thresholds, then promotes near-boundary rounded displays so output does not show values such as `1024.0 PB`.
- `IntGrouped` preserves the full separator byte sequence and defaults to `","` only when the separator is null or empty.
- `Currency` preserves the full symbol byte sequence, including embedded NUL bytes. Null symbols default to `"$"`; empty symbols are emitted as empty.
- `Currency` returns `NaN`, `Infinity`, or `-Infinity` for non-finite inputs. Large finite values are formatted without integer casts, avoiding overflow.
- `ToWords` supports the full signed 64-bit integer range, including quadrillions and quintillions.

### Zia Example

```zia
module FmtDemo;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Integer formatting
    Say("Int: " + Fmt.Int(12345));           // Output: Int: 12345
    Say("Hex: " + Fmt.Hex(255));             // Output: Hex: ff
    Say("HexPad: " + Fmt.HexPad(255, 4));   // Output: HexPad: 00ff
    Say("Bin: " + Fmt.Bin(10));              // Output: Bin: 1010
    Say("Oct: " + Fmt.Oct(64));              // Output: Oct: 100
    Say("Padded: " + Fmt.IntPad(42, 5, "0"));  // Output: Padded: 00042

    // Number formatting
    Say("Fixed: " + Fmt.NumFixed(3.14159, 2));  // Output: Fixed: 3.14

    // Boolean formatting
    Say("Bool: " + Fmt.Bool(true));          // Output: Bool: true
    Say("YesNo: " + Fmt.YesNo(false));       // Output: YesNo: no

    // Size formatting
    Say("Size: " + Fmt.SizeBytes(1048576));       // Output: Size: 1.0 MB
}
```

### BASIC Example

```basic
' Integer formatting
PRINT Zanna.Text.Fmt.Int(12345)              ' Output: "12345"
PRINT Zanna.Text.Fmt.IntRadix(255, 16)       ' Output: "ff"
PRINT Zanna.Text.Fmt.IntPad(42, 5, "0")      ' Output: "00042"

' Number formatting
PRINT Zanna.Text.Fmt.Num(2.5)                ' Output: "2.5"
PRINT Zanna.Text.Fmt.NumFixed(3.14159, 2)    ' Output: "3.14"
PRINT Zanna.Text.Fmt.Scientific(1234.5, 2)   ' Output: "1.23e+03"
PRINT Zanna.Text.Fmt.Percent(0.756, 1)       ' Output: "75.6%"

' Boolean formatting
PRINT Zanna.Text.Fmt.Bool(TRUE)              ' Output: "true"
PRINT Zanna.Text.Fmt.YesNo(FALSE)            ' Output: "no"

' Size formatting (auto-scales to KB, MB, GB, etc.)
PRINT Zanna.Text.Fmt.SizeBytes(1024)              ' Output: "1.0 KB"
PRINT Zanna.Text.Fmt.SizeBytes(1048576)           ' Output: "1.0 MB"
PRINT Zanna.Text.Fmt.SizeBytes(1234567890)        ' Output: "1.1 GB"

' Radix formatting
PRINT Zanna.Text.Fmt.Hex(255)                ' Output: "ff"
PRINT Zanna.Text.Fmt.HexPad(255, 4)          ' Output: "00ff"
PRINT Zanna.Text.Fmt.Bin(10)                 ' Output: "1010"
PRINT Zanna.Text.Fmt.Oct(64)                 ' Output: "100"
```

---

## Zanna.Diagnostics.Log

Structured logging with configurable log levels.

**Type:** Static utility class

### Log Levels

| Constant | Value | Description                    |
|----------|-------|--------------------------------|
| `LevelDebug` | 0 | Detailed debugging information |
| `LevelInfo`  | 1 | General informational messages |
| `LevelWarn`  | 2 | Warning conditions             |
| `LevelError` | 3 | Error conditions               |
| `LevelOff`   | 4 | Disable all logging            |

### Properties

| Property | Type                   | Description                                                    |
|----------|------------------------|----------------------------------------------------------------|
| `Level`  | `Integer` (read/write) | Current minimum log level (messages below this are suppressed) |

### Methods

| Method           | Signature          | Description                     |
|------------------|--------------------|---------------------------------|
| `Debug(message)` | `Void(String)`     | Log a debug message             |
| `Info(message)`  | `Void(String)`     | Log an info message             |
| `Warn(message)`  | `Void(String)`     | Log a warning message           |
| `Error(message)` | `Void(String)`     | Log an error message            |
| `Enabled(level)` | `Boolean(Integer)` | Check if a log level is enabled |

### Notes

- Default log level is `INFO` (debug messages suppressed)
- Log output goes to stderr with a local-time timestamp and level prefix
- Format: `[LEVEL] YYYY-MM-DD HH:MM:SS message`
- Message output is length-aware. Embedded NUL bytes and control characters such as newline, carriage return, and tab are escaped so one log call produces one physical log line, even when multiple threads log concurrently. If heap allocation for the full line fails, the logger falls back to serialized streaming instead of dropping the message.
- Log level reads and writes are atomic and safe to call concurrently.
- Assigning `Level` clamps values below 0 to `LevelDebug` and values above 4 to `LevelOff`.
- Set `Level = Zanna.Diagnostics.Log.LevelOff` to disable all logging

### Zia Example

```zia
module LogDemo;

bind Zanna.Terminal;
bind Zanna.Diagnostics.Log;

func start() {
    // Log at various levels (output goes to stderr)
    Info("Application started");
    Warn("Configuration file not found, using defaults");
    Error("Failed to connect to database");

    // Debug messages are suppressed by default (level < INFO)
    Debug("This won't appear");

    Say("Log demo complete");
}
```

### BASIC Example

```basic
' Basic logging
Zanna.Diagnostics.Log.Info("Application started")
Zanna.Diagnostics.Log.Warn("Configuration file not found, using defaults")
Zanna.Diagnostics.Log.Error("Failed to connect to database")

' Debug logging (suppressed by default)
Zanna.Diagnostics.Log.Debug("Entering processing function")

' Enable debug logging
Zanna.Diagnostics.Log.Level = Zanna.Diagnostics.Log.LevelDebug
Zanna.Diagnostics.Log.Debug("Now this message appears")

DIM userCount AS INTEGER = 42

' Check if level is enabled before expensive formatting
IF Zanna.Diagnostics.Log.Enabled(Zanna.Diagnostics.Log.LevelDebug) THEN
    Zanna.Diagnostics.Log.Debug("Active count: " + Zanna.Text.Fmt.Int(userCount))
END IF

' Suppress all logging
Zanna.Diagnostics.Log.Level = Zanna.Diagnostics.Log.LevelOff
```

---

## Zanna.Core.Parse

Safe parsing utilities with error handling. Unlike `Zanna.Core.Convert` which traps on invalid input, these methods allow
graceful error handling.

**Type:** Static utility class
**Runtime namespace:** `Zanna.Core.Parse`

### Methods

| Method                           | Signature                           | Description                               |
|----------------------------------|-------------------------------------|-------------------------------------------|
| `TryInt(text)`                   | `Option<Integer>(String)`           | Parse integer or return `None`       |
| `TryDouble(text)`                | `Option<Double>(String)`            | Parse double or return `None`        |
| `TryBool(text)`                  | `Option<Boolean>(String)`           | Parse boolean or return `None`       |
| `IntOr(text, default)`           | `Integer(String, Integer)`          | Parse integer or return default           |
| `DoubleOr(text, default)`        | `Double(String, Double)`            | Parse double or return default            |
| `BoolOr(text, default)`          | `Boolean(String, Boolean)`          | Parse boolean or return default           |
| `IsInt(text)`                    | `Boolean(String)`                   | Check if string is a valid integer        |
| `IsNum(text)`                    | `Boolean(String)`                   | Check if string is a valid number         |
| `IntRadix(text, radix, default)` | `Integer(String, Integer, Integer)` | Parse integer in given radix with default |

### Notes

- The `Try*` parser forms return managed `Option`
  objects. Raw output-pointer helpers are runtime-internal and are not part of
  the Zia or BASIC source surface.
- Use `TryDouble` and `DoubleOr` for double parsing; the former `TryNum` / `NumOr`
  spellings were retired by the public-surface standardization and are no longer
  available.
- **Boolean parsing** accepts (case-insensitive):
    - True: `"true"`, `"yes"`, `"1"`, `"on"`
    - False: `"false"`, `"no"`, `"0"`, `"off"`
- `IsInt` and `IsNum` accept optional leading `+` or `-`
- Null input is treated as parse failure: `Try*` returns `None`, `Is*` returns false, and `*Or`/`IntRadix` returns the supplied default.
- `IntRadix` supports radix 2-36 (digits 0-9, letters a-z). Radix 10 accepts leading `+` and `-`; other radices parse unsigned 64-bit bit patterns so `Zanna.Text.Fmt.Hex(-1)` and `Zanna.Text.Fmt.Bin(-1)` round-trip to `-1`. Prefixes such as `0x` and non-decimal signs are rejected.
- Leading/trailing whitespace is trimmed before parsing
- Numeric parsing is locale-independent, uses `.` as the decimal separator, and accepts strict
  decimal float syntax plus case-insensitive `NaN` and optionally signed `Inf`. C-style
  hexadecimal floats such as `0x1p4` and the display spelling `Infinity` are rejected.
- Embedded NUL bytes are rejected, even when the bytes before the NUL form a valid value.
- Internal C helpers still clear their output slots before returning failure, but frontend code should use the managed `Option` APIs.

### Zia Example

```zia
module ParseDemo;

bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Parse with default fallback
    var port = Zanna.Core.Parse.IntOr("8080", 3000);
    Say("Port: " + Fmt.Int(port));             // Output: Port: 8080

    var timeout = Zanna.Core.Parse.DoubleOr("invalid", 30.0);
    Say("Timeout: " + Fmt.Num(timeout));       // Output: Timeout: 30

    // Validate input
    if Zanna.Core.Parse.IsInt("42") {
        Say("42 is a valid integer");          // Output: 42 is a valid integer
    }
    if !Zanna.Core.Parse.IsInt("hello") {
        Say("hello is not a valid integer");   // Output: hello is not a valid integer
    }

    // Boolean parsing
    var enabled = Zanna.Core.Parse.BoolOr("yes", false);
    Say("Enabled: " + Fmt.Bool(enabled));      // Output: Enabled: true

    // Hex parsing with default
    var color = Zanna.Core.Parse.IntRadix("ff", 16, 0);
    Say("Color: " + Fmt.Int(color));           // Output: Color: 255
}
```

### BASIC Example

```basic
' Parse with default fallback
DIM portStr AS STRING = "8080"
DIM timeoutStr AS STRING = "invalid"
DIM verboseStr AS STRING = "yes"
DIM userInput AS STRING = "42"

DIM port AS INTEGER = Zanna.Core.Parse.IntOr(portStr, 8080)
DIM timeout AS DOUBLE = Zanna.Core.Parse.DoubleOr(timeoutStr, 30.0)
DIM verbose AS INTEGER = Zanna.Core.Parse.BoolOr(verboseStr, FALSE)

' Validate before use
IF Zanna.Core.Parse.IsInt(userInput) THEN
    DIM value AS INTEGER = Zanna.Core.Convert.ToInt64(userInput)
    PRINT "Valid integer: "; value
ELSE
    PRINT "Invalid input"
END IF

' Parse hex value with default
DIM color AS INTEGER = Zanna.Core.Parse.IntRadix("ff0000", 16, 0)

' Parse boolean from config
DIM enabled AS INTEGER = Zanna.Core.Parse.BoolOr("yes", FALSE)
PRINT enabled  ' Output: 1 (true)
```

### Comparison with Zanna.Core.Convert

| Scenario                      | Use               |
|-------------------------------|-------------------|
| Trusted input (internal data) | `Zanna.Core.Convert`   |
| User input (may be invalid)   | `Zanna.Core.Parse`     |
| Need default value            | `Zanna.Core.Parse.*Or` |
| Need validation only          | `Zanna.Core.Parse.Is*` |

---

## See Also

- [Core Types](core.md) - `String` manipulation methods
- [Text Processing](text/README.md) - `Codec` for encoding, `StringBuilder` for building strings
