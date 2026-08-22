# Zanna Studio stability implementation — 2026-08-21

This is the implementation scorecard for the 32-item Studio/runtime review.
Verification stayed focused because the shared build tree was concurrently in
use; no full rebuild or full `ctest` run was performed.

| # | Outcome | Primary implementation / evidence |
|---:|---|---|
| 1 | Implemented | UTF-8-safe editor cursor conversion; phase/model probes. |
| 2 | Implemented | Windows process stdin uses the bounded asynchronous pipe writer; `RTExecTests`. |
| 3 | Implemented | Process finalization is non-trapping and idempotent; process lifecycle tests. |
| 4 | Implemented | File-index cursor handles use a generation-safe registry rather than caller-visible raw pointers; ADR 0287 and native workspace tests. |
| 5 | Implemented | Autosave iterates every eligible dirty document; autosave probe. |
| 6 | Implemented | Save normalization returns and surfaces failure instead of silently discarding it; file-command probes. |
| 7 | Implemented | Document admission and opening consume one bounded snapshot, eliminating the double-read race; document probes. |
| 8 | Implemented | Rejected binary/oversized files reach a safe preview tab from ordinary open flows; navigation/admission probes. |
| 9 | Implemented | File indexing applies ordered, negatable, directory-aware nested gitignore rules; native workspace tests. |
| 10 | Implemented | Glob matching is memoized and bounded instead of recursively exponential; native workspace tests. |
| 11 | Implemented | File-index pages have independent result and traversal-work budgets; ADR 0287 and cursor tests. |
| 12 | Implemented | Combined process output enforces the exact configured cap instead of retaining two full streams; execution tests. |
| 13 | Implemented | BASIC declaration scanning builds one block-scope index, removing repeated end scans; semantic probes. |
| 14 | Implemented | Allocation-free `Zanna.String.ByteAt` replaces per-character immutable slicing in editor hot loops; ADR 0288 and string/editor probes. |
| 15 | Implemented | Gitignore rules and candidate decisions are cached per traversal; native workspace tests. |
| 16 | Implemented | Workspace index overlays retain path/document identity rather than duplicate source snapshots; workspace probes. |
| 17 | Implemented | PTY launch accepts a bounded environment overlay and sets a truthful terminal environment; Windows environment-block construction has a focused internal owner; ADR 0289, MinGW syntax validation, and PTY tests pass. |
| 18 | Implemented | Terminal input avoids redundant immutable copies before PTY write; terminal probes. |
| 19 | Implemented | PTY last-error state is cleared on successful operations; PTY tests. |
| 20 | Implemented | Process and PTY GUI activity share a refcounted process-wide watcher rather than one monitor thread per child; ADR 0281 and execution tests. |
| 21 | Implemented | Shared activity/watch and async writer primitives remove duplicated process/PTY lifecycle machinery; execution tests. |
| 22 | Implemented | VirtualTree normalizes invalid and duplicate selection identities; GUI runtime tests. |
| 23 | Implemented | VirtualTree bulk updates defer rebuild/selection reconciliation to one commit; ADR 0290 and hierarchy probes. |
| 24 | Implemented | large Build/Search/References/Debug surfaces use bounded virtual rows; tool-panel probes. |
| 25 | Implemented | BASIC signature help is compiler/AST-backed, preserving call-site spelling; ADR 0291 and BASIC async/controller probes. |
| 26 | Implemented ratchet reduction | Forbidden Studio layer edges fell from 47 to 42 by moving static command metadata into `services`; architecture guard passes. |
| 27 | Implemented ratchet reduction | Oversized Studio modules fell from 73 to 69; BASIC signature scan, debugger protocol encoding, and transform math now have focused owners; architecture guard passes over 524 sources. |
| 28 | Implemented ratchet reduction | Native files above 4,000 lines fell from 30 to 28. Asset/animation/effect and physics/cloth graphics-disabled stubs are separate real C translation units; strict compilation and pre/post `nm` symbol-equivalence checks pass. |
| 29 | Gate implemented; platform execution in progress | Schema-checked 48-row/42-applicable desktop matrix passes validation. Eleven macOS rows have retained same-host evidence; 31 real-platform/manual rows remain pending and are not represented as passes. |
| 30 | Implemented; platform evidence partial | D3D11 supports device-only hardware/WARP contexts and Linux OpenGL supports surfaceless EGL pbuffers without `libwayland-egl`; Windows cross-compilation and D3D/OpenGL source-contract tests pass, while live Windows/Linux runs remain matrix work. |
| 31 | Implemented | Terminal emulation adds delayed autowrap, DECAWM, and origin-relative cursor addressing with margin clamping; conformance and Studio terminal probes pass. |
| 32 | Implemented | Source Control can take current/incoming conflict sides and noninteractively continue/skip merge/rebase flows with commit gating; real-repository SCM probe passes. |

## Focused validation snapshot

- Studio architecture guard: 524 sources, 69 size debts, 42 layer debts — pass.
- Phase 0/1, BASIC query/async, scene editor, debugger, Run/Debug view, SCM,
  terminal, editor hot-path, formatting, and navigation probes — pass.
- D3D11/OpenGL backend source contracts and Canvas3D accelerated offscreen
  runtime test — pass.
- Windows-target D3D11 syntax compile with warnings-as-errors — pass.
- EGL adapter and both split stub families compile independently with
  warnings-as-errors — pass.
- Platform policy lint and platform-signoff manifest validation — pass.
- Source-health audit — pass with 28 mega files, 32 allocation hotspots, and
  313 unclassified graphics stubs (all at or below their ratchets).

The remaining work is evidence collection, not an unimplemented code path:
Windows and Linux live GPU runs plus the explicitly interactive/manual desktop
matrix scenarios require those real environments. The matrix remains honest
until that evidence exists.
