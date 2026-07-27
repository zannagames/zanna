---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Documentation Style Guide

House rules for everything under `docs/`. The goal is a tree that reads as one
coherent set of documentation: consistent names, consistent formatting, one
canonical home per topic.

## Layout

- **User docs vs internals.** [docs/README.md](../README.md) indexes
  user-facing documentation only; [docs/internals/README.md](README.md)
  indexes contributor documentation. Every page belongs to exactly one of the
  two indexes.
- **One canonical home per topic.** Don't fork a topic across files. If a
  document outgrows its section, split it and leave a link — never a copy.
  Tutorials link to the install guide; they do not re-teach installation.
- **Internal artifacts stay out of `docs/`.** Review reports, working logs,
  and audit findings live in `misc/reviews/`.
- **Runtime API**: `docs/generated/runtime/` (rtgen output) is the canonical
  signature reference. `docs/zannalib/` pages are curated concept guides with
  examples that link into the generated reference. Never hand-edit generated
  files; fix the `@summary`/`@details` fragments in
  `src/il/runtime/defs/` and regenerate.

## Files

- Filenames are **kebab-case**: `native-assembler.md`, not
  `Native_Assembler.md` or `NATIVE_ASSEMBLER.md`.
- Every hand-written page starts with frontmatter:

  ```text
  ---
  status: active
  audience: public | contributors
  last-verified: YYYY-MM-DD
  ---
  ```

  Bump `last-verified` only after actually checking the content against the
  current code — it is a verification stamp, not a modification date.
- ADRs are numbered `NNNN-kebab-title.md`, never reuse a number, start from
  [adr/0000-template.md](../adr/0000-template.md), and get a row in
  [adr/README.md](../adr/README.md).

## Formatting

- **Headings**: no decorative emoji in headings. Functional symbols in body
  text are fine where they carry meaning — `→` in mappings and dataflow
  diagrams, `✅`/`❌` in support matrices.
- **Code fences are always tagged.** Use `zia` for Zia, `basic` for BASIC, `il`
  or `llvm` for IL, `sh`/`bash` for shell, `powershell` for PowerShell, plus
  `cpp`, `c`, `cmake`, `json`, `ebnf`, and `text` for program output, diagrams,
  grammars, and anything else. The `zia`, `basic`, and `il` tags are what the
  project's own site highlighter recognizes (`zannaweb/`); GitHub renders them
  as plain code blocks.
- **Tables**: escape literal pipes in table cells as `\|` — GitHub splits
  cells on `|` even inside backticks.
- **Diagrams**: ASCII diagrams are fine; Mermaid is also fine (GitHub renders
  it natively, no dependency involved). Pick whichever reads better and keep
  one style within a page.
- **Signatures** in curated runtime docs use high-level types —
  `Push(item)` · `Void(Object)` — not IL types (`void(obj)`). IL-level types
  belong in IL docs and in the generated reference's runtime-target column.

## Command examples

- The canonical invocation in user-facing docs is the installed driver:
  `zanna run file.zia`, `zanna check`, `zanna -run file.il`.
- Build-tree paths (`./build/src/tools/...`) may appear only in
  troubleshooting sections and contributor docs where running from the build
  tree is the point.

## Tone

- Plain and factual. No superlatives, no maturity claims ("production-ready",
  "complete"), no self-referential edit history ("this document was recently
  reorganized...").
- Project status is **Pre-Alpha**; don't state or imply otherwise.

## Checks

Run `./scripts/check_docs.sh` before a release: it verifies relative links
resolve, frontmatter is present, filenames follow convention, ADR numbers are
unique, and fences are tagged.
