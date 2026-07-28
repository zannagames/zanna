---
status: active
audience: users and contributors
last-verified: 2026-07-27
---

# Project Scene Components

Zanna Studio can turn recurring gameplay data into reusable typed templates for
2D objects and 3D nodes. Put an optional `scene-components.json` file at the
root of a Studio workspace:

```json
{
  "version": 13,
  "components": [
    {
      "name": "enemy-spawn",
      "label": "Enemy Spawn",
      "target": "both",
      "createObjectType": "enemy",
      "createNodeName": "Enemy Spawn",
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
      "createNodeName": "Checkpoint",
      "fields": [
        {"key": "checkpoint.id", "type": "string", "default": "start"},
        {"key": "checkpoint.enabled", "type": "bool", "default": true}
      ]
    }
  ]
}
```

Open a 2D scene (`.scene2d`, or legacy `.scene`/`.level`) or 3D scene
(`.scene3d`, or legacy `.vscn`) file inside that workspace. The
**Project components** inspector lists definitions compatible with the active
editor.

## Authoring The Schema

Press **Edit Schema** in either scene editor to open the structured project-file
form. The application picker above it stays target-filtered, but the authoring
dropdown shows every definition in `scene-components.json`, including 2D-only
components while a 3D scene is active and vice versa.

The form supports:

- **New Component**, component **Earlier**/**Later**, stable name, label,
  `target`, 2D creation type, 3D creation name, description,
  **Save Component**, and **Delete Component**.
- **New Field**, field **Earlier**/**Later**, stable key, label, scalar type,
  typed default, description, **Save Field**, and **Delete Field**.
- **Undo Schema** and **Redo Schema** for the last 20 accepted project-file
  transitions.

When the file is absent, **New Component** creates a complete valid
`scene-components.json` with a starter Boolean field and a compatible creation
recipe. Each later action writes one complete, parser-validated file state.
Invalid identifiers, duplicate names/keys, wrong typed defaults, non-finite
numbers, incompatible recipes, the last-field deletion, and format limits are
rejected without changing disk.

Schema edits are separate from scene edits: they do not change scene bytes,
scene revision, dirty state, or scene undo/redo. Removing or renaming a
component or field also does not remove or rename values already authored in
scenes.

Studio preserves unknown supported-version JSON members on every retained root,
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

## Creating From A Component

Schema version 13 can make a component a direct creation action. Open the
Object inspector tab, choose a component, and press **Create Object** or
**Create Node**. No existing selection is required.

- A 2D recipe creates the declared `createObjectType` at the selected cell, or
  at the center visible cell when no valid cell is selected.
- A 3D recipe creates a uniquely named top-level node from `createNodeName` at
  the current viewport target.
- Every declared field is written with its exact type before the item is
  committed and selected.
- The entire result is one scene-history entry. Undo never exposes a generic
  placeholder or a partially configured item.

Omit the relevant recipe member when a component should only augment existing
content. For example, a collider component can remain **Add Missing**-only
instead of creating an empty 3D node. Studio never guesses a runtime object
type or node discriminator from the component label.

## Format Reference

| Member | Required | Contract |
| --- | --- | --- |
| Root `version` | Yes | Numeric integer `1` through `15`; use the lowest version required by the optional members below. |
| Root `components` | Yes | Array with at most 128 entries. |
| Component `name` | Yes | Stable portable identifier, at most 64 characters; unique without regard to case. |
| Component `target` | Yes | `2d-object`, `3d-node`, or `both`. |
| Component `label` | No | Non-empty display text, at most 128 characters; defaults to `name`. |
| Component `description` | No | Display text, at most 1,024 characters. |
| Component `createObjectType` | No (version 13) | Portable 2D runtime object type, at most 128 characters; allowed only for `2d-object` or `both`. |
| Component `createNodeName` | No (version 13) | Non-empty initial 3D node name, at most 128 characters; allowed only for `3d-node` or `both`. |
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

