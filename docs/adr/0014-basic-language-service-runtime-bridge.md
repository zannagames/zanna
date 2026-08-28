---
status: active
audience: contributors
last-verified: 2026-07-22
---

# ADR 0014: Zanna BASIC Language-Service Runtime Bridge

Date: 2026-06-25

Status: Accepted

## Context

Zanna Studio gives Zia files completion, diagnostics, hover, and document symbols via an
in-process runtime bridge: the Zia frontend's IDE engines are exposed as runtime functions
(`Zanna.Zia.Completion.*`, `Zanna.Zia.Toolchain.*`) registered in `src/il/runtime/runtime.def`,
implemented in `src/frontends/zia/rt_zia_completion.cpp` (linked via `fe_zia`/editor services),
with weak stubs in `zanna_runtime` so the runtime library never depends on a frontend. The IDE
(Zia code) consumes them in `zannastudio/src/editor/{completion,diagnostics,hover,symbols}.zia`.

Zanna BASIC has the same intelligence available — `parseAndAnalyzeBasic()` (parse + sema, no
lowering) and `BasicCompletionEngine::complete()` in `src/frontends/basic/` — but no runtime
bridge, so BASIC files are edit/build/run only in the IDE. Phase 4 of the overhaul (plan
`~/.claude/plans/zannastudio-needs-to-be-golden-blum.md`) adds completion, diagnostics, hover, and
symbols for BASIC. Exposing a new `Zanna.Basic.*` runtime surface is a cross-layer contract
(IDE ↔ runtime), covered by the spec-currency gate (ADR 0006).

## Decision

Add a single synchronous, in-process `Zanna.Basic.LanguageService` runtime class. (One class,
not the two-class Completion/Toolchain split the Zia bridge uses: `Zanna.*` class leaf names must
be globally unique — enforced by `test_runtime_class_qualified_surface` — and BASIC's completion
and toolchain are backed by the same `parseAndAnalyzeBasic` engine.)

- `Zanna.Basic.LanguageService.CheckForFile(str,str) -> obj<Seq>` — diagnostics.
- `Zanna.Basic.LanguageService.ItemsForFile(str,str,i64,i64) -> obj<Seq>` — completion items.
- `Zanna.Basic.LanguageService.SymbolsForFile(str,str) -> str` — document symbols.
- `Zanna.Basic.LanguageService.HoverInfoForFile(str,str,i64,i64) -> obj<Map>` — hover.

Each emits the **same record shapes** the IDE already consumes from the Zia bridge (identical Map
keys / the `name\tkind\ttype\tline` symbol string), so the IDE controllers reuse their existing
result-application code and only branch on the active language to pick the namespace.

**The runtime surface stays synchronous; Studio dispatches it asynchronously.** Keeping the four
runtime methods synchronous avoids a second public semantic-job ABI and lets non-GUI tools call
them directly. Studio must not call them on its frame thread, however: large or malformed BASIC
buffers can make parse/sema latency visible, and diagnostics plus semantic folding can overlap.
`zannastudio/src/editor/basic_query_job.zia` therefore invokes the synchronous surface through
`Zanna.Threads.Async.RunOwned`. Each feature owns at most one active callback and one latest-wins
replacement. Cancellation does not preempt the compiler; it prevents stale publication, lets the
active call drain, and starts only the newest queued snapshot. Controllers additionally validate
path, revision, scheduler generation, and cursor/pointer coordinates before applying results.
This is an IDE scheduling policy, not a runtime ABI change.

Native codegen that sees `rt_basic_toolchain_*`, `rt_basic_completion_*`, or
`Zanna.Basic.LanguageService.*` references force-loads `fe_basic`. This mirrors the `zia`
interpreter link and ensures the strong bridge overrides `zanna_runtime`'s weak unavailable stubs
on macOS, Windows, and Linux. The frontend's IL/support dependencies remain demand-loaded.

**No VM / semantic change.** This wraps existing frontend engines. The diagnostic conversion is
frontend-agnostic — both frontends produce `il::support::DiagnosticEngine`, so the BASIC bridge
reuses the same `Diagnostic → Map` mapping. BASIC's `CompletionKind` already matches the IDE's
1:1; `Severity{Note,Warning,Error}` maps to `{2,1,0}`.

Files mirror the Zia split: strong impl `src/frontends/basic/rt_basic_completion.cpp` (in
`fe_basic`, beside `BasicCompletion.cpp`); weak stubs `src/runtime/core/rt_basic_completion_stub.c`
(in `zanna_runtime`, returning empty/`unavailable` payloads so a frontend-less binary shows no
false editor warnings); header `src/runtime/graphics/common/rt_basic_completion.h`; `RT_FUNC` +
`RT_CLASS_BEGIN/END` blocks in `runtime.def`. `check_runtime_completeness.sh` must stay clean.

## Consequences

`language_service.zia` enables completion, diagnostics, hover, document symbols, and scanner-backed
navigation/signature help for BASIC. Hover is identifier-based (extract the token at the cursor,
look it up via the analyzer); unresolved symbols return `available=false`. Compiler-backed editor
features remain responsive under large-file edits and tab switches, at the cost of retaining one
immutable source snapshot per active feature until a non-preemptible call drains. A future external
`zbasic-server` could supersede the in-process bridge without changing the runtime contract.
