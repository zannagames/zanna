---
status: active
audience: contributors
last-verified: 2026-08-03
---

# ADR 0234: Studio Prefab Extraction, Placed Imports, and Play-Input Completion

## Status

Accepted (2026-08-03)

## Context

The 2026-08-03 full-stack review
(`docs/internals/graphics3d-deep-review-2026-08.md`) identified the Studio 3D
editor's three highest-impact workflow holes:

- **Prefabs were consume-only.** Import as Instance (ADR 0187) places
  references to existing `.scene3d` files, but a user who assembles a
  lamppost in-scene could not promote it to a reusable asset without
  hand-authoring a second file — the core engine-editor loop was broken at
  its most common step.
- **Asset drops ignored the drop point.** Material drops resolve the pointer
  to a node; asset drops discarded the coordinates and merged imports at
  their source origin, so you could not drop a prop where you pointed.
- **Play-mode input was left-button-only.** The embed channel protocol
  already carries wheel events (`kind 4`, `c = deltaY*120`) and a button
  index in the down/up events' third field, but the play controller always
  sent button 0 and never forwarded wheel motion; its 21-key allow-list also
  omitted common gameplay verbs (pause, map, save/load function keys).

## Decision

### Create Prefab from selection

The instance inspector row gains **Create…** (`CreatePrefabFromSelection`,
a neutral dispatch slot): it saves the selected nodes as a new `.scene3d`
beside the document and replaces them with one VSCN v7 instance reference —
the exact inverse of Import as Instance.

- Content is extracted from an independent copy of the canonical document
  (`LoadGraphFromContent(currentSerialized)`), never the live graph, and is
  re-centered on the primary node's pivot so the file is reusable; the
  replacement instance node takes that pivot, so hydrated content reappears
  exactly in place after the post-commit reload.
- The file is named from the sanitized primary node name (`guard_crate.scene3d`,
  numbered on collision) and written through `safe_io.WriteAllText`.
- One undoable transaction; every failure path restores the document
  (`RollBackGroupEdit`) and removes the written file. Undo restores the
  prior canonical bytes exactly (the extracted file stays on disk).
- **Exact-or-reject scope (v1):** only root-level, non-instance selections
  extract. Nested-parent extraction (which would require reparenting
  transforms) and extracting instance references/content are refused with
  explicit messages rather than approximated. Per-descendant overrides and
  nested extraction remain ADR 0187/0187-follow-up material.

### Placed imports (drop-at-point)

`ImportAssetPathAt(path, useSpawn, x, y, z)` joins the contract beside
`ImportAssetPath` (which now delegates with no spawn). The viewport drop
pump resolves the pointer exactly as material drops do and routes through
`ViewportSpawnPoint` (precise surface hit → ground plane → view target), so
a dropped asset lands where the user pointed: merged roots translate so
their combined world-bounds center lands on the spawn XZ and the bounds
floor sits at the spawn height; meshless imports move their first root's
pivot instead. Menu imports and the probe-facing `ImportAssetDrop`
keep source-authored placement; placement happens inside the single import
transaction.

### Play-input completion

The play controller now uses the protocol it always had:

- All three mouse buttons edge-forward with their index in the event's
  button slot (previously hardwired to 0).
- Wheel motion forwards as `EVENT_WHEEL` with the contract's
  `deltaY*120` encoding.
- The tracked-key set grows from 21 to 32: digits 4–5 and the common
  gameplay verbs P/M/I/C/X/Z/V/G/T plus F5/F9, chosen so the shipped
  demos (pause, save/load) are playable embedded. Full-keyboard and
  text-input forwarding remain future work (the protocol's `TEXT` event is
  still unused).

## Consequences

- No runtime or protocol changes: the embed channel contract was already
  sufficient; only the Studio side changed.
- Probes extended in place: `scene_prefab_instance_probe` gains the
  extraction round trip (file plainness, reference-only document bytes,
  hydration, byte-exact undo, instance-extraction refusal);
  `scene_dnd_probe` gains the placed-import assertion;
  `embed_channel_probe` pins wheel and buttoned-event round trips.
- `docs/../workflows.md` documents the create-extract-edit-reload loop.

## Alternatives considered

- **Extract nested selections by reparenting transforms into the prefab.**
  Deferred: correct handling of rotated/scaled ancestor frames belongs with
  the group-pivot work; v1 refuses rather than approximates (ADR 0166's
  exact-or-reject convention).
- **A save-as dialog for the prefab name.** Rejected for v1: the primary
  node's name is already the user's chosen label; renaming the file is a
  one-step explorer action, while a modal in the middle of the flow costs
  more than it adds.
- **Forwarding the full keyboard to embedded games.** Rejected for now: the
  bounded set exists to keep the per-frame poll cost fixed and predictable;
  32 keys cover the shipped demos, and the set is one list to extend.
