---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0268: Resolve Cross-Language Symbols by Case-Insensitive Fallback

## Status

Accepted (2026-08-17)

## Consulted

- ADR 0003 — IL Linkage and Module Linking
- `docs/languages/interop.md` — the documented mixed-language contract
- `src/il/link/ModuleLinker.cpp` — import resolution
- `src/tools/zanna/cmd_run.cpp` — mixed-project compile and link driver

## Context

`docs/languages/interop.md` documents mixed Zia/BASIC projects: one `zanna.project`
with `lang mixed`, one `entry`, `expose`/`EXPORT` on the defining side, and
`foreign func`/`DECLARE FOREIGN` on the calling side. Its worked example spells
the symbol `Factorial` on both sides.

That workflow did not run. `zanna run` exited 1 for every configuration that
actually crossed the language boundary, and for any project holding a non-entry
`.bas` file at all. Three independent defects were involved.

**The failure was silent.** `compileMixedProject` builds `il::support::Diagnostic`
values for its own link, optimize, and verify failures and returns them in an
`Expected`. The caller discards the payload on the documented assumption that
"diagnostics already printed" — true of the single-language paths, which print
before returning, and false of the mixed path. Users saw exit status 1 and no
output.

**Every BASIC module defines `main`.** BASIC has no separate library form: a
module's top-level statements lower into `@main`, which also carries the
`__mod_init$oop` call and global initialization. Linking a BASIC library module
against any entry module therefore failed with `multiple modules define 'main'`.
Zia only emits `main` for `start()`, so the mirror case happened to work, which
is why the defect looked intermittent.

**Identifier case does not survive the BASIC front end.** `Lexer.cpp` upper-cases
every identifier as it consumes it, so `Factorial` reaches the AST — and the IL —
as `FACTORIAL`. The source spelling is gone before the parser runs. A Zia
`expose func Factorial` and a BASIC `DECLARE FOREIGN FUNCTION Factorial` therefore
emit `@Factorial` and `@FACTORIAL` and cannot match, in either direction.

## Decision

**1. The mixed driver reports its own diagnostics.** `compileMixedProject` prints
link and optimization failures before returning them, restoring the caller's
stated invariant. Link errors are joined onto one line because diagnostics render
as single lines.

**2. A library module's `main` becomes a module initializer.** For each non-entry
module, `main` is renamed to `__zanna_lib_main$mixedlib<N>` and wrapped in a
synthesized `__zanna_lib_init$mixedlib<N>() -> void` marked `moduleInitializer`.
The linker already runs such initializers at the top of the merged `main`, which
reproduces the ordering a single-language build produces.

The body is wrapped, never rewritten or dropped. Dropping `main` would also drop
the library's `__mod_init$oop` call and its global initialization — a silent
behavioural change rather than a link fix.

**3. Unresolved imports fall back to a unique case-insensitive match.** When an
import resolves against neither the export index nor the entry definitions, the
linker consults a case-folded index of the same definitions:

- exactly one candidate — bind it, and record a rename so call sites reach the
  real name;
- more than one candidate — report
  `ambiguous import: @NAME matches multiple definitions ignoring case (...)`
  rather than choosing arbitrarily;
- none — the existing `unresolved import` error, unchanged.

The fallback is consulted **only after exact resolution has already failed**, so
it cannot change how any currently-linking program binds.

This changes cross-layer symbol resolution between the frontends and the IL
linker. It adds no IL opcode, grammar production, verifier rule, or runtime C ABI
surface.

## Consequences

- The interop guide's documented example runs in both directions, spelled as
  documented.
- BASIC's case insensitivity now extends across a module boundary, which is
  consistent with how the language behaves everywhere else.
- A case-sensitive frontend can no longer rely on case alone to distinguish two
  cross-module symbols. This is reported as an ambiguity error rather than
  silently resolved, so the failure mode is loud.
- Symbols that resolve exactly are unaffected; the fallback is unreachable for
  them.
- Non-entry module top-level code now runs as an initializer instead of being a
  link error. Programs that previously failed to link may now run.

## Alternatives Considered

**Preserve the BASIC source spelling for boundary symbols.** Semantically the
most direct fix, and it was rejected on blast radius. The spelling is discarded
in `Lexer.cpp` before the parser sees a token, so recovering it means changing
the token representation, threading it through the AST, and then renaming the
emitted symbol — which breaks BASIC's own internal calls to the same procedure
unless every call site is remapped in step. That is a frontend-wide change to fix
a boundary-only mismatch.

**Require callers to spell BASIC symbols in upper case.** Zero code change: the
Zia side writes `foreign func FACTORIAL`. Rejected because it contradicts the
documented contract, leaks a frontend implementation detail into user source, and
reads as a defect rather than a design.

**Drop the library module's `main` outright.** Simplest duplicate-`main` fix and
silently wrong: it discards global initialization and module init that exported
functions depend on. A regression test (`initchk`) pins this exact behaviour.

**Case-fold only symbols known to originate from BASIC.** More precise in
principle, but IL functions carry no frontend-origin marker, and adding one to
serve link-time name matching is a larger contract change than the unique-match
fallback it would refine.
