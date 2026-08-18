---
status: active
audience: public
last-verified: 2026-07-27
---

# Editable Scene Documents
> JSON-backed `Zanna.Game2D.SceneDocument` documents for IDE scene tools and game-owned spawn adapters.

**Part of [Game Utilities](README.md)**

`Zanna.Game2D.SceneDocument` is an editable scene document. It owns tile layers,
placed-object hierarchy and draw order, typed scalar properties, asset
references, diagnostics, and canonical JSON save/load. It does not instantiate
game classes by string name; game code loads a scene and maps
objects/properties into game-owned entities.

## Construction And I/O

| Method | Signature | Description |
|---|---|---|
| `SceneDocument.New(width, height, tileWidth, tileHeight)` | `SceneDocument(i64, i64, i64, i64)` | Create a scene with one base layer. Invalid map dimensions produce a diagnostic 1×1 document; nonpositive tile dimensions become 16. |
| `SceneDocument.LoadJsonResult(text)` | `Result(str)` | Load scene JSON as `Ok(SceneDocument)` or `Err(message)`. |
| `SceneDocument.LoadResult(path)` | `Result(str)` | Read and load a scene file as `Ok(SceneDocument)` or `Err(message)`. |
| `SceneDocument.LoadJson(text)` | `SceneDocument(str)` | Load scene JSON without trapping on malformed user input. |
| `SceneDocument.Load(path)` | `SceneDocument(str)` | Read and load a scene file. |
| `SceneDocument.ImportTiledResult(path)` | `Result(str)` | Import a loose Tiled JSON/TMX map and relative dependencies as `Ok(SceneDocument)` or `Err(message)`. |
| `SceneDocument.ImportTiledAssetResult(path)` | `Result(str)` | Import a Tiled map, external tilesets/templates, and image paths through embedded/ZPAK/filesystem asset lookup. |
| `SceneDocument.ImportTiled(path)` / `ImportTiledAsset(path)` | `SceneDocument(str)` | Nullable compatibility forms of the Result import methods. |
| `ToJson()` | `String()` | Emit canonical schema v1 JSON. |
| `Save(path)` | `Boolean(String)` | Save through a same-directory temporary file before replacement. |

Canonical scene files use the `.scene2d` extension (`.scene` and `.level`
remain accepted legacy aliases, ADR 0182). The loader also accepts legacy
unversioned JSON with `layers[].data` and LevelData-shaped scalar properties for
import compatibility.

Zanna Studio opens `.scene2d` (plus legacy `.scene` and `.level`) files in
its built-in 2D authoring
surface. Its Scene properties group creates, renames, updates, and removes
scene-wide null, Boolean, integer, floating-point, and string metadata as
undoable edits. A layer image reference can point to a PNG, JPEG, BMP, or GIF
atlas; relative paths resolve beside a saved scene. Studio shows the first 512
frames in a clickable palette and renders real atlas frames across visible
layers. These references remain external and are not embedded in scene JSON.
Replacing or clearing a reference is one undoable document edit, while Reload
Image refreshes external pixels without dirtying the scene. Studio limits a
source to 16 MB, a decoded image to 4,194,304 pixels, and aggregate
decoded/cached scene imagery to 8,388,608 pixels. Advanced Tiled
margin/spacing, image-collection, animation, collision, and tile-metadata
authoring remain outside this v1 surface.

## Tiled JSON And TMX Import

`ImportTiledResult` and `ImportTiledAssetResult` convert finite or infinite Tiled
maps into the canonical scene schema. Orthogonal, isometric, staggered,
hexagonal, and oblique projections are retained. The filesystem method resolves
dependencies relative to each owning file. The asset method uses normalized
logical names and can resolve the complete graph from embedded assets or a
mounted ZPAK; absolute, URI, and asset-root-escaping dependencies fail.

The supported mapping includes:

- JSON arrays, CSV, and Base64 little-endian GIDs, with raw, zlib, gzip, or Zstandard data;
- inline or external TSJ/TSX atlas, mixed, and image-collection tilesets,
  including margin, spacing, per-tile images, and tileset drawing offsets;
- signed infinite chunks, including encoded/compressed chunk payloads; chunk
  unions become bounded editable grids while their signed origin is retained;
- map-wide canonical tile IDs and Tiled horizontal, vertical, diagonal, and
  hexagonal 60/120-degree GID transforms;
- source-order group flattening with inherited visibility, opacity, tint,
  parallax, and fractional pixel offsets;
