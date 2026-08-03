---
status: active
audience: public
last-verified: 2026-08-03
---

# 2D Shapes, Text, And UI
> Vector-style drawing helpers, text measurement/rendering wrappers, nine-slice UI images, and retained debug overlays.

**Part of [Graphics](README.md)**

These classes draw into `Pixels` or onto `Canvas` and are grouped by what they render, not by backend.

## Classes

| Class | Purpose |
|-------|---------|
| `Path2D` | Dynamic path of move/line points that can draw line segments to `Pixels`. |
| `ShapeRenderer2D` | Convenience renderer for lines, rectangles, circles, and paths on `Pixels`. |
| `TextRenderer2D` | Built-in or `BitmapFont` text measurement and Canvas drawing wrapper. |
| `TextLayout2D` | Text measurement helper with scale, wrap width, alignment metadata, and optional font. |
| `BitmapFont` | Game-facing alias for `BitmapFont` loading and measurement. |
| `SdfFont` | SDF-ready font wrapper around `BitmapFont`; the current backend uses bitmap font raster drawing. |
| `NineSlice2D` | Stretchable nine-slice drawing into a `Pixels` target. |
| `DebugDraw2D` | Retained debug line, rectangle, and circle draw queue for `Pixels`. |

## Drawing Conventions

- Shape, path, and debug draw colors accept Canvas-style `0x00RRGGBB`, raw pixel `0xRRGGBBAA`, and `Color.Rgba(...)` values; alpha is ignored by these RGB-only CPU drawing primitives.
- `NineSlice2D` alpha-composites over the destination `Pixels`, making it suitable for UI panels generated from sprite assets.
- `NineSlice2D.New` requires a `Pixels` source image and retains it. Border widths are clamped to the source dimensions.

## Text And UI

`TextRenderer2D` wraps the built-in Canvas text path or a loaded `BitmapFont`. `TextLayout2D` adds measurement state such as scale, wrap width, and alignment metadata. Measurements respect explicit newlines and preserve leading, trailing, repeated, and whitespace-only spans. When `WrapWidth` is positive, words and whitespace runs stay intact when they fit; overlong runs split only between UTF-8 input units, and every reported width is the width of actual glyph content rather than an artificial full-width chunk.

`NineSlice2D` preserves corner pixels and stretches edges and center regions into a target.

`DebugDraw2D` stores transient diagnostics separately from the game scene. Clear it each frame after drawing if the overlays are frame-local.

## Notes

- `SdfFont` is an SDF-ready API surface, not a full signed-distance-field rasterizer yet. It accepts either a `BitmapFont` or `null` as its bitmap backing font.