## Versions 3–14: Preview And Creation Extensions

Later versions add declarative, editor-only visualization conventions while
keeping component fields and canonical scene formats unchanged:

| Version | Optional root member | Purpose |
| --- | --- | --- |
| 3 | `objectPreviews` | Map 2D object types/properties to sprite-atlas frames. |
| 4 | `scenePreview3D` | Map root metadata to the initial 3D view, ambient/clear color, fog, and overlay defaults. |
| 5 | `nodePreviews3D` | Map node metadata to transient project prefab previews. |
| 6 | extended `scenePreview3D` | Map sky palettes and sun/fill preview rigs. |
| 7 | extended `scenePreview3D` | Declare camera FOV, IBL intensity, and typed height fog. |
| 8 | `scenePreview2D` | Map scene properties to screen-space background images and a first-open grid default. |
| 9 | extended `scenePreview3D` | Map root metadata to a transient scene-level environment prefab. |
| 10 | extended `scenePreview3D` | Reproduce a portable runtime post-processing chain in the shaded viewport. |
| 11 | extended `objectPreviews` | Interleave 2D object sprites with tile layers and reproduce game draw priorities. |
| 12 | extended `scenePreview2D` / `scenePreview3D` | Declare the exact project output size used by locked Game View framing. |
| 13 | component `createObjectType` / `createNodeName` | Create gameplay-ready objects or nodes directly from a component recipe. |
| 14 | extended `scenePreview3D` | Compose independently matched and metadata-transformed environment layers beside the base environment. |

Studio never executes project code to apply these profiles, and preview
resolution never writes scene bytes, dirty state, revision, or history. The
structured component form preserves supported profiles and their required
schema version when editing ordinary component fields.

### Version 8 2D Scene Backgrounds

This example selects a runtime-baked region image from the scene's integer
`region` property. Because regions are one-based, `variantOffset` converts them
to the zero-based image list:

```json
{
  "version": 8,
  "components": [],
  "scenePreview2D": {
    "background": "assets/backgrounds/fallback.png",
    "variantProperty": "region",
    "variantOffset": -1,
    "variantImages": [
      "assets/backgrounds/region-01.png",
      "assets/backgrounds/region-02.png"
    ],
    "fit": "cover",
    "showGrid": false
  }
}
```

`background` is an optional fallback. `variantProperty` and `variantImages`
must appear together; the list contains 1–64 images. `variantOffset` is
optional and requires that pair. `fit` is `cover` (default), `contain`, or
`stretch`. `showGrid` only supplies the first-open default; a restored workspace
or explicit user toggle wins.

Image paths resolve beside the project-root `scene-components.json`. They must
be forward-slash project-relative PNG, JPEG, BMP, or GIF references with no
absolute prefix, drive/URI syntax, backslash, or parent traversal. Missing,
wrong-typed, and out-of-range variant values use `background`; without one the
ordinary editor clear color remains visible.

### Version 9 3D Scene Environments

Version 9 can add large runtime-generated visual context without inserting it
into the editable scene:

```json
{
  "version": 9,
  "components": [],
  "scenePreview3D": {
    "environmentPrefab": "assets/editor-previews/fallback.scene3d",
    "environmentVariantProperty": "terrain.kind",
    "environmentVariantOffset": -1,
    "environmentVariantPrefabs": [
      "assets/editor-previews/canyon.scene3d",
      "assets/editor-previews/ash-sea.scene3d"
    ]
  }
}
```

`environmentPrefab` is an optional fallback. `environmentVariantProperty` and
`environmentVariantPrefabs` must appear together; the list contains 1–64
assets. Studio reads an exact integer from scene-root metadata, adds the
optional bounded `environmentVariantOffset`, and selects the resulting list
entry. Missing, wrong-typed, and out-of-range values use the fallback; without
one, no environment is added.

