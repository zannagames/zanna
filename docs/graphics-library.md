---
status: active
audience: public
last-verified: 2026-09-01
---

# ZannaGFX Graphics Library

**Version:** 1.0.0 (`VGFX_VERSION_*` in `vgfx.h`)
**Location:** `src/lib/graphics/`

---

## Overview

ZannaGFX is a cross-platform software 2D graphics library integrated into the Zanna project. It provides window
management, pixel operations, drawing primitives, and input handling through a simple C API.

### Key Features

- **Pure software rendering** — No GPU dependencies
- **Cross-platform** — macOS (Cocoa), Linux (native Wayland with X11 fallback), Windows (Win32)
- **Simple API** — Immediate-mode drawing with direct framebuffer access
- **Event handling** — Keyboard, mouse, UTF-8 text input, and window events
- **Clipboard support** — System text clipboard helpers on desktop backends
- **FPS limiting** — Built-in frame rate control
- **Test infrastructure** — Mock backend for deterministic unit testing

### Design Principles

- **Zero external dependencies** — No SDL, GLFW, or other libraries
- **Native platform APIs** — Direct Cocoa/X11/Win32 integration
- **Pure C implementation** — C11 standard with C++ compatibility
- **Deterministic rendering** — Integer-only math for predictable results

---

## Platform Support

| Platform    | Backend                              | Notes                                                        |
|-------------|--------------------------------------|--------------------------------------------------------------|
| **macOS**   | Cocoa/AppKit                         | —                                                            |
| **Linux**   | Native Wayland, X11, or headless     | `AUTO` prefers Wayland and falls back to X11; see [Linux Platform](linux-platform.md) |
| **Windows** | Win32 GDI                            | —                                                            |
| **Testing** | Mock backend                         | Deterministic in-memory backend used by the unit tests       |

The Linux selector is controlled by `ZANNA_GRAPHICS_BACKEND` (`AUTO`, `WAYLAND`,
`X11`, `NATIVE`, `HEADLESS`) at configure time and by `WAYLAND_DISPLAY`/`DISPLAY`
at run time.

---

## Quick Start

### Building

ZannaGFX builds as part of the main Zanna build:

```bash
./scripts/build_zanna_linux.sh   # Linux
./scripts/build_zanna_mac.sh     # macOS
```

Or build the graphics library standalone:

```bash
cd src/lib/graphics
cmake -S . -B build
cmake --build build
```

### Basic Example

```c
#include "vgfx.h"

int main(void) {
    // Create window
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 800;
    params.height = 600;
    params.title = "My Window";
    params.fps = 60;

    vgfx_window_t win = vgfx_create_window(&params);
    if (!win) {
        return 1;
    }

    // Main loop
    while (!vgfx_close_requested(win)) {
        // Clear screen
        vgfx_cls(win, VGFX_BLACK);

        // Draw primitives
        vgfx_pset(win, 100, 100, VGFX_WHITE);
        vgfx_line(win, 50, 50, 200, 150, VGFX_RED);
        // vgfx_rect uses top-left x,y and width,height (not two corners)
        vgfx_rect(win, 300, 300, 100, 100, VGFX_GREEN);
        vgfx_circle(win, 500, 300, 50, VGFX_BLUE);

        // Handle events
        vgfx_event_t event;
        while (vgfx_poll_event(win, &event)) {
            if (event.type == VGFX_EVENT_KEY_DOWN) {
                if (event.data.key.key == VGFX_KEY_ESCAPE) {
                    break;
                }
            }
        }

        // Update display and limit FPS
        vgfx_update(win);
    }

    vgfx_destroy_window(win);
    return 0;
}
```

---

## API Quick Reference

### Window Management

