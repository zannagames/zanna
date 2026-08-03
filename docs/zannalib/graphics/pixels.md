---
status: active
audience: public
last-verified: 2026-08-03
---

# Images & Sprites
> Pixels, Sprite, SpriteSheet, Tilemap

**Part of [Zanna Runtime Library](../README.md) › [Graphics](README.md)**

---

## Zanna.Graphics.Pixels

Software image buffer for direct pixel manipulation. Use for procedural texture generation, image processing, or custom
rendering.

**Type:** Instance class

**Constructor:** `NEW Zanna.Graphics.Pixels(width, height)`

Creates a new pixel buffer initialized to transparent black (0x00000000). Negative dimensions and dimensions whose storage size would overflow fail cleanly instead of creating a wrapped or zero-sized buffer.

### Properties

| Property | Type    | Access | Description                    |
|----------|---------|--------|--------------------------------|
| `Width`  | Integer | Read   | Width of the buffer in pixels  |
| `Height` | Integer | Read   | Height of the buffer in pixels |

### Methods

| Method                            | Signature                                                            | Description                                                                       |
|-----------------------------------|----------------------------------------------------------------------|-----------------------------------------------------------------------------------|
| `Blur(radius)`                    | `Pixels(Integer)`                                                    | Return an alpha-aware box-blurred copy (`0` = exact copy, positive radii use separable horizontal+vertical passes) |
| `BlendPixel(x, y, color, alpha)`  | `Void(Integer, Integer, Integer, Integer)`                           | Source-over blend `0x00RRGGBB` over one pixel using `alpha` 0-255 |
| `Clear()`                         | `Void()`                                                             | Clear buffer to transparent black (0x00000000)                                    |
| `Clone()`                         | `Pixels()`                                                           | Create a deep copy of this buffer                                                 |
| `Copy(dx, dy, src, sx, sy, w, h)` | `Void(Integer, Integer, Pixels, Integer, Integer, Integer, Integer)` | Copy a rectangle from source to this buffer                                       |
| `Fill(color)`                     | `Void(Integer)`                                                      | Fill with raw `0xRRGGBBAA` storage                                                |
| `FillRgba(color)`                 | `Void(Integer)`                                                      | Explicit raw `0xRRGGBBAA` fill alias                                              |
| `FillColor(color)`                | `Void(Integer)`                                                      | Fill from `Color.Rgb()`, Canvas `0x00RRGGBB`, or `Color.Rgba()`                   |
| `FlipH()`                         | `Pixels()`                                                           | Return a horizontally mirrored copy (left-right)                                  |
| `FlipV()`                         | `Pixels()`                                                           | Return a vertically mirrored copy (top-bottom)                                    |
| `Get(x, y)`                       | `Integer(Integer, Integer)`                                          | Get raw `0xRRGGBBAA` storage at (x, y). Returns 0 if out of bounds                |
| `GetRgba(x, y)`                   | `Integer(Integer, Integer)`                                          | Get raw `0xRRGGBBAA` storage at (x, y). Returns 0 if out of bounds                |
| `GetColor(x, y)`                  | `Integer(Integer, Integer)`                                          | Get pixel at (x, y) as a `Color`-compatible value; use `Color.Get*` to read components |
| `Grayscale()`                     | `Pixels()`                                                           | Return a grayscale copy of the image                                              |
| `Invert()`                        | `Pixels()`                                                           | Return a copy with all colors inverted (255 minus each channel)                   |
| `Resize(width, height)`           | `Pixels(Integer, Integer)`                                           | Return an endpoint-preserving scaled copy using alpha-aware bilinear interpolation; target dimensions must be positive |
| `Rotate(angle)`                   | `Pixels(Double)`                                                     | Return a copy rotated around its pixel center by degrees (positive = clockwise); non-finite angles trap |
| `Rotate180()`                     | `Pixels()`                                                           | Return a 180-degree rotated copy                                                  |
| `RotateCounterClockwise()`                     | `Pixels()`                                                           | Return a 90-degree counter-clockwise rotated copy (swaps dimensions)              |
| `RotateClockwise()`                      | `Pixels()`                                                           | Return a 90-degree clockwise rotated copy (swaps dimensions)                      |
| `SaveBmp(path)`                   | `Integer(String)`                                                    | Save to a BMP file. Returns 1 on success, 0 on failure                            |
| `SavePng(path)`                   | `Integer(String)`                                                    | Save to a PNG file. Returns 1 on success, 0 on failure                            |
| `Scale(width, height)`            | `Pixels(Integer, Integer)`                                           | Return an endpoint-preserving scaled copy using nearest-neighbor interpolation; target dimensions must be positive |
| `Set(x, y, color)`                | `Void(Integer, Integer, Integer)`                                    | Set raw `0xRRGGBBAA` storage. Silently ignores out of bounds                      |
| `SetRgba(x, y, color)`            | `Void(Integer, Integer, Integer)`                                    | Explicit raw `0xRRGGBBAA` set alias                                               |
| `SetColor(x, y, color)`           | `Void(Integer, Integer, Integer)`                                    | Set from `Color.Rgb()`, Canvas `0x00RRGGBB`, or `Color.Rgba()`                    |
| `Tint(color)`                     | `Pixels(Integer)`                                                    | Return a copy with a color tint applied; `Color.Rgba` input also scales alpha     |
| `ToBytes()`                       | `Bytes()`                                                            | Convert to raw bytes (RGBA, row-major)                                            |

### Static Methods