- object and image layers, JSON or XML object templates, typed map/object properties,
  and file-property path resolution;
- full-tile collision objects, typed tile metadata, and tile animations with
  their authored per-frame durations.

Group names are joined with `/`. Layer properties become scene properties named
`tiled.layer.<group/layer>.<property>`. Imported objects preserve runtime-neutral
Tiled metadata in `tiled.*` properties, including `tiled.layer`, `tiled.numericId`,
`tiled.sourceX`, `tiled.sourceY`, dimensions, rotation, visibility, shape data,
template path, and tile-object GID. This retains fractional authored coordinates
even though the SceneDocument `x`/`y` compatibility fields are integers. Duplicate
authored object names fall back to the globally unique numeric Tiled ID. Image
layers use object type `tiled.image-layer` and carry `tiled.image`.

The preserved `tiledRuntime` section carries projection, render order, signed
origin, source-frame geometry, effective layer placement/parallax, canonical
variants, and image dependencies through `ToJson`, reload, and
`BuildTilemap()`. Integer and Boolean tile properties also populate Tilemap's
legacy fixed property table; other typed tile metadata remains in the preserved
scene section without aliasing between tilesets.

Malformed chunks, invalid or undeclared GIDs, overlapping GID ranges, cyclic or
unsafe dependencies, mismatched declared image sizes, and inputs above the
document/atlas resource limits return `Err` transactionally. See
[ADR 0140](../../adr/0140-tiled-map-and-scene-import.md) for the original adapter
and [ADR 0144](../../adr/0144-complete-tiled-map-import.md) for the complete
projection/composed-atlas contract.

`Save` writes a temporary file next to the target and replaces the target
only after that write succeeds. Failed replacement leaves the temporary file
removed and records an error diagnostic; an existing target is not deleted first.
It does not create a missing parent directory and does not reject a document merely
because `HasErrors()` is true.

```zia
module SceneDocumentDemo;

bind Zanna.Terminal;

func start() {
    var scene = Zanna.Game2D.SceneDocument.New(4, 3, 16, 16);
    scene.SetTile(0, 1, 2, 7);
    scene.SetStr("theme", "cave");
    Say(scene.GetStr("theme", "unknown"));
}
```

## Diagnostics

| Method | Description |
|---|---|
| `HasErrors()` | True when retained diagnostics include an error. |
| `Diagnostics()` | Compatibility `Seq<str>` of diagnostic messages. |
| `DiagnosticRecords()` | `Seq<Map>` with `code`, `severity`, `message`, `path`, `line`, `column`, and `source`. |
| `ClearDiagnostics()` | Clear retained diagnostic messages. Does not change the document's validity, so an invalid document stays invalid (VDOC-254). |

Prefer `LoadJsonResult` and `LoadResult` for production code that wants loading
to be an explicit `Ok`/`Err` value. Their `Err` contains one diagnostic message,
not the invalid document or its full diagnostic records. The compatibility
`LoadJson` and `Load` methods still return an invalid scene with diagnostics
instead of trapping for bad JSON, missing files, unknown versions, invalid
dimensions, v1 tile count mismatches, and resource-limit violations.

`ClearDiagnostics()` clears only the retained messages and last-error text; it no
longer touches the document's internal validity flag, so calling it on a failed
load still reports `HasErrors()` true — acknowledging messages cannot mask
normalized or incomplete data as a valid document (VDOC-254). A Result load selects
the first error-severity diagnostic for its `Err` message, so a warning added after
an error no longer replaces the reported error text (VDOC-255). Retain
`DiagnosticRecords()` when you need the full message set from a compatibility load.

Edit-time rejections, such as an over-long layer name or property key, are
reported as warning diagnostics. They do not make `HasErrors()` true unless a
load/save/schema error is also retained.

## Tiles And Layers

| Method | Description |
|---|---|
| `AddLayer(name)` | Add a layer and return its index, or `-1` if the scene is at its layer/cell limit. |
| `LayerCount()` | Number of layers. |
| `LayerName(layer)` / `SetLayerName(layer, name)` | Read or update layer names. Names must fit Tilemap's 31-byte layer-name limit. |
| `LayerVisible(layer)` / `SetLayerVisible(layer, visible)` | Read or update visibility. |
| `MoveLayer(from, to)` / `RemoveLayer(layer)` | Reorder or remove layers. The final layer is kept. |
| `GetTile(layer, x, y)` / `SetTile(layer, x, y, tile)` | Read or mutate tile IDs. Out-of-range reads return `0`; writes no-op. |
| `FillTiles(layer, x, y, width, height, tile)` | Fill a clamped rectangular region. |
| `FloodFill(layer, x, y, tile)` | Replace the exact four-connected region containing the cell and return the changed-cell count. Invalid cells and already-equal replacements return `0`. |
| `SetLayerAsset(layer, path)` / `LayerAsset(layer)` | Store a layer tileset/source asset path. |

