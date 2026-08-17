---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0261: Share the Compiled Pattern Engine and Unicode Word Boundaries

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0024 — Text Char and Editor Insert Helpers
- `src/runtime/text/rt_regex_internal.h` — internal regex contract
- `src/lib/gui/src/widgets/vg_findreplacebar.c` — native editor consumer
- `src/zannastudio/src/services/search_matcher.zia` — Studio consumer

## Context

The runtime `Pattern`/`CompiledPattern` implementation and the native GUI find
bar previously carried separate regular-expression engines. Their accepted
syntax, malformed-pattern behavior, case handling, zero-width progress, and
replacement semantics could drift by platform and by call site. Studio then
performed additional line-oriented matching in Zia, recompiling patterns and
publishing only the first occurrence on each line.

Whole-word search also treated every non-ASCII byte as an identifier byte in
one layer and as punctuation in another. That made results dependent on which
search surface was used and split valid Unicode identifiers.

## Decision

Build one internal, zero-dependency C regex engine as `zanna_regex_engine` and
link it into both the runtime text library and native GUI. The engine provides
recoverable compilation diagnostics, bounded iterative matching, exact capture
ranges, replacement-template expansion, and an explicit resume offset that
makes UTF-8-safe progress after zero-width matches.

Treat that archive as part of the native codegen dependency closure whenever
Base, Text, or Graphics is selected. CMake transitive link metadata does not
exist inside Zanna's in-process native linker, so both AArch64 and x86-64
pipelines must discover, build, and append `zanna_regex_engine` explicitly.
Installed-layout discovery uses the same public archive name.

Add these public runtime surfaces:

- `Zanna.Text.CompiledPattern.TryNew(str, i1) -> Result`
- `Zanna.Text.CompiledPattern.FindRangeFrom(str, i64, i1) -> seq<i64>`
- `Zanna.Text.CompiledPattern.ExpandReplacementAt(str, i64, str) -> Result`
- `Zanna.Text.Char.IsWord(str) -> i1`

`FindRangeFrom` returns `[startByte, endByte, resumeByte]`. Match coordinates
are UTF-8 byte offsets, consistent with existing string slicing and editor
columns. `TryNew` turns user-authored syntax errors into ordinary `Result`
errors; the legacy constructor retains its trapping compatibility contract.

Define word characters from a generated, sorted Unicode 16.0 table covering
letter, mark, number, connector-punctuation, and Join_Control code points.
Both runtime and GUI whole-word checks call the same internal table lookup.
ASCII-only case folding remains the documented case-insensitive behavior until
a separately reviewed Unicode case-folding contract is adopted.

Studio owns one compiled pattern per search or replacement request. It asks for
successive exact ranges, publishes every non-overlapping occurrence, and uses
the same retained object to expand captures during transactional replacement.
Native Find/Replace debounces human input, caps scanned bytes and match count,
and reports malformed patterns without trapping the application.

## Consequences

- Runtime, GUI, and Studio agree on syntax, captures, anchors, and malformed
  patterns on macOS, Windows, and Linux.
- Zero-width searches and replacements terminate while retaining valid anchor
  matches.
- Unicode whole-word results are deterministic and shared across layers.
- Interactive and workspace searches no longer compile once per line.
- The runtime gains additive C ABI symbols and installed declarations; runtime
  manifests and generated API documentation must include them.
- Updating Unicode classification requires regenerating the checked-in table
  and reviewing the version change.

## Alternatives Considered

- **Keep a POSIX regex adapter and a separate Windows fallback.** Rejected
  because behavior and failure modes would remain platform-dependent.
- **Implement search semantics entirely in Studio Zia.** Rejected because the
  native find bar and public runtime would still drift and capture expansion
  would be duplicated.
- **Treat all non-ASCII code points as word characters.** Rejected because
  punctuation and symbols then create false whole-word boundaries.
- **Adopt an external Unicode or regex library.** Rejected by Zanna's
  zero-dependency product constraint.