| Method                            | Signature                         | Description                                           |
|-----------------------------------|-----------------------------------|-------------------------------------------------------|
| `FromBytes(width, height, bytes)` | `Pixels(Integer, Integer, Bytes)` | Create from raw bytes (RGBA, row-major); traps on non-Bytes input or insufficient byte count |
| `LoadBmp(path)`                   | `Pixels(String)`                  | Load from a 24-bit BMP file. Returns null on failure  |
| `LoadPng(path)`                   | `Pixels(String)`                  | Load from a PNG file. Returns null on failure          |
| `LoadJpeg(path)`                  | `Pixels(String)`                  | Load from a JPEG file. Returns null on failure         |
| `LoadGif(path)`                   | `Pixels(String)`                  | Load first frame from a GIF file. Returns null on failure |
| `Load(path)`                      | `Pixels(String)`                  | Auto-detect format (PNG/JPEG/BMP/GIF) and load. Returns null on failure |

`LoadPng` rejects malformed chunk order, invalid IHDR compression/filter/interlace methods, indexed images without a palette, palette transparency entries that exceed the palette size, and PNG files without IEND. Sub-byte grayscale transparency compares the raw sample value from `tRNS`. `LoadBmp` uses checked file offsets when seeking to pixel data.
`ToBytes()` and `FromBytes()` always use canonical `RR GG BB AA` bytes for each pixel, independent of CPU endianness. `FromBytes` requires an actual `Zanna.Collections.Bytes` object.
JPEG loading validates component, quantization, Huffman, scan, and chroma sampling tables before decode, and applies EXIF orientation without leaking the pre-rotated image. Only baseline (sequential) DCT JPEGs are supported; progressive, lossless, and arithmetic-coded JPEGs are rejected and return null.

### Drawing Primitives

Drawing primitives accept `0x00RRGGBB`, `Color.Rgb()`, and tagged `Color.Rgba()` values. RGB-only inputs draw with alpha 255; `Color.Rgba()` preserves its alpha in pixel storage. Coordinates outside the pixel buffer are silently clipped, including extreme coordinates that would otherwise overflow rectangle or line endpoint math.

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetRgb(x, y, color)` | `Void(Integer, Integer, Integer)` | Set pixel using `0x00RRGGBB` format (alpha = 255) |
| `GetRgb(x, y)` | `Integer(Integer, Integer)` | Get pixel as `0x00RRGGBB` (alpha discarded) |
| `DrawLine(x1, y1, x2, y2, color)` | `Void(Integer...)` | Bresenham line between two points |
| `DrawBox(x, y, w, h, color)` | `Void(Integer...)` | Filled rectangle |
| `DrawFrame(x, y, w, h, color)` | `Void(Integer...)` | Rectangle outline |
| `DrawDisc(cx, cy, r, color)` | `Void(Integer...)` | Filled circle |
| `DrawRing(cx, cy, r, color)` | `Void(Integer...)` | Circle outline |
| `DrawEllipse(cx, cy, rx, ry, color)` | `Void(Integer...)` | Filled ellipse |
| `DrawEllipseFrame(cx, cy, rx, ry, color)` | `Void(Integer...)` | Ellipse outline |
| `FloodFill(x, y, color)` | `Void(Integer...)` | Iterative scanline flood fill (no recursion depth limit) |
| `DrawThickLine(x1, y1, x2, y2, thickness, color)` | `Void(Integer...)` | Line with thickness (parallelogram body + rounded endcap circles) |
| `DrawTriangle(x1, y1, x2, y2, x3, y3, color)` | `Void(Integer...)` | Filled triangle; collinear vertices draw the longest edge |
| `DrawBezier(x1, y1, cx, cy, x2, y2, color)` | `Void(Integer...)` | Quadratic Bezier curve |
| `DrawText(x, y, text, color)` | `Void(Integer, Integer, String, Integer)` | Draw built-in 8x8 bitmap-font text |
| `DrawTextBg(x, y, text, fg, bg)` | `Void(Integer, Integer, String, Integer, Integer)` | Draw text with a filled background cell behind each glyph pixel |
| `DrawTextScaled(x, y, text, scale, color)` | `Void(Integer, Integer, String, Integer, Integer)` | Draw built-in text with integer pixel scaling |
| `DrawTextScaledBg(x, y, text, scale, fg, bg)` | `Void(Integer, Integer, String, Integer, Integer, Integer)` | Draw scaled text with a filled background cell behind each glyph pixel |
| `DrawTextCentered(y, text, color)` | `Void(Integer, String, Integer)` | Draw text centered horizontally in this buffer |
| `DrawTextRight(margin, y, text, color)` | `Void(Integer, Integer, String, Integer)` | Draw text right-aligned with a margin |
| `DrawTextCenteredScaled(y, text, color, scale)` | `Void(Integer, String, Integer, Integer)` | Draw scaled text centered horizontally |
| `TextWidth(text)` | `Integer(String)` | Measure built-in text width in pixels at 1x |
| `TextHeight()` | `Integer()` | Return the built-in font height (8 pixels) |
| `TextScaledWidth(text, scale)` | `Integer(String, Integer)` | Measure built-in text width in pixels at integer scale |

> **Color format note:** `Pixels.Set`, `Pixels.Fill`, and `Pixels.Get` are raw-storage APIs for
> `0xRRGGBBAA`; `SetRgba`, `FillRgba`, and `GetRgba` are explicit raw aliases. Drawing primitives
> and `SetColor`/`FillColor`/`GetColor` accept or return values compatible with `Zanna.Graphics.Color`.
> `SetRgb`/`GetRgb` remain RGB-only convenience methods.

Text rendering uses the same embedded monospace 8x8 bitmap font as `Canvas.Text`. Non-ASCII UTF-8
codepoints measure as one cell and render with the fallback glyph. All text methods clip silently at
the buffer edge and accept `0x00RRGGBB`, `Color.Rgb()`, or tagged `Color.Rgba()` colors.

#### Zia Example — Drawing into an off-screen buffer

```zia
module PixelsDrawDemo;

