---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0222: Schema v19 — Typed Scene Settings Forms and Labeled Enums

- Status: Accepted
- Date: 2026-07-29
- Deciders: Zanna Studio maintainers
- Tags: zannastudio, scene-editors, schema, authoring

## Context

The most important data in a real game scene is edited as raw metadata.
Ashfall's mission-01 carries 78 root keys — `lenv.*` sky/sun/fog floats,
`af.*` identity/start/water, `obj.*` objectives — all authored through a
generic key|kind|value grid with no grouping, pickers, sliders, labels, or
bounds. Enum-like fields author bare integers; color rigs author as nine
separate float rows. The `scene-components.json` schema (v18) already
proves the pattern for object/node components: bounded, whole-file-reject,
presentation-only.

## Decision

Schema version 19 adds a `sceneComponents` array of **scene-level typed
components**, plus richer field presentation kinds. Everything is
additive, bounded, and presentation-only: stored scene scalars keep their
v1 kinds and byte formats, so parity gates stay byte-green and a scene
authored through forms is indistinguishable from one authored raw.

### Scene-level components

`{"name", "label", "target": "3d-scene" | "2d-scene", "description",
"fields": [...], "repeat"?: {...}}` — at most 32. 3D targets bind fields
to scene **root metadata**; 2D targets bind to **scene properties**. The
editors render each component as a grouped, labeled form section in a
"Scene Settings" group above the raw grid; the raw grid remains
authoritative for undeclared keys.

### Field kinds (beyond string/int/float/bool/enum)

- `color` — one picker bound to three unit-float keys (`keys: [R,G,B]`)
  or one packed-int key (single `key`).
- `float`/`int` with `min`/`max`/`step`/`slider`/`unit` — bounded sliders
  with a live value label; sliders settle-commit (one history entry per
  scrub, ADR 0221).
- `angle` with `storageUnit: degrees|radians` — displays degrees,
  serializes per storage unit.
- `vec2`/`vec3` — labeled X/Y/Z inputs over a `keys` array (exact arity).
- Labeled enums — `choices` mixes legacy strings with
  `{"value","label"}` objects; optional `storedKind: "int"` validates all
  values are integers and stores int scalars. Applies to object/node
  component enums too (parsed into `choiceLabels` parallel to `choices`).
- `ref` with `{"kind": position-xz|metadata-int|node-name|object-id,
  "match"}` — a picker affordance over ordinary scalar writes;
  presentation only.
- `group` — sub-header text; consecutive fields under one group render
  beneath one heading.

### Bounded repeat groups

`"repeat": {"countKey", "indexPattern" (contains "{i}"), "max" ≤ 32,
"itemLabel"}` — fields use keys relative to the expanded prefix
(`obj.{i}.` + `kind` → `obj.3.kind`). The editor renders one form section
per current item (count read from `countKey`, clamped to `max`), plus
"Add <item>" and "Remove Last" verbs; add writes the item's defaulted
keys and the incremented count in **one transaction**, remove deletes the
last item's keys and decrements the count in one transaction. Arbitrary-
index removal is deferred (recorded here): last-only keeps key compaction
trivial and undo exact.

### Commit contract

Forms follow ADR 0221: pickers/checks/dropdowns commit on change, text on
Enter/blur, sliders on settle. One gesture = one undoable transaction
(multi-key rows write all their keys in that one transaction). Browsing
never dirties a document; programmatic refresh consumes its own edges.

### Mechanics

New parser layers `scene_component_schema_forms{,_model}.zia` extend the
layered parser (the v18 model and authoring files are at their 1,000-line
ceiling and gain only the schema fields). The shared renderer
`scene_component_form.zia` builds typed rows on the ADR 0221 kit;
`scene_settings_inspector_{3d,2d}.zia` bind it to root metadata / scene
properties and report keyed write batches the editors apply through their
existing transactional paths.

### Proven on the games

- ashfall-scenes: Scene Settings covering all 78 mission root keys —
  Mission identity, Player Start (yaw as angle), Mission Ambient (color),
  Water, Terrain Offset, Sky & Light Rig (five color swatches, intensity
  and cloud sliders, direction vectors, fog, flags), and an Objectives
  repeat group (labeled kind enum stored as int); enemy-spawn
  `spawn.archetype` gains archetype name labels.
- xenoscape-scenes: Region settings (2d-scene: region id, labeled theme
  enum stored as int, player start vec2) and labeled
  `interaction.type` choices.

## Consequences

- Opening mission-01 shows grouped labeled forms with swatches and
  sliders instead of a 78-row grid; scene bytes are untouched until a
  real edit, and every edit is one undoable entry.
- Unknown/malformed v19 declarations reject the whole schema file with
  one message (house rule), never partially apply.
- Object-side labeled-enum **rendering** in the generic metadata grid is
  deferred; v19 parses and carries the labels so the typed object form
  work (P7 colliders pass) can adopt them without a schema change.
- `ref` pickers draw on the existing overlay layers; `position-xz` pairs
  are edited as vec2 until the viewport pick flow lands.
- Forms re-read values whenever the document revision changes (undo/redo
  and API edits stay live); an uncommitted text draft can therefore be
  re-baselined by a concurrent edit from another surface — same behavior
  class as the raw grids.
- The repeat add/remove transaction contract is probed through the exact
  commit batches the verbs produce; real-pointer clicks on the 3D right
  inspector are not probeable at the 1120-px probe window because the 3D
  editor's intrinsic width exceeds it (pre-existing; recorded for the P8
  layout pass).

## Links

- `docs/adr/0221-live-commit-inspector-contract.md`
- `src/zannastudio/src/ui/scene_component_schema_forms.zia`
- `src/zannastudio/src/ui/scene_component_form.zia`
- `examples/games/ashfall-scenes/scene-components.json`
