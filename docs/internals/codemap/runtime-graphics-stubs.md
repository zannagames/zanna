---
status: active
audience: contributors
last-verified: 2026-09-13
---

# CODEMAP: Runtime Graphics Stubs

When `ZANNA_ENABLE_GRAPHICS` is not defined, `src/runtime/CMakeLists.txt` swaps
the graphics component's sources for `RT_GRAPHICS_DISABLED_SOURCES`. That happens
with `ZANNA_GRAPHICS_MODE=OFF`, and also with `AUTO` on Linux hosts that lack X11
development headers. The list keeps
backend-free sources that compile in both modes and adds the stub translation units in
`src/runtime/graphics/common/`. Together they must define every C symbol the
generated VM handler table references, so the graphics-disabled runtime still
links. Unavailable stateful graphics operations fail deterministically.

## Stub Policy

| API Shape | Required Behavior |
|-----------|-------------------|
| Availability queries | Return a deterministic false/disabled value |
| Constructors and stateful draw/render APIs | Raise `InvalidOperation` through the trap layer |
| Result-returning loaders | Return `Err("<Class>.<Member>: graphics support not compiled in")` |
| Destructors/finalizers | No-op if no resource could have been acquired |
| Queries and mutations on handles that cannot exist | Documented fallback (`0`, `NULL`, empty string) or no-op; `ZANNA_GRAPHICS_STUBS_STRICT=1` makes them trap |
| Fluent members | Return the receiver unchanged |
| Consuming entry points | Release the buffers their contracts transfer, exactly like the real implementation |
| Backend-independent constants | Return the graphics-build value |
| Pure helpers with no backend dependency | Remain functional by compiling the real source in both modes |

Silent success is not acceptable for unavailable Canvas, Sprite, Canvas3D,
scene, model, or backend constructors. A disabled build must fail at the first
attempted use, not later through a null dereference or corrupted layout
calculation. Every stub's doc comment names its classification ("Trapping stub",
"Silent fallback stub", "no-op"), which the source-health audit checks.

## File Split

| File | Ownership |
|------|-----------|
| `rt_graphics_stubs_internal.h` | Shared `rt_graphics_unavailable_()` trap, `rt_graphics_unavailable_result_()` Err builder, strict-mode macros, and the public headers every stub unit includes |
| `rt_graphics_stubs.c` | Historical anchor; exports nothing |
| `rt_canvas_stubs.c` | 2D Canvas lifecycle, draw, input/event fallback, backend-free text metrics, TtfFont |
| `rt_canvas3d_stubs.c` | Canvas3D lifecycle and render calls, Camera3D, PostFX3D, InstanceBatch3D |
| `rt_3d_render_stubs.c` | Newer Canvas3D, Camera3D, PostFX3D, InstanceBatch3D, Mesh3D, Material3D, Light3D, SceneAsset, and glTF members (split out to keep the older files under 4000 lines) |
| `rt_3d_asset_stubs.c` | Mesh, material, model, texture, and loader calls |
| `rt_3d_textureasset_stubs.c` | TextureAsset3D |
| `rt_3d_animation_effect_stubs.c` | Animation controllers and blends, decals, sprites, atlases, particles, skeletons |
| `rt_3d_scene_stubs.c` | Scene graph, scene nodes, sky, light baker |
| `rt_3d_physics_stubs.c` | Physics world, bodies, joints, colliders, characters |
| `rt_3d_cloth_stubs.c` | Cloth3D, world cloth hooks, CCD clamp counters |
| `rt_3d_world_stubs.c` | Terrain, water, navigation, paths, transforms |
| `rt_3d_game_stubs.c` | Game3D subsystems whose sources are wrapped in `ZANNA_ENABLE_GRAPHICS` (AI, perception, interaction, footsteps, surfaces, minimaps, persistence, world streaming) plus the internal world-step hooks `rt_game3d.c` calls |
| `rt_graphics_media_stubs.c` | Video playback and 3D sound sources |
| `rt_disabled_runtime_stubs.c` | Supplemental helper stubs (progress bars, Canvas3D statistics, scene/physics rebasing, glTF preload bundles) |

Backend-free code compiles in both modes instead of being stubbed. Examples:
`graphics/2d/rt_color.c` (all Color math), `graphics/text/rt_font.c`,
`graphics/text/rt_bitmapfont.c`, the Pixels sources, GUI model code in
`graphics/gui/rt_gui_ide.cpp` (commands, virtual lists and trees, accessibility,
test harness), and the Game3D core in `graphics/3d/rt_game3d*.c`. Shared helpers
such as the built-in font's UTF-8 text measurement live in the unguarded part of
`rt_graphics_internal.h`, so both modes compute identical results.

## Rules For Changes

- A new `RT_FUNC` whose implementation compiles only with graphics needs a stub
  in the file that owns its class.
- Never stub a symbol that a both-mode source already defines. Two strong
  definitions fail the graphics-disabled link.
- A stub file must include every header that declares the functions it defines,
  so signature drift is a compile error rather than a silent ABI mismatch.
- Game3D sources compile in both modes. Graphics3D payload types are incomplete
  there, so size-checked handle validation uses the `RT_GAME3D_*_PAYLOAD_SIZE`
  macros from `rt_game3d_internal.h`. Field access stays behind
  `#ifdef ZANNA_ENABLE_GRAPHICS`, and the internal headers' graphics-disabled
  branches supply fail-closed checked casts (`rt_canvas3d_checked_or_stack`,
  `rt_camera3d_checked_or_stack`, `scene3d_checked`, `scene_node3d_checked`).
- Includes of ZannaGFX/ZannaGUI headers by bare name belong inside
  `#ifdef ZANNA_ENABLE_GRAPHICS`; those include directories are absent from
  graphics-disabled builds.

## Required Coverage

Changes to this surface should run:

```bash
./scripts/check_runtime_completeness.sh
./scripts/audit_runtime_surface.sh --summary-only --build-dir=build
ctest --test-dir build --output-on-failure -R 'graphics_surface|audio_surface|runtime_surface'
```

Graphics-enabled builds cannot see graphics-disabled compile or link failures.
Verify those in a separate build directory, never the shared `build/` tree:

```bash
cmake -S . -B /tmp/zanna-gfxoff -DZANNA_GRAPHICS_MODE=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/zanna-gfxoff --target test_rt_graphics_surface_link \
    test_rt_canvas_unavailable test_rt_color_utils -j8
ctest --test-dir /tmp/zanna-gfxoff --output-on-failure \
    -R 'test_rt_graphics_surface_link|test_rt_canvas_unavailable|test_rt_color_utils'
```

When a stub behavior changes, add a focused disabled-graphics test that verifies
the trap/default contract rather than relying only on link success.