bind Zanna.Graphics;
bind Zanna.Graphics.Color as Color;

func start() {
    // Create an off-screen pixel buffer and draw into it
    var buf = Pixels.New(320, 240);

    // All drawing uses 0x00RRGGBB — same as Canvas and Color.Rgb()
    buf.DrawBox(0, 0, 320, 240, Color.Rgb(30, 30, 30));       // dark background
    buf.DrawDisc(160, 120, 80, Color.Rgb(0, 120, 220));        // blue filled circle
    buf.DrawRing(160, 120, 80, Color.Rgb(255, 255, 255));      // white outline
    buf.DrawLine(0, 0, 319, 239, Color.Rgb(255, 80, 0));       // orange diagonal
    buf.DrawEllipse(160, 60, 60, 25, Color.Rgb(200, 0, 200)); // purple ellipse
    buf.FloodFill(5, 5, Color.Rgb(10, 10, 50));                // flood fill corner
    buf.DrawText(16, 16, "Pixels", Color.Rgb(255, 255, 255));  // direct text rasterization

    // Blit the finished buffer to a canvas for display
    var c = Canvas.New("Pixels Draw Demo", 320, 240);
    while !c.get_ShouldClose() {
        c.Poll();
        c.Blit(0, 0, buf);
        c.Flip();
    }
}
```

### Color Format

Raw pixel storage uses packed 32-bit RGBA in the format `0xRRGGBBAA`:

- `RR` - Red component (0-255)
- `GG` - Green component (0-255)
- `BB` - Blue component (0-255)
- `AA` - Alpha component (0-255, where 255 = opaque)

`Pixels.Get` and `Pixels.GetRGBA` return the raw `0xRRGGBBAA` integer. `Pixels.Set`, `SetRgba`,
`Fill`, and `FillRgba` write raw `0xRRGGBBAA` storage. `GetColor`, `SetColor`, and `FillColor` are
the color-compatible forms for `Zanna.Graphics.Color` values and Canvas-style `0x00RRGGBB` colors.

**Drawing primitives** (`DrawLine`, `DrawBox`, etc.) use the same color inputs as Canvas methods and
`Color.Rgb()`. This makes it straightforward to share color constants between on-screen canvas drawing
and off-screen pixel buffer operations. `Color.Rgba()` values preserve alpha in the destination pixels.

Use packed literals like `0xRRGGBBAA` with `Set`/`Get`/`Fill` or the explicit `*RGBA` aliases, and
`Zanna.Graphics.Color.Rgb()` or `Color.Rgba()` with `SetColor`/`GetColor`/`FillColor` and drawing
primitives.

> `Zanna.Graphics.Color.Rgba()` returns a tagged `0xAARRGGBB` color value so alpha can be preserved
> even when `a = 0`. It does **not** match raw `0xRRGGBBAA` storage. Use `SetColor`/`FillColor` for
> tagged colors, or use `Set`/`Fill`/`SetRgba`/`FillRgba` for explicit raw storage.

### Zia Example

```zia
module PixelsDemo;

bind Zanna.Terminal;
bind Zanna.Graphics.Pixels as Pixels;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var p = Pixels.New(64, 64);
    Say("Size: " + Fmt.Int(p.get_Width()) + "x" + Fmt.Int(p.get_Height()));

    // Color-compatible Set/Get work with Color.Rgb/Rgba and Canvas RGB values
    p.SetColor(0, 0, Color.Rgb(255, 0, 0));
    p.SetColor(1, 0, Color.Rgba(0, 0, 255, 128));
    var c = p.GetColor(0, 0);
    Say("Pixel(0,0) red: " + Fmt.Int(Color.GetRed(c)));

    // Raw storage is available when needed
    p.Set(2, 0, 0x10203040);
    Say("Raw Pixel(2,0): " + Fmt.Int(p.Get(2, 0)));

    // Fill and clear
    p.FillColor(Color.Rgba(0, 255, 0, 192));
    p.Clear();

    // Clone
    var clone = p.Clone();
    Say("Clone: " + Fmt.Int(clone.get_Width()) + "x" + Fmt.Int(clone.get_Height()));

    // Transform operations return new Pixels objects
    var flipped = p.Clone();
    flipped = flipped.FlipH();
    var rotated = p.RotateClockwise();
    var scaled = p.Scale(128, 128);
    Say("Scaled: " + Fmt.Int(scaled.get_Width()) + "x" + Fmt.Int(scaled.get_Height()));
}
```

### Example

```basic
DIM pixels AS Zanna.Graphics.Pixels
pixels = NEW Zanna.Graphics.Pixels(256, 256)

' Create a gradient
DIM x AS INTEGER
DIM y AS INTEGER
FOR y = 0 TO 255
    FOR x = 0 TO 255
        ' Red increases left-to-right, green increases top-to-bottom
        DIM r AS INTEGER = x
        DIM g AS INTEGER = y
        DIM color AS INTEGER = Zanna.Graphics.Color.Rgb(r, g, 0)
        pixels.SetRgb(x, y, color)
    NEXT x
NEXT y

' Copy a region
DIM copy AS Zanna.Graphics.Pixels
copy = NEW Zanna.Graphics.Pixels(64, 64)
copy.Copy(0, 0, pixels, 100, 100, 64, 64)

' Clone the entire buffer
DIM backup AS Zanna.Graphics.Pixels
backup = pixels.Clone()