Paths resolve beside the project-root schema and must be forward-slash,
project-relative `.scene3d` or `.vscn` references with no absolute prefix,
drive/URI syntax, backslash, or parent traversal. The resolved prefab is drawn
through the same camera, depth, lighting, atmosphere, and viewport mode as the
canonical graph, but remains outside the hierarchy, picking, save data, dirty
state, and undo history. Procedural games should bake a deterministic ordinary
scene asset; Studio never executes the generator or game code.

### Version 10 3D Post-Processing

Version 10 can reproduce the portable color pipeline that gives a running game
its final look:

```json
{
  "version": 10,
  "components": [],
  "scenePreview3D": {
    "tonemapMode": 2,
    "tonemapExposure": 1.17,
    "bloomThreshold": 0.9,
    "bloomIntensity": 0.25,
    "bloomPasses": 3,
    "colorGradeBrightness": 0.03,
    "colorGradeContrast": 1.03,
    "colorGradeSaturation": 1.07,
    "vignetteRadius": 0.98,
    "vignetteSoftness": 0.12,
    "fxaa": true
  }
}
```

Each multi-value effect is complete or absent. `tonemapMode` is an integer from
0 through 2 and `tonemapExposure` is 0–16. Bloom threshold/intensity are 0–16
and passes are an integer from 0 through 32. Color-grade brightness is -1–1;
contrast and saturation are 0–4. Vignette radius is 0–1 and softness is
0.001–1. `fxaa` is Boolean. A wrong type, fractional integer, missing partner,
or out-of-range value rejects the whole schema.

Studio always builds the portable runtime order: tonemap, bloom, color grade,
vignette, then FXAA. Shaded and Shaded+Wire views use the retained chain and
read pixels after `Canvas3D` frame finalization, so the displayed image includes
the effects. Pure Wireframe detaches the chain, and editor overlays are drawn
after readback so selections and gizmos remain crisp. The camera inset stays
unprocessed for inspection. SSAO, SSR, depth of field, temporal antialiasing,
and motion blur are intentionally not schema members because their depth,
history, or backend requirements cannot yet promise a consistent portable
preview.

### Version 11 2D Object Draw Stacks

Version 11 can make overlapping object sprites and foreground tile layers
match the order used by the game:

```json
{
  "version": 11,
  "components": [],
  "objectPreviews": [
    {
      "objectType": "pickup",
      "sprite": "assets/sprites/gameplay.png",
      "frame": 12,
      "drawOrder": 10
    },
    {
      "objectType": "player-start",
      "sprite": "assets/sprites/gameplay.png",
      "frame": 20,
      "drawOrder": 20,
      "afterLayer": 0
    },
    {
      "objectType": "enemy",
      "sprite": "assets/sprites/gameplay.png",
      "frame": 30,
      "drawOrder": 30
    }
  ]
}
```

`drawOrder` is an exact integer from -128 through 127 and defaults to `0`.
Smaller values paint first; higher values paint and pick later. Equal values
retain canonical object-array order. `afterLayer` is an exact integer from
-1 through 15 and defaults to `15`: -1 paints before every tile layer, 0
paints after the first layer, and a value beyond the current final layer paints
after all layers. A layer boundary takes precedence over `drawOrder`, so an
object cannot jump across a foreground layer merely by raising its priority.

The single-object inspector exposes the effective boundary and priority.
**Apply** stores an exception as the ordinary integer properties
`editor.afterLayer` and `editor.drawOrder`; **Use Project** removes both.
Those edits are one undo transaction. Missing, wrong-kind, or out-of-range
properties fall back to the project rule, and project defaults never add
properties or change scene bytes. The grid, guides, selections, gizmos, route
lines, light halos, and marker fallback remain editor overlays above the
game-like sprite/tile composite.

### Version 12 Game Output Frames

Version 12 lets Game View use the same output shape as the running game rather
than whichever shape the editor lane happens to have:

```json
{
  "version": 12,
  "components": [],
  "scenePreview2D": {
    "outputWidth": 1280,
    "outputHeight": 720
  },
  "scenePreview3D": {
    "outputWidth": 1600,
    "outputHeight": 900
  }
}
```

