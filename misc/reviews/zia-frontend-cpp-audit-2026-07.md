---
status: complete
audience: contributors
last-verified: 2026-07-26
---

# Zia Frontend C++ Correctness and Optimization Audit (July 2026)

This audit covers the C++ implementation under `src/frontends/zia/`. The first
completed remediation set concentrates on untrusted input boundaries: editor
delta JSON, lexer token retention, file imports, and completion cursor
coordinates. These paths are exercised continuously by IDE/editor traffic and
can receive incomplete or adversarial input.

No IL opcode, IL grammar, verifier rule, runtime C ABI signature, or
cross-layer dependency was changed. Therefore this remediation does not require
an ADR.

## Resolved findings and recommendations

| ID | Area | Finding | Implemented recommendation |
|---:|---|---|---|
| ZIA-AUD-001 | Mirror JSON | A valid array followed by arbitrary bytes was accepted. | Require whitespace followed by physical end-of-input after `]`. |
| ZIA-AUD-002 | Mirror JSON | A trailing comma before `]` was accepted. | Reject array trailing commas. |
| ZIA-AUD-003 | Mirror JSON | A trailing comma before `}` was accepted. | Reject object trailing commas. |
| ZIA-AUD-004 | Mirror JSON | Empty delta objects reached later validation ambiguously. | Reject empty objects structurally. |
| ZIA-AUD-005 | Mirror JSON | Unknown object fields were silently ignored. | Reject fields outside `r/sl/sc/el/ec/t`. |
| ZIA-AUD-006 | Mirror JSON | Duplicate revision fields silently overwrote one another. | Track presence and reject duplicate `r`. |
| ZIA-AUD-007 | Mirror JSON | Duplicate coordinate fields silently overwrote one another. | Reject duplicate `sl/sc/el/ec`. |
| ZIA-AUD-008 | Mirror JSON | Revision was optional, weakening journal ordering. | Require `r` in every delta. |
| ZIA-AUD-009 | Mirror JSON | Replacement text was optional. | Require `t` in every delta. |
| ZIA-AUD-010 | Mirror JSON | Negative coordinates entered a signed parse/narrowing path. | Accept only unsigned JSON integers for coordinates and revisions. |
| ZIA-AUD-011 | Mirror JSON | Decimal accumulation could overflow its host integer type. | Use checked `uint64_t` multiply/add accumulation. |
| ZIA-AUD-012 | Mirror JSON | Large coordinates narrowed to `int` implementation-dependently. | Reject values above `INT_MAX` before conversion. |
| ZIA-AUD-013 | Mirror JSON | Unknown backslash escapes were accepted as their trailing byte. | Reject escapes not defined by JSON. |
| ZIA-AUD-014 | Mirror JSON | The valid escaped-solidus form was not handled explicitly. | Decode `\/` as `/`. |
| ZIA-AUD-015 | Mirror JSON | Valid backspace and form-feed escapes were not decoded. | Decode `\b` and `\f`. |
| ZIA-AUD-016 | Mirror JSON | Raw control bytes inside strings were accepted. | Reject unescaped bytes below U+0020. |
| ZIA-AUD-017 | Mirror JSON | BMP `\uXXXX` values were truncated to their low byte. | Encode decoded scalar values as UTF-8. |
| ZIA-AUD-018 | Mirror JSON | Supplementary characters represented by surrogate pairs were corrupted. | Combine valid high/low pairs and emit four-byte UTF-8. |
| ZIA-AUD-019 | Mirror JSON | Lone high surrogates were accepted. | Require a following low surrogate. |
| ZIA-AUD-020 | Mirror JSON | Lone low surrogates were accepted. | Reject unpaired low surrogates. |
| ZIA-AUD-021 | Mirror JSON | Escaped field names were parsed inconsistently. | Reject escapes in this fixed-schema key grammar. |
| ZIA-AUD-022 | Mirror JSON | Control bytes were accepted in field names. | Reject control bytes while scanning keys. |
| ZIA-AUD-023 | Mirror JSON | Replacement text could grow without a document-oriented bound. | Bound decoded text by the 64 MiB mirror limit. |
| ZIA-AUD-024 | Mirror JSON | Delta JSON processing had no batch byte limit. | Reject batches above 16 MiB. |
| ZIA-AUD-025 | Mirror JSON | A batch could contain an unbounded number of tiny edits. | Cap a batch at 100,000 deltas. |
| ZIA-AUD-026 | Mirror JSON | A replacement could grow the mirror without limit. | Check the post-replacement size before mutation. |
| ZIA-AUD-027 | Mirror JSON | Later malformed deltas could leave earlier edits applied. | Stage the complete batch and publish only on success. |
| ZIA-AUD-028 | Mirror JSON | `substr + text + substr` created multiple full-document temporaries. | Use checked in-place `std::string::replace` on the staged copy. |
| ZIA-AUD-029 | Mirror JSON | The last delta revision could be below the advertised batch revision. | Require the applied revision to equal `end_revision`. |
| ZIA-AUD-030 | Mirror JSON | An empty batch could claim to advance a document revision. | Reject forward-moving empty batches. |
| ZIA-AUD-031 | Mirror state | A negative full-sync revision wrapped to a huge `uint64_t`. | Ignore full syncs with negative revisions. |
| ZIA-AUD-032 | Mirror state | Full sync bypassed the mirror size limit. | Ignore full-sync bodies above 64 MiB. |
| ZIA-AUD-033 | Mirror state | Strict offset conversion trusted a non-null output pointer. | Validate the output pointer before writing. |
| ZIA-AUD-034 | Mirror state | Equal-revision delta calls contradicted the forward-only contract. | Require `end_revision` to be strictly newer. |
| ZIA-AUD-035 | Lexer | An enormous decimal literal retained the complete spelling. | Cap retained numeric spellings at 4,096 bytes while consuming the token. |
| ZIA-AUD-036 | Lexer | Hexadecimal literals had the same unbounded retention path. | Apply the numeric cap to hexadecimal scanning. |
| ZIA-AUD-037 | Lexer | Binary literals had the same unbounded retention path. | Apply the numeric cap to binary scanning. |
| ZIA-AUD-038 | Lexer | Octal literals had the same unbounded retention path. | Apply the numeric cap to octal scanning. |
| ZIA-AUD-039 | Lexer | Malformed based-literal tails could independently grow without bound. | Consume oversized recovery tails without appending them. |
| ZIA-AUD-040 | Lexer | Oversized tokens did not produce a dedicated bounded error token. | Emit one range diagnostic and one bounded `Error` token. |
| ZIA-AUD-041 | Lexer | Lookahead used `pos + offset` in its bounds predicate. | Compare the offset with remaining bytes before addition. |
| ZIA-AUD-042 | Lexer API | The header relied on transitive declarations for size, integer, and vector types. | Include `<cstddef>`, `<cstdint>`, and `<vector>` directly. |
| ZIA-AUD-043 | Imports | In-memory source-provider imports bypassed the disk source-size limit. | Apply the same 64 MiB limit to provider snapshots. |
| ZIA-AUD-044 | Imports | The imported-file limit used `>` and admitted one extra file. | Reject a new unique file when the count is already at the limit. |
| ZIA-AUD-045 | Imports | The file-limit check ran before processed/in-progress deduplication. | Deduplicate the normalized path before enforcing capacity. |
| ZIA-AUD-046 | Imports | A missing or malformed imported file could still yield `resolve() == true`. | Accumulate import failures and return false after cleanup. |
| ZIA-AUD-047 | Imports | Recursive failure returned before normal traversal cleanup. | Let each active frame unwind its stack/set state before returning failure. |
| ZIA-AUD-048 | Imports | One recursive failure prevented diagnostics for remaining sibling binds. | Continue resolving siblings while remembering failure. |
| ZIA-AUD-049 | Completion | Negative cursor lines were retained in completion replacement ranges. | Clamp lines to one before context extraction. |
| ZIA-AUD-050 | Completion | Negative columns converted to huge `size_t` offsets. | Clamp columns to zero before conversion. |
| ZIA-AUD-051 | Completion | Columns past line end were returned unchanged to editor clients. | Clamp the stored column to the active line length. |
| ZIA-AUD-052 | Completion | `lineStart + column` could overflow before its end-of-line clamp. | Clamp against line length before adding to `lineStart`. |
| ZIA-AUD-053 | Completion | Backward prefix scanning narrowed line length to `int`. | Scan with `size_t` and bound the protocol-facing length. |
| ZIA-AUD-054 | Completion | Result limiting narrowed `items.size()` to `int`. | Compare in `size_t` after validating positive `maxResults`. |
| ZIA-AUD-055 | Completion | Replacement end positions could describe nonexistent columns. | Publish the clamped cursor column in every completion item. |

