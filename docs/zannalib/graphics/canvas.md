---
status: active
audience: public
last-verified: 2026-07-26
---

# Canvas & Color
> Canvas drawing surface and Color utilities

**Part of [Zanna Runtime Library](../README.md) › [Graphics](README.md)**

---

## Zanna.Graphics.Canvas

2D graphics canvas for visual applications and games.

**Type:** Instance (obj)
**Constructor:** Zia `Canvas.New(title, width, height)` · BASIC `NEW Zanna.Graphics.Canvas(title, width, height)`

### Properties

| Property      | Type    | Description                                        |
|---------------|---------|----------------------------------------------------|
| `DeltaTime`   | Integer | Milliseconds elapsed between last two `Flip()` calls, rounded to the nearest millisecond (first frame may be `0`) |
| `DeltaTimeSec` | Number | Seconds elapsed between last two `Flip()` calls, using the same clamp as `DeltaTime` |
| `Height`      | Integer | Canvas height in pixels                            |
| `ShouldClose` | Integer | Non-zero if the user requested to close the canvas |
| `Width`       | Integer | Canvas width in pixels                             |

### Methods

| Method                                | Signature                             | Description                                                |
|---------------------------------------|---------------------------------------|------------------------------------------------------------|
| `Arc(cx, cy, radius, startAngle, endAngle, color)` | `Void(Integer...)`         | Draws a filled clockwise arc (pie slice)                   |
| `ArcFrame(cx, cy, radius, startAngle, endAngle, color)` | `Void(Integer...)`    | Draws a clockwise arc outline                             |
| `BeginFrame()`                         | `Integer()`                           | Call Poll(), return 0 if ShouldClose, else 1. Simplifies game loop |
| `Bezier(x1, y1, cx, cy, x2, y2, color)` | `Void(Integer...)`                  | Draws a quadratic Bezier curve                             |
| `Blit(x, y, pixels)`                  | `Void(Integer, Integer, Pixels)`      | Blits a Pixels buffer to the canvas at (x, y); honors the active clip rect |
| `BlitAlpha(x, y, pixels)`             | `Void(Integer, Integer, Pixels)`      | Blits with alpha blending (respects alpha channel and clip rect) |
| `BlitRegion(dx, dy, pixels, sx, sy, w, h)` | `Void(Integer...)`               | Blits a region of a Pixels buffer to the canvas; honors the active clip rect |
| `Box(x, y, w, h, color)`              | `Void(Integer...)`                    | Draws a filled rectangle                                   |
| `BoxAlpha(x, y, w, h, color, alpha)`  | `Void(Integer...)`                    | Draws a source-over alpha-blended rectangle                |
| `Clear(color)`                        | `Void(Integer)`                       | Clears the canvas with a solid color                       |
| `ClearClipRect()`                     | `Void()`                              | Clears clipping rectangle; restores full canvas drawing    |
| `CopyRect(x, y, w, h)`                | `Pixels(Integer...)`                  | Copies canvas region to a Pixels buffer; returns null for invalid or excessively large rectangles |
| `Disc(cx, cy, r, color)`              | `Void(Integer...)`                    | Draws a filled circle                                      |
| `DiscAlpha(cx, cy, r, color, alpha)`  | `Void(Integer...)`                    | Draws a source-over alpha-blended filled circle            |
| `Ellipse(cx, cy, rx, ry, color)`      | `Void(Integer...)`                    | Draws a filled ellipse                                     |
| `EllipseAlpha(cx, cy, rx, ry, color, alpha)` | `Void(Integer...)`              | Draws a source-over alpha-blended filled ellipse           |
| `EllipseFrame(cx, cy, rx, ry, color)` | `Void(Integer...)`                    | Draws an ellipse outline                                   |
| `Flip()`                              | `Void()`                              | Presents the back buffer and displays drawn content        |
| `FloodFill(x, y, color)`              | `Void(Integer, Integer, Integer)`     | Flood fills connected RGBA-matching area starting at (x, y), constrained by the active clip rect |
| `Focus()`                             | `Void()`                              | Brings the window to the front and gives it focus          |
| `Frame(x, y, w, h, color)`            | `Void(Integer...)`                    | Draws a rectangle outline                                  |
| `Fullscreen()`                        | `Void()`                              | Enters fullscreen mode                                     |
| `GetFps()`                            | `Integer()`                           | Returns the current target FPS (-1 = unlimited)            |
| `GetMonitorHeight()`                  | `Integer()`                           | Returns the height of the monitor containing this window   |
| `GetMonitorWidth()`                   | `Integer()`                           | Returns the width of the monitor containing this window    |
| `GetPixel(x, y)`                      | `Integer(Integer, Integer)`           | Gets pixel color at (x, y)                                 |
| `GetScale()`                          | `Double()`                            | Returns the HiDPI display scale factor (1.0 normal, 2.0 on Retina) |
| `GetWindowX()`                        | `Integer()`                           | Returns the window X position in screen coordinates        |
| `GetWindowY()`                        | `Integer()`                           | Returns the window Y position in screen coordinates        |
| `GradientH(x, y, w, h, c1, c2)`      | `Void(Integer...)`                    | Draws a horizontal gradient (left c1 to right c2), honoring the active clip rect |
| `GradientV(x, y, w, h, c1, c2)`      | `Void(Integer...)`                    | Draws a vertical gradient (top c1 to bottom c2), honoring the active clip rect |
| `IsFocused()`                         | `Boolean()`                           | Returns true if the window has keyboard focus              |
| `IsMaximized()`                       | `Boolean()`                           | Returns true if the window is maximized                    |
| `IsMinimized()`                       | `Boolean()`                           | Returns true if the window is minimized (iconified)        |
| `KeyHeld(keycode)`                    | `Integer(Integer)`                    | Returns non-zero if the specified key is held down         |
| `Line(x1, y1, x2, y2, color)`         | `Void(Integer...)`                    | Draws a line between two points                            |
| `Maximize()`                          | `Void()`                              | Maximizes the window                                       |
| `Minimize()`                          | `Void()`                              | Minimizes (iconifies) the window                           |
| `Plot(x, y, color)`                   | `Void(Integer, Integer, Integer)`     | Sets a single pixel                                        |
| `Poll()`                              | `Integer()`                           | Polls for input events; returns event type (0 = none); destroys the window and sets `ShouldClose` when a close event arrives or the backend event pump fails |
| `PreventClose(prevent)`               | `Void(Integer)`                       | Blocks (1) or allows (0) the window close button           |
| `PolygonPath(path, color)`            | `Void(Path2D, Integer)`               | Draws a filled polygon from a Path2D                       |
| `PolygonFramePath(path, color)`       | `Void(Path2D, Integer)`               | Draws a polygon outline from a Path2D                      |
| `PolylinePath(path, color)`           | `Void(Path2D, Integer)`               | Draws connected line segments from a Path2D                |
| `Ring(cx, cy, r, color)`              | `Void(Integer...)`                    | Draws a circle outline                                     |
| `Restore()`                           | `Void()`                              | Restores the window after minimize or maximize             |
| `RoundBox(x, y, w, h, radius, color)` | `Void(Integer...)`                    | Draws a filled rectangle with rounded corners              |
| `RoundFrame(x, y, w, h, radius, color)` | `Void(Integer...)`                  | Draws a rectangle outline with rounded corners             |
| `SaveBmp(path)`                       | `Integer(String)`                     | Saves canvas to BMP file (returns 1 on success)            |
| `SavePng(path)`                       | `Integer(String)`                     | Saves canvas to PNG file (returns 1 on success)            |
| `Screenshot()`                        | `Pixels()`                            | Captures entire canvas contents to a Pixels buffer         |
| `SetClipRect(x, y, w, h)`             | `Void(Integer...)`                    | Sets clipping rectangle; all drawing is constrained to it  |
| `SetMaxDeltaTime(max)`                 | `Void(Integer)`                       | Set maximum DeltaTime clamp in ms. After startup, `DeltaTime` auto-clamps to `[1, max]`; `DeltaTimeSec` reports the clamped value divided by 1000 |
| `SetFps(fps)`                         | `Void(Integer)`                       | Set the target frame rate (-1 = unlimited)                 |
| `SetPosition(x, y)`                   | `Void(Integer, Integer)`              | Move the window to screen coordinates                      |
| `SetTitle(title)`                     | `Void(String)`                        | Changes the window title at runtime                        |
| `Text(x, y, text, color)`             | `Void(Integer, Integer, String, Integer)` | Draws text at (x, y) with the specified color          |
| `TextBg(x, y, text, fg, bg)`          | `Void(Integer, Integer, String, Integer, Integer)` | Draws text with foreground and background colors |
| `TextCentered(y, text, color)`         | `Void(Integer, String, Integer)`      | Draw text horizontally centered on the canvas              |
| `TextCenteredScaled(y, text, scale, color)` | `Void(Integer, String, Integer, Integer)` | Draw scaled text horizontally centered               |
| `TextHeight()`                        | `Integer()`                           | Returns the height of rendered text in pixels (always 8)   |
| `TextRight(margin, y, text, color)`    | `Void(Integer, Integer, String, Integer)` | Draw text right-aligned with margin from right edge    |
| `TextWidth(text)`                     | `Integer(String)`                     | Returns the width of rendered text in pixels (8 per decoded codepoint) |
| `ThickLine(x1, y1, x2, y2, thickness, color)` | `Void(Integer...)`            | Draws a line with specified thickness (parallelogram body + rounded endcap circles) |
| `Triangle(x1, y1, x2, y2, x3, y3, color)` | `Void(Integer...)`                 | Draws a filled triangle; collinear vertices draw the longest edge |
| `TriangleFrame(x1, y1, x2, y2, x3, y3, color)` | `Void(Integer...)`            | Draws a triangle outline                                   |
| `Windowed()`                          | `Void()`                              | Exits fullscreen mode (returns to windowed)                |

