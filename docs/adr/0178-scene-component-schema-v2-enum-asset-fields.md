---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0178: Add Enum and Asset Fields to Scene Component Schemas

## Status

Accepted (2026-07-24)

## Context

Project scene component schemas (ADR 0160, `scene-components.json`) support
only the scalar field types `string`, `int`, `float`, `bool`, and `null`.
Real gameplay templates immediately want two more shapes: a **choice from a
fixed set** (an enemy archetype, a pickup kind, an interaction type) and a
**reference to a project asset** (a sprite path, an audio cue). Today both
degrade to free-text string fields, so the editor cannot offer a dropdown or
the bounded asset browser, and typos become silent gameplay bugs.

Separately, renaming or retyping a schema field deliberately leaves existing
scene values untouched (ADR 0160), and nothing helps an author propagate an
intentional rename across a workspace of scenes.

## Decision

### Format version 2

`scene-components.json` accepts root `version` values `1` and `2`. Version 1
files keep their exact current contract. Version 2 keeps every version-1 rule
and limit and adds two field types:

- `"type": "enum"` — requires `"choices"`: an array of 1..64 distinct
  strings (unique without regard to case, each a portable identifier of at
  most 64 characters). An omitted `default` is the first choice; a present
  `default` must be one of the choices. The authored scene value remains an
  ordinary **string** object property/metadata value, so game code reads it
  through the existing typed string getters and runtimes need no new kind.
- `"type": "asset"` — value is an ordinary **string** property holding a
  workspace-relative path with the same portability rules as layer asset
  references. Optional `"assetKinds"`: an array of 1..4 values from
  `"image"`, `"audio"`, `"scene"`, `"any"` (default `["any"]`), used only to
  filter the editor's asset browser. An omitted `default` is the empty
  string.

A file whose root `version` is `2` but that uses only version-1 features is
valid. Studio's structured schema form always writes the lowest version that
its content requires. Unknown-member preservation, atomic writes, external
conflict detection, and file undo/redo keep their ADR 0160 contracts in both
versions. A version-2 file presented to a tool that only understands
version 1 is rejected wholesale, exactly as any unknown version is today;
that failure mode is documented rather than papered over.

Editor behavior: enum fields render as dropdowns; a scene value that is not a
current choice is displayed truthfully as an off-schema value and is never
mutated implicitly. Asset fields use the existing bounded project asset
browser and store portable relative paths. **Add Missing** keeps its exact
type-conflict rule, treating enum and asset values as string-kind for
conflict purposes.

### Migration assistant

When the schema form renames a field key, renames a component, or changes a
field type, Studio offers — never performs automatically — a workspace
migration:

1. A bounded read-only scan of the schema's workspace root counts exact-kind
   matches in every `.scene`/`.level` file and lists `.vscn` files as
   manual-migration refusals. Directory, file, and matched-file ceilings mark
   the result truncated, and a truncated scan is reported rather than treated
   as complete.
2. The author reviews the scan summary in the schema form's migration offer
   and must explicitly confirm before anything is written.
3. Application is per-file transactional: each file is re-validated against
   its scanned modification time and match count, rewritten completely, and
   saved through the scene document's atomic same-directory replacement — or
   refused and reported, leaving it byte-identical. Documents open in Studio
   surface the standard external-change conflict flow afterward instead of
   being mutated behind their live buffers.
4. Value conversion is limited to representation-preserving cases (rename
   within one kind; anything→string rendering). int→float is refused because
   the 2D format serializes integral floats without a decimal point, so the
   kind would silently revert on the next load. Lossy conversions are refused
   per file and reported.

The scan and application never run on schema Undo/Redo, which continues to
touch only the project file.

## Consequences

- Spawn tables, interaction types, and sprite references become
  dropdown/browser-backed instead of free text, removing a whole class of
  authoring typos.
- Game code is unaffected: enum and asset values are ordinary string
  properties at runtime.
- Old Studio builds reject version-2 files loudly instead of misreading them.
- The migration assistant makes intentional schema evolution practical while
  keeping ADR 0160's "no silent data rewriting" stance: every migration is
  explicit, previewed, bounded, and per-file transactional.
- `scene-components.md` and the palette/schema-form/probe suite must cover
  both versions, choice validation, browser filtering, off-schema display,
  and refused lossy migrations.

## Alternatives Considered

- **Model enums as int codes with display labels.** Rejected: scene data
  becomes unreadable without the schema, and games would need the schema file
  at runtime to interpret saves.
- **A new runtime property kind for enums/assets.** Rejected: it would change
  the scene JSON schema and every runtime consumer for what is purely an
  editor affordance over strings.
- **Automatic migration on schema save.** Rejected: ADR 0160 deliberately
  keeps schema edits free of scene mutation; silent workspace rewrites on a
  schema keystroke would be the exact drift that rule exists to prevent.
- **Embedding migration history in the schema file.** Rejected: it turns a
  palette definition into a migration log and still cannot cover scenes
  edited outside the workspace.