' Convert to bytes for serialization
DIM data AS Zanna.Collections.Bytes
data = pixels.ToBytes()

' Save to BMP file
pixels.SaveBmp("output.bmp")

' Load from BMP file
DIM loaded AS Zanna.Graphics.Pixels
loaded = Zanna.Graphics.Pixels.LoadBmp("input.bmp")
IF loaded <> NULL THEN
    PRINT "Loaded image: "; loaded.Width; "x"; loaded.Height
END IF

' Transform operations (all return new Pixels objects)
DIM flipped AS Zanna.Graphics.Pixels
flipped = pixels.FlipH()     ' Mirror horizontally
flipped = pixels.FlipV()     ' Mirror vertically

DIM rotated AS Zanna.Graphics.Pixels
rotated = pixels.RotateClockwise()  ' Rotate 90 degrees clockwise
rotated = pixels.RotateCounterClockwise() ' Rotate 90 degrees counter-clockwise
rotated = pixels.Rotate180() ' Rotate 180 degrees

' Scale to new dimensions (nearest-neighbor interpolation)
DIM scaled AS Zanna.Graphics.Pixels
scaled = pixels.Scale(128, 128)  ' Scale to 128x128
scaled = pixels.Scale(pixels.Width * 2, pixels.Height * 2)  ' Double size

' Resize with bilinear interpolation (smoother)
DIM resized AS Zanna.Graphics.Pixels
resized = pixels.Resize(128, 128)

' Image processing (all return new Pixels objects)
DIM inverted AS Zanna.Graphics.Pixels
inverted = pixels.Invert()      ' Invert all colors
DIM gray AS Zanna.Graphics.Pixels
gray = pixels.Grayscale()       ' Convert to grayscale
DIM tinted AS Zanna.Graphics.Pixels
tinted = pixels.Tint(16711680) ' Apply red tint
DIM blurred AS Zanna.Graphics.Pixels
blurred = pixels.Blur(3)        ' Box blur with radius 3

' PNG support
DIM pngImg AS Zanna.Graphics.Pixels
pngImg = Zanna.Graphics.Pixels.LoadPng("image.png")
pixels.SavePng("output.png")
```

### Notes

- Pixel data is stored in row-major order (row 0 first, then row 1, etc.)
- Out-of-bounds reads return 0 (transparent black)
- Out-of-bounds writes are silently ignored
- The `Copy` method automatically clips to buffer boundaries with overflow-safe source and destination arithmetic
- `BlendPixel` uses straight-alpha Porter-Duff source-over and preserves source RGB when blending over transparent pixels
- `ToBytes` returns 4 bytes per pixel (width × height × 4 total bytes)
- BMP support is limited to 24-bit uncompressed format (most common)
- When loading BMP files, alpha is set to 255 (opaque) for all pixels
- All transform operations (flip, rotate, scale, resize) return new Pixels objects
- `Scale` and `Resize` return `NULL` for non-positive target dimensions instead of silently resizing to `1x1`
- `FlipH`, `FlipV`, and `Rotate180` preserve zero-width or zero-height buffers without reading outside storage
- RotateCW and RotateCCW swap width and height dimensions
- `Rotate(angle)` rotates around the image's pixel center and expands the output just enough to contain the rotated pixel centers
- Scale uses endpoint-preserving nearest-neighbor interpolation (fast, no blending)
- Resize uses endpoint-preserving, alpha-aware bilinear interpolation (smoother, better for non-integer scale factors)
- Image processing methods (Invert, Grayscale, Tint, Blur) return new Pixels objects
- Tint multiplies RGB by the provided color and multiplies alpha when the color includes alpha
- Blur accumulates the complete two-dimensional box footprint in premultiplied-alpha form and quantizes only the final pixel, so low-alpha colors are not lost between its horizontal and vertical passes
- PNG loading validates chunk CRCs and zlib checksums, and supports palette, grayscale, RGB, RGBA, and truecolor `tRNS` transparency

---

## Zanna.Graphics.Sprite

2D sprite with transform, animation, and collision detection for game development.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics.Sprite(pixels)` or `Zanna.Graphics.Sprite.FromFile(path)`

### Static Methods

| Method           | Signature         | Description                                          |
|------------------|-------------------|------------------------------------------------------|
| `FromFile(path)` | `Sprite(String)`  | Load sprite from BMP, PNG, JPEG, or GIF file. Animated GIFs load all frames and preserve per-frame delays. Returns NULL on failure |

### Properties

| Property     | Type    | Access | Description                                  |
|--------------|---------|--------|----------------------------------------------|
| `FlipX`      | Integer | R/W    | Horizontal flip (1 = flipped, 0 = normal)    |
| `FlipY`      | Integer | R/W    | Vertical flip (1 = flipped, 0 = normal)      |
| `Frame`      | Integer | R/W    | Current animation frame index                |
| `FrameCount` | Integer | Read   | Total number of animation frames             |
| `Height`     | Integer | Read   | Height of current frame in pixels            |
| `Rotation`   | Integer | R/W    | Rotation in degrees                          |
| `ScaleX`     | Integer | R/W    | Horizontal scale (100 = 100%, values below 1 clamp to 1) |
| `ScaleY`     | Integer | R/W    | Vertical scale (100 = 100%, values below 1 clamp to 1)   |
| `Visible`    | Integer | R/W    | Visibility (1 = visible, 0 = hidden)         |
| `Width`      | Integer | Read   | Width of current frame in pixels             |
| `X`          | Integer | R/W    | X position in pixels                         |
| `Y`          | Integer | R/W    | Y position in pixels                         |

### Methods