### Color Format

Colors are specified as 32-bit integers in `0x00RRGGBB` format or with `Zanna.Graphics.Color.Rgb/Rgba`:

- Red: `0x00FF0000`
- Green: `0x0000FF00`
- Blue: `0x000000FF`
- White: `0x00FFFFFF`
- Black: `0x00000000`

Use `Zanna.Graphics.Color.Rgb()` or `Zanna.Graphics.Color.Rgba()` to create colors from components. RGB-only drawing calls use the RGB channels from either form. Alpha-aware calls such as `BoxAlpha`, `DiscAlpha`, `EllipseAlpha`, and `BlitAlpha` use straight-alpha source-over compositing; the explicit alpha parameter controls shape opacity. `FloodFill` compares and writes alpha as part of the filled region when given a tagged RGBA color. `Color.Rgba()` values carry an internal explicit-alpha tag; use `Color.Get*` or `Color.ToHex()` instead of raw integer equality for RGBA colors. `Color.GetAlpha(Color.Rgb(...))` returns `0` because plain RGB stores no alpha byte; drawing helpers still treat plain RGB colors as opaque.

### Zia Example

```zia
module GameDemo;

bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;

func start() {
    var c = Canvas.New("My Game", 800, 600);

    // Main loop
    while !c.get_ShouldClose() {
        c.Poll();
        c.Clear(Color.Rgb(0, 0, 0));

        // Draw shapes
        c.Box(100, 100, 200, 150, Color.Rgb(255, 0, 0));
        c.Disc(400, 300, 50, Color.Rgb(0, 0, 255));
        c.Line(0, 0, 800, 600, Color.Rgb(0, 255, 0));
        c.Frame(50, 50, 100, 100, Color.Rgb(255, 255, 255));
        c.Ring(600, 200, 40, Color.Rgb(255, 255, 0));
        c.Text(10, 10, "Hello Zia!", Color.Rgb(255, 255, 255));

        c.Flip();
    }
}
```

