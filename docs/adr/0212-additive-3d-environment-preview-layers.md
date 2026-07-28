---
status: active
audience: contributors
last-verified: 2026-07-27
---

# ADR 0212: Compose Additive 3D Environment Preview Layers

## Status

Accepted (2026-07-27)

The generic additive-prefab contract remains active. ADR 0213 supersedes only
this decision's original Ashfall flat-water proxy with a runtime-backed
`Water3D` layer and fulfills the shared-frame composition requirement.

## Context

ADR 0203 lets a project select one transient scene-environment prefab from
typed root metadata. That closes the largest gap when a game builds one piece
of visual context—such as terrain—after loading its canonical VSCN.

Real levels often build several independent pieces. Ashfall mission 05, for
example, has no procedural terrain but creates a `Water3D` pool from
`af.wantWater`, center, and half-extent metadata. Other missions use the
existing terrain mapping. A single mutually exclusive environment slot cannot
express terrain plus water, a distant shell plus weather, or several optional
set-dressing layers. Baking every combination produces an exponential asset
matrix and stops responding truthfully when an author edits the source
metadata.

Studio still must not execute project startup or rendering code. Derived
presentation must remain bounded, declarative, cross-platform, and completely
outside canonical scene content and history.

## Decision

### Component-schema version 14

`scene-components.json` version 14 adds an optional
`scenePreview3D.environmentLayers` array containing 1–32 layer definitions.
Each definition has:

- required `matchProperty`, a portable canonical root-metadata key;
- required `matchValue`, whose JSON kind is Boolean, integer, or string;
- required `prefab`, a safe project-relative `.scene3d` or `.vscn` path;
- optional `positionXProperty`, `positionYProperty`, and
  `positionZProperty`;
- optional `scaleXProperty`, `scaleYProperty`, and `scaleZProperty`; and
- an optional paired `yawProperty` and `yawUnit`, where the unit is `degrees`
  or `radians`.

Each configured transform key must resolve to float metadata. Unmapped
position axes use zero, unmapped scale axes use one, and absent yaw uses zero.
Position is clamped to ±1,000,000, scale to ±1,000, and yaw to a finite bounded
input before matrix construction. A missing or wrong-typed transform key omits
that matched layer and reports preview status rather than silently moving it.

Matching is exact in both kind and value. An integer does not match a float or
string containing the same digits. Missing or wrong-typed match metadata simply
leaves that layer inactive. String values retain their exact content and are
bounded to 128 characters.

The existing version-9 base/fallback/variant environment mapping remains
unchanged. Additive layers compose after that base; they do not replace it.
Schemas older than version 14 that contain `environmentLayers`, empty or
oversized arrays, unsafe paths, fractional integer matches, invalid property
keys, or incomplete yaw pairs reject atomically.

### Transient composition

For every matching layer, Studio instantiates the ordinary preview asset under
an ownerless wrapper in the existing disposable project-preview `SceneGraph`.
It applies the root-derived translation, yaw, and nonuniform scale to that
wrapper. Base environment, additive layers, and node previews share the
existing 256-asset, 4,096-node, and 64 MB scene-read budgets.

All layers draw through the same retained camera, depth buffer, atmosphere,
lighting, post-processing, and Game View frame as the canonical scene. They do
not appear in the hierarchy, cannot become canonical selection or picking
owners, and never enter VSCN bytes, revision, dirty state, or undo history.
Any canonical edit or project-schema reload rebuilds the disposable graph, so
changed source metadata changes the preview on the next accepted scene state.

### Ashfall water

The initial decision used a Boolean-matched, centered two-by-two prefab scaled
from `af.watX/Y/Z` and `af.watW/D`. That preserved the exact footprint but not
the game's procedural surface. ADR 0213 replaces this project-specific use with
the general version-15 runtime-water contract; Ashfall no longer ships the flat
water prefab.

## Consequences

- Projects can show several independently optional runtime-built visuals
  without a combinatorial prefab set.
- Editing canonical root position or size metadata produces a correspondingly
  transformed preview after the normal scene transaction.
- Boolean, integer, and string conditions cover common feature gates and
  variants while remaining simple enough to validate and reason about.
- Versions 1–13 and projects without additive layers preserve their existing
  behavior.
- The feature adds no dependency, runtime C ABI, IL, VSCN, or platform-adapter
  change.
- Probes must cover schema bounds and typed matching, authoring preservation,
  real Ashfall water placement, rendered-graph participation, and canonical
  content/history isolation.

## Alternatives Considered

- **Replace the terrain mapping with one prefab per mission.** Rejected because
  it duplicates common terrain, scales poorly, and makes metadata edits stale.
- **Bake water into mission VSCN files.** Rejected because the game already
  treats water as runtime-derived presentation and would either render a
  duplicate or require editor-only canonical nodes.
- **Recognize Ashfall water keys in Studio.** Rejected because project
  vocabulary belongs to project data, not product code.
- **Execute `Water3D` construction from the game inside Studio.** Rejected
  because arbitrary project execution is not a safe or deterministic authoring
  contract.
- **Serialize `Water3D` as a new VSCN component.** Deferred. A general canonical
  water component may be valuable later, but it requires a broader runtime and
  file-format decision; additive preview layers close the immediate
  editor/game fidelity gap without changing either surface.
