---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Zanna.Graphics3D — Architecture

## System Overview

```text
Zia / BASIC program
    ↓ (runtime.def → rtgen → RuntimeRegistry)
Canvas3D / Mesh3D / Camera3D / Material3D / Light3D  (rt_canvas3d.c, rt_mesh3d.c, ...)
    ↓ (vgfx3d_backend_t vtable dispatch)
┌─────────────────────────────────────────────────┐
│ Software    │ Metal      │ D3D11     │ OpenGL   │
│ (always)    │ (macOS)    │ (Windows) │ (Linux)  │
│ _sw.c       │ _metal.m   │ _d3d11.c  │ _opengl.c│
└─────────────────────────────────────────────────┘
    ↓
ZannaGFX (vgfx.h) — window management, framebuffer, input
    ↓
Platform (NSWindow / HWND / X11 Window)
```

## Backend Abstraction (vgfx3d_backend.h)

All rendering dispatches through a vtable:

```c
typedef struct vgfx3d_backend {
    const char *name;
    /* Lifecycle */
    void *(*create_ctx)(vgfx_window_t win, int32_t w, int32_t h);
    void (*destroy_ctx)(void *ctx);
    /* Frame */
    void (*clear)(void *ctx, vgfx_window_t win, float r, float g, float b);
    void (*resize)(void *ctx, int32_t w, int32_t h);
    void (*begin_frame)(void *ctx, const vgfx3d_camera_params_t *cam);
    void (*submit_draw)(void *ctx, vgfx_window_t win, const vgfx3d_draw_cmd_t *cmd,
                        const vgfx3d_light_params_t *lights, int32_t light_count,
                        const float *ambient, int8_t wireframe, int8_t backface_cull);
    void (*end_frame)(void *ctx);
    /* Render target */
    void (*set_render_target)(void *ctx, vgfx3d_rendertarget_t *rt);
    /* Shadow mapping */
    void (*shadow_begin)(void *ctx, int32_t slot, float *depth_buf,
                         int32_t w, int32_t h, const float *light_vp);
    void (*shadow_draw)(void *ctx, const vgfx3d_draw_cmd_t *cmd);
    void (*shadow_end)(void *ctx, int32_t slot, float bias);
    /* Skybox */
    void (*draw_skybox)(void *ctx, const void *cubemap);
    /* Instanced rendering (NULL = N individual submit_draw fallback) */
    void (*submit_draw_instanced)(void *ctx, vgfx_window_t win, const vgfx3d_draw_cmd_t *cmd,
                                  const float *instance_matrices, int32_t instance_count, ...);
    /* Display */
    void (*present)(void *ctx);
    int (*readback_rgba)(void *ctx, uint8_t *out, int32_t w, int32_t h, int32_t stride);
    void (*present_postfx)(void *ctx, const vgfx3d_postfx_chain_t *postfx);
    void (*apply_postfx)(void *ctx, const vgfx3d_postfx_chain_t *postfx);
    void (*set_gpu_postfx_enabled)(void *ctx, int8_t enabled);
    void (*set_gpu_postfx_snapshot)(void *ctx, const vgfx3d_postfx_chain_t *postfx);
    void (*show_gpu_layer)(void *ctx);
    void (*hide_gpu_layer)(void *ctx);
} vgfx3d_backend_t;
```

**Selection logic** (`vgfx3d_select_backend`):
1. Try platform GPU backend (Metal/D3D11/OpenGL)
2. If `create_ctx` returns NULL → fall back to software
3. Software backend always succeeds

Canvas exposes the selected backend through `Canvas3D.Backend`. Production code should use
`Canvas3D.BackendCapabilities` or `Canvas3D.BackendSupports(name)` when deciding whether to
enable optional paths such as window readback, GPU post effects, hardware instancing, skybox, or
shadow maps. Those queries are derived from the active vtable hooks plus Canvas-owned software
fallbacks, so they remain stable if backend names or platform selection change.

## Source Organization

The 3D runtime keeps public ABI boundaries in the existing C translation units,
while private helper-heavy regions live in adjacent `.inc` companions:

- `render/rt_canvas3d_frame_postfx.inc` owns per-frame GPU post-FX latch and
  motion-vector gating helpers used by `rt_canvas3d.c`.
- `rt_game3d_indices.inc` owns the World3D body/name indexes used by
  `rt_game3d.c` to avoid repeated entity scans in collision and name lookup paths.
- `assets/rt_fbx_triangulation.inc` owns FBX polygon projection and ear-clipping
  helpers used by `rt_fbx_loader.c`.

These files are private implementation slices, not standalone compilation
units; they preserve static helper scope while keeping the largest runtime files
readable.

## Game3D Streaming Vertical Slice

`examples/3d/openworld_slice/` is the current architecture smoke for the
Game3D layer above the raw renderer. It keeps the runtime boundaries explicit:
`Zanna.Game3D.WorldStream3D` owns cell and terrain residency telemetry,
manifest loading, deterministic per-update load budgeting, and editor/debug
inspection hooks, `Zanna.Game3D.AssetHandle3D`
owns deferred model completion, `World3D.bakeNavMesh` triggers a scene-derived
editor bake, and lower-level `Zanna.Graphics3D.NavMesh3D` / `NavAgent3D` remain
the navigation primitives. The sample's CTest runs on the
software backend, moves a stream center through all four far-apart quadrants of
a >4 km² stand-in, checks one-cell/one-tile bounded residency plus
rendered heightmapped `Terrain3D` payload access and matching static
heightfield collider residency, validates
entry names/centers/resident flags through the inspection hooks, checks staged
stream requests through `pendingRequestCount`, steps
character/physics/nav state, verifies the world-scoped nav bake includes the
streamed terrain source, loads a synthetic skinned glTF agent through
Game3D assets, crossfades its auto-bound animator, binds LookAt IK, validates a
terrain-sampled TwoBone foot IK target, validates KTX2
texture-asset material fallback and compressed-format residency telemetry, reads `World3D`
entity/body/draw/visibility/stream telemetry counters, captures the final
post-FX plus overlay frame, compares it to the committed software baseline, and
repeats the same fixed-step sequence for deterministic replay. A companion
`g3d_openworld_slice_perf_probe` CTest records deterministic software frame-loop
metrics, optional backend GPU frame time, and validates that the telemetry
counters remain populated; the current named local Release software and Metal
measurements live under
`examples/3d/openworld_slice/baselines/`; `g3d_openworld_slice_perf_harness`
wraps the probe and validates the required counters for CI logs. A second companion,
`g3d_openworld_slice_long_traversal`, repeats all-quadrant stream
churn and replay checks without the final-frame readback cost. A third
companion, `g3d_openworld_slice_visibility_dense_probe`, authors a dense
city/forest occlusion scene and records PVS draw-call/fill-proxy reduction while
comparing software final-frame pixels against the no-PVS baseline. A fourth
companion, `g3d_openworld_slice_gpu_smoke`, requests the platform GPU backend
and reports a clean skip when that backend is unavailable; when it runs, it also
submits a degenerate-normal/tangent normal-mapped mesh and a 24-light
clustered/forward+ draw so GPU shader basis fallbacks and many-light upload
paths stay exercised outside unit tests. The same smoke now follows with a
3-cascade primary directional CSM fixture, covering the lifted four-slot shadow
payload and backend cascade split metadata on the platform GPU lane. On D3D11,
`Canvas3D.FrameGpuTimeUs` is backed by timestamp/disjoint queries so the Windows
GPU smoke and perf probes can record real device-frame timing alongside CPU
wall-clock metrics.