### Example

```basic
' Create a canvas
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("My Game", 800, 600)

' Main loop
DO WHILE NOT canvas.ShouldClose
    ' Poll events
    canvas.Poll()

    ' Clear to black
    canvas.Clear(0)

    ' Draw a red filled rectangle
    canvas.Box(100, 100, 200, 150, 16711680)

    ' Draw a blue filled circle
    canvas.Disc(400, 300, 50, 255)

    ' Draw a green line
    canvas.Line(0, 0, 800, 600, 65280)

    ' Draw a white rectangle outline
    canvas.Frame(50, 50, 100, 100, 16777215)

    ' Draw a yellow circle outline
    canvas.Ring(600, 200, 40, 16776960)

    ' Draw text with transparent background
    canvas.Text(10, 10, "Hello World!", 16777215)

    ' Draw text with solid background (useful for HUDs)
    canvas.TextBg(10, 30, "Score: 1000", 16776960, 128)

    ' Present
    canvas.Flip()
LOOP
```

### Text Rendering

The canvas includes a built-in 8x8 pixel bitmap font for rendering text:

- **Font size:** 8x8 pixels per character
- **Character set:** ASCII 32-126 (printable characters)
- UTF-8 text is measured by decoded codepoint count; unsupported glyphs render as `?`
- Characters are drawn pixel-by-pixel to the canvas
- Use `Text` for text with a transparent background
- Use `TextBg` for text with a solid background (useful for HUDs/overlays)
- Use `TextWidth(text)` to measure text width in pixels (8 per decoded codepoint)
- Use `TextHeight()` to get font height in pixels (always 8)
- Use `TextCentered(y, text, color)` to draw text horizontally centered on the canvas
- Use `TextRight(margin, y, text, color)` to draw text right-aligned with a margin
- Use `TextCenteredScaled(y, text, scale, color)` to draw scaled text centered

