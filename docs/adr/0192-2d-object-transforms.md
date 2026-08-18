---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0192: Optional 2D Scene Object Transforms

## Status

Accepted (2026-07-26)

## Context

`Zanna.Game2D.SceneDocument` objects carry `type`, `id`, integer pixel
`x`/`y`, an optional parent link, and a typed scalar property bag. There is
no first-class notion of rotation, scale, mirroring, tinting, or pivot:
Studio draws every object sprite at exactly one authored cell, games that
want a flipped or rotated placement encode it ad hoc in gameplay
properties, and the editor cannot offer Unity-style rect-tool editing
because the model cannot express its result. Bringing the 2D editor to
parity (sprite-true rendering, rotate/scale handles, pixel-accurate
hit-testing) requires the document model to own these fields.

The scene JSON format is covered by golden and parity tests
(xenoscape-scenes, ashfall-scenes) that pin byte-stability, so any
extension must leave existing documents' serialized bytes untouched.

## Decision

### Model (additive, optional, default-invisible)

Each scene object gains optional transform fields with these defaults:

| Field | JSON key | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| rotation | `rotation` | float | `0.0` | Degrees clockwise about the pivot |
| scale x | `scaleX` | float | `1.0` | Horizontal scale factor |
| scale y | `scaleY` | float | `1.0` | Vertical scale factor |
| flip x | `flipX` | bool | `false` | Mirror horizontally about the pivot |
| flip y | `flipY` | bool | `false` | Mirror vertically about the pivot |
| tint | `tint` | int | `0xFFFFFFFF` | RGBA multiply color (white = none) |
| pivot x | `pivotX` | float | `0.5` | Normalized pivot within the sprite |
| pivot y | `pivotY` | float | `0.5` | Normalized pivot within the sprite |

Semantics:

- Fields are first-class object keys beside `x`/`y`, not property-bag
  entries; the loader consumes them before the property catch-all, so
  gameplay properties named `rotation` cannot exist ambiguously (they are
  claimed by the model exactly like `x` has always been).
- **A field serializes only when it differs from its default.** A document
  that never uses transforms round-trips byte-identically, which keeps
  every existing golden and parity test valid without regeneration.
- Values are sanitized on load: non-finite floats become their defaults,
  rotation is normalized into `[0, 360)`, scale factors are clamped to
  `[-10000, 10000]` excluding the degenerate `0` (which becomes `1`),
  pivots are clamped to `[0, 1]`, and tint is masked to 32 bits.
- `x`/`y` remain integer scene pixels and remain the object's authored
  anchor point; the transform is applied about the pivot of the object's
  rendered sprite footprint. Objects without a sprite render as markers
  and ignore rotation/scale visually, but the fields still round-trip.

### Runtime surface

`rt_scene_editor.cpp` gains typed accessors following the existing
naming (`rt_game_scene_object_*`), each edit-tracked exactly like
`SetObjectPosition`:

- `ObjectRotation(index) -> f64` / `ObjectSetRotation(index, degrees)`
- `ObjectScaleX/ObjectScaleY(index) -> f64` /
  `ObjectSetScale(index, scaleX, scaleY)`
- `ObjectFlipX/ObjectFlipY(index) -> i1` /
  `ObjectSetFlip(index, flipX, flipY)`
- `ObjectTint(index) -> i64` / `ObjectSetTint(index, rgba)`
- `ObjectPivotX/ObjectPivotY(index) -> f64` /
  `ObjectSetPivot(index, pivotX, pivotY)`

Setters sanitize exactly as the loader does, so a document can never hold
values the loader would reject.

### Consumers

- **Studio** renders `editor.sprite` objects at native frame size scaled
  by zoom and object scale, rotated about the pivot, flipped, and tinted;
  selection outlines and hit-testing use the transformed bounds; the rect
  tool writes these fields as one-gesture transactions.
- **`BuildTilemap`/`Game2D` spawn paths** expose the fields to gameplay
  code through the same accessors; the engine does not interpret them
  beyond making them available (games own their sprite pipelines).

## Consequences

- Legacy scenes are byte-stable by construction; only documents that
  author transforms grow the new keys.
- The property-bag namespace loses eight key names to the model. A legacy
  document that used any of them as gameplay properties would see them
  claimed as transforms on load; a diagnostic
  (`scene.schema.invalid_type`) fires when their JSON types don't match
  the field, mirroring `x`/`y` handling. Project scans found no such use.
- Editor features (rect tool, sprite-true picking) become expressible;
  alpha-aware picking stays deferred (recorded in Studio status).

## Alternatives considered

- **Reserved property-bag keys** (`transform.rotation` …): zero schema
  change, but every consumer would re-parse stringly-typed scalars, the
  editor could not trust sanitization, and the keys would leak into the
  generic property UI. First-class fields match how `x`/`y`/`parent`
  already work.
- **A nested `transform` JSON object**: cleaner grouping but a bigger
  serializer delta and a second structural level for diagnostics; flat
  optional keys keep the writer's only-when-non-default rule trivial.
