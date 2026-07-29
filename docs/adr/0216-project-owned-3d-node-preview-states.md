---
status: active
audience: contributors
last-verified: 2026-07-28
---

# ADR 0216: Add Project-Owned 3D Node Preview States

## Status

Accepted (2026-07-28)

## Context

ADR 0199 and ADR 0215 let Studio replace canonical gameplay marker nodes with
transient project prefabs or imported models. Showing every matching marker is
useful for static dressing, but it is misleading for stateful encounters.

Ashfall missions contain several waves in one canonical scene. The running game
activates one wave at a time, while Studio previously rendered the enemy art
for all four waves simultaneously. Mission 01 therefore showed twenty enemies
where the opening game frame showed five. The scene remained technically
editable, but the viewport no longer represented a state the player could see.
The extra art also made selection, framing, and the already-dense command bar
harder to read.

Studio cannot infer a project's state vocabulary or execute project code to
discover it. Hiding canonical nodes would also be wrong: later waves must
remain available in the hierarchy and their metadata must remain editable. A
project needs a bounded declarative way to expose meaningful preview states,
and Studio needs to remember the chosen state without writing presentation
choices into VSCN or history.

## Decision

### Component-schema version 18

`scene-components.json` version 18 adds one optional
`nodePreviewFilter3D` object:

```json
{
  "version": 18,
  "components": [],
  "nodePreviews3D": [
    {
      "matchProperty": "spawn.archetype",
      "matchValue": 0,
      "prefab": "assets/enemies/grunt.glb"
    }
  ],
  "nodePreviewFilter3D": {
    "label": "Wave",
    "allLabel": "All waves",
    "matchProperty": "spawn.wave",
    "defaultIndex": 1,
    "states": [
      {"label": "Wave 1", "matchValue": 0},
      {"label": "Wave 2", "matchValue": 1}
    ]
  }
}
```

The object requires a non-empty `label`, a portable `matchProperty`, and one to
32 states. `allLabel` is optional and defaults to `All`. Display labels contain
one to 64 characters and are unique without regard to case, including the All
label.

Each state requires a label and an exact Boolean, integer, or string
`matchValue`. Fractional numbers are invalid. Duplicate kind/value identities
reject the complete schema. `defaultIndex` is optional and selects the explicit
All option at index zero or a configured state at indices one through N. It
defaults to All and must stay within that range.

The filter requires at least one `nodePreviews3D` rule. Using it in a schema
before version 18, supplying a wrong type, unsafe property, duplicate
label/value, empty or oversized state list, or invalid default rejects the
complete schema and publishes no partial preview state.

### Filter disposable art, not authored data

All remains an explicit first option. For a configured state, a canonical node
that has the filter property contributes node-preview art only when both its
metadata kind and value match that state. A node without the property remains
visible. This lets an encounter filter suppress inactive enemy waves without
hiding props, pickups, or other unrelated previews.

The selector applies only while rebuilding the disposable node-preview graph.
It does not filter scene-level environment prefabs, additive environment
layers, runtime water, canonical meshes, or render-only material overlays. It
does not change the canonical hierarchy, node visibility, selection, metadata,
VSCN bytes, dirty state, revision, or undo/redo history. A suppressed owner
increments a separate filtered count and does not increment missing/fallback
counts.

Changing the option discards and rebuilds transient node art through the same
bounded asset path. Transform following, visual bounds, surface placement, and
picking continue to operate only on the active wrappers and still remap to
their canonical owners.

### Per-document responsive presentation

The selected index belongs to `Scene3DWorkspaceState`, follows the owning
document across tab changes, and is restored from the bounded session record.
A new or older session uses the project's `defaultIndex`; a saved index outside
the active schema is clamped back to that default. Reloading or editing the
schema updates the option list and rebuilds previews without changing the
scene.

Studio presents the selector contextually in the 3D view row only when the
active project declares it. Wide layouts show the project label beside the
dropdown; narrower layouts retain the dropdown and omit the redundant label.
The viewport status uses a short active-state, preview, direct-model, and
filtered summary. Detailed camera, material, fallback, missing-resource, and
render information remains available in the wrapped status tooltip and
accessibility description rather than expanding permanent chrome.

### Ashfall

Ashfall declares Wave 1 through Wave 4 over the exact integer `spawn.wave`
metadata and defaults to Wave 1. Nodes without `spawn.wave` remain present.
Mission 01 now opens with five direct enemy rigs, the same active encounter as
the game, while All waves remains one selection away for encounter-wide layout
work.

## Consequences

- Stateful scenes can show one real gameplay state instead of an impossible
  union of every transient actor.
- Canonical inactive-state nodes stay available for hierarchy and metadata
  authoring.
- The default view contains fewer disposable assets, improves visual clarity,
  and reduces preview loading/render work.
- The selector adds one contextual control without returning secondary camera
  or creation actions to permanent chrome.
- Version 18 is bounded, cross-platform, zero-dependency, and additive.
- The decision adds no runtime C ABI, IL, VSCN, workflow, dependency, or
  platform-adapter change.
- Probes must cover version gating, typed state identity, bounds, structured
  authoring preservation, per-document/session ownership, All/state switching,
  filtered counts, responsive controls, real Ashfall counts, and exact
  canonical content/history isolation.

## Alternatives Considered

- **Hide canonical nodes for inactive states.** Rejected because preview state
  must not remove authored data from the hierarchy or selection.
- **Put active wave in scene metadata.** Rejected because the choice is an
  editor session preference, not gameplay source data.
- **Infer waves from Ashfall property names.** Rejected because state names,
  discriminants, defaults, and values belong to each project.
- **Execute project code to ask for active actors.** Rejected because Studio
  previews must remain bounded, deterministic, and safe to open.
- **Render every state and rely on visibility toggles.** Rejected because it
  preserves the original misleading default and asks users to mutate authored
  visibility for a presentation-only question.