## Runtime Input Guards

Graphics3D clamps public numeric state before it enters renderer-facing structs. `Canvas3D` clamps
clear, ambient, fog, and frame-delta inputs before they reach backend color state or camera shake,
validates camera position/projection floats at `Begin`, sanitizes material shader uniforms while
packing draw commands, and stores stable motion-history keys for single and instanced draws.
Mesh, skinned, morphed, blended, and instanced draw entry points validate finite model matrices before
they reach culling, terrain splat capture, or backend command queues.
`Camera3D` clamps clip planes, aspect, ortho size, positions, orbit/follow distances, and projection
narrowing so view/projection matrices and picking rays stay finite; FPS camera motion also bounds
per-frame movement and wraps yaw before computing direction vectors. `SceneNode` and `Transform3D`
sanitize TRS components, reduce very large Euler/axis rotations before trig, normalize quaternions,
keep LOD distances non-negative, and lazily refresh child world matrices when a parent's world
revision changes. Iterative subtree traversals are still used for sync, search, bounds, lights, and
draw collection so deep imported hierarchies do not depend on C stack depth. Node animation clips reject
non-finite samples and non-increasing key times before they reach the sampler. `Collider3D` sanitizes
primitive dimensions, substitutes positive fallback extents for zero/non-finite primitive sizes,
validates heightfield `Pixels` handles, rejects compound-child cycles, and guards heightfield
allocation sizes. `Physics3D` keeps world gravity, time steps, body motion state, damping, impulses,
and character-controller settings finite before they feed integration and broadphase code; capsule
primitive narrow-phase uses the body's quaternion orientation when deriving its world-space axis, and
ray queries use analytic sphere/capsule/box tests with AABB fallback for complex colliders. Mesh
colliders use the body-centric physics broadphase before per-mesh BVH traversal for sphere, capsule,
box, and convex-hull triangle contacts. Contact assembly publishes bounded multi-point manifolds for
AABB pairs and clipped OBB face contacts. The warm-started contact solver batches awake contacts into
independent islands before its velocity and position passes, and contact/query result buffers use checked reserve-count arithmetic so a
pathological broadphase cannot wrap allocation sizes.
`Mesh3D` rejects invalid procedural dimensions, non-finite OBJ/STL attributes, malformed OBJ face
tokens, collinear triangles, and overflowing OBJ face indices; generated planes face +Y, generated UV
spheres avoid zero-area pole triangles, importers skip isolated degenerate faces, normals/tangents skip
overflowing intermediate vectors, bone weights are filtered and renormalized, and failed mesh builds
are not cloned as drawable meshes. Public `AddVertex`/`AddTriangle` validation traps do not poison
the mesh, so callers can handle a bad append and keep building; allocation and importer failures still
mark the build failed. Empty or unsupported OBJ files are rejected instead of returning drawable
zero-triangle meshes. Imported OBJ faces with partial normals fill only missing normals so authored
normal data is preserved. Negative-determinant mesh transforms reverse triangle winding to keep mirrored
geometry cullable from the expected side. Exact binary STL files stream triangle records directly from
disk, while non-exact or ASCII STL files keep the bounded buffered path and close geometry batches on
failure before returning.
`Particles3D` bounds emitter ranges,
rates, alpha, spread, shape, update time, positions, gravity, and emitter extents. `InstanceBatch3D`
stores only finite float-range matrix elements for culling and backend submission. `Light3D` clamps colors, intensities, attenuations, spot angles,
and fallback directions before the light list is copied into backend parameters. `PostFX3D` bounds
every effect parameter and exports the sanitized ordered chain, including bloom pass counts, to GPU
backends. `Mat4` operations validate matrix handles before reading storage and reject non-finite
inverse determinants. `MorphTarget3D`, `Skeleton3D`, animation players, and animation blenders
bound vertex counts, keyframe growth, blend times, speeds, authored matrices, and morph delta floats
before draw calls can copy them into backend commands. Render targets guard stride and pixel-count
math before allocating or copying color/depth buffers. These guards are intentionally in the runtime classes instead
of individual backends so software, Metal, D3D11, and OpenGL receive the same clean state.

Graphics3D object handles use stable internal class IDs from `rt_graphics3d_ids.h`. Public APIs that
store or dereference opaque Graphics3D handles validate those IDs before casting, which prevents a
Mesh3D/Path3D/Terrain3D-style handle mix-up from being interpreted as another runtime struct. The
IDs are negative and module-scoped so they do not collide with legacy class-id `0` objects, and
trusted struct-handle shortcuts are gated behind an explicit internal build flag.
The handle guard policy covers render targets, cubemaps, scene nodes, physics worlds/bodies/joints,
colliders, particles, and water resources: wrong-class handles no-op or return safe defaults before
any retained reference or struct dereference happens. Resource setters such as `Particles3D.Texture`
and `Water3D.Texture`/`NormalMap`/`EnvMap` keep the previous valid binding when given a foreign
object. Pixel and matrix resource slots validate both class ID and payload size before reading the
underlying runtime storage, so undersized same-ID objects are rejected as invalid handles. Physics
triggers also treat tracked bodies as weak world membership: a body removed from the
world emits one exit transition and is then forgotten.
Renderer-internal stack fixtures are available only when the graphics runtime is built with
`RT_G3D_ALLOW_STACK_FIXTURES=1` for tests. Production builds validate `Canvas3D` and `Camera3D`
handles by class id and reject arbitrary non-heap pointers before reading object fields.

The advanced helpers follow the same numeric guard policy. `TextureAtlas3D` validates atlas and
Pixels handles before copying image data, duplicates tile edge padding for bilinear correctness, and
keeps dirty cached Pixels intact on rebuild failure.
`Terrain3D`, `Sprite3D`, `Decal3D`, `Water3D`, `Vegetation3D`, `Path3D`, `NavMesh3D`, and
`NavAgent3D` clamp or reject non-finite sizes, frame rectangles, normals, Perlin parameters, wave
parameters, density maps, spline points, empty navigation meshes, and steering distances at the
runtime boundary. Water and vegetation allow zero-delta updates for dirty geometry rebuilds and
camera-relative culling refreshes without advancing simulation time.

### Metal Window Presentation Model

The Metal backend now follows the same split as the other GPU runtimes:

- direct mode: when GPU postfx is disabled, window-backed draws render straight into the current CAMetalLayer drawable and `present()` just schedules that drawable for display
- postfx mode: the main scene renders into an HDR `RGBA16F` scene target, optional legacy overlays render into a separate UNORM overlay target, `apply_postfx` can composite the tonemapped scene to the drawable before final overlays, and `present_postfx` remains the single-step fallback
- overlay composition: screen-space overlays are blended after bloom / tonemap / SSAO / DOF / motion blur, so UI stays crisp and the post stack keeps using the main 3D scene camera, depth, and motion history
- material/shadow safety: vertex color and alpha are part of the Metal base-color contract, shader normalization has zero-vector fallbacks, and shadow maps are exposed only as a contiguous completed prefix with clip-space `w` and depth-range checks before sampling