| Method                      | Signature                          | Description                                           |
|-----------------------------|------------------------------------|-------------------------------------------------------|
| `AddFrame(pixels)`          | `Void(Pixels)`                     | Add an animation frame                                |
| `Contains(x, y)`            | `Integer(Integer, Integer)`        | Check if point is inside sprite (returns 1 or 0)      |
| `Draw(canvas)`              | `Void(Canvas)`                     | Draw the sprite to a canvas                           |
| `GetFrameDelayAt(frame)`    | `Integer(Integer)`                 | Get one frame's delay in milliseconds                 |
| `Move(dx, dy)`              | `Void(Integer, Integer)`           | Move sprite by delta amounts                          |
| `Overlaps(other)`           | `Integer(Sprite)`                  | Check bounding box overlap (returns 1 or 0)           |
| `SetFrameDelay(ms)`         | `Void(Integer)`                    | Set delay between animation frames (milliseconds)     |
| `SetFrameDelayAt(frame, ms)`| `Void(Integer, Integer)`           | Set one frame's delay in milliseconds                 |
| `SetOrigin(x, y)`           | `Void(Integer, Integer)`           | Set origin point for rotation/scaling                 |
| `Update()`                  | `Void()`                           | Advance animation (call each frame)                   |

Scaled sprite bounds used for `Contains()` and `Overlaps()` never collapse below `1x1`, even when
very small scale values are provided. Movement, hit tests, and overlap checks saturate at the int64 coordinate limits instead of wrapping, so sprites at the extreme edge of the coordinate range keep consistent 1-pixel-inclusive bounds.
Sprite drawing fails closed if an intermediate transform allocation fails, rather than drawing a partially transformed frame.
Setting `Frame` wraps into the available frame range, including negative frame indexes. `Sprite.New`
and `AddFrame` require real `Pixels` objects and retain the supplied frame buffers. Call
`Pixels.Clone()` first when you need the sprite to keep an independent snapshot. `AddFrame` grows the
frame table as needed instead of using a fixed frame cap. `SetFrameDelay(ms)` updates the default and
all existing frame delays; `SetFrameDelayAt(frame, ms)` customizes a single frame. Delays below 1 ms
are normalized to 1 ms. Assigning the already-current `Frame` is idempotent and does not restart the
animation clock. Rotation transforms the configured origin analytically, including origins outside
the frame; it does not allocate transparent padding proportional to the origin's distance.

### Zia Example

```zia
module SpriteDemo;

bind Zanna.Terminal;
bind Zanna.Graphics.Sprite as Sprite;
bind Zanna.Graphics.Pixels as Pixels;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    // Create sprite from pixels
    var px = Pixels.New(32, 32);
    px.Fill(0xFF0000FF);
    var s = Sprite.New(px);

    Say("Size: " + Fmt.Int(s.get_Width()) + "x" + Fmt.Int(s.get_Height()));

    // Position and transform
    s.set_X(100);
    s.set_Y(200);
    s.set_ScaleX(150);
    s.set_ScaleY(150);
    s.set_Rotation(45);
    Say("Pos: " + Fmt.Int(s.get_X()) + "," + Fmt.Int(s.get_Y()));

    // Movement
    s.Move(10, 20);
    Say("After move: " + Fmt.Int(s.get_X()) + "," + Fmt.Int(s.get_Y()));
}
```

### Example

```basic
' Create a sprite from file
DIM player AS Zanna.Graphics.Sprite
player = Zanna.Graphics.Sprite.FromFile("player.bmp")

IF player <> NULL THEN
    ' Set position
    player.X = 100
    player.Y = 200

    ' Set origin to center for rotation
    player.SetOrigin(player.Width / 2, player.Height / 2)

    ' Scale to 150%
    player.ScaleX = 150
    player.ScaleY = 150

    ' Add animation frames
    DIM frame2 AS Zanna.Graphics.Pixels
    frame2 = Zanna.Graphics.Pixels.LoadBmp("player2.bmp")
    player.AddFrame(frame2)
    player.SetFrameDelay(100)  ' 100ms between frames

    ' Game loop
    DO WHILE NOT canvas.ShouldClose
        canvas.Poll()
        canvas.Clear(0)

        ' Update animation
        player.Update()

        ' Move with arrow keys
        IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Right) THEN player.Move(5, 0)
        IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Left) THEN player.Move(-5, 0)

        ' Rotate with Q/E
        IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Q) THEN player.Rotation = player.Rotation - 2
        IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.E) THEN player.Rotation = player.Rotation + 2

        ' Draw sprite
        player.Draw(canvas)

        canvas.Flip()
    LOOP
END IF
```

### Collision Detection

```basic
DIM player AS Zanna.Graphics.Sprite
DIM enemy AS Zanna.Graphics.Sprite

' ... initialize sprites ...

' Check bounding box collision
IF player.Overlaps(enemy) = 1 THEN
    PRINT "Collision detected!"
END IF

' Check if mouse is over sprite
DIM mx AS INTEGER = Zanna.Input.Mouse.X
DIM my AS INTEGER = Zanna.Input.Mouse.Y
IF player.Contains(mx, my) = 1 THEN
    PRINT "Mouse over player!"
END IF
```

---

## Zanna.Graphics.SpriteAnimator

Named animation clip controller for `Sprite` frame playback.

**Type:** Instance (obj)
**Constructor:** `Zanna.Graphics.SpriteAnimator.New()`

### Properties

| Property    | Type    | Access | Description                                  |
|-------------|---------|--------|----------------------------------------------|
| `Current`   | String  | Read   | Name of the current clip, or an empty string |
| `IsPlaying`| Boolean | Read   | True when a clip is currently playing        |

### Methods

