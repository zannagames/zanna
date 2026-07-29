---
status: active
audience: public
last-verified: 2026-07-29
---

# 2D Rendering and Effects
> Offscreen surfaces, texture handles, draw command queues, effects, post-processing, blend state, and color helpers.

**Part of [Graphics](README.md)**

These classes sit directly on top of `Pixels` and `Canvas`. They cover rendering state and image-processing workflows; tilemaps, UI helpers, animation, and collision live on separate pages.

## Classes

| Class | Purpose |
|-------|---------|
| `RenderTarget2D` | Offscreen RGBA surface with alpha-aware `DrawPixels` and `DrawRegion`. |
| `Surface2D` | RenderTarget-compatible surface with its own runtime type identity. |
| `Texture2D` | Retained `Pixels` texture handle with `Filter`, `Wrap`, `ClonePixels`, and `FromFile`. |
| `GpuTexture2D` | Texture-compatible handle with its own runtime type identity; currently CPU-backed. |
| `Renderer2D` | Retained draw command stream for pixels/textures with tint, alpha, blend mode, and render target flushing. |
| `Material2D` | Tint, alpha, and blend-mode state that can produce processed pixel copies. |
| `Shader2D` | CPU image effect wrapper: none, invert, grayscale, tint, and blur. |
| `PostProcess2D` | Distinct effect pass wrapper for applying `Shader2D`-style operations to a `Pixels` buffer. |
| `Sampler2D` | Reusable texture sampler state for `Filter` and `Wrap`. |
| `BlendState2D` | Reusable renderer blend state for blend mode, tint, and alpha. |
| `SpriteRenderer2D` | Draw helper that applies `Material2D`, `Sampler2D`, and `BlendState2D` before queueing pixels or textures. |
| `RenderPass2D` | Source-target postprocess pass using an optional `Shader2D` or `PostProcess2D`. |
| `RenderGraph2D` | Ordered collection of `RenderPass2D` objects for simple pass chains. |
| `Palette2D` | 256-entry palette that can quantize `Pixels` to nearest palette colors or run legacy indexed remaps. |
| `Gradient2D` | RGBA gradient sampling and horizontal/vertical fills for `Pixels`. |

## Color And Blend Conventions

- `Pixels` storage is raw `0xRRGGBBAA`.
- `Palette2D` and `Gradient2D` store raw `0xRRGGBBAA` colors, and also accept tagged `Color.Rgba(...)` values by converting them into raw pixel storage. Public `GetColor` and `Sample` return `Color`-compatible values; use `GetRgba` and `SampleRgba` for raw storage integers.
- Renderer/material/blend-state tint uses `-1` for no tint. A tint value of `0` is black.
- Blend modes use `0 = alpha`, `1 = opaque`, `2 = additive`. Alpha mode uses straight-alpha source-over, matching `Pixels.BlendPixel` and `Canvas.BlitAlpha`; additive mode scales source RGB by source alpha, adds it to the destination, and clamps each channel.
- `Texture2D.Filter` uses `0 = nearest`, `1 = linear`. Linear sampling interpolates RGB in premultiplied-alpha space so transparent edge texels do not bleed black into partially transparent results. Texture-region draws clamp or wrap within the requested region before sampling the backing image, so atlas neighbors do not bleed into bilinear samples.
- `Texture2D.Wrap` uses `0 = clamp`, `1 = repeat`.
- `Texture2D.New` requires a `Pixels` object and retains it for the texture lifetime. `Renderer2D` also retains queued sources before publishing commands, so queued draw calls keep their source objects alive until `Begin`, `End`, or object cleanup clears the queue. `FlushToTarget` replays without clearing.
- `RenderTarget2D.DrawRegion` supports drawing from its own `Pixels` buffer. Overlapping self-copies use a source snapshot so pixels are copied from the original region rather than from already-written output.
- Scaled and rotated renderer draws also snapshot when a texture is backed by the destination `Pixels`, making self-draws independent of scan order.
- Graphics2D handle validators check the runtime class and minimum object size. Passing the wrong object type to render target, texture, tileset, tile layer, viewport, shader, or post-process APIs returns safe defaults or no-ops instead of interpreting unrelated memory as that type. Surface/texture/scaler compatibility APIs accept both the canonical and alias runtime classes.

## Render Targets, Textures, And Renderer

```zia
var target = RenderTarget2D.New(320, 180)
var spritePixels = Pixels.Load("assets/player.png")
var texture = Texture2D.New(spritePixels)

var renderer = Renderer2D.New(256)
renderer.Begin()
renderer.SetAlpha(255)
renderer.DrawTexture(texture, 32, 48)
renderer.DrawTextureScaled(texture, 96, 48, 64, 64)
renderer.DrawTextureRotatedAt(texture, 160, 48, 16, 16, 45.0)
renderer.FlushToTarget(target)

canvas.BlitAlpha(0, 0, target.Pixels)
```

`Renderer2D` keeps retained references to queued sources, so textures and pixels can be queued safely during a frame. Calling `Begin` clears the previous command list. `FlushToTarget(target)` draws to an offscreen target without ending the batch; `End(canvas)` draws to a Canvas with the queued blend modes, clears queued commands, and makes repeated `End` calls a no-op until the next `Begin`. `DrawTextureScaled(texture, x, y, width, height)` uses the texture's nearest or linear filter. `DrawTextureRegion(texture, x, y, sx, sy, width, height)` samples out-of-bounds source texels through the texture's clamp or repeat wrap mode. `DrawTextureRotated(texture, x, y, angleDegrees)` rotates around the texture center; `DrawTextureRotatedAt(texture, x, y, pivotX, pivotY, angleDegrees)` rotates around a source-local pivot. Rotation angles must be finite; valid angles are normalized before trigonometry. Additive `End(canvas)` clips correctly when a queued source is partially off-screen.