## Regression coverage

The remediation extends these existing CTest targets:

- `test_zia_doc_mirror`: strict JSON structure, duplicate/missing fields,
  invalid escapes, Unicode and surrogate decoding, atomic multi-delta failure,
  revision completion, and valid multi-edit batches.
- `test_zia_lexer`: bounded decimal/hex/binary/octal and malformed-tail tokens.
- `test_zia_binds`: strict resolver failure propagation.
- `test_zia_completion_engine`: negative and past-end cursor clamping.

The tests intentionally group closely related malformed inputs so the suite
remains fast while every finding above has a direct assertion or shares the
same bounded scanner invariant.

## Validation results

- Incremental canonical build: passed with warnings-as-errors.
- Targeted CTests: 4/4 passed (`test_zia_doc_mirror`, `test_zia_lexer`,
  `test_zia_binds`, and `test_zia_completion_engine`).
- Platform-policy lint: passed.
- Standard `zia` label baseline: 427/428 passed. The one failure,
  `zia_zannastudio_scene_editor_2d`, reproduces before and after this change
  with an unrelated compact-layout clipping assertion.
- Extended `zia` run (including slow tests, excluding that reproduced 2D
  baseline): 445/446 passed. The sole failure was the slow
  `zia_zannastudio_scene_editor_3d` compact-layout clipping assertion in the
  same Studio UI subsystem. No changed frontend or frontend unit test failed.