Each profile is independent; a project need declare only the scene kind it
uses. `outputWidth` and `outputHeight` must appear together as exact integers
from 64 through 8,192. Either member in an older schema, a missing partner, a
fractional value, or an out-of-range dimension rejects the whole schema. A 2D
profile may contain only this pair.

With the pair present, Game View renders the largest centered project-aspect
rectangle inside the viewport and fills unused space with neutral matte. The
2D editor fits authored pixels to that frame and centers the conventional
player start. The 3D editor creates its offscreen render target at the frame
aspect and reapplies the project gameplay camera. Camera navigation is locked
until exit; the exact prior 2D scroll/zoom or 3D editor camera returns
afterward. Profiles without the pair retain freely navigable Game View.

The dimensions remain project-owned preview data. They never enter scene JSON,
VSCN, dirty state, revision, history, or runtime configuration.

### Version 13 Component Creation Recipes

Version 13 makes runtime identity explicit on the component itself:

```json
{
  "version": 13,
  "components": [
    {
      "name": "enemy-spawn",
      "target": "2d-object",
      "createObjectType": "enemy",
      "fields": [
        {"key": "enemy.type", "type": "int", "default": 0}
      ]
    },
    {
      "name": "encounter",
      "target": "3d-node",
      "createNodeName": "Encounter",
      "fields": [
        {"key": "game.kind", "type": "string", "default": "encounter"}
      ]
    }
  ]
}
```

`createObjectType` uses the same portable character set as scene-data keys and
is limited to 128 characters. `createNodeName` is trimmed, must not be empty,
and is limited to 128 characters. A wrong JSON kind, incompatible target,
empty value, overlong value, or either member in versions 1 through 12 rejects
the complete schema.

A `both` component may provide either recipe or both. Recipe absence is
meaningful: the component remains available for **Add Missing**, but its Create
action stays disabled. Creating applies every typed default atomically, selects
the result, and lets existing project object/node preview rules render it
immediately. Recipe data never enters the canonical scene; only the created
object or node and its ordinary typed fields do.

### Version 14 Additive 3D Environment Layers

Version 14 lets a scene combine several pieces of runtime-built visual context
instead of selecting only one base environment:

```json
{
  "version": 14,
  "components": [],
  "scenePreview3D": {
    "environmentVariantProperty": "terrain.kind",
    "environmentVariantOffset": -1,
    "environmentVariantPrefabs": [
      "assets/editor-previews/canyon.scene3d",
      "assets/editor-previews/ash-sea.scene3d"
    ],
    "environmentLayers": [
      {
        "matchProperty": "weather.dustStorm",
        "matchValue": true,
        "prefab": "assets/editor-previews/dust-storm.scene3d",
        "positionXProperty": "weather.centerX",
        "positionYProperty": "weather.centerY",
        "positionZProperty": "weather.centerZ",
        "scaleXProperty": "weather.radiusX",
        "scaleYProperty": "weather.height",
        "scaleZProperty": "weather.radiusZ"
      }
    ]
  }
}
```

`environmentLayers` contains 1–32 entries. Every entry requires a portable
`matchProperty`, a Boolean/integer/string `matchValue`, and a safe
project-relative `.scene3d` or `.vscn` `prefab`. Studio compares the canonical
scene root's metadata kind and value exactly; missing, wrong-typed, or unequal
metadata leaves that layer inactive.

The six optional position/scale property names map individual axes from
root-level **float** metadata. Unmapped positions default to zero and unmapped
scales to one. An optional `yawProperty` must be paired with a `yawUnit` of
`degrees` or `radians`. A matched layer with a missing or wrong-typed mapped
transform is omitted with preview status instead of being placed at a guessed
location.

