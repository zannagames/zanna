---
status: accepted
audience: contributors
last-verified: 2026-09-18
---

# ADR 0376: A Bind Is Not Inherited

## Status

Accepted (2026-09-18). Changes Zia name visibility: which declarations of other
files a file may name. The IL, the runtime C ABI and the bind syntax are
unchanged.

## Context

The import resolver loads every file reachable through `bind` into one program.
Semantic analysis then resolved names program-wide: bare names through the
global scope and the type registry, qualified names through a program-wide map
of module exports. A file could therefore use any declaration reachable through
the bind graph, including modules that only the files it binds had bound, and
nothing reported it.

Legacy Baseball's architecture review (ledger ZB-54) found six source files and
three probes using modules they never bound (`game_state` → `constants`,
`season_engine` → `player`, `choreographer` → `seq_hash`, …). Such hidden edges
defeat every bind-graph layering guard, make removing a bind in one file break
unrelated files, and make a file fail when checked on its own ("Unknown type")
while it compiles inside its program. Zanna Studio had 31 such uses and the
demos 24; a thread pool in the web-server demo called `main.handleClient`
without binding `main`.

## Decision

- **A declaration of another file is visible only in files that bind that file
  directly** (by path, with or without an alias). A bind is not inherited from
  the files a file binds. The rule covers every kind of top-level declaration —
  functions, globals and constants, classes, structs, interfaces, enums and type
  aliases — in every position: calls, annotations, `new`, `is`/`as`,
  `extends`, `implements`, generic arguments, struct literals and enum variants,
  spelled bare (`Page`) or qualified (`inner.Page`). A file may still qualify
  its own declarations with its own module name.
- **Unbound means not in scope, not an error at lookup.** A name exported by an
  unbound file is simply absent, so another binding of the same short name (a
  runtime import, an enclosing namespace, a bound module) still resolves. The
  error is reported only where the name is otherwise undefined.
- **Diagnostic.** `V-ZIA-UNBOUND-MODULE` names the module to bind:
  "'helper' is declared in module 'inner', which this file does not bind",
  "Type 'Page' belongs to module 'inner', which this file does not bind", or
  "Module 'inner' is not bound in this file", with the help text "Add a `bind`
  for module 'inner' to this file; a bind is not inherited from the files it
  binds." It is reported once per (file, module) — one bind fixes every use —
  and the unresolved type no longer triggers follow-on errors (`new` of it, or
  non-exhaustive `match` on it). `zanna check --diagnostic-format=json`
  therefore lists exactly the binds a file needs.
- **Runtime namespace binds are out of scope.** `bind Zanna.Graphics;` and its
  aliases are still visible program-wide (ledger ZB-69); the same rule will be
  applied to them in a follow-up.

## Consequences

- Every file states its dependencies; layering guards that read the bind graph
  see every edge, and a file checks the same on its own as inside its program.
- Programs that relied on inherited binds stop compiling until the binds are
  added; the diagnostic names each one. Migrated with this change: Legacy
  Baseball (9 binds, plus 12 in its vendored ZannaSQL copy, identical to
  upstream), Zanna Studio (31 binds in 10 files; one line used its module name
  where only an alias was bound), and the demos (24 binds in 23 files; the web
  server's thread pool now receives its handler as a function value). Zanna's
  own sources and tests needed none.
- Zanna Studio's architecture baseline (ADR 0266) records
  `src/app/studio_application_base.zia` at 936 lines, up from 924: exactly the
  twelve bind lines this rule made explicit, not new code.
- Regression coverage: `ZiaBinds.BindIsNotInheritedFromBoundFiles` in
  `src/tests/zia/test_zia_binds.cpp` compiles fifteen ways of naming an unbound
  module's declarations, expecting one diagnostic each, and the same programs
  with the bind added, expecting a clean compile.