| Method                                      | Signature                                      | Description                                        |
|---------------------------------------------|------------------------------------------------|----------------------------------------------------|
| `AddClip(name, start, count, delayMs, loop)` | `Boolean(String, Integer, Integer, Integer, Integer)` | Add or replace a named clip                 |
| `Play(name)`                                | `Boolean(String)`                              | Start the named clip from its first frame          |
| `Stop()`                                    | `Void()`                                       | Stop playback                                     |
| `Update(sprite)`                            | `Void(Sprite)`                                 | Advance the current clip and update sprite frame   |
| `Destroy()`                                 | `Void()`                                       | Free the animator                                 |

`AddClip` matches names case-sensitively. Adding a duplicate name replaces the existing clip in-place instead of consuming another clip slot. Calling `Play` on the current clip restarts it from the first frame.

---

## Zanna.Graphics.SpriteSheet

Sprite sheet/atlas for named region extraction from a single texture. Defines named rectangular regions within an atlas image and extracts them as individual `Pixels` buffers.
Region names must be non-empty. Empty names are ignored so accidental default strings do not create unfindable or ambiguous regions.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics.SpriteSheet(atlas)` or `Zanna.Graphics.SpriteSheet.FromGrid(atlas, frameW, frameH)`

### Static Methods (Constructors)

| Method                                 | Signature                       | Description                                                             |
|----------------------------------------|---------------------------------|-------------------------------------------------------------------------|
| `New(atlas)`                           | `SpriteSheet(Pixels)`           | Create from atlas Pixels buffer                                         |
| `FromGrid(atlas, frameW, frameH)`      | `SpriteSheet(Pixels, Int, Int)` | Auto-slice atlas into uniform cells; regions named `"0"`, `"1"`, etc.  |

### Properties

| Property      | Type    | Access | Description                              |
|---------------|---------|--------|------------------------------------------|
| `RegionCount` | Integer | Read   | Number of defined named regions          |
| `Width`       | Integer | Read   | Width of the underlying atlas in pixels  |
| `Height`      | Integer | Read   | Height of the underlying atlas in pixels |

### Methods

| Method                              | Signature                          | Description                                            |
|-------------------------------------|------------------------------------|--------------------------------------------------------|
| `GetRegion(name)`                   | `Pixels(String)`                   | Extract region as a new Pixels buffer; NULL if missing |
| `HasRegion(name)`                   | `Boolean(String)`                  | Check if a region name exists                          |
| `RegionNames()`                     | `Seq()`                            | Return all region names as an owning Seq of string snapshots |
| `RemoveRegion(name)`                | `Boolean(String)`                  | Remove a named region; returns false if not found      |
| `SetRegion(name, x, y, w, h)`       | `Void(String, Int, Int, Int, Int)` | Define a named rectangular region within the atlas     |

### Notes

- `FromGrid()` automatically slices the atlas into equal cells, named `"0"`, `"1"`, etc. (left-to-right, top-to-bottom)
- `GetRegion()` returns a new Pixels object on each call — cache results for repeated use
- Region coordinates are in pixels, relative to the atlas top-left corner
- `SetRegion()` rejects regions with negative coordinates, non-positive width or height, or bounds outside the atlas; rejected updates leave any existing region unchanged
- `RegionNames()` returns a snapshot; the strings remain valid even if the sheet later changes or is released
- All regions share the underlying atlas Pixels object

### Zia Example

```zia
module SpriteSheetDemo;

bind Zanna.Graphics;
bind Zanna.Terminal;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var atlas = Pixels.New(128, 128);
    atlas.Fill(0xC86432FF);

    var sheet = SpriteSheet.New(atlas);
    Say("Regions: " + Fmt.Int(sheet.RegionCount));  // 0

    // Define named regions
    sheet.SetRegion("idle", 0, 0, 32, 32);
    sheet.SetRegion("walk1", 32, 0, 32, 32);
    sheet.SetRegion("walk2", 64, 0, 32, 32);

    SayBool(sheet.HasRegion("idle"));    // true
    SayBool(sheet.HasRegion("jump"));    // false
    Say("Regions: " + Fmt.Int(sheet.RegionCount));  // 3

    // Extract region as Pixels
    var idleFrame = sheet.GetRegion("idle");
    Say("Frame: " + Fmt.Int(idleFrame.Width) + "x" + Fmt.Int(idleFrame.Height));  // 32x32

    // List all region names
    var names = sheet.RegionNames();

    // Remove a region
    SayBool(sheet.RemoveRegion("walk2"));  // true
    Say("Regions: " + Fmt.Int(sheet.RegionCount));  // 2

    // Auto-slice from uniform grid
    var gridSheet = SpriteSheet.FromGrid(atlas, 32, 32);
    Say("Grid regions: " + Fmt.Int(gridSheet.RegionCount));  // 16
}
```

### Example

```basic
' Load atlas image
DIM atlas AS Zanna.Graphics.Pixels
atlas = Zanna.Graphics.Pixels.LoadBmp("sprites.bmp")

' Method 1: Manual region definition
DIM sheet AS Zanna.Graphics.SpriteSheet
sheet = Zanna.Graphics.SpriteSheet.New(atlas)

sheet.SetRegion("player_idle",  0,  0, 32, 48)
sheet.SetRegion("player_walk1", 32, 0, 32, 48)
sheet.SetRegion("player_walk2", 64, 0, 32, 48)
sheet.SetRegion("enemy",         0, 48, 32, 32)

PRINT "Region count: "; sheet.RegionCount   ' Output: 4

' Check existence and extract
IF sheet.HasRegion("player_idle") THEN
    DIM frame AS Zanna.Graphics.Pixels
    frame = sheet.GetRegion("player_idle")
    PRINT "Frame size: "; frame.Width; "x"; frame.Height