Tile ID `0` means empty/not drawn. Tile ID `N > 0` maps to tileset frame
`N - 1` when rendered by `Tilemap` or by Zanna Studio's layer-atlas preview.
For saved scene documents, Studio can choose supported layer images from a
bounded searchable view of every open workspace root and stores a portable
scene-relative path when possible. External image metadata changes refresh the
canvas and palette without changing scene JSON or undo history; Reload Image
forces a reread when the filesystem's timestamp/size metadata is too coarse to
identify a rewrite.

`FloodFill` uses four-way adjacency: diagonal contact alone does not join two
regions. It is bounded by the existing 1,048,576-cell layer limit and allocates
its complete queue and visited set before changing the first tile, so an
allocation failure cannot leave a partial fill. See
[ADR 0171](../../adr/0171-bounded-scene-flood-fill-and-studio-tile-tools.md).

## Tile Behavior

The `collision`, `tileProperties`, `animations`, and `autotiles` sections load
into typed document state, serialize canonically, and retain unrecognized
members verbatim ([ADR 0176](../../adr/0176-typed-tile-behavior-sections.md)).
`BuildTilemap()` registers them on the returned runtime copy. Autotile rules do
not rewrite document cells; call `Tilemap.ApplyAutoTile()` on that copy when the
game wants base-layer cells resolved to their N/E/S/W variants. Zanna Studio's
default live autotile preview performs the same bounded resolution for display
only, then applies optional animation preview to the resolved tile ID.

| Method | Description |
|---|---|
| `TileCollision(tile)` / `SetTileCollision(tile, kind)` | Read or author one tile's collision kind: `0` none, `1` solid, `2` one-way-up. Kind `0` removes the record. |
| `CollisionTiles()` | Ascending tile IDs with a non-none kind. |
| `CollisionLayer()` / `SetCollisionLayer(layer)` | The layer consulted by Tilemap collision queries; the setter accepts `0..15`. |
| `TilePropertyKind(tile, key)` | `int`, `bool`, or empty when absent. |
| `TilePropertyInt/Bool(tile, key, default)` | Exact-kind typed reads. |
| `SetTilePropertyInt/Bool(tile, key, value)` | Typed writes; at most 64 properties per tile and 4,096 across all tiles. |
| `RemoveTileProperty(tile, key)` / `TilePropertyKeys(tile)` / `TilePropertyTiles()` | Remove or enumerate (lexicographic keys, ascending tiles). |
| `SetTileAnim(baseTile, frames, durationsMs)` | Author 1..4,096 frames with equal-length positive per-frame durations; at most 65,536 frames across all rules. |
| `TileAnimFrames/TileAnimDurations(baseTile)` / `TileAnimBases()` / `RemoveTileAnim(baseTile)` | Read, enumerate, or remove animation rules. |
| `SetAutotileRule(baseTile, variants)` | Author exactly 16 variant tile IDs. |
| `AutotileVariants(baseTile)` / `AutotileBases()` / `RemoveAutotileRule(baseTile)` | Read, enumerate, or remove autotile rules. |

Setters validate tile IDs against Tilemap's `1..4095` behavior-table range and
reject invalid input as edit-time warning no-ops. Loaders accept any integer
tile ID so legacy files round-trip losslessly; out-of-range records are
retained and re-emitted but remain inert in `BuildTilemap()`, exactly as
before. Legacy uniform `frameCount`/`msPerFrame` animations load by expanding
to per-frame `durations`, and the redundant uniform members are not re-emitted.
Empty sections are omitted from JSON entirely, and a document that is loaded
but never edited is never re-serialized.

## Camera And Lighting Sections

The `camera` and `lighting` sections carry scene-global settings as typed
`int`/`string` fields
([ADR 0177](../../adr/0177-scene-camera-and-lighting-sections.md)). Application
stays game-owned: read the fields and configure your own
`Zanna.Graphics.Camera` and `Zanna.Game.Lighting2D` instances. Placed
point lights are ordinary objects carrying a project `light` component, not
section entries.

