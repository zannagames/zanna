---
status: active
audience: public
last-verified: 2026-09-13
---

# Zanna.Game2D.LevelDocument

This loader reads a compact JSON level into one `Zanna.Graphics2D.Tilemap` plus a bounded
array of spawn records. It is distinct from the newer editable
[`Zanna.Game2D.SceneDocument`](scene.md).

## JSON shape

```json
{
  "width": 50,
  "height": 18,
  "tileWidth": 32,
  "tileHeight": 32,
  "properties": {
    "theme": "grasslands",
    "playerStartX": 96,
    "playerStartY": 480
  },
  "layers": [
    { "name": "terrain", "type": "tiles", "data": [0, 0, 1, 1] }
  ],
  "objects": [
    { "type": "enemy", "id": "slime", "x": 640, "y": 480 }
  ]
}
```

`width` and `height` must be positive. Missing/non-positive tile dimensions default to 32. Tile
values accept boxed integers or doubles (doubles truncate); other values become zero. Short data
arrays leave remaining cells empty and excess values are ignored.

By design the loader flattens the level into a single base `Tilemap`: all `type == "tiles"` entries
write into it in array order, layer names are not preserved, and later layers overwrite earlier
cells (including with zero). Use one tiles layer, or the last tiles layer as the authoritative one,
when authoring level JSON (VDOC-239). The loader retains at most 512 objects. Theme, object type,
and object ID are stored in 32-byte fields; values longer than 31 bytes are truncated **on a UTF-8
character boundary** so the stored value is always valid UTF-8 — a multi-byte sequence straddling
the cutoff is dropped whole rather than left as an invalid fragment (VDOC-239).

## API

- `Zanna.Game2D.LevelDocument.Load(path)` returns a `Zanna.Game2D.LevelDocument`, or null for a
  missing file, empty content, malformed JSON, invalid dimensions, or allocation failure. A missing
  path is pre-checked with the non-trapping existence helper (mirroring `Zanna.Game.Config.Load`),
  so the most common load failure soft-fails to null instead of trapping (VDOC-238). Other I/O
  faults (permission, non-regular file, short read) still surface as traps from the hardened read
  path.
- `Tilemap`, `ObjectCount`, `PlayerStartX`, `PlayerStartY`, and `Theme` expose the loaded values.
- `ObjectType(index)`, `ObjectId(index)`, `ObjectX(index)`, and `ObjectY(index)` return an empty
  string or zero for an invalid index, which is indistinguishable from stored empty/zero data.

## Example

```zia
module LevelDocumentExample;

func start() {
    // ... the application supplies an existing, validated level file.
    var level = Zanna.Game2D.LevelDocument.Load("levels/level1.json");
    var count = level.ObjectCount;
    Zanna.Terminal.SayInt(count);
}
```
