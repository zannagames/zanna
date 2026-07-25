---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0189: Add a Project Material Library

## Status

Accepted (2026-07-24)

## Context

Materials exist only inside individual scenes. A project palette — Ashfall's
surface classes, a game's canonical metal/rock/foliage set — must be rebuilt
or copy-pasted per scene, and nothing shows the palette at a glance. The
asset-library ADR (0180) established the per-root project-metadata pattern;
what materials need is storage that already round-trips full PBR state with
embedded maps — which is exactly a VSCN file.

## Decision

A workspace root may contain **`materials.scene3d`**: an ordinary 3D scene
whose **top-level nodes each carry one named library material** (node name =
material name, unique case-insensitively; node holds a material and no
mesh). Because it is a plain scene, the full existing pipeline applies —
embedded texture maps, KTX2 sources, VSCN round-trip — and the file is even
openable in the 3D editor as an escape hatch.

The material inspector gains a **Library group**:

- A bounded list of library materials with name and the existing bounded
  thumbnail rendering; missing/invalid library files show a truthful status
  and never publish a partial list (the 1 MB/parse fail-closed discipline of
  ADR 0180, with scene-appropriate byte budgets).
- **Save to Library** stages an independent clone of the selected node's
  material (the existing clone-safe staging), writes it under a chosen
  unique name, and saves the library through a staged temp +
  expected-mtime-guarded atomic replacement (the `asset-library.json`
  transaction model). Overwriting an existing name is an explicit confirm.
- **Apply from Library** clones the chosen library material through the
  existing batch material transaction — per-node independent clones, one
  canonical history entry, exact rollback; unselected sharing-group users
  are untouched. The scene never references the library file at runtime:
  application always copies, so scenes stay self-contained and games need no
  library awareness.
- Library edits never touch scene content, history, revision, or dirty
  state; external changes to the library are detected on the existing
  bounded polling cadence and reload the list.

Ownership follows the ADR 0160/0180 root rules: a scene consults the library
of its owning workspace root; unsaved scenes use one only in single-root
workspaces.

## Consequences

- Project palettes become visible, reusable, and versionable; Ashfall's
  surface classes ship as a seeded library the recreation and Studio share.
- Zero new formats: the library is a scene, the transaction model is proven,
  thumbnails and clone-safety are reused.
- Applying copies (rather than references) trades automatic palette-wide
  updates for self-contained scenes; palette-wide updates remain possible by
  reapplying, and reference semantics can layer on later via ADR 0187-style
  references if ever justified.
- A probe must pin: fail-closed listing, save/overwrite naming rules, atomic
  conflict-guarded writes, apply-as-batch-clone semantics with rollback, and
  scene-neutrality of library operations.

## Alternatives Considered

- **A JSON material format.** Rejected: it would re-encode PBR state and
  embedded maps that VSCN already serializes losslessly.
- **Library references from scenes.** Rejected for v1: runtime consumers and
  packaging would need library resolution; copies keep scenes self-contained
  today without foreclosing references later.
- **Per-scene "material palettes" section.** Rejected: the point is sharing
  across scenes; per-scene storage is what exists already.
