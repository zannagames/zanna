---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0176: Make Tile Behavior Sections Fully Authorable

## Status

Accepted (2026-07-24)

## Context

Canonical 2D scene JSON already carries four tile-behavior sections —
`collision`, `tileProperties`, `animations`, and `autotiles` — and
`SceneDocument.BuildTilemap()` already applies them to the returned `Tilemap`
(`applyPreservedTilemapSections` in `rt_scene_editor.cpp`). The Tiled importer
writes them. But `SceneDocument` exposes no read or write API for any of them:
they load as opaque preserved sections. Zanna Studio therefore cannot author
tile solidity, one-way platforms, per-tile data, animations, or autotile
rules, and a game that wants tile behavior must register it in code the way
Xenoscape's `registerSolidTiles()` and `registerTileAnims()` do — data the
scene file was designed to carry.

Closing this gap requires no new schema. It requires promoting the existing,
already-consumed shapes to typed document state with the same exact-kind,
bounded, transactional discipline that scene and object properties follow
(ADR 0158, ADR 0174).

## Decision

`SceneDocument` parses the four behavior sections into typed document state on
load and serializes them canonically in `ToJson()`. The accepted shapes are
exactly the shapes `BuildTilemap()` consumes today. New runtime methods (C ABI
`rt_game_scene_*`, registered in `game_ui.def`):

```zia
// Collision (kinds: 0 none, 1 solid, 2 one-way-up)
TileCollision(tile) -> Integer
SetTileCollision(tile, kind)
CollisionTiles() -> Seq            // ascending tile IDs with non-none kinds
CollisionLayer() -> Integer
SetCollisionLayer(layer)

// Per-tile typed properties (exact kinds: int, bool)
TilePropertyKind(tile, key) -> String   // "int", "bool", or ""
TilePropertyInt(tile, key, default) -> Integer
TilePropertyBool(tile, key, default) -> Boolean
SetTilePropertyInt(tile, key, value)
SetTilePropertyBool(tile, key, value)
RemoveTileProperty(tile, key)
TilePropertyKeys(tile) -> Seq           // lexicographic
TilePropertyTiles() -> Seq              // ascending tile IDs with properties

// Tile animations (per-frame durations, milliseconds)
SetTileAnim(baseTile, frames, durationsMs)   // Seqs of equal length
TileAnimFrames(baseTile) -> Seq
TileAnimDurations(baseTile) -> Seq
RemoveTileAnim(baseTile)
TileAnimBases() -> Seq                  // ascending base tile IDs

// Autotiles (exactly 16 variant tile IDs per rule)
SetAutotileRule(baseTile, variants)
AutotileVariants(baseTile) -> Seq
RemoveAutotileRule(baseTile)
AutotileBases() -> Seq                  // ascending base tile IDs
```

Bounds and validation, enforced before mutation:

- Tile and variant IDs are `1..4095`, matching `Tilemap`'s behavior-table
  addressability; collision kind is `0..2`. Invalid IDs and kinds are
  edit-time warning no-ops, consistent with existing edit rejections.
- Property keys follow the existing 128-byte key limit; at most 64 properties
  per tile and 4,096 tile-property entries per document. Values are exact-kind
  `int` or `bool` and round-trip without coercion.
- Animations have 1..4,096 frames; every duration is a positive integer; the
  `durations` list length equals the `frames` length. Legacy records using
  uniform `frameCount`/`msPerFrame` load by expanding to per-frame durations.
- Autotile rules have exactly 16 variants.
- Setting a collision kind of none, an empty property set, or removing the
  last animation/autotile entry removes the record; empty sections are omitted
  from JSON entirely.

Canonical serialization is deterministic: sections emit in the existing fixed
rich-section order, records in ascending tile-ID order, property keys in
lexicographic order, and animations always emit per-frame `durations`.
Unrecognized members inside a behavior section root or record are retained
verbatim and re-emitted, mirroring the unknown-member preservation contract of
`scene-components.json` (ADR 0160), so no authored data is silently destroyed.
A document that is loaded and never edited is never re-serialized, so legacy
files are not rewritten by mere opening; the first accepted edit canonicalizes
the document as usual.

`BuildTilemap()` switches from re-parsing preserved JSON to reading the typed
state. Its observable behavior is unchanged and remains pinned by existing
tests. Malformed entries that `BuildTilemap()` silently skipped now also
record warning diagnostics at load, and are dropped from typed state exactly
where they were previously dropped from the tilemap.

Studio's transaction contract: each accepted tile-behavior edit refreshes
canonical content once and creates one history entry; exact no-ops create no
history. This is the same rule object properties follow.

## Consequences

- Studio can grow a tile-behavior inspector (collision, properties,
  animations, autotiles) without parsing JSON in interpreted UI code.
- Games can replace code registries with authored scene data; `BuildTilemap()`
  applies behavior automatically, and typed readers make behavior inspectable.
- Loading a legacy uniform-duration animation and saving after an edit
  canonicalizes it to per-frame durations; consumers see identical playback.
- Runtime tests must pin typed round-trips, legacy-shape loads, canonical
  ordering, unknown-member retention, every bound above, and
  `BuildTilemap()` equivalence between preserved-JSON and typed-state paths.
- `docs/zannalib/game/scene.md` documents the sections as first-class instead
  of preserved-only.

## Alternatives Considered

- **Keep hand-editing the JSON sections.** Rejected: no validation, no undo
  integration, and Studio would need a raw JSON editor inside a visual tool.
- **Author behavior only through Tiled import.** Rejected: it makes an
  external editor mandatory for a first-party format that already defines the
  data.
- **Store behavior in scene-level properties.** Rejected: the sections exist,
  are consumed by `BuildTilemap()`, and are written by the importer; a second
  representation would create silent divergence.
- **Per-layer behavior tables.** Rejected: `Tilemap`'s behavior table is
  map-global; inventing per-layer semantics the renderer cannot honor would
  make the editor lie.