```c
// Create and destroy
vgfx_window_t vgfx_create_window(const vgfx_window_params_t* params);
void vgfx_destroy_window(vgfx_window_t window);

// Window state
int32_t vgfx_close_requested(vgfx_window_t window);  // Non-zero if close was requested
int vgfx_update(vgfx_window_t window);  // Present + process events + FPS limit

// Query window properties
// Returns non-zero on success, 0 on failure.
int vgfx_get_size(vgfx_window_t window, int32_t* out_width, int32_t* out_height);
int vgfx_get_framebuffer(vgfx_window_t window, vgfx_framebuffer_t* out_fb);

// Additional window control
void vgfx_set_fps(vgfx_window_t window, int32_t fps);
void vgfx_set_title(vgfx_window_t window, const char* title);
void vgfx_set_fullscreen(vgfx_window_t window, int fullscreen);
int vgfx_is_fullscreen(vgfx_window_t window);
```

### Drawing Primitives

```c
// Pixel operations
void vgfx_pset(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t color);
int vgfx_point(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t *out_color);
void vgfx_cls(vgfx_window_t window, vgfx_color_t color);

// Lines and shapes
// Note: line uses start/end points; rect/fill_rect use top-left + width/height
void vgfx_line(vgfx_window_t window, int32_t x1, int32_t y1,
               int32_t x2, int32_t y2, vgfx_color_t color);
void vgfx_rect(vgfx_window_t window, int32_t x, int32_t y,
               int32_t w, int32_t h, vgfx_color_t color);
void vgfx_fill_rect(vgfx_window_t window, int32_t x, int32_t y,
                    int32_t w, int32_t h, vgfx_color_t color);
void vgfx_circle(vgfx_window_t window, int32_t cx, int32_t cy,
                 int32_t radius, vgfx_color_t color);
void vgfx_fill_circle(vgfx_window_t window, int32_t cx, int32_t cy,
                      int32_t radius, vgfx_color_t color);

// Clipping
void vgfx_set_clip(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h);
void vgfx_clear_clip(vgfx_window_t window);
```

### Input Handling

```c
// Event polling
int vgfx_poll_event(vgfx_window_t window, vgfx_event_t* out_event);
int vgfx_peek_event(vgfx_window_t window, vgfx_event_t* out_event);
void vgfx_clear_events(vgfx_window_t window);
int32_t vgfx_event_overflow_count(vgfx_window_t window);

// State queries. Mouse positions are logical pixels when a window coordinate
// scale is active.
int vgfx_key_down(vgfx_window_t window, vgfx_key_t key);
int vgfx_mouse_pos(vgfx_window_t window, int32_t* out_x, int32_t* out_y);
int vgfx_mouse_button(vgfx_window_t window, vgfx_mouse_button_t button);
```

### Colors

```c
// Predefined colors
#define VGFX_BLACK       0x000000
#define VGFX_WHITE       0xFFFFFF
#define VGFX_RED         0xFF0000
#define VGFX_GREEN       0x00FF00
#define VGFX_BLUE        0x0000FF
#define VGFX_YELLOW      0xFFFF00
#define VGFX_CYAN        0x00FFFF
#define VGFX_MAGENTA     0xFF00FF
#define VGFX_GRAY        0x808080

// Color construction
vgfx_color_t vgfx_rgb(uint8_t r, uint8_t g, uint8_t b);
void vgfx_color_to_rgb(vgfx_color_t color, uint8_t* r, uint8_t* g, uint8_t* b);
```

### Events

```c
typedef enum {
    VGFX_EVENT_NONE,
    VGFX_EVENT_KEY_DOWN,
    VGFX_EVENT_KEY_UP,
    VGFX_EVENT_TEXT_INPUT,
    VGFX_EVENT_MOUSE_MOVE,
    VGFX_EVENT_MOUSE_DOWN,
    VGFX_EVENT_MOUSE_UP,
    VGFX_EVENT_RESIZE,
    VGFX_EVENT_CLOSE,
    VGFX_EVENT_FOCUS_GAINED,
    VGFX_EVENT_FOCUS_LOST
} vgfx_event_type_t;
```

`VGFX_EVENT_TEXT_INPUT` emits translated Unicode codepoints. On Linux/X11, committed text now queues every UTF-8 codepoint produced by the platform input method instead of truncating composed input to the first character.

