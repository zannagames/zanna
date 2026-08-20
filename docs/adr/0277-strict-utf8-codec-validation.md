---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0277: Expose Strict Whole-String UTF-8 Validation

## Status

Accepted (2026-08-20)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0027 — Runtime API Contract Metadata
- ADR 0255 — Bounded and No-Clobber Whole-File Text I/O
- ADR 0258 — Undoable State-Preserving Full-Document Replacement
- `src/runtime/core/rt_string_internal.h` — shared strict UTF-8 predicate

## Context

Runtime Strings preserve exact byte lengths and may therefore contain arbitrary
bytes. Zanna Studio previously admitted editor input by checking only extensions,
size, and NUL bytes in the first 4 KiB. A malformed UTF-8 sequence, a NUL later
in the file, or a UTF-16-like payload whose early window happened to avoid NUL
could reach compiler, editor, and C-string-oriented paths as if it were text.

Several runtime parsers already use the internal strict UTF-8 scalar validator,
but Zia programs had no allocation-free way to validate one complete String.
Re-encoding through Hex or Base64 is both expensive and incapable of expressing
the actual text contract cleanly.

## Decision

Add `Zanna.Text.Codec.IsValidUtf8(String) -> Boolean`, backed by
`rt_codec_is_valid_utf8`. It validates the String's exact byte length with the
shared strict scalar predicate and never traps or allocates. It rejects bare
continuations, truncated sequences, overlong forms, UTF-16 surrogate scalars,
and values above U+10FFFF.

U+0000 remains valid UTF-8. Consumers such as Studio whose downstream pipeline
requires NUL-free text must enforce that independent content policy. Studio
performs both checks on the same bounded full-file read before creating an
editable buffer.

## Consequences

- Studio can make text admission a complete-file decision without inventing a
  second UTF-8 implementation in Zia.
- Runtime callers can distinguish byte Strings from valid Unicode text without
  an allocation or exception path.
- The method is an additive public runtime C ABI and registry surface.
- Valid UTF-8 does not imply C-string safety; the API documentation explicitly
  preserves that distinction.

## Alternatives Considered

- **Validate only a prefix.** Rejected because malformed content can occur at
  any byte and boundary-truncated multibyte sequences require the complete span.
- **Treat every runtime String as UTF-8.** Rejected because existing byte-string
  codecs intentionally preserve arbitrary bytes and embedded NUL.
- **Reject U+0000 in `IsValidUtf8`.** Rejected because that would conflate
  Unicode validity with a consumer-specific text-buffer policy.

## Tests

- `test_rt_codec` covers empty, ASCII, multibyte, and embedded-U+0000 inputs,
  plus bare continuation, overlong, surrogate, truncated, and out-of-range
  encodings.
- `zia_zannastudio_phase0_phase1` covers Studio rejection of malformed UTF-8
  and NUL-containing files before editable-buffer creation.
