---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0206: Add Project-Owned 2D Object Preview Draw Stacks

## Status

Accepted (2026-07-27)

## Context

Project-owned object preview profiles let Studio replace abstract 2D scene
markers with the sprite frames a game uses. Scene backgrounds and
runtime-exact autotile previews close two more visual gaps. The final composite
can still be wrong because Studio historically paints every object after every
tile layer in canonical object-array order.

That order is a document-organization convention, not necessarily a game's
render order. Xenoscape's canonical region files group enemies first, pickups
second, interactions later, and the player start near the end. The game paints
pickups, the player, enemies and bosses, then interactions and markers. Two
overlapping objects can therefore show a different winner in Studio than in
the game. A foreground tile layer also cannot cover a sprite, so bridges,
canopies, roofs, and other overdraw cannot be previewed faithfully.

Reordering the canonical object array is not a sufficient contract. It would
couple hierarchy organization to rendering, change scene bytes just to alter
an editor preview, and still could not interleave objects with tile layers.
Executing a project's renderer would give game code authority inside Studio
and make authoring depend on mutable runtime state.

## Decision

### Component-schema version 11

`scene-components.json` version 11 extends each version-3 `objectPreviews`
record with two optional exact integers:

- `drawOrder` is in `[-128, 127]` and defaults to `0`. Smaller values paint
  first; larger values paint and pick later.
- `afterLayer` is in `[-1, 15]` and defaults to `15`. `-1` means before every
  tile layer; `0` means after the first layer; other values name the same
  zero-based boundary. A value beyond the current scene's final layer clamps
  to the boundary after all layers.

Using either member requires schema version 11. A wrong type, fractional
number, out-of-range value, or version-1-through-10 use rejects the complete
schema without publishing partial preview data. A version-11 rule that omits
both members retains the historical after-all/priority-zero behavior.
Structured component edits preserve these members and infer version 11 while
any retained rule uses them.

### Stable bounded composite

Studio constructs the visual object stack with a bounded stable bucket pass:

1. tile-layer boundary, from before layer zero through after the final layer;
2. signed `drawOrder`, from -128 through 127;
3. canonical object-array index as the stable tie break.

The algorithm is linear in object count plus the fixed boundary/priority
domain; it does not sort untrusted project data. At boundary zero Studio paints
eligible sprite previews, then each visible tile layer followed by that
layer's object boundary. Project background art remains below all canonical
content.

Only the game's sprite-like imagery participates in this stack. The grid,
guides, route lines, light halos, selection outlines, handles, and fallback
editor markers remain legible editor overlays above the scene composite.
Missing or invalid sprite assets keep the existing deterministic marker
fallback.

Canvas point picking and overlapping marquee result order use the same stack.
Point picking walks it top-to-bottom and retains the existing transformed
sprite-footprint test, so the object whose pixels visually win also receives
the click. Canonical hierarchy order remains the stable fallback and continues
to govern hierarchy presentation.

### Per-object authoring override

A single selected object exposes a **Preview draw stack** inspector group with
an after-layer dropdown, bounded integer priority, **Apply**, and
**Use Project**. Apply writes the pair as ordinary typed object properties:

```text
editor.afterLayer
editor.drawOrder
```

An exact in-range integer override wins over the project rule. A missing,
wrong-kind, or out-of-range override falls back independently to the project
value rather than corrupting the stack. Apply writes and verifies both
properties as one scene-history transaction; failure restores the complete
prior scene. Use Project removes both as one transaction. Reapplying an
identical effective inherited value or identical explicit pair is a no-op.

The `editor.*` namespace is an editor convention. Games may ignore it, and
project rules never add either key to canonical scene data. No scene format,
runtime API, runtime C ABI, dependency, or platform-specific branch changes.

## Consequences

- Overlapping project sprites and foreground tile layers can match a game's
  actual composite without rearranging canonical scene objects.
- Visual topmost order, canvas picking, and marquee ordering share one
  deterministic source of truth.
- Existing projects and scenes preserve their prior rendering because the
  default remains after all tile layers with stable document-order ties.
- Projects can define the common render categories once while individual
  scene objects retain an explicit, undoable exception path.
- Tests must pin version gating and bounds, structured-authoring preservation,
  layer interleaving, stable ties, wrong-kind fallback, rendered pixels,
  topmost pointer picking, inspector transactions, and real-project order.

## Alternatives Considered

- **Use canonical object order only.** Rejected because organization and game
  rendering differ, and it cannot interleave sprites with tile layers.
- **Add more Earlier/Later hierarchy buttons.** Rejected because they mutate
  canonical organization and still cannot express foreground overdraw.
- **Store a free-form floating depth.** Rejected because non-finite values,
  precision ties, and unbounded sorting add failure modes without helping the
  targeted 2D category/layer model.
- **Put the fields in the canonical SceneDocument schema.** Rejected because
  project-wide game conventions should not duplicate into every object, and
  preview-only defaults must not dirty scenes.
- **Execute the game's draw loop in Studio.** Rejected because project code,
  mutable state, and input/time dependencies are not a safe or deterministic
  editor contract.