### Clipboard

```c
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format);
char *vgfx_clipboard_get_text(void);      // malloc'd UTF-8 string or NULL
void vgfx_clipboard_set_text(const char *text);
void vgfx_clipboard_clear(void);
```

The clipboard helpers operate on UTF-8 text and back the higher-level `Zanna.GUI.ClipboardText` and `Zanna.System.Clipboard` runtime surfaces. Linux/X11 uses the modern `CLIPBOARD` selection with `UTF8_STRING` conversion rather than legacy cut buffers.

---

## Testing

### Unit Tests

ZannaGFX includes comprehensive unit tests using the mock backend:

```bash
# Build tests
cmake --build build --target test_window test_pixels test_drawing test_input

# Run all graphics tests
ctest --test-dir build -R "test_window|test_pixels|test_drawing|test_input"
```

### Test Coverage

- **test_window** — Window lifecycle
- **test_pixels** — Pixel operations and framebuffer
- **test_drawing** — Drawing primitives
- **test_input** — Input and event queue
- **test_wayland_backend**, **test_wayland_loader** — Native Wayland adapter and its
  runtime symbol loader
- **test_linux_auto_backend** — `AUTO` backend selection and Wayland-to-X11 fallback

---

## Examples

### Example Programs

Located in `/src/lib/graphics/examples/`:

- **api_test.c** — API validation across backends
- **basic_draw.c** — Interactive drawing and input demo
- **quick_test.c** — Automated visual test (30 frames)

### Build Examples

```bash
cmake --build build --target basic_draw quick_test
./build/examples/basic_draw
```

---

## Integration with Zanna

### Using from CMake

```cmake
# Link against ZannaGFX
target_link_libraries(your_target PRIVATE zannagfx)

# Include headers
target_include_directories(your_target PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib/graphics/include
)
```

### BASIC / Zia Integration

Both BASIC and Zia access graphics through the `Zanna.Graphics.*` runtime namespace, which wraps ZannaGFX internally. Example:

```basic
USING Zanna.Graphics
DIM c AS Canvas
LET c = Canvas.New("My Window", 800, 600)
Canvas.Clear(c, Color.Rgb(0, 0, 0))
Canvas.Box(c, 100, 100, 200, 150, Color.Rgb(255, 0, 0))
Canvas.Flip(c)
```

---

## Architecture

### Component Structure

```text
src/lib/graphics/
├── include/
│   ├── vgfx.h           # Public API (window, drawing, input, color, clipboard)
│   └── vgfx_config.h    # Configuration macros
├── src/
│   ├── vgfx.c           # Core implementation
│   ├── vgfx_draw.c      # Drawing algorithms
│   ├── vgfx_internal.h  # Internal structures
│   ├── vgfx_platform_macos.m    # macOS Cocoa/AppKit backend (fully implemented)
│   ├── vgfx_platform_linux.c    # Linux X11 backend (fully implemented)
│   ├── vgfx_platform_win32.c    # Windows Win32 backend (fully implemented)
│   └── vgfx_platform_mock.c     # Mock backend for tests
├── tests/               # Unit tests
└── examples/            # Example programs
```

### Platform Abstraction

The library uses a platform abstraction layer with function pointers:

```c
// Platform backend functions
int vgfx_platform_init_window(struct vgfx_window* win, const vgfx_window_params_t* params);
void vgfx_platform_destroy_window(struct vgfx_window* win);
int vgfx_platform_process_events(struct vgfx_window* win);
int vgfx_platform_present(struct vgfx_window* win);
void vgfx_platform_sleep_ms(int32_t ms);
int64_t vgfx_platform_now_ms(void);
```

Each platform implements these functions in its backend file.

### Mock Backend

The mock backend (`vgfx_platform_mock.c`) provides:

- **Deterministic time control** — `vgfx_mock_set_time_ms()`
- **Event injection** — `vgfx_mock_inject_key_event()`, etc.
- **No OS dependencies** — Pure in-memory simulation
- **Test automation** — Reproducible test scenarios