**Note:** `TextWidth` and `TextHeight` are static methods -- they do not require a canvas instance.

### Blitting Pixels Buffers

Use `Blit`, `BlitRegion`, and `BlitAlpha` to copy Pixels buffers to the canvas. These APIs use
logical canvas coordinates, respect `SetClipRect`, and scale correctly on fractional HiDPI displays.
`BlitAlpha` uses straight-alpha source-over compositing and preserves the resulting alpha channel:

```basic
' Load an image
DIM sprite AS Zanna.Graphics.Pixels
sprite = Zanna.Graphics.Pixels.LoadBmp("player.bmp")

' Draw the sprite (opaque blit)
canvas.Blit(playerX, playerY, sprite)

' Draw with alpha blending (for transparent sprites)
canvas.BlitAlpha(playerX, playerY, sprite)

' Draw only a portion of the sprite (for sprite sheets)
canvas.BlitRegion(screenX, screenY, spriteSheet, frameX, frameY, 32, 32)
```

### Extended Drawing Primitives

The canvas supports additional drawing primitives for more complex shapes:

```basic
' Draw a thick line (5 pixels wide, rounded caps)
canvas.ThickLine(10, 10, 200, 150, 5, 16711680)

' Draw rounded rectangles
canvas.RoundBox(50, 50, 150, 80, 15, 65280)   ' Filled with 15px radius corners
canvas.RoundFrame(50, 150, 150, 80, 15, 255) ' Outline only

' Flood fill an area (like paint bucket tool)
canvas.FloodFill(100, 100, 16776960)

' Flood fill can also preserve alpha from Color.Rgba
canvas.FloodFill(120, 100, Zanna.Graphics.Color.Rgba(255, 255, 0, 128))

' Draw triangles
canvas.Triangle(100, 50, 50, 150, 150, 150, 16711935)      ' Filled triangle
canvas.TriangleFrame(200, 50, 150, 150, 250, 150, 65535) ' Triangle outline

' Draw ellipses (horizontal and vertical radii)
canvas.Ellipse(400, 300, 80, 50, 8421504)      ' Filled ellipse
canvas.EllipseFrame(400, 400, 80, 50, 16777215) ' Ellipse outline
```

### Advanced Curves & Shapes

The canvas supports arcs, Bezier curves, and general polygons:

```basic
' Draw arcs (pie slices) - angles in degrees, 0 = right, 90 = down
canvas.Arc(200, 200, 50, 0, 90, 16711680)       ' Filled quarter-circle (bottom-right)
canvas.ArcFrame(300, 200, 50, 45, 135, 65280) ' Arc outline

' Draw a quadratic Bezier curve (start, control point, end)
canvas.Bezier(10, 100, 100, 10, 190, 100, 255)

' Polyline and polygon path calls take a Path2D handle.
DIM points AS Zanna.Graphics.Path2D
points = Zanna.Graphics.Path2D.New(4)
points.MoveTo(50, 10)
points.LineTo(10, 90)
points.LineTo(90, 90)

canvas.PolygonPath(points, 16711935)      ' Filled polygon
canvas.PolygonFramePath(points, 65535)    ' Polygon outline
```