`SpriteRenderer2D` snapshots `Sampler2D` state when queuing a texture draw. It does not mutate the `Texture2D`; call `Sampler2D.ApplyToTexture(texture)` only when you explicitly want to change the texture's stored filter and wrap properties. Material and blend overrides are scoped to that draw call, so later direct `Renderer2D` draws keep the renderer state you set.

## Passes And Color Helpers

```zia
var palette = Palette2D.New()
palette.SetColor(3, 0xFF0000FF)
palette.SetColor(4, Color.Rgba(0, 0, 255, 128))
var recolored = palette.Apply(sourcePixels)
var legacyRecolored = palette.ApplyLegacy(oldAlphaByteIndexedPixels)

var gradient = Gradient2D.New(0x000000FF, Color.Rgba(255, 255, 255, 192), 16)
gradient.FillHorizontal(pixels)
```

`Palette2D.Apply` maps each source pixel to the nearest defined palette color in RGBA space and writes a new `Pixels` buffer. Empty palettes return a distinct exact clone. `ApplyLegacy` keeps the older indexed behavior: the source red byte is the palette index, and `0x000000II` alpha-byte indices are also accepted for assets that used fully transparent pixels as palette indices.

`Palette2D.GetColor(index)` and `Gradient2D.Sample(t)` return values that work with `Color.GetR/G/B/A`, preserving alpha from raw pixel storage. `Gradient2D.Sample(t)` and `Gradient2D.SampleRgba(t)` take a normalized `Number` position from `0.0` to `1.0`; values above `1.0` are accepted as legacy percent positions. `Palette2D.GetRgba(index)` and `Gradient2D.SampleRgba(t)` return raw `0xRRGGBBAA`. Use `Gradient2D.SamplePercent(percent)` and `Gradient2D.SampleRgbaPercent(percent)` when you want the explicit integer percent form.

`Gradient2D` uses `Steps <= 2` as smooth interpolation. Larger `Steps` values quantize into that many discrete levels, including both endpoints. For example, a three-step gradient samples start, midpoint, and end colors; horizontal and vertical fills use the same interpolation and quantization as the sampling methods. A fill advances the destination mutation generation once when content changes and preserves it for an idempotent repeat.

## Video Playback

### Zanna.Graphics.VideoPlayer

Software video decoder that exposes each decoded frame as a `Pixels` object for display on a `Canvas` or as a `Texture2D`. Suitable for cutscenes, loading screens, and in-game video surfaces.

**Type:** Instance (obj)
**Constructor:** `VideoPlayer.Open(path)`

#### Properties

| Property    | Type    | Access | Description |
|-------------|---------|--------|-------------|
| `Width`     | Integer | Read   | Frame width in pixels |
| `Height`    | Integer | Read   | Frame height in pixels |
| `Duration`  | Double  | Read   | Total video duration in seconds |
| `Position`  | Double  | Read   | Current playback position in seconds |
| `IsPlaying` | Integer | Read   | Non-zero while the video is playing |
| `Frame`     | Object  | Read   | The most recently decoded frame as `Pixels` |

#### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Play()` | `Void()` | Start or resume playback |
| `Pause()` | `Void()` | Pause at the current frame |
| `Stop()` | `Void()` | Stop and reset to the start |
| `Seek(time)` | `Void(Double)` | Seek to a position in seconds |
| `Update(deltaSeconds)` | `Void(Double)` | Advance the decoder by the elapsed time |
| `SetVolume(volume)` | `Void(Double)` | Set playback volume `[0.0–1.0]` |

```zia
bind Zanna.Graphics.VideoPlayer as VideoPlayer;

var video = VideoPlayer.Open("assets/intro.ogv")
video.SetVolume(0.8)
video.Play()

// per frame
video.Update(deltaSeconds)
canvas.Blit(0, 0, video.Frame)
```

```basic
DIM video AS Zanna.Graphics.VideoPlayer
video = Zanna.Graphics.VideoPlayer.Open("assets/intro.ogv")
video.Play()

' per frame
video.Update(deltaSeconds)
canvas.Blit(0, 0, video.Frame)
```

`VideoPlayer` methods validate that their receiver is a `VideoPlayer` handle; unrelated objects are ignored or return zero/null defaults. `Seek` clamps non-finite or out-of-range times before frame selection. `Frame` returns the same `Pixels` object each frame until the video ends; copy if you need to retain a specific frame. Calling `Update` more than once per logical frame skips decoding for the excess calls.

---

## Notes

- `RenderTarget2D`, `GpuTexture2D`, and `Viewport2D` expose compatibility behavior while retaining distinct runtime class IDs for code that needs exact type identity.
- `PostProcess2D` has its own runtime class. It is accepted wherever a render pass expects an effect, but it is not treated as a `Shader2D` by direct shader APIs.
- `RenderPass2D.New` requires valid `RenderTarget2D` or `Surface2D` source and target objects; invalid handles return `null`. `SetSource` and `SetTarget` accept valid render surfaces or `null`, and ignore unrelated handles. `SetShader` accepts either a `Shader2D`, a `PostProcess2D`, or `null`; unrelated handles are ignored.
- `VideoPlayer` decodes in software on the calling thread. For best results call `Update` once per frame with the actual elapsed seconds rather than a fixed timestep.
