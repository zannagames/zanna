---
status: active
audience: users and contributors
last-verified: 2026-07-23
---

# Project Scene Components

Zanna Studio can turn recurring gameplay data into reusable typed templates for
2D objects and 3D nodes. Put an optional `scene-components.json` file at the
root of a Studio workspace:

```json
{
  "version": 1,
  "components": [
    {
      "name": "enemy-spawn",
      "label": "Enemy Spawn",
      "target": "both",
      "description": "Common enemy placement fields.",
      "fields": [
        {
          "key": "enemy.archetype",
          "label": "Archetype",
          "type": "string",
          "default": "grunt"
        },
        {"key": "enemy.level", "type": "int", "default": 1},
        {"key": "enemy.radius", "type": "float", "default": 4.0},
        {"key": "enemy.active", "type": "bool", "default": true},
        {"key": "enemy.target", "type": "null"}
      ]
    },
    {
      "name": "checkpoint",
      "label": "Checkpoint",
      "target": "3d-node",
      "fields": [
        {"key": "checkpoint.id", "type": "string", "default": "start"},
        {"key": "checkpoint.enabled", "type": "bool", "default": true}
      ]
    }
  ]
}
```

Open a `.scene`, `.level`, or `.vscn` file inside that workspace. The
**Project components** inspector lists definitions compatible with the active
editor.

## Authoring The Schema

Press **Edit Schema** in either scene editor to open the structured project-file
form. The application picker above it stays target-filtered, but the authoring
dropdown shows every definition in `scene-components.json`, including 2D-only
components while a 3D scene is active and vice versa.

The form supports:

- **New Component**, component **Earlier**/**Later**, stable name, label,
  `target`, description, **Save Component**, and **Delete Component**.
- **New Field**, field **Earlier**/**Later**, stable key, label, scalar type,
  typed default, description, **Save Field**, and **Delete Field**.
- **Undo Schema** and **Redo Schema** for the last 20 accepted project-file
  transitions.

When the file is absent, **New Component** creates a complete valid
`scene-components.json` with a starter Boolean field. Each later action writes
one complete, parser-validated file state. Invalid identifiers, duplicate
names/keys, wrong typed defaults, non-finite numbers, the last-field deletion,
and format limits are rejected without changing disk.

Schema edits are separate from scene edits: they do not change scene bytes,
scene revision, dirty state, or scene undo/redo. Removing or renaming a
component or field also does not remove or rename values already authored in
scenes.

Studio preserves unknown version-1 JSON members on every retained root,
component, and field object, although an accepted structured edit
pretty-formats the complete file. Existing files use an atomic rooted
replacement guarded by expected metadata and exact current bytes; missing files
use a no-overwrite atomic create. If another tool changes the file, Studio
rejects the local write or history action and asks for **Reload**. A bounded
background check normally notices external changes automatically, reloads
them, and clears history that no longer matches disk.

## Applying A Component

Select one or more 2D objects or 3D nodes, choose a component, and press
**Add Missing**. Studio applies the whole selection as one undoable action:

- Missing fields receive their exact typed defaults.
- Existing fields of the same type keep their current values.
- Any same-name field with a different type rejects the complete operation.
- Reapplying an already-complete component does not create history.
- A failed runtime write or scene serialization restores the prior document.

Select a field and press **Edit Field** to copy it into the ordinary typed
property/metadata controls. A matching value on one selected item is retained
as the draft; otherwise the schema default is used. The 3D raw editor requires
exactly one node, while Add Missing supports a node group.

Use **Reload** to accept an external edit immediately. Studio also checks for
external changes periodically and automatically changes schema ownership when a
scene moves to another workspace root.

## Format Reference

| Member | Required | Contract |
| --- | --- | --- |
| Root `version` | Yes | Numeric integer `1` or `2`. |
| Root `components` | Yes | Array with at most 128 entries. |
| Component `name` | Yes | Stable portable identifier, at most 64 characters; unique without regard to case. |
| Component `target` | Yes | `2d-object`, `3d-node`, or `both`. |
| Component `label` | No | Non-empty display text, at most 128 characters; defaults to `name`. |
| Component `description` | No | Display text, at most 1,024 characters. |
| Component `fields` | Yes | Between 1 and 64 fields. |
| Field `key` | Yes | Portable scene-data key, at most 128 characters; unique within the component. |
| Field `type` | Yes | `string`, `int`, `float`, `bool`, or `null`; version 2 adds `enum` and `asset`. |
| Field `choices` | For `enum` | 1..64 distinct portable identifiers (unique without regard to case, at most 64 characters each). |
| Field `assetKinds` | No (`asset` only) | 1..4 unique values from `image`, `audio`, `scene`, `any`; defaults to `["any"]`. |
| Field `label` | No | Non-empty display text, at most 128 characters; defaults to `key`. |
| Field `description` | No | Display text, at most 1,024 characters. |
| Field `default` | No | JSON scalar matching `type`; string defaults are at most 16,384 characters. |

Portable identifiers use ASCII letters, digits, `.`, `_`, `:`, and `-`.
Descriptions and labels may use ordinary display text.

Omitted defaults are deterministic:

| Type | Default |
| --- | --- |
| `string` | Empty string |
| `int` | `0` |
| `float` | `0.0` |
| `bool` | `false` |
| `null` | null |
| `enum` | The first choice |
| `asset` | Empty string |

## Version 2: Enum And Asset Fields

Version-2 files (ADR 0178) may declare `enum` fields with a `choices` list and
`asset` fields whose `assetKinds` filter the editor's project asset browser.
Both author ordinary **string** scene values, so game code reads them through
the existing typed string getters and version-1 runtime consumers need no new
kind. An enum `default` must be a declared choice. The structured schema form
always writes the lowest version its content requires, so a file that stops
using version-2 kinds returns to `"version": 1`. A version-2 file presented to
an older Studio is rejected wholesale, exactly like any unknown version.

## Migration Assistant

Renaming or retyping a saved field offers — never performs automatically — a
bounded workspace migration. The offer first scans the schema's workspace root
read-only, counting exact-kind matches in `.scene`/`.level` files and listing
`.vscn` files as manual-migration refusals, then requires a second explicit
confirmation before writing. Application is per-file transactional: each file
re-validates its scanned modification time and match count, rewrites
completely, and saves atomically — or is refused and reported byte-identical.
Conversions are limited to representation-preserving cases (rename within one
kind, anything to string); everything else is refused. Documents open in
Studio surface the standard external-change conflict flow afterward.

The complete file is limited to 1 MB, with at most 2,048 fields across all
components. Studio rejects the entire file if any entry is malformed or any
limit is exceeded; it never offers a partially parsed palette.

## Workspace Ownership

A saved scene uses the schema from its owning workspace root. Nested roots use
the longest matching root path. An unsaved scene may use a schema only when
exactly one root is open. This avoids silently applying definitions from the
wrong project in a multi-root workspace.

## Runtime Consumption

Component application writes ordinary canonical scene data:

- 2D code reads object values through `SceneDocument.ObjectHas`,
  `ObjectPropertyKind`, and the typed `ObjectGet*` methods.
- 3D code reads node values through `SceneNode.MetadataHas`, `MetadataKind`,
  and the typed `MetadataGet*` methods after loading/instantiating the VSCN.

The schema does not create an ECS, attach scripts, or interpret keys. Treat
field names as the stable adapter contract between the project and its game
code. Renaming a schema field does not migrate existing scenes.

The architectural rationale and transaction rules are recorded in
[ADR 0160](../../../docs/adr/0160-project-scene-component-schemas.md).
