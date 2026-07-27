---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0197: Add Project-Owned 2D Object Preview Profiles

## Status

Accepted (2026-07-26)

## Context

ADR 0179 lets an individual 2D scene object opt into a sprite preview with
`editor.sprite` and `editor.frame`. That is useful for one-off objects and asset
drop, but it does not scale to a game whose scene files contain hundreds of
typed spawn records. Repeating the same atlas path on every enemy, pickup, and
interaction bloats scene JSON, creates noisy diffs, and leaves existing scenes
as anonymous editor markers even though their project already has stable type
and property conventions.

A per-user Studio mapping is not acceptable: collaborators and CI must see the
same authored scene. Executing arbitrary game code in the editor is also not a
safe or deterministic preview contract. The project needs a bounded declarative
bridge between its runtime-facing object records and editor-facing art.

## Decision

### Component-schema version 3

`scene-components.json` version 3 adds an optional top-level
`objectPreviews` array. Each entry contains:

- `objectType` (required string): the case-insensitive scene object type.
- `sprite` (required string): an image reference resolved by the existing
  scene-image rules.
- `frame` (optional non-negative int, default `0`): the fallback atlas frame.
- `frameProperty` (optional portable property key): an integer object property
  used to choose a variant.
- `frameOffset` (optional bounded int, default `0`): added to the property value.
- `frameMap` (optional array of at most 256 non-negative frame ints): maps a
  zero-based property value to a non-linear atlas frame. It is mutually
  exclusive with `frameOffset`.

At most 128 preview definitions are accepted and object types are
case-insensitively unique. `frameOffset` and `frameMap` require
`frameProperty`. Invalid, duplicate, oversized, or wrong-version content rejects
the complete schema without publishing partial preview rules, matching the
existing component-schema failure policy.

### Resolution and precedence

Studio resolves an object's preview in this order:

1. authored `editor.sprite` plus `editor.frame`;
2. authored `sprite` plus `editor.frame`;
3. the project rule matching the object's `type`;
4. the existing generic marker.

For a project rule, a missing or non-integer `frameProperty`, an out-of-range
`frameMap` index, or an invalid computed frame uses the rule's fallback
`frame`. Image decode, cache, pixel, and external-refresh budgets remain shared
with layer imagery exactly as in ADR 0179. Resolving a rule never mutates the
scene, document revision, dirty state, or history.

### Project ownership for explicitly opened scenes

When a saved scene is outside the currently open workspace, Studio walks its
ancestor directories under a fixed bound and recognizes the nearest
`zanna.project`. That root supplies project-local scene authoring configuration
and an honest external-project status label. It does not silently replace or
mutate the user's open workspace. Untitled scenes may still use the sole open
workspace root.

## Consequences

- Existing game scenes gain consistent sprite previews without per-object
  editor metadata or runtime changes.
- Preview rules travel with the project and render identically for every
  collaborator.
- A game may bake deterministic preview atlases from the same procedural art it
  uses at runtime; Studio only consumes ordinary bounded images.
- Component schemas remain backward compatible: versions 1 and 2 publish no
  project preview rules.
- Studio probes must pin schema validation, precedence, property-driven frame
  selection, marker fallback, external-project discovery, shared image budgets,
  and the invariant that rendering does not dirty scene content.

## Alternatives Considered

- **Write `editor.sprite` onto every generated object.** Rejected because it
  duplicates project-wide truth throughout scene files and makes art-path
  changes produce large unrelated diffs.
- **Keep a mapping in user settings.** Rejected because a scene would look
  different across machines and the mapping would not travel with source.
- **Load and execute the game's renderer inside Studio.** Rejected because game
  startup side effects, arbitrary code, frame timing, and runtime-only state are
  not a deterministic authoring contract.
- **Add preview metadata to the runtime scene format.** Rejected because the
  concern is project authoring presentation; the existing project schema is the
  appropriate zero-runtime-impact boundary.
