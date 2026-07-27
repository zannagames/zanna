---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Graphics Library

Cross-platform software 2D graphics library (`src/lib/graphics/`) and Zanna runtime graphics classes (`src/runtime/graphics/`).

## Runtime Classes (`src/runtime/graphics/`)

Key 2D runtime components are implemented by these files. The 3D tree is mapped separately
in [Graphics3D Architecture](../graphics3d-architecture.md).

| File | Purpose |
|------|---------|
| `src/runtime/graphics/2d/rt_camera.h` / `.c` | 2D viewport camera with zoom, bounds, transforms, and internal-only dirty tracking |
| `src/runtime/graphics/2d/rt_canvas.c` | Canvas/window runtime bridge |
| `src/runtime/graphics/2d/rt_color.c` | Color utilities (RGB, HSL, lerp, brighten/darken) |
| `src/runtime/graphics/2d/rt_drawing.c` / `rt_drawing_advanced.c` | Canvas drawing primitives |
| `src/runtime/graphics/2d/rt_pixels.h` / `.c` plus `rt_pixels_*` | Software image buffers, transforms, drawing, PNG/JPEG, and other I/O |
| `src/runtime/graphics/2d/rt_sprite.h` / `.c` | Animated sprites with scaling, collision, and multiple frames |
| `src/runtime/game/rt_spriteanim.h` / `.c` | `Zanna.Game.SpriteAnimation` playback state |
| `src/runtime/graphics/2d/rt_spritebatch.h` / `.c` | Batched sprite rendering with stable equal-depth ordering |
| `src/runtime/graphics/2d/rt_texatlas.h` / `.c` | Named-region texture atlas and grid slicing |
| `src/runtime/graphics/2d/rt_spritesheet.h` / `.c` | Named region extraction from a source image |
| `src/runtime/graphics/2d/rt_tilemap.h` / `.c` plus `rt_tilemap_io.c` | Multi-layer tile rendering, animation, culling, and persistence |
| `src/runtime/graphics/2d/rt_scene.h` / `.c` | 2D scene nodes and scene graphs |

Graphics-disabled builds use `src/runtime/graphics/common/rt_graphics_stubs.c`; see
[Runtime Graphics Stubs](runtime-graphics-stubs.md) for the required trap/no-op/helper policy.

## Low-Level C Library

The underlying ZannaGFX library provides platform-specific window management and drawing.

## Public API (`include/`)

| File            | Purpose                                             |
|-----------------|-----------------------------------------------------|
| `vgfx_config.h` | Configuration macros and defaults                   |
| `vgfx.h`        | Complete public API: window, drawing, input, events |

## Core Implementation (`src/`)

| File              | Purpose                                                             |
|-------------------|---------------------------------------------------------------------|
| `vgfx.c`          | Platform-agnostic core: window lifecycle, event queue, FPS limiting |
| `vgfx_draw.c`     | Drawing primitives: line, rect, circle (Bresenham, midpoint)        |
| `vgfx_internal.h` | Internal structures and platform backend declarations               |

## Platform Backends (`src/`)

| File                    | Purpose                                |
|-------------------------|----------------------------------------|
| `vgfx_platform_linux.c` | Linux X11 backend — functional; 32-bit RGBA XImage (depth=32) preserves alpha channel correctly |
| `vgfx_platform_macos.m` | macOS Cocoa backend (fully functional) |
| `vgfx_platform_mock.c`  | Mock backend for deterministic testing |
| `vgfx_platform_win32.c` | Windows Win32 backend — functional; RGBA→BGRA conversion uses 4-pixel unrolled batch for reduced per-frame overhead |

## Tests (`tests/`)

| File             | Purpose                     |
|------------------|-----------------------------|
| `test_drawing.c` | Drawing primitive tests     |
| `test_harness.h` | Test harness macros         |
| `test_input.c`   | Input and event queue tests |
| `test_pixels.c`  | Pixel operation tests       |
| `test_window.c`  | Window lifecycle tests      |
| `vgfx_mock.h`    | Mock backend test helpers   |

## Examples (`examples/`)

| File           | Purpose                                 |
|----------------|-----------------------------------------|
| `api_test.c`   | API validation (all backends)           |
| `basic_draw.c` | Interactive demo with drawing and input |
| `quick_test.c` | Automated visual test (30 frames)       |

## Documentation

Paths in this table are relative to `src/lib/graphics/`.

| File                        | Purpose                            |
|-----------------------------|------------------------------------|
| `docs/ZANNA_INTEGRATION.md` | BASIC runtime integration guide    |
| `DRAWING_PRIMITIVES.md`     | Drawing algorithm details          |
| `gfxlib.md`                 | Complete specification             |
| `INTEGRATION.md`            | CMake integration guide            |
| `MACOS_BACKEND.md`          | macOS backend implementation notes |
| `README.md`                 | Main documentation and quick start |
| `STATUS.md`                 | Backend and feature status         |
| `TEST_INFRASTRUCTURE.md`    | Test harness overview              |
| `STATUS.md`                 | Implementation status tracker      |
| `TEST_INFRASTRUCTURE.md`    | Testing approach                   |
