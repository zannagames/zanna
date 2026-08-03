---
status: active
audience: public
last-verified: 2026-08-03
---

# 2D Tilemaps and Layers
> Tilesets, dense tile layers, object layers, autotile resolution, tilemap rendering helpers, and atlas import helpers.

**Part of [Graphics](README.md)**

Use this page for tile-oriented map data and editor/import helpers. The core `Tilemap` class is documented with [Images and Sprites](pixels.md).

## Classes

| Class | Purpose |
|-------|---------|
| `TileSet2D` | Uniform grid tileset over a `Pixels` image. |
| `TileLayer2D` | Dense tile ID layer with visibility and percent opacity. |
| `ObjectLayer2D` | Rect object layer for collision, triggers, spawn points, and editor metadata. |
| `AutoTile2D` | 16-mask autotile resolver that can apply resolved tile IDs to a `TileLayer2D`. |
| `TilemapRenderer2D` | Tilemap draw facade with draw-count tracking and optional chunk-cache association. |
| `TileChunkCache2D` | Chunk sizing and dirty-count tracking for tilemap renderers and editors. |
| `TexturePackerAtlas` | Texture-atlas authoring wrapper over `TextureAtlas` for named regions. |
| `AsepriteImporter` | Grid-to-atlas helper for Aseprite-style sprite sheets. |
| `TiledMapLoader` | Tile-size helper plus dependency-aware Tiled JSON/TMX import to a render-ready `Tilemap`. |

## Tilesets, Layers, And Atlas Helpers

```zia
var tiles = TileSet2D.New(Pixels.Load("assets/tiles.png"), 16, 16)
var ground = TileLayer2D.New(128, 64)
ground.Fill(0)
ground.Set(10, 12, 7)

var objects = ObjectLayer2D.New(32)
objects.AddRect(160, 192, 16, 16, 1)

var auto = AutoTile2D.New()
auto.SetVariant(5, 42)
auto.Apply(ground, 10, 12, 5)
```

`TileSet2D.New` requires a `Pixels` image whose dimensions are at least one full tile. The tileset retains the image and indexes tiles left-to-right, top-to-bottom from zero. `TileLayer2D.Get` returns `-1` for out-of-bounds coordinates, and `TileLayer2D.Opacity` is clamped to `0..100`. TileSet and TileLayer APIs validate their handles; invalid non-matching handles return safe defaults or no-op.

`Zanna.Graphics2D.Tilemap` and `Zanna.Game2D.SceneDocument.BuildTilemap()` use the render
tile ID convention `0` = empty/not drawn and `N > 0` = tileset frame `N - 1`.
This keeps scene documents able to distinguish an empty cell from the first
drawable tileset frame.

`ObjectLayer2D.AddRect` normalizes negative width or height by moving the origin to the rectangle's top-left corner. Zero-size rectangles and dimensions that cannot be represented are rejected.

## TiledMapLoader

`TiledMapLoader.LoadResult(path)` imports a loose finite or infinite Tiled JSON
or TMX map and returns `Ok(Tilemap)` or `Err(message)`. `LoadAssetResult(path)` uses
the asset manager for the root, external TSJ/TSX tilesets, templates, and images,
so the dependency graph can live in a mounted ZPAK or embedded asset blob. `Load`
and `LoadAsset` are nullable compatibility forms.

Both paths use the same normalization as
[`SceneDocument.ImportTiled*`](../game/scene.md#tiled-json-and-tmx-import), then
call `BuildTilemap` and compose all reachable atlas/image-collection art into one
bounded source-frame atlas. Mixed tilesets, margin/spacing, tile drawing offsets,
larger artwork, GID transforms, inherited tint/opacity, and fractional layer
placement are retained. Orthogonal, isometric, staggered, hexagonal, and oblique
maps share the same logical collision grid; projection affects drawing,
coordinate conversion, viewport culling, scaled drawing, and scaled hit tests.
The map-authored tile size is used; `SetTileSize` remains the default only for
`NewTilemap(width, height)`.

## Notes

- `TexturePackerAtlas`, `AsepriteImporter`, and `TiledMapLoader` are runtime-side helpers for common 2D asset layouts.
- `TilemapRenderer2D.DrawCount` reports the number of non-empty, valid tiles drawn by the last `Draw` or `DrawRegion` call, not the number of renderer method calls.
- `Tilemap.CountDrawnRegion(x, y, width, height)`, `Tilemap.CountDrawnVisible(canvas, offsetX, offsetY)`, and `Tilemap.CountDrawnVisibleScaled(canvas, offsetX, offsetY, scalePercent)` expose drawable-tile counting logic for tests, debug overlays, and editor diagnostics.
- `Tilemap.DrawScaled` and `Tilemap.HitTestScaled` scale imported source frames,
  projection geometry, and authored offsets while keeping the caller's camera
  offset in destination pixels. Staggered and hexagonal hit tests use their
  exact cell shapes rather than an orthogonal approximation.
- Native and scaled Tilemap drawing use source-over alpha compositing, so
  transparent tile texels preserve lower layers and existing canvas content.
- `AsepriteImporter` is a grid helper rather than a `.aseprite` parser; `TiledMapLoader`
  does parse supported Tiled JSON/TMX files and returns explicit Result diagnostics.
- Atlas regions are validated against their backing `Pixels` buffer before registration.
- `AsepriteImporter.SetGrid(width, height)` treats non-positive dimensions as an unset grid. `ToAtlas(pixels)` returns `null` until both frame dimensions are positive.
