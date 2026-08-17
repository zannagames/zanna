---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0264: Share One Semantic Analysis per Editor Revision

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0008 — Semantic Token Overlay Uses Registry-Only Semantics
- `src/frontends/zia/rt_zia_completion.cpp` — asynchronous editor analysis
- `src/zannastudio/src/editor/semantic_tokens.zia` — token overlay consumer
- `src/zannastudio/src/editor/symbols.zia` — outline and folding consumer

## Context

Zanna Studio independently requested semantic tokens and document symbols for
the same path and editor revision. Each asynchronous request parsed and bound
the complete source. Enabling the outline, semantic folding, and highlighting
could therefore spend multiple worker slots and repeat the most expensive part
of the language pipeline while still producing results from nominally identical
snapshots.

## Decision

Add `Zanna.Zia.Completion.BeginAnalysisForFile(source, path)`. It returns the
existing `SemanticJobHandle`, with additive job kind 7 (`Analysis`). The worker
parses and semantically analyzes the copied source exactly once, then snapshots
document symbols, semantic tokens, and structured diagnostics before compiler
state is destroyed.

The existing `SemanticJob.Symbols`, `SemanticJob.Tokens`, and
`SemanticJob.Diagnostics` accessors accept both their original single-result
job kinds and the new Analysis kind. Existing begin methods and job-kind
ordinals remain unchanged.

Studio owns one revision-keyed `SemanticModel` for the active text editor. It
debounces revisions, rejects stale path/revision results, and shares the
published symbols and tokens with the highlighter, outline, and semantic-fold
consumer. Controllers retain their standalone job paths for isolated use and
compatibility probes.

## Consequences

- Normal Zia editing performs one compiler analysis instead of separate token
  and symbol analyses for a revision.
- Outline visibility and folding consume the same semantic truth as coloring.
- The model can later supply diagnostics without another parse.
- The runtime surface grows additively by one method and one job-kind ordinal;
  existing binaries and source remain compatible.
- A shared job may compute a result channel that no visible controller consumes,
  trading small serialization work for eliminating a full parse and bind.

## Alternatives Considered

- **Rely on independent parse caches.** Rejected because detached worker jobs do
  not share a safe compiler result and can occupy both bounded worker slots.
- **Make controllers exchange completed job handles directly.** Rejected because
  it couples view visibility and update order to result ownership.
- **Expose compiler AST pointers through the runtime ABI.** Rejected because
  compiler object lifetimes and layouts are not a stable language contract.