This keeps the no-postfx path cheap while preserving the correct scene-history inputs required by the GPU postfx path.

### D3D11 Window Presentation Model

The D3D11 backend now uses two window-backed presentation modes:

- direct mode: when GPU postfx is disabled, draws render straight into the swapchain backbuffer
- postfx mode: the main scene renders into HDR scene targets, optional legacy overlays render into a separate UNORM overlay target, `apply_postfx` can composite the tonemapped scene to the swapchain before final overlays, and `present_postfx` remains the single-step fallback; multi-pass readback uses backend-owned post-FX scratch targets so screenshot composition does not overwrite the live scene color
- overlay composition: the first overlay pass clears the overlay target to transparent black, while later overlay passes in the same frame preserve the existing overlay contents before final compositing; final overlays replayed after `apply_postfx` draw directly over the already-composited swapchain while preserving scene temporal history and avoiding stale separate-overlay state; separate-overlay compositing uses premultiplied-alpha blending because the overlay render target already contains alpha-blended color
- motion history: only opaque scene draws write the D3D11 motion-vector render target; alpha-blended and additive draws write color only so they do not corrupt motion blur / temporal reconstruction inputs
- overlay/depth-disabled draws: D3D11 treats depth-test-disabled submissions as color-only for motion targets, matching HUD/debug overlay semantics and preventing screen-space quads from seeding temporal motion
- culling convention: D3D11 marks counter-clockwise triangles as front-facing so terrain and mesh winding matches the runtime/OpenGL convention instead of culling foreground surfaces
- texture-space conversion: D3D11 shader code converts clip/NDC coordinates to top-left-origin texture UVs for shadow maps, post-FX world reconstruction, and motion-vector sampling so vertical motion and shadow lookups match the rest of the runtime
- shader/post-FX numeric guards: D3D11 rejects degenerate, non-finite, and oversized direction vectors before CPU constant upload and in HLSL safe-normalize helpers; authored positions and matrix components are bounded, morph position/normal deltas plus every accumulated candidate are validated, invalid skinning matrix products are skipped, and shadow projection rejects invalid homogeneous/projected coordinates before texture access; normal maps skip degenerate tangent bases, height-fog exponent and optical-depth math are bounded, motion blur samples a centered kernel, DOF treats aperture as blur strength, and FXAA compares neighbors against the center pixel; scene/depth neighbor sampling isolates malformed texels, post-FX world reconstruction validates signed homogeneous `w`, TAA validates complete motion/depth/clip payloads before reprojection, and SSR validates masks plus every primary, marching, refinement, and hit depth/clip sample before texture access; invalid motion vectors collapse to zero, the CPU clamps SSR ray steps to the shader's 8-48 iteration contract, negative HDR values are removed before rational tonemap curves, and bloom replaces non-finite/negative input, keeps its Karis denominator positive, and clamps outputs to the finite RGBA16F range
- morphing/skinning robustness: D3D11 consumes all 64 packed morph weights, ignores non-finite morph/bone weights and deltas, validates accumulated morph/skin results, normalizes usable bone weights in the shader, and falls back to the original position/vector when a skinned vertex has no usable weights; instanced skinning now requires an exact packed-palette layout and confines each joint lookup to that instance's palette slice, while oversized uploads are clamped to the shader-visible limit and unused palette entries are identity-padded
- resource lifetime: scene resolves fall back to a backend pass-through composite instead of presenting stale swapchain contents, target binding requires complete texture/RTV/DSV/SRV/staging resource sets, texture/cubemap caches unbind all shader slots used by main draws (including the shadow-atlas and BRDF-LUT slots) before replacement or pruning while preserving a resident floor, and lazily joined process-wide HLSL sources use atomic publication so concurrent context creation cannot race or leak losing allocations; failed row decodes and IBL generations are negative-cached instead of retrying forever, corrupt row/native-mip/cubemap continuation cursors fail rather than silently rewinding partially uploaded resources, deferred native-texture and IBL telemetry uses cache-owned scalars instead of dereferencing non-owned runtime objects, actively requested paused uploads refresh their cache age, valid texture/cubemap uploads can fall back to one-draw temporary SRVs only within budget, shadow pass begin/end paths clear stale active depth outputs before restoring color targets, and shadow slots are advertised only as a contiguous prefix completed in the current frame after all CPU-side draw submissions succeed, with per-light cascade counts clamped to shader-visible SRVs
- allocation fallback/readback: if an offscreen D3D11 target cannot be allocated, the backend downgrades to an available target before clear/draw; resize failure attempts to recreate the prior swapchain render/depth targets before returning, a successful resize immediately rebinds the new swapchain target, and a successful resize with failed target recreation records the new dimensions so the next frame can retry the missing RTV/depth set; readback and render-target sync unbind output resources for `CopyResource`, require one mip/array slice/sample with matching zero sample quality, validate exact staging usage/access/bind/misc flags, supported RTT color-format discriminators, and live RTT metadata, always unmap a successfully mapped resource even if the driver reports a null data pointer, validate mapped row pitch, report device removal on map failure, and restore the previous target binding afterward; depth probes select RTT, offscreen-scene, or direct-swapchain depth from the active route, use that source's dimensions, detach its output/SRV bindings before typeless R32 copies, validate the entire queued batch before the first copy so no skipped slot can publish stale staging data, preserve pending GPU work, distinguish a busy map from a terminal failure, sanitize reversed-Z results, and reset their state on scene/RTT target recreation; screenshot-time TAA resolves use scratch targets without advancing live temporal history; readback after `apply_postfx` sources the already-composited swapchain instead of replaying the effect chain, while readback after `Present()` uses a structurally validated pre-present snapshot only when the snapshot texture exists and otherwise falls back to scene/backbuffer sources; failed screenshot-time post-FX or overlay replay falls back to the best remaining source instead of returning an empty readback; new main/RTT frames and real post-FX enable toggles clear the composited-swapchain marker while load-existing overlay passes and same-state post-FX reapplies may preserve it; RTT load-existing passes still refresh scene temporal history because they render to independent targets; reusing a live RTT preserves its dirty CPU-mirror marker until synchronization succeeds
- descriptor validation: D3D11 samplers are initialized with valid comparison/max-anisotropy defaults, constant/static/dynamic/SRV buffers clear stale output pointers and validate device state plus `ByteWidth` bounds before D3D calls, constant-buffer updates require the exact dynamic/constant-bind/CPU-write descriptor contract and report device-removal context on failed maps, typed float SRVs honor the feature-level 11 texel-count ceiling and grow geometrically, constant-buffer structs have compile-time HLSL size/offset checks, constant buffers are aligned, bounded, and zero-pad unwritten aligned bytes, dynamic uploads recheck capacity after growth, compact and instanced uploads use checked size arithmetic before CPU scratch growth, mesh index uploads use the validated draw index count, texture uploads validate RGBA row pitches and saturate byte counters, readback staging accepts only the RGBA8/HDR16F color formats or typeless R32 depth format that the selected path can consume, invalid target/readback classifications fall back to backbuffer-safe policy, all twelve custom parameters, clear colors, camera/fog state, object/instance/bone/shadow matrices, material scalars/UV transforms/enums, light types/positions/directions/spot cones/cascade splits, shadow bias, material UV selectors, complete clustered-light counts/offsets/indices, post-FX sample counts, tonemap mode, post-FX float parameters, and post-FX chain storage are sanitized before cbuffer upload or indexed iteration, native compressed uploads validate resident mip count, chain dimensions, block layout, payload size, previous-mip consistency, and continued-upload mip state, IBL cubemap mip overlays validate the whole layout and every payload extent before atomic destination updates, and morph-target cache reuse includes normal-delta presence so position-only payloads cannot satisfy normal-morphed draws
- depth/presentation ordering: depth-bias rasterizers account for reversed-Z scene passes versus standard-Z shadow passes and cache the convention as part of the state key; windowed swapchains leave refresh-rate selection to the desktop and disable DXGI's implicit Alt+Enter transition so window ownership cannot drift behind backend state; finite-but-overflowing or singular VP products fall back coherently with their inverse and invalidate temporal history; GPU timing queries are harvested without blocking the caller and clear stale telemetry on disjoint or failed results; soft-particle depth snapshots follow the actual RTT, scene, or direct-swapchain target; each bloom entry filters the current chain source with its own threshold; and repeated bloom entries retain authored order instead of sharing a scene-only prefilter