Polyline and polygon path calls ignore invalid handles.

### Canvas Utilities

Read pixels, copy regions, take screenshots, and draw gradients:

```basic
' Get a pixel color from the canvas
DIM color AS INTEGER
color = canvas.GetPixel(100, 100)

' Copy a rectangular region from canvas to a Pixels buffer
DIM region AS Zanna.Graphics.Pixels
region = canvas.CopyRect(0, 0, 200, 200)

' Capture the entire canvas to a Pixels buffer
DIM screenshot AS Zanna.Graphics.Pixels
screenshot = canvas.Screenshot()

' Save the entire canvas to a BMP or PNG file
DIM success AS INTEGER
success = canvas.SaveBmp("screenshot.bmp")  ' BMP (no compression, universally supported)
success = canvas.SavePng("screenshot.png")  ' PNG (lossless, smaller than BMP)
IF success = 1 THEN
    PRINT "Screenshot saved!"
END IF

' Draw gradient backgrounds
canvas.GradientH(0, 0, 800, 600, 16711680, 255)  ' Red to blue (horizontal)
canvas.GradientV(0, 0, 800, 600, 0, 16777215)  ' Black to white (vertical)
```

`CopyRect` and `Screenshot` return `NULL` when the canvas is closed or unavailable. `SaveBmp` and `SavePng` snapshot the canvas into a temporary `Pixels` buffer, write the file, release the temporary buffer, and return `0` on invalid canvas handles or write failure.
`GradientH` and `GradientV` accept `Color.Rgba()` endpoint colors and preserve explicit alpha in the framebuffer.

### Canvas Clipping

Restrict drawing to a rectangular region. All drawing operations will be clipped to the
specified bounds until `ClearClipRect()` is called. This includes direct framebuffer-backed paths
such as `Blit`, `BlitRegion`, `BlitAlpha`, `FloodFill`, `GradientH`, and `GradientV`.
Clipped gradients keep their original color ramp; they do not restart from the clipped edge.
Line, rectangle, circle, ellipse, alpha-shape, pixel-read, and blit paths clip before narrowing to the backend coordinate type, so very large or offscreen coordinates do not wrap into visible pixels.

```basic
' Set a clipping region (x=100, y=100, width=200, height=150)
canvas.SetClipRect(100, 100, 200, 150)

' This circle will only appear within the clip region
canvas.Disc(150, 150, 100, 16711680)

' Drawing outside the clip region is ignored
canvas.Box(0, 0, 50, 50, 65280)  ' Not visible (outside clip)

' Restore full canvas drawing
canvas.ClearClipRect()

' Now drawing works across the entire canvas again
canvas.Box(0, 0, 50, 50, 65280)  ' Visible
```

**Use Cases for Clipping:**
- **UI panels:** Clip content to panel boundaries
- **Scrollable regions:** Clip to viewport during scrolling
- **Minimap rendering:** Clip map to minimap area
- **Text overflow:** Prevent text from drawing outside containers

### Window Controls

Change the window title, toggle fullscreen, manage window state, and query display information at runtime:

```basic
' Create a canvas
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("My Game", 800, 600)

' Update window title dynamically (e.g., show FPS or game state)
canvas.SetTitle("My Game - Level 1")

' HiDPI / Retina: all Canvas APIs use logical coordinates; the runtime applies the window scale automatically
DIM scale AS DOUBLE = canvas.GetScale()   ' 2.0 on Retina, 1.0 otherwise

' Window state management
canvas.Minimize()            ' Minimize (iconify) the window
canvas.Maximize()            ' Maximize the window
canvas.Restore()             ' Restore from minimize or maximize
IF canvas.IsMinimized() = 1 THEN PRINT "minimized"
IF canvas.IsMaximized() = 1 THEN PRINT "maximized"

' Focus management
canvas.Focus()               ' Bring window to front
IF canvas.IsFocused() = 1 THEN PRINT "has focus"

' Frame rate control
canvas.SetFps(60)            ' Limit to 60 fps
PRINT "FPS: "; canvas.GetFps()

' Prevent accidental close (e.g., while saving)
canvas.PreventClose(1)       ' Block the close button
' ... save work ...
canvas.PreventClose(0)       ' Re-enable close button

' Toggle fullscreen with F11 key
DIM isFullscreen AS INTEGER = 0
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()

    IF Zanna.Input.Keyboard.WasPressed(Zanna.Input.Key.F11) THEN
        IF isFullscreen = 1 THEN
            canvas.Windowed()
            isFullscreen = 0
        ELSE
            canvas.Fullscreen()
            isFullscreen = 1
        END IF
    END IF

    canvas.Flip()
LOOP
```

