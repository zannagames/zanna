---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0288: Expose Allocation-Free String Byte Access

## Status

Accepted

## Context

Studio's BASIC and Zia lightweight scanners repeatedly called `MidLen(text,
position, 1)` to classify ASCII syntax bytes. `MidLen` is a codepoint-oriented,
owned-string operation, so each character test performs UTF-8 traversal and may
allocate a new string. The scanners otherwise use byte-based `Length`, columns,
and search offsets. This made linear lexical passes allocation-heavy and also
mixed byte positions with codepoint slicing after non-ASCII text.

Adding a public string primitive changes the runtime C ABI and registry surface.

## Decision

Add `Zanna.String.ByteAt(text, offset) -> Integer`, backed by
`rt_str_byte_at(rt_string, int64_t)`. `offset` is zero-based and byte-oriented,
matching `Length`, `Substring`, and search offsets. A live in-range string
returns an unsigned value in `0..255`; null, negative, and out-of-range access
return `-1`. Forged non-null handles trap through the standard string-handle
validator.

Lexical scanners should use `ByteAt` for delimiter and ASCII identifier
classification, and take one `Substring` only after locating a complete token.
User-facing Unicode slicing remains the responsibility of `Mid`/`MidLen`.

## Consequences

- Hot lexical loops no longer allocate one-character strings.
- Byte columns stay consistent in the presence of UTF-8 string/comment content.
- Callers must not confuse a byte with a Unicode codepoint; decoding APIs remain
  necessary for user-perceived characters.

## Alternatives Considered

Returning a one-character string would preserve existing comparisons but retain
allocation/reference-count traffic. Returning a decoded codepoint would require
variable-width position advancement and would not match the compiler-facing
byte-column contract used by these scanners.