#### D3D11 Boundary Hardening Audit (July 2026)

The D3D11 backend's focused boundary audit closed these 20 correctness and robustness findings. The portable regressions live in `test_vgfx3d_backend_d3d11_shared`; shader-source assertions keep the CPU/HLSL contracts testable on non-Windows builders, while `g3d_test_canvas3d_point_shadows_d3d11` exercises the six-face path on a live Windows device.

1. Preserve the valid cube-shadow projection discriminator during light upload.
2. Apply the homogeneous divide to cube-face projections, matching perspective shadows and HLSL.
3. Reject unknown projection discriminators in the CPU shadow-coordinate helper.
4. Disable cube shadows unless all six consecutive face slots are complete.
5. Force cube shadows to a single selected face instead of treating malformed metadata as cascades.
6. Cap CPU-uploaded cascade counts to the four available split values.
7. Apply the same four-cascade cap defensively in both HLSL shadow paths.
8. Return a validated cube face directly from HLSL before directional cascade selection.
9. Clamp atlas comparison samples to half a texel inside their tile so PCF cannot bleed into a neighbor.
10. Share atlas row/column constants across allocation, viewports, and HLSL, with compile-time slot-capacity checks.
11. Do not advertise pending 2D streaming fallbacks as completed material maps.
12. Require the splat control map and all four layers to be completed resources before enabling splatting.
13. Do not advertise the pending white cubemap fallback as a resident reflection/IBL environment.
14. On bloom constant-buffer failure, log the device error, unbind the source SRV, restore opaque blending, and clear transient bloom state.
15. Reject cached bloom mip counts outside the fixed six-entry target arrays before iteration.
16. Bound camera coordinates at frame ingress and again for post-FX/SSR constant upload.
17. Bound the height-fog base coordinate before shader subtraction/exponent math.
18. Validate both the live element count and recorded capacity before a typed-float SRV update.
19. Reject unknown internal color-target formats instead of silently creating UNORM storage.
20. Abandon a timestamp query after 120 non-blocking busy polls so one lost query cannot disable GPU telemetry permanently.

The follow-up [Windows runtime reliability audit](../windows-runtime-reliability-audit.md) records
the D3D11 shader/resource-lifetime repairs together with the socket, wait, watcher, TLS, process,
entropy, and locale changes that share the Windows runtime boundary.

This split keeps the no-postfx path cheap while preserving correct motion/depth history for SSAO, DOF, and motion blur when the GPU postfx path is active.

### OpenGL Window Presentation Model

The OpenGL backend now follows the same high-level split, adapted to its GLX/swapchain model:

- direct mode: when GPU postfx is disabled, window-backed draws render straight into the default framebuffer and `present()` only swaps buffers
- postfx mode: the main scene renders into an HDR scene FBO, screenshots/readback can composite that scene through the backend-owned postfx shader, and 2D overlay passes preserve scene history instead of overwriting it
- overlay composition: when a screen overlay follows a GPU-postfx main scene, OpenGL first composites the postfx result to the default framebuffer through `apply_postfx`, then renders the overlay directly on top so SSAO / DOF / motion-blur history remains sourced from the 3D scene; if the chain is absent or disabled, the backend still performs a no-op scene composite instead of presenting stale backbuffer contents
- texture origin normalization: `Pixels` and `CubeMap3D` faces use a top-left origin, so OpenGL flips RGBA rows before `glTexImage2D` / cubemap face upload to match software, Metal, and D3D11 sampling
- cubemap seam filtering: OpenGL enables `GL_TEXTURE_CUBE_MAP_SEAMLESS`, while the software backend remaps bilinear taps across neighboring faces so skyboxes and reflections do not introduce backend-specific face seams; failed cubemap reuploads invalidate the stale GL texture cache entry instead of reusing older face data
- target/readback validation: OpenGL rejects invalid RTT dimensions, bounds HDR readback allocation math, sanitizes shadow indices against completed slots, and falls back to raw scene readback when postfx readback cannot allocate or apply the chain

Like D3D11, this keeps the no-postfx path cheap while preserving the scene depth/history inputs required by the advanced GPU postfx path.

Canvas finalization owns the shared ordering contract above the backends. GPU
post-FX frames use a split `apply_postfx` + final-overlay replay + `present`
sequence when the backend exposes it, so HUD geometry is drawn over the
tonemapped scene instead of being fed into bloom, SSAO, DOF, or motion blur.
Backends without that split still present through `present_postfx`, where the
overlay target is composited as a fallback. `ScreenshotFinal()` and `Flip()`
share the same post-FX-plus-overlay ordering.
`TryCopyScreenshotTo()` and `TryCopyScreenshotFinalTo()` use the same ordering
while writing into caller-owned, same-size `Pixels`. Software and render-target
paths copy directly; GPU window readback grows a canvas-owned RGBA staging
buffer only when required and reuses it on later captures. A successful write
bumps the destination `Pixels` generation so backend texture caches cannot
retain stale content.
Screen-space overlay replays submit with alpha blending/no depth writes so
coplanar HUD primitives such as panels, accent bars, and text do not hide each
other through the depth buffer.

### RenderTarget3D Readback Ownership

GPU backends now treat `RenderTarget3D` color buffers as lazily synchronized CPU mirrors instead of forcing a readback at the end of every RTT frame.

- `Canvas3D.NewOffscreen(target)` creates only the software backend context,
  retains `target`, and installs no platform window, input canvas, resize
  callback, or presentation state; normal frame, scene, overlay, post-FX, and
  screenshot paths resolve their output through that target
- a windowless canvas reports zero `WindowWidth`/`WindowHeight`; its active
  dimensions come from the bound target, which callers replace with
  `SetRenderTarget` instead of calling `Resize` or `ResetRenderTarget`