END IF

' Method 2: Uniform grid layout
DIM gridSheet AS Zanna.Graphics.SpriteSheet
gridSheet = Zanna.Graphics.SpriteSheet.FromGrid(atlas, 32, 32)
' Regions auto-named "0", "1", "2", ... (left-to-right, top-to-bottom)
DIM frame0 AS Zanna.Graphics.Pixels
frame0 = gridSheet.GetRegion("0")

' List all region names
DIM names AS OBJECT
names = sheet.RegionNames()

' Remove a region
IF sheet.RemoveRegion("enemy") THEN
    PRINT "Region removed"
END IF
```

---

## Zanna.Graphics2D.Tilemap

Efficient tile-based 2D map rendering for platformers, RPGs, and strategy games.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics2D.Tilemap(width, height, tileWidth, tileHeight)`

### Properties

| Property     | Type    | Access | Description                    |
|--------------|---------|--------|--------------------------------|
| `Width`      | Integer | Read   | Map width in tiles             |
| `Height`     | Integer | Read   | Map height in tiles            |
| `TileWidth`  | Integer | Read   | Width of each tile in pixels   |
| `TileHeight` | Integer | Read   | Height of each tile in pixels  |
| `TileCount`  | Integer | Read   | Number of tiles in tileset     |

### Methods

| Method                                         | Signature                                    | Description                                           |
|------------------------------------------------|----------------------------------------------|-------------------------------------------------------|
| `Clear()`                                      | `Void()`                                     | Clear map (set all to 0)                              |
| `CollideBody(body)`                            | `Integer(Object)`                            | Resolve a Physics2D.Body against solid/one-way tiles (returns 1 on collision). One-way tiles compare against the body's previous Physics2D step, so teleports into a platform do not count as a landing. |
| `Draw(canvas, offsetX, offsetY)`               | `Void(Canvas, Integer, Integer)`             | Draw every visible layer in layer order using the scroll offset |
| `DrawScaled(canvas, offsetX, offsetY, scalePercent)` | `Void(Canvas, Integer, Integer, Integer)` | Draw visible tiles at an integer percent zoom; `100` is 1x |
| `DrawRegion(canvas, ox, oy, vx, vy, vw, vh)`   | `Void(Canvas, Integer...)`                   | Draw a tile-coordinate sub-region across every visible layer |
| `Fill(index)`                                  | `Void(Integer)`                              | Fill entire map with a tile                           |
| `FillRect(x, y, w, h, index)`                  | `Void(Integer...)`                           | Fill rectangular region                               |
| `GetCollision(tileId)`                         | `Integer(Integer)`                           | Get collision type for a tile ID; tile `0` always reports `0` |
| `GetTile(x, y)`                                | `Integer(Integer, Integer)`                  | Get tile index at position                            |
| `HitTestScaled(pixelX, pixelY, offsetX, offsetY, scalePercent)` | `Map(Integer...)` | Convert a widget-local point through pan/zoom into tile coordinates |
| `IsSolidAt(pixelX, pixelY)`                    | `Integer(Integer, Integer)`                  | Check if a pixel position is on a solid tile          |
| `LoadCsv(path, tileWidth, tileHeight)`          | `Tilemap(String, Integer, Integer)`          | Load a CSV tile layer into a new tilemap              |
| `Load(path)`                                   | `Tilemap(String)`                            | Load a saved JSON tilemap                             |
| `CountDrawnVisibleScaled(canvas, offsetX, offsetY, scalePercent)` | `Integer(Canvas, Integer, Integer, Integer)` | Count drawable visible tiles at zoom without drawing |
| `ResolveAnimTile(tileId)`                      | `Integer(Integer)`                           | Return the current visible tile for an animated base tile |
| `SetCollision(tileId, collType)`               | `Void(Integer, Integer)`                     | Set collision type for a tile ID (0=none, 1=solid, 2=one_way_up); tile `0` remains empty |
| `SetTileAnim(baseTile, startTile, frameCount)`  | `Void(Integer, Integer, Integer)`            | Register a sequential tile animation                  |
| `SetTileAnimFrame(baseTile, frame, tileId)`     | `Void(Integer, Integer, Integer)`            | Override one frame in a tile animation                |
| `SetTile(x, y, index)`                         | `Void(Integer, Integer, Integer)`            | Set tile at position (0 = empty)                      |
| `SetTileset(pixels)`                           | `Void(Pixels)`                               | Set the tileset image (tiles arranged in grid); invalid non-Pixels handles are ignored |
| `ToPixelX(tileX)`                              | `Integer(Integer)`                           | Convert tile X to pixel X                             |
| `ToPixelY(tileY)`                              | `Integer(Integer)`                           | Convert tile Y to pixel Y                             |
| `ToTileX(pixelX)`                              | `Integer(Integer)`                           | Convert pixel X to tile X                             |
| `ToTileY(pixelY)`                              | `Integer(Integer)`                           | Convert pixel Y to tile Y                             |
| `UpdateAnims(deltaMs)`                         | `Void(Integer)`                              | Advance tile animation timers                         |

Advanced runtime support also includes multi-layer tilemaps, per-layer tilesets,
JSON save/load, auto-tiling rules, per-tile properties, and tile animation state.
Layer names are limited to the fixed runtime name slot (31 bytes); overlong names
are rejected without adding a layer. Tileset assignment requires real `Pixels`
handles for both map-level and per-layer tilesets. `Save` / `Load` preserve layer
visibility, collision-layer selection, collision types, tile properties,
auto-tile rules, animation progress and per-frame durations, plus imported
projection/source-frame/signed-origin/layer-parallax state. Saves report failure
on write or close errors. JSON loading ignores layers beyond the runtime layer
cap, normalizes negative saved animation frames, rejects overlong CSV rows
instead of truncating them, clamps CSV tile values that exceed the int64 range,
and applies duplicate animation records to the matching base tile instead of the
last parsed animation.