**Platform Behavior:**
- **macOS:** Uses native Cocoa fullscreen (menu bar hidden, dock accessible via mouse)
- **Linux:** Uses X11 EWMH protocol (_NET_WM_STATE_FULLSCREEN)
- **Windows:** Uses Win32 API (removes decorations, covers taskbar)

**Notes:**
- Fullscreen mode uses the display's native resolution
- Window size (Width/Height) remains unchanged; content is scaled
- Use `Fullscreen()` to enter and `Windowed()` to exit fullscreen mode
- `GetScale()` returns 2.0 on HiDPI (Retina) displays — multiply pixel dimensions by this factor for sharp rendering
- If the window moves between displays with different DPI scales, logical drawing, clipping, size queries, and input coordinates automatically follow the new scale
- `SetFps(-1)` disables frame rate limiting (default); `GetFps()` returns the configured target
- `PreventClose(1)` blocks the OS close button; the `ShouldClose` property will not become true until you call `PreventClose(0)`

### Frame Management

`BeginFrame()` and `SetMaxDeltaTime()` simplify the standard game loop pattern. Instead of
manually calling `Poll()` and checking `ShouldClose`, use `BeginFrame()` which combines
both steps into a single call. `SetMaxDeltaTime()` clamps the `DeltaTime`
and `DeltaTimeSec` properties to prevent
physics explosions after lag spikes or window drags.

```zia
module GameLoop;

bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Color as Color;

func start() {
    var c = Canvas.New("Game", 800, 600);
    c.SetFps(60);
    c.SetMaxDeltaTime(50);

    while c.BeginFrame() != 0 {
        var dt = c.DeltaTimeSec;  // First frame may be 0.0; with SetMaxDeltaTime(50), later positive frames clamp to <= 0.05

        // Game logic using dt for frame-independent movement
        c.Clear(Color.Rgb(0, 0, 0));
        c.Text(10, 10, "Running...", Color.Rgb(255, 255, 255));
        c.Flip();
    }
}
```

**Notes:**
- `BeginFrame()` calls `Poll()` internally, then returns 0 if `ShouldClose` is set, otherwise 1
- If the platform event pump fails, `Poll()` marks `ShouldClose`, tears down the backend window, updates input actions, and returns `0` without querying stale mouse or event state.
- Without `SetMaxDeltaTime()`, `DeltaTime` is rounded to the nearest millisecond, so very short uncapped frames may report `0` or `1`
- `SetMaxDeltaTime(max)` sets the upper clamp for `DeltaTime` in milliseconds; after the first positive frame, rounded values clamp to `[1, max]`
- `SetMaxDeltaTime()` remains available as a compatibility alias for
  `SetMaxDeltaTime()`.
- Prefer `DeltaTimeSec` for velocity and animation math expressed in units per second
- A typical max of 50 ms (20 FPS equivalent) prevents large time steps that can break physics or animation

---

## Zanna.Graphics.Color

Color utility functions for graphics operations.

**Type:** Static utility class

### Methods