- backends mark the render target color as dirty when an RTT pass finishes
- [`rt_rendertarget3d_as_pixels()`](../../src/runtime/graphics/3d/render/rt_rendertarget3d.c) and [`rt_canvas3d_screenshot()`](../../src/runtime/graphics/3d/render/rt_canvas3d_overlay.c) call the backend-owned sync hook only when CPU pixels are actually requested
- [`rt_canvas3d_begin()`](../../src/runtime/graphics/3d/render/rt_canvas3d.c) synchronizes the camera's effective projection aspect against the active output size before `begin_frame`, so window resizes and RTT passes share the correct frustum
- while a render target is bound, Canvas3D overlay sizing, screenshots, and `Width`/`Height` queries follow the active target dimensions instead of the window dimensions
- `SetRenderTarget` validates the Canvas3D and RenderTarget3D handles and ensures color/depth backing storage before changing canvas/backend state
- `SetRenderTarget` and `ResetRenderTarget` are rejected while `Canvas3D` is inside `Begin`/`End`, so all queued draws in a frame flush to one consistent output
- `ResetRenderTarget` and `Canvas3D` teardown ask the backend to detach the active RTT binding before destroying backend state; render-target sync callbacks are cleared with the binding so later CPU readback cannot call into stale GPU context
- `RenderTarget3D.NewHdr()` keeps the GPU color attachment in `RGBA16F` on GPU backends; backend sync hooks now fill both a `Pixels`-compatible tonemapped RGBA8 mirror and a linear RGBA32F CPU mirror so CPU-supported render-target postfx can operate before final `AsPixels()` conversion
- this avoids unconditional GPU stalls on RTT-heavy frames while preserving the `RenderTarget3D.AsPixels()` contract

## Software Renderer Pipeline (vgfx3d_backend_sw.c)

```text
Per mesh:
  1. Vertex Transform    model_matrix × position → world space
                         MVP × position → clip space (4D homogeneous)
                         model_matrix × normal → world normal

  2. Per-Vertex Lighting  Blinn-Phong: ambient + Σ(diffuse + specular) per light
                          Vertex color × diffuse_color used as base albedo
                          Result: lit RGBA color per vertex (Gouraud shading)
                          SKIPPED when normal map present (per-pixel instead)

Per triangle:
  3. Frustum Clipping     Sutherland-Hodgman against 6 clip planes
                          Input: 3 vertices → Output: 0-9 vertices (convex polygon)
                          Interpolates all attributes at clip boundaries

  4. Fan Triangulation    Polygon → N-2 triangles (fan from vertex 0)

Per sub-triangle:
  5. Perspective Divide   clip.xyz / clip.w → NDC [-1,1]³

  6. Viewport Transform   NDC → screen pixels (Y-flip: NDC +Y = screen top)

  7. Rasterization        Half-space edge functions with incremental stepping
                          Bounding box → per-pixel test → barycentric interpolation

  8. Per-Fragment:
     a. Z-test            Compare interpolated depth against Z-buffer
     b. Texture sample    Per-slot sampler state, UV0/UV1 selection, texture transform,
                          and perspective-correct UV: u = (u/w) / (1/w)
     c. Terrain splat     If has_splat: sample weight map + 4 layer textures per-pixel
     d. Normal map        If normal_map: sample TBN, perturb normal per-pixel
     e. Per-pixel light   If normal_map: full Blinn-Phong per-pixel (with specular map)
     f. Shadow lookup     If shadow_active: transform to light space, 3x3 PCF depth compare
     g. Emissive map      Additive emissive texture contribution
     h. Environment map   Roughness-aware cubemap reflection (optional)
     i. Fog               Linear fog using projection-aware view distance
     j. Color compose     Gouraud × texture (default) or per-pixel lit result
     k. Pixel write       Clamp to [0,255] → write RGBA to framebuffer
     l. Z-buffer update   Store new depth value

Shadow Pass (before main pass, when shadows enabled):
  For each opaque mesh:
    Transform vertices by light view-projection (orthographic)
    Clip triangles against the homogeneous light frustum
    Rasterize normalized [0,1] depth into shadow depth buffer (1024×1024)
    No lighting, no texturing, no color — depth writes only
  Main pass samples shadows through percentage-closer filtering to soften single-texel edges.
  When `SetShadowCascades(count > 1)` is enabled on a supporting backend, Canvas3D
  dedicates contiguous shadow slots to the primary directional light, builds
  camera-depth cascade split distances, and the backend shader resolves the
  concrete shadow slot per fragment from that split metadata.
```

## Vertex Format (vgfx3d_vertex_t — 92 bytes)

```c
float pos[3];              //  0: object-space position
float normal[3];           // 12: vertex normal
float uv[2];               // 24: TEXCOORD_0
float uv1[2];              // 32: TEXCOORD_1
float color[4];            // 40: RGBA vertex color
float tangent[4];          // 56: tangent.xyz + handedness sign
uint8_t bone_indices[4];   // 72: bone palette indices
float bone_weights[4];     // 76: blend weights
```

`Mesh3D.AddVertex` initializes `uv1` from `uv` for manually authored geometry and legacy assets. VSCN accepts both the older `vgfx3d_vertex_le_v1` 84-byte layout and the current `vgfx3d_vertex_le_v2` 92-byte layout, converting old vertices with `uv1 = uv` during load.

## Skybox And Cubemap Notes

- `begin_frame` forwards the camera forward vector plus an orthographic flag so every backend can reconstruct skybox directions, view vectors, and fog distances consistently
- GPU skyboxes use a full-screen triangle path with inverse-projection and inverse-view-rotation reconstruction for perspective cameras, and a direct forward-vector sample for orthographic cameras
- Degenerate camera-forward inputs are normalized to the engine's conventional `-Z` view direction before skybox sampling; the Metal shader source test also guards the backend-side zero-vector fallback.
- cubemap caches key uploads by both a stable cubemap identity and per-face `Pixels` generations, preventing stale skyboxes or environment maps from surviving allocator address reuse
- the CPU skybox fallback keeps a tight RGBA cache keyed by cubemap generation, output size, and camera state; stable fallback frames are row blits instead of full per-pixel cubemap resampling
- D3D11 now prunes cubemap cache residency by age, matching the bounded-cache policy already used by OpenGL and Metal
- environment reflections read roughness from either the scalar material value or the metallic-roughness texture and choose cubemap mips where available; the software backend mirrors this with a roughness-dependent blur kernel

## Matrix Conventions

- **Storage:** Row-major (`m[r*4+c]`)
- **Multiply:** Right-multiply with column vectors: `result = M × v`
- **Composition:** `MVP = P × V × M` (projection × view × model)
- **NDC:** OpenGL convention, Z in [-1, 1]
- **Coordinate system:** Right-handed (+X right, +Y up, +Z toward viewer)
- **Screen space:** Y-down (top-left origin)

### GPU Backend Matrix Handling

| Backend | Matrix Upload | Winding |
|---------|--------------|---------|
| Software | Direct (row-major float) | CCW tested in clip space |
| Metal | Transpose to column-major | `MTLWindingCounterClockwise` |
| D3D11 | `row_major` HLSL qualifier | `FrontCounterClockwise = TRUE` |
| OpenGL | `glUniformMatrix4fv(..., GL_TRUE, ...)` transposes on upload | `GL_CCW` (no Y-flip) |

### Depth Range Correction

The projection matrix outputs OpenGL NDC (Z: [-1,1]). Metal and D3D11 expect Z: [0,1]. The vertex shader remaps:

```glsl
// Metal (MSL) and D3D11 (HLSL):
output.z = output.z * 0.5 + output.w * 0.5;
```

OpenGL natively supports [-1,1], so no correction is needed.

## File Structure

