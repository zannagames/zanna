---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0265: Keep One Canonical C Header per Language Service

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0014 — BASIC Language Service Runtime Bridge
- ADR 0101 — Modular Runtime Definitions and Authored API Documentation
- ADR 0264 — Share One Semantic Analysis per Editor Revision

## Context

The public GUI umbrella header repeated every Zia and BASIC language-service
prototype even though the focused `rt_zia_completion.h` and
`rt_basic_completion.h` headers already declared those C entry points. Adding
a service operation required synchronized edits to two declarations, and
`rtgen` could encounter either copy while recovering parameter names and C
signatures for generated runtime metadata.

The focused headers were not installed with the public runtime headers, so an
external C consumer could not avoid the 6,000-line GUI umbrella. The BASIC
header also used a repository-relative include that was invalid after the
runtime headers were installed into their flat public include directory.

## Decision

- `rt_zia_completion.h` is the sole authored declaration surface for the Zia
  completion, diagnostics, project-index, document-mirror, and semantic-job C
  ABI.
- `rt_basic_completion.h` is the sole authored declaration surface for the
  BASIC language-service C ABI.
- Both focused headers are public installed runtime headers and include public
  dependencies by installed header name.
- `rt_gui.h` includes both focused headers to retain source compatibility for
  existing umbrella-header consumers. It does not repeat their prototypes.
- Runtime canonical names, frontend signatures, classes, generated frontend
  extern metadata, and reference documentation continue to come from the
  modular `runtime.def` definition set. `rtgen` audits that each registered C
  symbol has a matching declaration in the runtime header tree.

No function signature, symbol name, ownership rule, calling convention, or
runtime class changes as part of this decision.

## Consequences

- A language-service ABI addition has one authoritative C declaration instead
  of two copies that can drift.
- Focused C/C++ consumers can include only the service they use; existing
  `rt_gui.h` consumers continue to compile unchanged.
- Installed source packages now contain the focused headers and their includes
  resolve identically in build and install trees.
- The GUI umbrella becomes smaller, while the runtime generator and strict
  surface audit still cover the declarations recursively.

## Alternatives Considered

- **Keep duplicate prototypes and strengthen comparison tests.** Rejected
  because validation does not remove the extra edit point or the unnecessary
  include cost.
- **Remove language services from `rt_gui.h` without includes.** Rejected
  because that would break source compatibility for consumers that relied on
  the umbrella header.
- **Generate exact C prototypes directly from frontend signatures.** Deferred
  because frontend signatures intentionally omit C-only details such as hidden
  receivers and exact handle spellings; inventing those details would weaken
  ABI review rather than centralize it.