Each matching prefab receives its own transient wrapper and composes after the
version-9 base environment. Layers share depth, lighting, atmosphere, post-FX,
and the project Game View frame, but never appear in hierarchy, picking,
selection, VSCN bytes, dirty state, or history. Use a centered unit asset when
metadata supplies its dimensions—for example, a distant shell or weather volume
scaled by authored extents. Use version-15 runtime water layers rather than a
flat prefab when the game uses `Water3D`.

### Version 15 Runtime-Backed 3D Water Layers

Version 15 lets Studio construct bounded instances of the public runtime
`Water3D` rather than approximating a procedural surface with a static plane:

```json
{
  "version": 15,
  "components": [],
  "scenePreview3D": {
    "waterLayers": [
      {
        "matchProperty": "water.enabled",
        "matchValue": true,
        "positionXProperty": "water.x",
        "positionYProperty": "water.y",
        "positionZProperty": "water.z",
        "widthProperty": "water.halfWidth",
        "depthProperty": "water.halfDepth",
        "widthMultiplier": 2.0,
        "depthMultiplier": 2.0,
        "texture": "assets/editor-previews/water-albedo.png",
        "normalMap": "assets/editor-previews/water-normal.png",
        "resolution": 64,
        "animate": true,
        "waves": [
          {
            "dirX": 1.0,
            "dirZ": 0.3,
            "speed": 0.05,
            "amplitude": 0.4,
            "wavelength": 1.2
          }
        ]
      }
    ]
  }
}
```

`waterLayers` contains 1–8 entries. Every entry requires the same portable
`matchProperty` and exact Boolean, integer, or string `matchValue` used by
version-14 environment layers. Missing, wrong-kind, or unequal root metadata
leaves the layer inactive.

Static `positionX`, `positionY`, and `positionZ` default to zero. Static
`width` and `depth` default to one and must remain in `[0.01, 100000]`.
Each corresponding `*Property` replaces that static value from exact float
root metadata. `widthMultiplier` and `depthMultiplier` are optional positive
values in the same range, but each is valid only with its corresponding
property. The final dimensions must remain in range; positions are bounded to
±1,000,000. A matched layer with missing, wrong-typed, nonpositive, or
out-of-range mapped data is omitted with preview status.

The optional color is an all-or-none `colorR/G/B/A` group in `[0, 1]`.
`texture` and `normalMap` use the same safe, project-relative raster-path and
bounded decode contract as other scene previews. `resolution` is an integer
from 8 through 64 and defaults to 64. `animate` is a Boolean and defaults to
false.

`waves` may contain zero through eight complete records. `dirX` and `dirZ`
must form a nonzero direction and each stay within ±1,000; `speed` stays
within ±1,000; `amplitude` is in `[0, 1000]`; and `wavelength` is in
`[0.01, 100000]`. Studio passes these values to `Water3D.AddWave` in declared
order. An animated surface advances at most 30 times per second, with each
elapsed step capped at 100 milliseconds.

Studio opens one explicit `Canvas3D` frame and submits the canonical scene,
transient project-prefab graph, and water surfaces before ending and finalizing
it. Depth, transparent ordering, lighting, atmosphere, post-processing, and
frame statistics therefore see one complete authored view. Water remains
presentation-only: it is absent from hierarchy, picking, selection, VSCN
serialization, dirty state, revision, and undo history.

See [ADR 0197](../../../docs/adr/0197-project-owned-2d-object-preview-profiles.md)
through
[ADR 0213](../../../docs/adr/0213-runtime-backed-water-preview-layers.md)
for the complete bounds, precedence, and 3D profile contracts.

## Migration Assistant

Renaming or retyping a saved field offers — never performs automatically — a
bounded workspace migration. The offer first scans the schema's workspace root
read-only, counting exact-kind matches in 2D scene files (`.scene2d`, plus
legacy `.scene`/`.level`) through SceneDocument and in 3D scenes
(`.scene3d`/`.vscn`) through the canonical VSCN loader, then requires a
second explicit confirmation before writing. Grafted prefab instance content
is excluded from 3D counts and conversion — it never serializes back into
the referencing file. Application is per-file transactional: each file
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