```text
src/runtime/graphics/
├── Core Rendering
│   ├── rt_canvas3d.h/c            Canvas3D lifecycle + vtable dispatch
│   ├── rt_canvas3d_internal.h     Internal struct definitions
│   ├── rt_graphics3d_ids.h        Stable class IDs and handle-validation helpers
│   ├── rt_mesh3d.c                Mesh3D (construction, generators, OBJ loader, Clear)
│   ├── rt_camera3d.c              Camera3D (projection, view, orbit, FPS, ray cast)
│   ├── rt_material3d.c            Material3D (legacy + PBR surface state, maps, clone/instance)
│   └── rt_light3d.c               Light3D (directional, point, ambient)
├── Scene Graph
│   ├── rt_scene3d.c/h             SceneGraph + SceneNode hierarchy, frustum culling, LOD, binding sync
│   ├── rt_transform3d.c           Transform3D (standalone TRS transform)
│   └── vgfx3d_frustum.c/h        Frustum culling math
├── Physics
│   ├── rt_physics3d.c/h           Physics3DWorld + Body3D (AABB/sphere/capsule)
│   └── rt_raycast3d.c/h           Ray3D + RayHit3D intersection tests
├── Animation
│   ├── rt_skeleton3d.c/h          Skeleton3D + Animation3D + AnimPlayer3D
│   ├── rt_animcontroller3d.c/h    AnimController3D state flow, events, root motion, masks
│   ├── rt_morphtarget3d.c         MorphTarget3D blend shapes
│   └── vgfx3d_skinning.c/h       Vertex skinning math
├── Rendering Backends
│   ├── vgfx3d_backend.h           Backend vtable interface
│   ├── vgfx3d_backend_sw.c        Software rasterizer (always available)
│   ├── vgfx3d_backend_metal.m     Metal GPU backend (macOS)
│   ├── vgfx3d_backend_d3d11.c     D3D11 GPU backend (Windows)
│   ├── vgfx3d_backend_d3d11_shared.c/h  Shared D3D11 packing/history helper layer
│   ├── vgfx3d_backend_opengl.c    OpenGL 3.3 GPU backend (Linux)
│   └── vgfx3d_backend_opengl_shared.c/h Shared OpenGL target/history helper layer
├── Effects & Advanced
│   ├── rt_particles3d.c/h         Particles3D emitter system
│   ├── rt_postfx3d.c/h            PostFX3D (bloom, FXAA, tonemap, vignette, SSAO, DOF, motion blur)
│   ├── rt_sprite3d.c/h            Sprite3D billboards (cached mesh/material)
│   ├── rt_decal3d.c/h             Decal3D surface projections
│   ├── rt_water3d.c/h             Water3D animated Gerstner wave surface
│   ├── rt_terrain3d.c/h           Terrain3D heightmap terrain (LOD, splatting)
│   ├── rt_vegetation3d.c/h        Vegetation3D procedural grass/foliage
│   ├── rt_instbatch3d.c/h         InstanceBatch3D instanced rendering
│   ├── rt_cubemap3d.c             CubeMap3D environment/skybox
│   ├── rt_rendertarget3d.c        RenderTarget3D offscreen rendering
│   └── rt_texatlas3d.c/h          TextureAtlas3D texture arrays
├── Animation (extended)
│   └── rt_anim_blend3d            AnimBlend3D weight-based animation blending
├── Physics (extended)
│   └── rt_joints3d.c/h            DistanceJoint3D + SpringJoint3D constraints
├── Navigation & Paths
│   ├── rt_navmesh3d.c/h           NavMesh3D A* pathfinding
│   ├── rt_navagent3d.c/h          NavAgent3D path following, steering, and character/node bindings
│   └── rt_path3d.c/h              Path3D spline following
├── Asset Loading
│   ├── rt_fbx_loader.c/h          FBX binary plus minimal ASCII format loader
│   ├── rt_gltf.c/h                glTF 2.0 format loader
│   └── rt_model3d.c/h             SceneAsset unified prefab/import wrapper
└── Audio bindings
    └── rt_game3d_audio.c          Game3D facade that calls audio-owned spatial APIs
```

SpatialAudio3D math and the SoundListener3D/SoundSource3D object implementation
live under `src/runtime/audio/`. Graphics3D scene code keeps only the binding
shims that pass node/camera transforms into the audio runtime.

## Asset Import Hardening

`SceneAsset.Load` is the production-facing import path. It routes `.vscn`, `.fbx`, `.gltf`, `.glb`, `.obj`, and `.stl` files into one retained asset container and treats resource-list allocation failures as hard load failures instead of returning partially populated models. OBJ imports preserve `mtllib`/`usemtl` material groups as synthesized child nodes, STL imports synthesize one default-material mesh node, and FBX imports can split geometry by `LayerElementMaterial` so polygon material assignments survive instantiation.

The glTF loader enforces the following importer contract before renderer-facing objects are created:

1. GLB input must be GLB 2.0 with a matching declared length, a first JSON chunk, aligned chunk sizes, and non-overrunning chunks.
2. `asset.version` must identify glTF 2.x.
3. Buffer, bufferView, accessor, and sparse-accessor byte ranges use checked integer arithmetic. Negative offsets, overflowing spans, invalid strides, and out-of-buffer ranges fail the view resolution.
4. External `.gltf` buffers and images are relative-only after URI decoding. `./` relative paths are accepted; absolute paths, URI schemes, `..` traversal, and NUL-containing references are rejected before opening files.
5. Valid primitives without authored materials get a shared default white PBR material so scene-graph rendering does not silently skip them.
6. Accessors used by positions, normals, UVs, colors, tangents, joints, weights, and indices must match the component/type/count contract expected by the runtime importer before any mesh is emitted.
7. Runtime skin import accepts skeletons up to 1024 bones. Meshes bound to skins larger than the 256-slot draw palette are partitioned at import into sub-meshes whose bone sets fit, each carrying a palette-slot-to-skeleton-bone map that the draw path gathers against the animator's full palette; skins above 1024 bones are rejected.
8. Node hierarchies are validated for invalid child references, duplicate parents, and cycles before a scene root is built.

glTF material import maps core metallic-roughness PBR plus selected extensions onto `Material3D`. The vertex format carries `TEXCOORD_0` and `TEXCOORD_1`; each material texture slot stores its own `textureInfo.texCoord`, `KHR_texture_transform`, wrap mode, and nearest/linear filter state. Canvas draw commands forward that per-slot metadata to software, Metal, D3D11, and OpenGL. OpenGL uses sampler objects so one uploaded texture can be reused by multiple material slots without sampler-state aliasing.

glTF animation import now covers both skeletal clips and scene-node clips. Bone-targeted transform channels still feed `Skeleton3D` / `Animation3D`; non-joint node translation, rotation, scale, and morph `weights` channels are stored as retained node animation clips and bound automatically when a `SceneAsset` is instantiated. `SceneGraph.SyncBindings(dt)` advances those node clips and applies morph weights before draw submission.

FBX import supports binary FBX plus a minimal ASCII fallback for simple geometry. The scene adapter preserves `Model` hierarchy, folds common pre/post/geometric transform properties into runtime TRS, creates default materials for unassigned meshes, and splits multi-material polygon ranges into child mesh nodes for display. Texture and external material paths are normalized to relative safe references before the loader opens companion files.

