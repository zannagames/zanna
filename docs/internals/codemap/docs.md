---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Documentation

Layout of the documentation tree (`docs/`). The
[main index](../../README.md) covers user-facing documentation; the
[internals index](../README.md) covers contributor documentation.

## Directory Layout

| Directory | Audience | Purpose |
|-----------|----------|---------|
| `docs/` (root) | public | Index plus a small set of user guides (getting started, FAQ, memory management, installer guide, 2D/3D graphics guides) |
| `docs/getting-started/` | public | Per-platform setup: `macos.md`, `linux.md`, `windows.md` |
| `docs/tutorials/` | public | `zia-tutorial.md`, `basic-tutorial.md` |
| `docs/book/` | public | The Zanna Book — five-part long-form course |
| `docs/languages/` | public | Zia/BASIC references and grammars, interop, arithmetic semantics, lifetime model |
| `docs/il/` | public | `il-guide.md` (normative IL spec) and `il-passes.md` |
| `docs/zannalib/` | public | Curated runtime library guides (concepts + examples) |
| `docs/generated/runtime/` | public | Auto-generated API reference (`rtgen`, DO NOT EDIT) |
| `docs/gameengine/` | public | Game engine guide and example-game tour |
| `docs/tools/` | public | CLI reference, REPL, debugging, language server |
| `docs/cross-platform/` | contributors | Platform differences and compliance checklist |
| `docs/specs/` | contributors | Formal specs: errors, numerics, object layout/ABI, threading, backend ABIs, encoding tables (JSON) |
| `docs/internals/` | contributors | Architecture, VM/backend guides, howtos, testing, this codemap tree |
| `docs/internals/codemap/` | contributors | Per-subsystem source maps |
| `docs/adr/` | contributors | Architecture decision records (`README.md` index, `0000-template.md`) |
| `docs/release_notes/` | public | Release notes per version |
| `docs/man/` | public | Troff man pages installed by CMake |

## Conventions

- Filenames are kebab-case.
- Every hand-written page carries `status` / `audience` / `last-verified`
  frontmatter.
- Internal review artifacts and working logs live in `misc/reviews/`, not in
  `docs/`.