Animated tiles keep collision from the base tile ID stored in the map. Changing
the visual animation frame does not change solidity or one-way behavior unless
you also change the base tile's collision type. Tile ID `0` is reserved for empty
space and stays non-solid even if `SetCollision(0, ...)` is called;
`SetTileAnim(0, ...)` and negative base tile IDs are ignored so empty space cannot
animate into a visible tile. Registering an animation for an existing base tile
replaces the old animation. Autotile variants omitted from a partial rule resolve
to the rule's base tile. Invalid collision types are ignored. Negative animation
deltas are ignored; very large deltas advance in one modulo step instead of
looping once per elapsed frame. Default sequential animation frame IDs saturate
at the int64 limit instead of wrapping.

`FillRect`, tile drawing, scaled tile drawing, file offsets, tile-to-pixel
conversion, and scaled hit-test pan/zoom math clip or saturate extreme
coordinates rather than wrapping. Imported Tilemaps retain logical cell size
separately from source-frame size and honor Tiled render order and projected
draw order. Tilemap drawing derives a conservative visible projected region
from the canvas size, scroll offset, and artwork extent. Scaled draw and
hit-test helpers use `scalePercent`; values less than or equal to zero are
ignored or return an out-of-bounds hit result.
Tile artwork is alpha-composited for both native and scaled draws, so transparent
pixels in an upper tile or layer reveal the already-rendered content below.

### Zia Example

```zia
module TilemapDemo;

bind Zanna.Terminal;
bind Zanna.Graphics2D.Tilemap as TM;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var map = TM.New(10, 10, 16, 16);
    Say("Map: " + Fmt.Int(map.get_Width()) + "x" + Fmt.Int(map.get_Height()));
    Say("Tile size: " + Fmt.Int(map.get_TileWidth()) + "x" + Fmt.Int(map.get_TileHeight()));

    // Fill with grass (tile 1)
    map.Fill(1);
    Say("Tile(0,0): " + Fmt.Int(map.GetTile(0, 0)));

    // Place individual tiles
    map.SetTile(5, 5, 3);
    Say("Tile(5,5): " + Fmt.Int(map.GetTile(5, 5)));

    // Fill a rectangle region
    map.FillRect(2, 2, 3, 3, 7);

    // Coordinate conversion
    Say("ToTileX(32): " + Fmt.Int(map.ToTileX(32)));
    Say("ToPixelX(2): " + Fmt.Int(map.ToPixelX(2)));

    map.Clear();
}
```

### Example

```basic
' Create a 100x100 tilemap with 16x16 pixel tiles
DIM map AS Zanna.Graphics2D.Tilemap
map = NEW Zanna.Graphics2D.Tilemap(100, 100, 16, 16)

' Load tileset (tiles arranged in a grid)
DIM tileset AS Zanna.Graphics.Pixels
tileset = Zanna.Graphics.Pixels.LoadBmp("tileset.bmp")
map.SetTileset(tileset)

' Fill with grass (tile index 1)
map.Fill(1)

' Add some walls (tile index 2)
map.FillRect(10, 10, 5, 5, 2)

' Set individual tiles
map.SetTile(20, 20, 3)  ' Place a tree

' Scroll position
DIM scrollX AS INTEGER = 0
DIM scrollY AS INTEGER = 0

' Game loop
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    canvas.Clear(0)

    ' Scroll with arrow keys
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Right) THEN scrollX = scrollX + 4
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Left) THEN scrollX = scrollX - 4
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Up) THEN scrollY = scrollY - 4
    IF Zanna.Input.Keyboard.IsDown(Zanna.Input.Key.Down) THEN scrollY = scrollY + 4

    ' Draw tilemap
    map.Draw(canvas, -scrollX, -scrollY)

    ' Convert mouse position to tile coordinates
    DIM mx AS INTEGER = Zanna.Input.Mouse.X + scrollX
    DIM my AS INTEGER = Zanna.Input.Mouse.Y + scrollY
    DIM tileX AS INTEGER = map.ToTileX(mx)
    DIM tileY AS INTEGER = map.ToTileY(my)

    canvas.Text(10, 10, "Tile: " + STR$(tileX) + "," + STR$(tileY), 16777215)

    canvas.Flip()
LOOP
```

### Collision Types

`SetCollision` assigns physics behaviour to a tile ID:

| Value | Constant     | Behaviour                                                                      |
|-------|--------------|--------------------------------------------------------------------------------|
| `0`   | none         | Passthrough — no collision                                                     |
| `1`   | solid        | Full AABB collision on all four sides                                          |
| `2`   | one\_way\_up | Body lands on top only. A body coming from below passes through. Fast downward crossings use the body's previous-step motion to decide whether the platform top was crossed this frame. |

### Tileset Layout

Tiles in the tileset image are arranged left-to-right, top-to-bottom:

```text
+---+---+---+---+
| 0 | 1 | 2 | 3 |
+---+---+---+---+
| 4 | 5 | 6 | 7 |
+---+---+---+---+
```

- **Index 0** is typically empty/transparent
- Indices start at 0, counted left-to-right, then top-to-bottom

---


## See Also

- [Canvas & Color](canvas.md)
- [Scene Graph](scene.md)
- [Graphics Overview](README.md)
- [Zanna Runtime Library](../README.md)