VSCN saves the current vertex layout as `vgfx3d_vertex_le_v2` and serializes material `textureSlots` alongside texture references, so saved imported scenes preserve UV-set choices, transforms, and sampler state on reload. The loader still accepts the older `vgfx3d_vertex_le_v1` vertex blob for compatibility, but rejects malformed JSON/base64, non-triangle or out-of-range index buffers, broken asset references, and partial child subtrees before returning a scene.

## Shader Architecture

All three GPU backends now share the same material contract: the legacy Blinn-Phong path remains for compatibility, and `Material3D.NewPBR` uses the same direct-light metallic/roughness PBR path across Metal, D3D11, and OpenGL.

Metal, D3D11, and OpenGL now all use small shared helper layers to keep target selection, frame-history updates, cache growth, and upload/readback policy consistent with the portable tests. Metal also now caches morph payloads by `morph_key` / `morph_revision`, applies morph normal deltas in the MSL vertex path, and keeps mipmapped texture/cubemap caches pruned by frame age.

For D3D11 specifically, the CPU and HLSL sides also share explicit packed `float4` layouts for morph weights, material custom parameters, and per-slot material UV transforms.

| Stage | Software | Metal (MSL) | D3D11 (HLSL) | OpenGL (GLSL 330) |
|-------|----------|-------------|---------------|-------------------|
| Vertex | CPU transform | `vertex_main` | `VSMain` | `main()` vertex |
| Lighting | Per-vertex or per-pixel PBR in software | Per-pixel legacy/PBR | Per-pixel legacy/PBR | Per-pixel legacy/PBR |
| Fragment | CPU rasterizer | `fragment_main` | `PSMain` | `main()` fragment |

Shaders are embedded as C string literals and compiled at runtime:
- Metal: `[device newLibraryWithSource:...]`
- D3D11: `D3DCompile(...)`
- OpenGL: `glCompileShader` + `glLinkProgram`

### Metal Fragment Shader Features

The Metal fragment shader supports the following features (MTL-01 through MTL-08):

| Feature | Texture Slot | Shader Path |
|---------|-------------|-------------|
| Diffuse texture | `[[texture(0)]]` | `baseColor *= sample` in both lit/unlit paths |
| Normal map | `[[texture(1)]]` | TBN perturbation with orthonormalized tangent |
| Specular map | `[[texture(2)]]` | Modulates `specColor` before Blinn-Phong |
| Emissive map | `[[texture(3)]]` | `emissive *= sample` added to final result |
| Spot lights | — | Smoothstep cone via `inner_cos`/`outer_cos` |
| Distance fog | — | Linear blend via `fogNear`/`fogFar` in PerScene |
| Wireframe | — | `setTriangleFillMode:MTLTriangleFillModeLines` |
| Texture cache | — | Per-frame `NSMutableDictionary` keyed by Pixels pointer |
| Skeletal skinning | `buffer(3)` | 4-bone palette in vertex shader |
| Morph targets | `buffer(4-5)` | Per-vertex delta accumulation in vertex shader |
| Shadow mapping | `[[texture(4)]]` | Depth-only pass + `sample_compare` in fragment |
| Terrain splat | `[[texture(5-9)]]` | 4-layer weight blend with per-layer UV scale |
| Instanced draw | vtable hook | N draws with shared vertex/index buffers |
| Post-processing | separate pipeline | Ordered snapshot chain: bloom, FXAA, tonemap, vignette, color grade, SSAO, DOF, motion blur |

## GC Integration

All Graphics3D objects are GC-managed via `rt_obj_new_i64`:

| Type | Finalizer | What it frees |
|------|-----------|---------------|
| Canvas3D | Yes | Backend context + optional vgfx window + retained active render target |
| Mesh3D | Yes | Vertex array + index array |
| Camera3D | No | Scalar fields only |
| Material3D | No | Scalar fields + object reference (GC-managed) |
| Light3D | No | Scalar fields only |
| Terrain3D | Yes | Height data, chunk mesh caches, splat textures |
| TextureAtlas3D | Yes | Atlas pixel buffer + cached Pixels copy |
| Vegetation3D | Yes | Instance buffers, density map, blade mesh/material |
| Sprite3D | Yes | Texture ref, cached billboard mesh/material |
| Decal3D | Yes | Texture ref, lazy mesh/material |
| Water3D | Yes | Texture refs, mesh, material |
| NavMesh3D | Yes | Baked vertex/triangle arrays |
| NavAgent3D | Yes | Path point buffer and bound nav/scene references |
| SoundListener3D / SoundSource3D | Yes | Bound node/camera/sound references and global-list links |

Temporary Vec3/Mat4 objects created for debug drawing, audio node binding, path integration, and
navigation path conversion must be released in the same call that creates them. Deferred Canvas3D
draws keep their own queued values, frame-temp object references, and per-frame snapshots of mesh
vertex/index data; helper functions should not leak local objects just because the draw command is
later consumed by the backend.

## Threading Model

Public Canvas3D, SceneGraph, and Game3D handle mutation remains main-thread-only.
No concurrent access to Canvas3D, no nested Begin/End, and no scene mutation
during Draw traversal. Internal workers may use `Zanna.Threads.Pool`,
`Zanna.Threads.Parallel`, plain copied data, and retained ordered-map results;
they must not dereference or mutate renderer-facing handles directly.
`World3D.workerCount` may enable internal throughput jobs; those jobs must merge
results in deterministic order before they affect simulation or renderer state.
`g3d_3dnext2_surface_probe` covers the ordered `Zanna.Threads.Parallel` map
surface plus `World3D.runFramesOnly` replay parity across worker counts.
`scripts/g3d_tsan_concurrency_lane.sh` configures a focused ThreadSanitizer
build for the worker pool, ordered map/reduce, general runtime concurrency,
asset-worker decode paths, Game3D worker parity, the open-world streaming hitch
probe, and the Graphics3D commit queue.

Worker paths that need to create renderer-facing resources enqueue callbacks
through the internal Graphics3D main-thread commit queue. The queue accepts
worker submissions, but only the main thread may drain and run those callbacks;
`test_rt_g3d_commit_queue` guards FIFO budgeted drains, worker-enqueue/main-
thread-commit behavior, and the negative case where a worker attempts to drain
the queue directly.

## Integration with 2D

Canvas3D coexists with the existing 2D Canvas system:
- Both use the same ZannaGFX window infrastructure
- GPU backends render via their own surface (CAMetalLayer / swap chain / GLX)
- Software backend writes to the same vgfx framebuffer as 2D Canvas
- Input handling (Keyboard, Mouse, Pad) is shared
- Canvas3D.Poll() forwards keyboard/mouse events to the input modules

## Scene Graph and Frustum Culling

`SceneGraph.SyncBindings(dt)` is the explicit integration step for node bindings. It applies body-driven transforms, node-driven kinematic pushes, and animator root motion before rendering.

Canvas3D sorts opaque deferred draws front-to-back before submission.
`Canvas3D.SetFrustumCulling(true)` additionally applies coarse AABB-vs-frustum
rejection to the deferred canvas draw queue. `SetOcclusionCulling(true)` enables
that frustum rejection plus a conservative 64 x 64 CPU coverage/depth grid.
When drawing through SceneGraph, the BVH spatial index selects the candidate draw
set before Canvas3D sorting, so CPU occlusion grid work is bounded by indexed
spatial candidates. This is not a hardware occlusion-query or Hi-Z visibility
system.