| Method                   | Signature                                     | Description                                                                     |
|--------------------------|-----------------------------------------------|---------------------------------------------------------------------------------|
| `Brighten(color, amount)` | `Integer(Integer, Integer)`                  | Brightens a color by the given amount (0-100)                                   |
| `Complement(color)`      | `Integer(Integer)`                            | Returns the complementary color (opposite on color wheel)                       |
| `Darken(color, amount)`  | `Integer(Integer, Integer)`                   | Darkens a color by the given amount (0-100)                                     |
| `Desaturate(color, amount)` | `Integer(Integer, Integer)`               | Decreases saturation of a color (0-100)                                         |
| `FromHex(hex)`           | `Integer(String)`                             | Parses `#RRGGBB` or `#RRGGBBAA`; invalid input returns `0`                      |
| `FromHsl(h, s, l)`       | `Integer(Integer, Integer, Integer)`          | Creates a color from hue, saturation (0-100), lightness (0-100); hue wraps modulo 360 |
| `GetAlpha(color)`            | `Integer(Integer)`                            | Extracts the stored alpha byte (plain `Color.Rgb` returns 0)                    |
| `GetBlue(color)`            | `Integer(Integer)`                            | Extracts blue component (0-255) from a packed color                             |
| `GetGreen(color)`            | `Integer(Integer)`                            | Extracts green component (0-255) from a packed color                            |
| `GetHue(color)`            | `Integer(Integer)`                            | Extracts hue (0-360) from a packed color                                        |
| `GetLightness(color)`            | `Integer(Integer)`                            | Extracts lightness (0-100) from a packed color                                  |
| `GetRed(color)`            | `Integer(Integer)`                            | Extracts red component (0-255) from a packed color                              |
| `GetSaturation(color)`            | `Integer(Integer)`                            | Extracts saturation (0-100) from a packed color                                 |
| `Grayscale(color)`       | `Integer(Integer)`                            | Converts a color to grayscale                                                   |
| `Invert(color)`          | `Integer(Integer)`                            | Inverts a color (255 minus each channel)                                        |
| `Lerp(c1, c2, t)`        | `Integer(Integer, Integer, Integer)`          | Linearly interpolates between two colors (t: 0-100, where 0=c1, 100=c2)        |
| `Rgb(r, g, b)`           | `Integer(Integer, Integer, Integer)`          | Creates a color value from red, green, blue components (0-255 each)             |
| `Rgba(r, g, b, a)`       | `Integer(Integer, Integer, Integer, Integer)` | Creates a color with alpha from red, green, blue, alpha components (0-255 each) |
| `Saturate(color, amount)` | `Integer(Integer, Integer)`                  | Increases saturation of a color (0-100)                                         |
| `ToHex(color)`           | `String(Integer)`                             | Converts a color to hex string and preserves explicit alpha, including `#RRGGBB00` |

`Color.ToHex(Color.Rgba(r, g, b, a))` round-trips through `Color.FromHex()` as `#RRGGBBAA`, including `#RRGGBB00`.
Color transforms such as `Brighten`, `Darken`, `Invert`, `Grayscale`, `Saturate`, `Desaturate`,
`Complement`, and `Lerp` preserve explicit `Color.Rgba` alpha tags. RGB-only inputs are treated as
fully opaque when interpolated with an alpha-tagged color.

### Zia Example

```zia
module ColorDemo;

bind Zanna.Terminal;
bind Zanna.Graphics.Color as Color;
bind Zanna.Text.Fmt as Fmt;

func start() {
    var red = Color.Rgb(255, 0, 0);
    var green = Color.Rgb(0, 255, 0);
    var blue = Color.Rgb(0, 0, 255);
    var semi = Color.Rgba(255, 0, 0, 128);

    Say("Red: " + Fmt.Int(red));
    Say("Green: " + Fmt.Int(green));
    Say("Blue: " + Fmt.Int(blue));
    Say("Semi-transparent: " + Fmt.Int(semi));

    // Extract components
    Say("R: " + Fmt.Int(Color.GetRed(red)));
    Say("H: " + Fmt.Int(Color.GetHue(red)));

    // Color manipulation
    var bright = Color.Brighten(blue, 50);
    var lerped = Color.Lerp(red, blue, 50);
    var gray = Color.Grayscale(green);
    Say("Bright blue: " + Fmt.Int(bright));

    // HSL and hex
    var hsl = Color.FromHsl(120, 100, 50);
    var hex = Color.ToHex(red);
    Say("Hex: " + hex);
}
```

### Example

