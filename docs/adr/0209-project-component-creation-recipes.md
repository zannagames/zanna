---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0209: Add Project Component Creation Recipes

## Status

Accepted (2026-07-27)

## Context

Project scene components originally described typed fields that Studio could
add to an existing 2D object or 3D node. That made recurring gameplay metadata
safer, but the author still had to create a generic placeholder, choose or type
its runtime identity, place it, select the component, and run **Add Missing**.
The intermediate placeholder was a real history entry and could temporarily be
invalid for the game.

This was especially visible in the example projects. A Xenoscape enemy is an
object whose canonical type is `enemy`; an Ashfall enemy spawn is a named node
whose `af.kind` discriminator is `spawn`. The component labels alone are not a
safe way to infer either contract. Projects may name a component for authors,
reuse it across targets, or intentionally provide an augment-only component
such as Ashfall's collider template.

Creation belongs beside the project component fields. It is project-wide,
declarative authoring data, while placement and canonical transactions remain
the responsibility of each scene editor.

## Decision

### Component-schema version 13

`scene-components.json` version 13 adds two optional members to each component:

```json
{
  "name": "enemy-spawn",
  "target": "2d-object",
  "createObjectType": "enemy",
  "fields": [
    {"key": "enemy.type", "type": "int", "default": 0}
  ]
}
```

```json
{
  "name": "enemy-spawn",
  "target": "3d-node",
  "createNodeName": "Enemy Spawn",
  "fields": [
    {"key": "af.kind", "type": "string", "default": "spawn"}
  ]
}
```

- `createObjectType` is a trimmed portable identifier of at most 128
  characters and requires a `2d-object` or `both` target.
- `createNodeName` is a trimmed, non-empty display name of at most 128
  characters and requires a `3d-node` or `both` target.
- Either member in a schema older than version 13 rejects the complete schema.
- Neither member is inferred from `name`, `label`, fields, or preview rules.
- A component may omit the relevant member and remains an augment-only
  template. A `both` component may declare either or both recipes.

The structured component form exposes both members and enables only those
compatible with the selected target. Changing a component to a single-target
definition removes the now-inapplicable recipe. New starter components receive
an immediately usable recipe for each supported target. Structured edits
preserve unknown supported-version data and infer version 13 while any creation
member remains.

### Selection-free palette action

The project component palette remains visible on the Object inspector tab even
when no object or node is selected. A component with a recipe enables a
target-specific **Create Object** or **Create Node** action. **Add Missing** and
raw field editing remain selection-dependent, so the UI does not imply that an
augment-only component can create content.

Creation is explicit user intent. Loading a project, selecting a component, or
refreshing a schema never creates scene content.

### Atomic 2D creation

The 2D editor creates the canonical object with `createObjectType`, a unique
stable identifier derived from the component name, and a placement chosen in
this order:

1. the selected valid scene cell;
2. the center visible canvas cell, clamped to scene bounds.

It writes and verifies every typed component default before one canonical
commit. Strings, exact integers, finite floats, Booleans, explicit nulls,
enums, and asset strings retain the same kinds as component application. The
new object becomes the sole selection. Any field refusal or commit failure
restores the complete prior scene and selection.

### Atomic 3D creation

The 3D editor creates a top-level node named from `createNodeName`, adding a
numeric suffix only when needed for scene uniqueness. Its local position is the
current viewport target. Every typed metadata default is written and verified
before the node enters the live graph; the graph is then serialized once and
the node becomes the sole selection.

Creation is disabled during lightmap baking and at the editor's bounded node
limit. A field, allocation, or serialization failure publishes no partial
history state. Project node-preview matching runs after the hierarchy refresh,
so a discriminator such as `af.kind=spawn` displays its project prefab in the
same action.

### History and compatibility

One successful recipe creation adds exactly one ordinary scene undo snapshot.
Undo removes the complete gameplay-ready item; redo restores its exact
canonical bytes. There is no visible or undoable generic intermediate item.
Existing schemas through version 12 and version-13 components without recipes
retain their previous apply-only behavior.

No runtime API, runtime C ABI, IL, canonical 2D scene format, VSCN format,
external dependency, or platform-specific branch changes are required.

## Validation

The component-schema probe covers valid target-specific and `both` recipes,
older-version use, wrong JSON kinds, incompatible targets, empty names, bounds,
and fail-closed publication. The structured-authoring probe covers starter
recipes, edits, target normalization, version inference, and preservation
through unrelated field edits.

The real Xenoscape preview probe creates an `enemy` at a chosen canvas cell,
verifies its exact typed default and project draw-stack preview, then proves
one-step undo/redo and byte-identical restoration. The real Ashfall preview
probe creates an `Enemy Spawn` at the viewport target, verifies every runtime
metadata discriminator/default and immediate prefab preview, then proves the
same exact history behavior.

## Consequences

- Common gameplay entities become one-click authoring actions instead of
  multi-step generic placeholders.
- Newly created content is valid for the project's loader from its first
  canonical history state.
- Project authors explicitly control runtime identity without Studio guessing
  from presentation labels.
- Augment-only components remain honest and cannot accidentally create empty
  nodes or meaningless objects.
- New projects that use the structured form start with a usable creation path;
  projects may remove a recipe whenever application-only behavior is desired.

## Alternatives Considered

- **Infer creation from the component name or label.** Rejected because
  presentation names are not runtime contracts and may change independently.
- **Infer creation from preview rules.** Rejected because many gameplay items
  have no preview, multiple rules may match one item, and preview data is
  intentionally presentation-only.
- **Create a generic item and apply the component as a second transaction.**
  Rejected because undo exposes an invalid intermediate state and failures can
  leave partial content.
- **Require a prefab for every component.** Rejected because metadata-only
  gameplay markers are first-class content and should not require geometry or
  a new runtime capability.
- **Put recipes in canonical scene files.** Rejected because they are
  project-wide authoring conventions, not per-scene runtime data.