SceneGraph also owns an authored interior PVS layer. `AddVisibilityZone` registers
world-space room/sector AABBs, and `AddVisibilityPortal` registers directed
visibility links. During `SceneGraph.Draw`, the camera's containing zone seeds a
portal graph walk; drawables intersecting unreachable zones are skipped before
submission. Drawables outside all authored zones remain visible so outdoor
geometry can coexist with interior PVS graphs.
The open-world slice's `visibility_dense_probe.zia` is the named authored
city/forest fixture for this path: on the local macOS software Release lane it
reduces 169 authored drawables to 49 submitted draws, reports 120 PVS skips,
cuts the authored fill proxy by 50.407%, and verifies the optimized final frame
matches the no-PVS baseline.

`SceneGraph.Draw()` performs depth-first traversal of the scene node tree:

1. Extract VP matrix from camera, build frustum planes (Gribb-Hartmann)
2. For each visible node: recompute world matrix if dirty (lazy TRS propagation)
3. If node has a mesh: transform its object-space AABB to world space (8-corner expansion), test against frustum (p-vertex/n-vertex method). Animated or morph-capable meshes skip frustum rejection because their deformed bounds can move outside static mesh bounds. Skip static draws if fully outside.
4. Children are ALWAYS traversed even if parent mesh is culled (child transforms may place them inside the frustum independently).

When a node is bound to an `AnimController3D`, the draw path forwards the controller's blended bone palette into the deferred draw command so skinned meshes render through the scene graph without manually calling `DrawMeshAnimated`.

## Skeletal Animation Pipeline

Bone palette computation (per-frame, in `compute_bone_palette`):

1. Start with bind pose for all bones (local transforms)
2. Override with sampled animation channels (keyframes are kept sorted by time; interpolation uses SLERP for rotation and lerp for position/scale; missing keyframe components fall back to bind pose)
3. Optional crossfade: blend local transforms between outgoing and incoming animations across all bones, with source-only/target-only channels blended against bind pose
4. Two-phase global computation:
   - Phase 1: compute global transforms (`globals[i] = globals[parent] * local[i]`) — requires topological order
   - Phase 2: compute palette (`palette[i] = globals[i] * inverse_bind[i]`)
5. CPU skinning: for each vertex, `pos = sum(weight[b] * palette[b] * base_pos)`, normals renormalized

`GetBoneMatrix` returns the global transform from phase 1; renderer upload paths use the phase-2
skinning palette. Skeleton topology is frozen once a mesh/player/blender/controller binds it so
allocated pose buffers cannot be overrun by later bone additions. Deferred skinned and morphed draw
paths retain the source mesh/animator/morph objects until frame submission so queued commands never
hold stack-wrapper or freed-payload pointers.

## Particle Billboard Rendering

`Particles3D.Draw()` uses one temporary billboard build per call, then chooses submission strategy by blend mode:

1. Extract camera right/up vectors from the view matrix (rows 0 and 1)
2. For alpha blend mode: sort particles back-to-front by distance from camera (insertion sort)
3. Build vertex buffer: 4 vertices per particle (center ± right*halfSize ± up*halfSize), with per-vertex color/alpha from lifetime interpolation
4. Build index buffer: 2 triangles per quad (CCW winding)
5. Additive mode submits one batched `DrawMesh` call through a dedicated additive blend state and preserves each particle's interpolated alpha; alpha mode submits one keyed quad draw per particle so blending sorts correctly against the rest of the scene

## Post-Processing Chain

`PostFX3D` applies effects to the software framebuffer in `Canvas3D.Flip()` and exports the same
ordered effect chain for GPU `present_postfx` / readback paths:

1. Convert framebuffer RGBA8 → float RGB buffer. The scene buffer is
   scene-referred linear on every backend (ADR 0247): the conversion is a plain
   `/255` with no sRGB decode, and the tone-mapping pass performs the single
   display encode for the whole chain. GPU backends thread the same contract as
   chain state (`sceneIsHdr` means "no tonemap pass has run yet", never a pixel
   format), and Metal ping-pongs chain intermediates through `RGBA16F` with one
   BGRA8 resolve at the end of the chain.
2. Apply each enabled effect in chain order:
   - **Bloom**: bright extract (half-res) → separable Gaussian blur (N passes) → additive composite; GPU paths receive the authored pass count and use it as bloom radius
   - **Tone mapping**: Reinhard (`c/(c+1)`) or ACES filmic (Narkowicz approximation) + gamma correction
   - **FXAA**: luminance-based edge detection → 3x3 average on high-contrast pixels
   - **Color grading**: brightness/contrast/saturation adjustments
   - **Vignette**: radial distance falloff from center
   - **SSAO / DOF / motion blur / TAA / SSR**: use bounded depth and camera snapshots on the deterministic software path and bounded backend snapshots on hardware paths
   - **Auto exposure / color LUT / sun shafts**: run in the software reference path; ordered backend exports retain their effect discriminator even where a hardware pass is not implemented
3. Convert float RGB → RGBA8 back to framebuffer

### Display-transform invariant

**A tone curve consumes scene-linear input, and exactly one display encode may reach the
framebuffer.** ADR 0247 defines every backend scene target as scene-referred linear regardless of
its precision. Authored color values are interpreted in that working space; texture sampling paths
perform any required decode before lighting. Post-FX passes therefore must not infer color space
from pixel format or re-linearize chain intermediates.

Each backend carries the same chain-state contract: the source remains linear until a tonemap pass
performs the display encode. Metal threads this explicitly across its RGBA16F ping-pong, while
OpenGL and D3D11 start their post-FX chains with linear scene content. The software path converts
its linear RGBA8 storage with a plain `/255`. Any change to scene-target formats, material color
handling, or ordered post-FX passes must re-check this invariant on **every** backend; OpenGL is
`__linux__`-guarded and cannot be compiled from a macOS host, so it is the one most easily left
behind.

The CPU working representation is packed `RGBRGB...`; alpha remains in the render target and is
preserved. Frame, effect, and temporal buffers are retained by the chain and grow geometrically up
to the render-target policy ceiling. TAA snapshots the current frame before resolving so its
neighborhood clamp is independent of traversal order. The shared display-gamma table is built once
per process.

Authored and backend chains accept at most 4,096 entries. Mutable pointer/count/capacity fields are
traversal mirrors, not ownership proof: allocation metadata uses address-bound cookies, and only a
valid owner tuple may be resized or freed. By-value backend-chain copies are therefore borrowed
views. `Clear()` preserves warm allocations but releases a retained LUT and resets TAA, previous
view-projection, and auto-exposure state.

PostFX is stored per-canvas (on the `rt_canvas3d` struct), allowing independent effect chains on multiple windows.

## FPS Mouse Capture

When `Mouse.Capture()` is active, `Canvas3D.Poll()` uses a warp-to-center approach:

1. Read current platform cursor position
2. Compute delta as `(cursor_pos - window_center)`
3. Force-set mouse deltas (bypasses normal begin_frame delta computation)
4. Process keyboard/mouse button events (mouse move events skipped when captured)
5. Warp cursor back to window center (`CGWarpMouseCursorPosition` on macOS)
6. Only warps when window has focus (`isKeyWindow` check prevents affecting other apps)