---

## Documentation

### Detailed Specification

For complete implementation details, see:

- **[gfxlib.md](../src/lib/graphics/gfxlib.md)** — Full specification
- **[INTEGRATION.md](../src/lib/graphics/INTEGRATION.md)** — Build integration details
- **[STATUS.md](../src/lib/graphics/STATUS.md)** — Implementation status
- **[MACOS_BACKEND.md](../src/lib/graphics/MACOS_BACKEND.md)** — macOS backend notes
- **[DRAWING_PRIMITIVES.md](../src/lib/graphics/DRAWING_PRIMITIVES.md)** — Algorithm details
- **[TEST_INFRASTRUCTURE.md](../src/lib/graphics/TEST_INFRASTRUCTURE.md)** — Testing approach

### API Reference

Full API documentation is available in the header comments:

- **[vgfx.h](../src/lib/graphics/include/vgfx.h)** — Public API with Doxygen comments

---

## Build Configuration

### CMake Options

```cmake
# Standalone build options
option(VGFX_BUILD_TESTS "Build ZannaGFX tests" ON)
option(VGFX_BUILD_EXAMPLES "Build ZannaGFX examples" ON)
```

When building as part of Zanna:

- Tests are controlled by `ZANNA_BUILD_TESTING`
- Examples are controlled by `BUILD_EXAMPLES`

### Platform Detection

The build system automatically selects the correct backend:

- **macOS**: Cocoa backend (`vgfx_platform_macos.m`)
- **Linux**: X11 backend (`vgfx_platform_linux.c`)
- **Windows**: Win32 backend (`vgfx_platform_win32.c`)

---

## Future Enhancements

### Implemented in Runtime Layer

The following features have been implemented in the Zanna runtime layer (`Zanna.Graphics.*`):

- ✅ **Sprites** — `Zanna.Graphics.Sprite` class with animation and collision detection
- ✅ **Tilemaps** — `Zanna.Graphics2D.Tilemap` class for tile-based game rendering
- ✅ **Camera** — `Zanna.Graphics.Camera` class for 2D viewport management
- ✅ **Image processing** — Invert, grayscale, tint, blur, bilinear resize
- ✅ **Color utilities** — HSL conversion, lerp, brighten/darken
- ✅ **Gradients** — Horizontal and vertical gradient drawing

- ✅ **Image loading** — BMP, PNG, JPEG, and GIF decoders; BMP and PNG encoders
- ✅ **Bitmap fonts** — `Zanna.Graphics.BitmapFont` (BDF and PSF)

### Planned Features (Future)

- **Palette modes** — 8-bit indexed color
- **Multiple windows** — Multi-window support

---

## Performance Notes

### Rendering Performance

ZannaGFX uses pure software rendering with the following characteristics:

- **Framebuffer format**: 32-bit RGBA (4 bytes per pixel)
- **Drawing algorithms**: Integer-only Bresenham (lines) and midpoint (circles)
- **Memory access**: Direct memory writes (no GPU overhead)
- **FPS limiting**: Voluntary frame rate control via sleep

### Typical Performance

On modern hardware (2020+ Mac):

- **800×600 window**: 60 FPS sustained
- **Full-screen drawing**: ~1000 primitives per frame at 60 FPS
- **Event latency**: <1ms for keyboard/mouse

---

## License

ZannaGFX is part of the Zanna project and distributed under the GNU GPL v3.
See [LICENSE](../LICENSE) for details.

---

## Summary

ZannaGFX provides a simple, deterministic, cross-platform 2D graphics solution for the Zanna project:

- Builds as part of Zanna, with a standalone CMake path for library-only work
- Covered by unit tests running against the deterministic mock backend
- Backends: Cocoa on macOS, native Wayland with X11 fallback on Linux, Win32 GDI on Windows

For questions or contributions, see the [main Zanna documentation](README.md).