| Method | Description |
|---|---|
| `CameraFieldKind(key)` / `LightingFieldKind(key)` | `int`, `string`, or empty when absent. |
| `CameraGetInt/GetStr(key, default)` / `LightingGetInt(key, default)` | Exact-kind typed reads. |
| `CameraSetInt/SetStr(key, value)` / `LightingSetInt(key, value)` | Typed writes with known-field validation. |
| `CameraRemove(key)` / `LightingRemove(key)` / `CameraKeys()` / `LightingKeys()` | Remove or enumerate fields. |

Known camera fields: `mode` (`"follow"`/`"fixed"`), `minX`/`minY`/`maxX`/`maxY`
(map onto `Camera.SetBounds` when all four are present), `deadzoneWidth`/
`deadzoneHeight` (non-negative), `followLerpPct` (`1..100`, for
`SmoothFollow`), and `zoomPct` (`10..1000`). Known lighting fields: `darkness`
(`0..255`), `tint` and `playerLightColor` (32-bit ARGB), and
`playerLightRadius` (`0..4096`). Out-of-range known-field writes are edit-time
warning no-ops; unknown keys are accepted for game-owned extension, and
non-int/string members in loaded files are retained verbatim.

## Objects And Properties

Objects have reserved metadata fields: `type`, `id`, `x`, and `y`, plus an
organizational parent managed by the hierarchy APIs. Custom data lives in typed
scalar properties: `null`, bool, int, float, or string.

| Method | Description |
|---|---|
| `AddObject(type, id, x, y)` | Add a placed object and return its index. |
| `ObjectCount()` / `RemoveObject(index)` | Count or remove objects. |
| `ObjectType(index)` / `ObjectId(index)` | Read object metadata. |
| `SetObjectMetadata(index, type, id)` | Update the reserved type and ID together. |
| `ObjectX(index)` / `ObjectY(index)` / `SetObjectPosition(index, x, y)` | Read or update position. |
| `ObjectParent(index)` | Return the organizational parent index, or `-1` for a root or invalid index. |
| `TrySetObjectParent(index, parent)` | Set a parent or make the object a root with `-1`. Invalid indices, self-parenting, and cycles return false without mutation. |
| `DuplicateObject(index, id)` | Deep-copy an object, its parent, and its typed properties immediately after the source, replacing the copied ID. Descendants are not copied. Returns the new index or `-1`. |
| `ObjectGetInt/Str/Float/Bool(index, key, default)` | Typed property reads; incompatible kinds return the supplied default. |
| `ObjectPropertyKind(index, key)` | Return `null`, `bool`, `int`, `float`, or `string`; return an empty string when absent. |
| `ObjectSetNull(index, key)` / `ObjectSetInt/Str/Float/Bool(index, key, value)` | Typed property writes. |
| `ObjectHas(index, key)` / `ObjectKeys(index)` / `ObjectRemove(index, key)` | Inspect or remove object properties. |
| `CountOfType(type)` / `ObjectOfType(type, n)` / `FindObjectOption(id)` | Search helpers returning counts, indexes, or `Option[Integer]`. |
| `MoveObject(from, to)` | Reorder objects while remapping every structural parent index. |

Hierarchy is organizational: `x` and `y` remain absolute scene-space
coordinates and do not inherit a parent transform. The object array remains
the global draw order; array order among objects with the same parent is their
sibling order. Removing an object promotes its direct children to the removed
object's parent. Adding an object creates a root.

Canonical version-1 JSON stores a non-root parent in the reserved integer
property `zanna.hierarchy.parentIndex`. Older runtimes preserve that typed
property even though they do not interpret it. Current runtimes hide it from
generic `ObjectHas`, `ObjectKeys`, `ObjectPropertyKind`, and typed property
access, and generic setters/removers cannot alter it. Malformed types,
out-of-range links, self-parenting, and cycles produce load diagnostics and are
normalized to roots. The reserved entry counts toward the 256-property
serialized limit, leaving 255 public properties on a non-root object. Parenting
a root already holding 256 public properties is rejected. At child capacity,
existing public properties may still be replaced or removed, but a new key is
rejected; making the object a root frees the reserved slot. See
[ADR 0164](../../adr/0164-backward-compatible-2d-scene-object-hierarchy.md)
for the compatibility and index-remapping contract.