```basic
DIM red AS INTEGER
red = Zanna.Graphics.Color.Rgb(255, 0, 0)

DIM green AS INTEGER
green = Zanna.Graphics.Color.Rgb(0, 255, 0)

DIM blue AS INTEGER
blue = Zanna.Graphics.Color.Rgb(0, 0, 255)

DIM purple AS INTEGER
purple = Zanna.Graphics.Color.Rgb(128, 0, 128)

DIM semiTransparent AS INTEGER
semiTransparent = Zanna.Graphics.Color.Rgba(255, 0, 0, 128)  ' 50% transparent red

' Extract individual components
DIM r AS INTEGER = Zanna.Graphics.Color.GetRed(purple)   ' 128
DIM g AS INTEGER = Zanna.Graphics.Color.GetGreen(purple)   ' 0

' Create from HSL
DIM orange AS INTEGER
orange = Zanna.Graphics.Color.FromHsl(30, 100, 50)

' Parse hex strings
DIM fromHex AS INTEGER
fromHex = Zanna.Graphics.Color.FromHex("#FF8000")

' Color manipulation
DIM bright AS INTEGER = Zanna.Graphics.Color.Brighten(blue, 30)
DIM dark AS INTEGER = Zanna.Graphics.Color.Darken(red, 20)
DIM mixed AS INTEGER = Zanna.Graphics.Color.Lerp(red, blue, 50)  ' 50% blend
DIM gray AS INTEGER = Zanna.Graphics.Color.Grayscale(green)
DIM inv AS INTEGER = Zanna.Graphics.Color.Invert(red)
DIM comp AS INTEGER = Zanna.Graphics.Color.Complement(red)

' Use with graphics canvas
canvas.Box(10, 10, 100, 100, red)
canvas.Disc(200, 200, 50, purple)
```

---


## Camera Parallax Layers

The `Zanna.Graphics.Camera` class supports up to 8 parallax scrolling layers for creating depth effects in scrolling games.

### Parallax Properties

| Property         | Type    | Description                              |
|------------------|---------|------------------------------------------|
| `ParallaxCount`  | Integer | Number of active parallax layers (0-8)   |

### Parallax Methods

| Method                                | Signature                        | Description                                  |
|---------------------------------------|----------------------------------|----------------------------------------------|
| `AddParallax(pixels, scrollX, scrollY)` | `Integer(Pixels, Integer, Integer)` | Add a parallax layer; returns index (0-7) or -1 if full |
| `RemoveParallax(index)`               | `Void(Integer)`                  | Remove parallax layer by index               |
| `ClearParallax()`                     | `Void()`                         | Remove all parallax layers                   |
| `DrawParallax(canvas)`                | `Integer(Canvas)`                | Draw all parallax layers to canvas; returns count drawn. Camera zoom and rotation are applied too |

### Scroll Factors

The scroll factor controls how fast a layer scrolls relative to the camera:

| Factor | Effect                                      |
|--------|----------------------------------------------|
| 100    | Scrolls at camera speed (foreground)         |
| 50     | Scrolls at half camera speed (mid-distance)  |
| 25     | Scrolls at quarter speed (far background)    |
| 0      | Static (fixed backdrop)                      |

Each layer's Pixels buffer is tiled horizontally and vertically to fill the viewport.
Camera zoom and rotation are applied to those tiles during `DrawParallax`, so the
background stays visually aligned with the current view transform instead of only
tracking camera translation.
Pathological combinations that would require an extremely large number of tiles
in one call are skipped and do not count as drawn.

### Parallax Example

```zia
module ParallaxDemo;

bind Zanna.Graphics.Canvas as Canvas;
bind Zanna.Graphics.Camera as Camera;
bind Zanna.Graphics.Pixels as Pixels;

func start() {
    var c = Canvas.New("Parallax", 800, 600);
    var cam = Camera.New(800, 600);

    // Load background layers
    var sky = Pixels.LoadBmp("sky.bmp");
    var mountains = Pixels.LoadBmp("mountains.bmp");
    var trees = Pixels.LoadBmp("trees.bmp");

    // Add layers: furthest first, closest last
    cam.AddParallax(sky, 10, 0);        // Sky: barely moves
    cam.AddParallax(mountains, 40, 0);  // Mountains: slow scroll
    cam.AddParallax(trees, 80, 0);      // Trees: nearly camera speed

    while !c.get_ShouldClose() {
        c.Poll();
        c.Clear(0);

        // Move camera (e.g., player movement)
        cam.Move(2, 0);

        // Draw parallax layers (tiled, alpha-blended)
        cam.DrawParallax(c);

        // Draw game world on top...
        c.Flip();
    }
}
```

---

## See Also

- [Images & Sprites](pixels.md)
- [Scene Graph](scene.md)
- [Graphics Overview](README.md)
- [Zanna Runtime Library](../README.md)
