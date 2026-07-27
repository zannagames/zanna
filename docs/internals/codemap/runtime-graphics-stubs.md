---
status: active
audience: contributors
last-verified: 2026-07-26
---

# CODEMAP: Runtime Graphics Stubs

`src/runtime/graphics/common/rt_graphics_stubs.c` is compiled when
`ZANNA_ENABLE_GRAPHICS` is not defined. It preserves link compatibility for the
graphics/runtime surface while making unavailable stateful graphics operations
fail deterministically.

## Stub Policy

| API Shape | Required Behavior |
|-----------|-------------------|
| Availability queries | Return a deterministic false/disabled value |
| Constructors and stateful draw/render APIs | Raise `InvalidOperation` through the trap layer |
| Destructors/finalizers | No-op if no resource could have been acquired |
| Pure helpers with no backend dependency | Remain functional |
| Text measurement helpers | Remain functional when they use embedded/backend-free data |
| Null-handle query fallbacks | Allowed only when documented and covered by tests |

Silent success is not acceptable for unavailable Canvas, Sprite, Canvas3D,
scene, model, or backend APIs. A disabled build must fail at the first attempted
use, not later through a null dereference or corrupted layout calculation.

## Review Risk

The stub file is large because it covers multiple runtime domains:

| Domain | Examples |
|--------|----------|
| 2D Canvas | window lifecycle, drawing primitives, event polling |
| Pixel/Sprite helpers | image buffers, sprite draw paths, atlas helpers |
| Canvas3D core | 3D canvas lifecycle, camera, render queue |
| 3D assets | meshes, materials, models, loaders |
| 3D scene systems | scene graph, terrain, water, navigation, particles |
| Compatibility helpers | pure math/text/metric helpers that do not need a display |

The target split is:

| Target File | Ownership |
|-------------|-----------|
| `rt_canvas_stubs.c` | 2D Canvas lifecycle, draw, input/event fallback |
| `rt_pixels_sprite_stubs.c` | Pixels/Sprite/SpriteBatch/TextureAtlas disabled behavior |
| `rt_canvas3d_stubs.c` | Canvas3D lifecycle and render calls |
| `rt_3d_asset_stubs.c` | Mesh, material, model, texture, loader calls |
| `rt_3d_scene_stubs.c` | Scene, terrain, water, nav, particles, post-FX |
| `rt_graphics_helper_stubs.c` | Backend-free helpers kept live in disabled builds |

## Required Coverage

Changes to this surface should run:

```bash
./scripts/audit_runtime_surface.sh --summary-only --build-dir=build
ctest --test-dir build --output-on-failure -R 'graphics_surface|audio_surface|runtime_surface'
```

When a stub behavior changes, add a focused disabled-graphics test that verifies
the trap/default contract rather than relying only on link success.