Compatibility methods `SetObjectProperty`, `GetObjectProperty`, and
`DeleteObjectProperty` remain string wrappers over typed object properties.

Scene-level typed properties use the same scalar rules:

| Method | Description |
|---|---|
| `GetInt/Str/Float/Bool(key, default)` | Typed scene property reads. |
| `PropertyKind(key)` | Return `null`, `bool`, `int`, `float`, or `string`; return an empty string when absent. |
| `SetNull(key)` / `SetInt/Str/Float/Bool(key, value)` | Typed scene property writes. |
| `Keys()` | Return all scene property keys in deterministic lexicographic order. |
| `Has(key)` / `Remove(key)` | Inspect or remove scene properties. |

Compatibility methods `SetProperty`, `GetProperty`, and `DeleteProperty` remain
available for string-oriented callers. See
[ADR 0158](../../adr/0158-scene-level-property-authoring.md) for the
enumeration, exact-kind, and explicit-null authoring contract.

## Assets And Tilemap Copies

`AssetDescriptors()` returns `Seq<Map>` records with:

- `path`: scene-authored asset path
- `kind`: `tileset`, `sprite`, `image`, `audio`, or `unknown`
- `owner`: `scene`, `layer`, `object`, or `section`
- `layer`, `object`: owner indexes, or `-1`
- `key`: source field/property key
- `section`: rich section name when applicable
- `source`: scene file path for `Load`, or empty for `LoadJson`

`AssetPaths()` returns the unique path strings derived from descriptors.
Descriptors are guaranteed for explicit scene fields such as `tilesetAsset` and
layer `asset`. String scene/object properties and preserved rich sections are
also scanned by asset-like key names for compatibility; treat those matches as
best-effort conventions until the schema grows typed asset properties.

`BuildTilemap()` creates a new `Zanna.Graphics2D.Tilemap` render/collision copy.
Scene layer `0` maps to the Tilemap base layer; additional scene layers are
added up to the Tilemap layer limit. Mutating the returned Tilemap does not
change the scene or saved JSON. Asset paths are not resolved or loaded by
`BuildTilemap()`; use `Zanna.Assets.Resolver.Resolve` and bind tilesets in game
or editor code.

`TiledMapLoader.Load*` is the convenience path when the caller wants the imported
map and tileset images bound in one operation. It calls the same normalized
importer and builds one bounded composed atlas with a shared source frame. That
atlas crops spaced/margined sheets, places bottom-left-anchored image-collection
art, applies GID transforms and effective tint/opacity, and preserves logical
cell dimensions independently from artwork dimensions.

When present, preserved `collision`, `tileProperties`, `animations`,
`autotiles`, and `tiledRuntime` sections are applied to the returned Tilemap
using the matching runtime APIs. Variable animation durations, projected
coordinate conversion, fractional layer placement, parallax, render order, and
signed origins therefore survive scene JSON round trips.

## Resource Limits

Loads retain diagnostics and normalize or drop excess data rather than allocating
without bounds. The current limits are:

| Resource | Limit |
|---|---:|
| JSON input | 16 MiB |
| Cells per layer | 1,048,576 |
| Total cells across layers | 4,194,304 |
| Layers | 16 |
| Objects | 65,536 |
| Scene properties | 16,384 |
| Properties per object | 256 |
| Property key | 128 bytes |
| String property value | 64 KiB |
| Preserved rich/unknown sections | 4 MiB total |
| Retained diagnostics | 256 |
| Layer name compatible with `Tilemap` | 31 bytes |

## JSON Schema V1

```json
{
  "version": 1,
  "name": "descent",
  "width": 150,
  "height": 16,
  "tileWidth": 64,
  "tileHeight": 64,
  "tilesetAsset": "tiles/world.png",
  "properties": {
    "theme": "grasslands",
    "playerStartX": 96,
    "playerStartY": 480
  },
  "layers": [
    {
      "name": "base",
      "visible": true,
      "asset": "tiles/world.png",
      "tiles": [0, 1, 0, 2]
    }
  ],
  "objects": [
    {
      "type": "enemy",
      "id": "slime-1",
      "x": 640,
      "y": 480,
      "properties": {
        "hp": 3,
        "sprite": "sprites/slime.png"
      }
    }
  ]
}
```

Known rich sections such as `camera`, `lighting`, `collision`,
`tileProperties`, `animations`, and `autotiles` are preserved through load/save
round trips even when no higher-level editor API has changed them.
