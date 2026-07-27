---
status: active
audience: public
last-verified: 2026-07-26
---

# Encoding & Identity
> Codec (Base64, Hex, URL encoding), Uuid

**Part of [Zanna Runtime Library](../README.md) › [Text Processing](README.md)**

---

## Zanna.Text.Codec

String-based encoding and decoding utilities for Base64, Hex, and URL encoding.

**Type:** Static utility class

### Methods

| Method           | Signature        | Description                              |
|------------------|------------------|------------------------------------------|
| `Base64Encode(str)` | `String(String)` | Base64-encode a string's bytes           |
| `Base64Decode(str)` | `String(String)` | Decode a Base64 string to original bytes |
| `HexEncode(str)`    | `String(String)` | Hex-encode a string's bytes (lowercase)  |
| `HexDecode(str)`    | `String(String)` | Decode a hex string to original bytes    |
| `UrlEncode(str)` | `String(String)` | URL-encode a string (percent-encoding)   |
| `UrlDecode(str)` | `String(String)` | URL-decode a string                      |

### Notes

- All methods operate on the runtime string byte length, so embedded `NUL` bytes are preserved
- For arbitrary binary buffers, `Bytes.ToBase64`/`Bytes.FromBase64` and `Bytes.ToHex`/`Bytes.FromHex` remain the preferred APIs
- **URL Encoding:**
    - Unreserved characters (A-Z, a-z, 0-9, `-`, `_`, `.`, `~`) pass through unchanged
    - All other characters are encoded as `%XX` (lowercase hex)
    - Decoding treats `+` as space (form encoding convention)
    - Invalid or incomplete `%XX` escapes are left unchanged
- **Base64:** RFC 4648 standard alphabet with `=` padding
- **Hex:** Encoding emits lowercase digits; decoding accepts uppercase or lowercase input
- Invalid input to `Base64Decode` or `HexDecode` will trap

### Zia Example

```zia
module CodecDemo;

bind Zanna.Terminal;
bind Zanna.Text.Codec as Codec;

func start() {
    Say("Base64: " + Codec.Base64Encode("Hello"));        // SGVsbG8=
    Say("Decoded: " + Codec.Base64Decode("SGVsbG8="));     // Hello
    Say("Hex: " + Codec.HexEncode("Hello"));               // 48656c6c6f
    Say("HexDec: " + Codec.HexDecode("48656c6c6f"));       // Hello
    Say("UrlEnc: " + Codec.UrlEncode("hello world"));   // hello%20world
    Say("UrlDec: " + Codec.UrlDecode("hello%20world")); // hello world
}
```

### BASIC Example

```basic
' URL encoding for query parameters
DIM original AS STRING = "key=value&name=John Doe"
DIM encoded AS STRING = Zanna.Text.Codec.UrlEncode(original)
PRINT encoded  ' Output: "key%3dvalue%26name%3dJohn%20Doe"

DIM decoded AS STRING = Zanna.Text.Codec.UrlDecode(encoded)
PRINT decoded = original  ' Output: 1 (true)

' Base64 encoding for data transmission
DIM data AS STRING = "Hello, World!"
DIM b64 AS STRING = Zanna.Text.Codec.Base64Encode(data)
PRINT b64  ' Output: "SGVsbG8sIFdvcmxkIQ=="

DIM restored AS STRING = Zanna.Text.Codec.Base64Decode(b64)
PRINT restored  ' Output: "Hello, World!"

' Hex encoding for display
DIM hex AS STRING = Zanna.Text.Codec.HexEncode("ABC")
PRINT hex  ' Output: "414243"

DIM unhex AS STRING = Zanna.Text.Codec.HexDecode(hex)
PRINT unhex  ' Output: "ABC"
```

---

## Zanna.Text.Uuid

UUID version 4 (random) generation and canonical byte/string conversion. The layout follows
[RFC 9562](https://www.rfc-editor.org/rfc/rfc9562.html), which supersedes RFC 4122 while retaining
the UUIDv4 representation used here.

**Type:** Static utility class

### Properties

| Property | Type   | Description                                                 |
|----------|--------|-------------------------------------------------------------|
| `Empty`  | String | Returns the nil UUID "00000000-0000-0000-0000-000000000000" |

### Methods

| Method             | Signature         | Description                            |
|--------------------|-------------------|----------------------------------------|
| `Generate()`       | `String()`        | Generate a new random UUID v4          |
| `IsValid(guid)`    | `Boolean(String)` | Check the canonical 36-character UUID syntax |
| `ToBytes(guid)`    | `Bytes(String)`   | Convert UUID string to 16-byte array   |
| `FromBytes(bytes)` | `String(Bytes)`   | Convert 16-byte array to UUID string   |

### Notes

- Generated UUIDs set the UUIDv4 version and RFC variant bits
- Format: `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` where:
    - `4` indicates version 4 (random UUID)
    - `y` is one of `8`, `9`, `a`, or `b` (variant indicator)
- `New()` and `FromBytes()` emit lowercase hex; `IsValid()` accepts either hex case
- `IsValid()` checks shape and hexadecimal digits only; it does not require any particular version
  or variant bits, so the nil UUID and non-v4 UUID strings are valid inputs
- `New()` uses Zanna's cryptographic RNG: the approved-mode DRBG when enabled, otherwise
  `BCryptGenRandom` on Windows, `getrandom()` with `/dev/urandom` fallback on Linux, and
  `arc4random_buf()` on macOS. Entropy failure traps and aborts instead of using weak randomness
- `Empty` returns a fresh string handle containing the nil UUID
- `IsValid()` checks the runtime string byte length; extra embedded bytes make the value invalid
- `ToBytes()` traps if the UUID format is invalid
- `ToBytes()` and `FromBytes()` use the canonical UUID byte order and do not transform version or
  variant bits
- `FromBytes()` traps if the Bytes object is not exactly 16 bytes

### Zia Example

```zia
module UuidDemo;

bind Zanna.Terminal;
bind Zanna.Text.Uuid as Uuid;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var id = Uuid.Generate();
    Say("UUID: " + id);                                    // canonical 36-character form
    Say("Valid: " + Fmt.Bool(Uuid.IsValid(id)));            // true
    Say("Invalid: " + Fmt.Bool(Uuid.IsValid("not-uuid"))); // false
}
```

### BASIC Example

```basic
' Generate a new UUID
DIM id AS STRING = Zanna.Text.Uuid.Generate()
PRINT id  ' Example: "550e8400-e29b-41d4-a716-446655440000"

' Check if a string is a valid UUID
DIM valid AS INTEGER = Zanna.Text.Uuid.IsValid(id)
PRINT valid  ' Output: 1 (true)

DIM invalid AS INTEGER = Zanna.Text.Uuid.IsValid("not-a-uuid")
PRINT invalid  ' Output: 0 (false)

' Get the empty/nil UUID
DIM empty AS STRING = Zanna.Text.Uuid.Empty
PRINT empty  ' Output: "00000000-0000-0000-0000-000000000000"

' Convert to/from bytes for storage or transmission
DIM bytes AS OBJECT = Zanna.Text.Uuid.ToBytes(id)
DIM restored AS STRING = Zanna.Text.Uuid.FromBytes(bytes)
PRINT restored = id  ' Output: 1 (true)
```

---


## See Also

- [Data Formats](formats.md)
- [Pattern Matching](patterns.md)
- [Formatting & Generation](formatting.md)
- [Text Processing Overview](README.md)
- [Zanna Runtime Library](../README.md)
