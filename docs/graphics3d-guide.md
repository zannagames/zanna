---
status: active
audience: public
last-verified: 2026-08-28
---

# Zanna.Graphics3D — User Guide

## Overview

Zanna.Graphics3D is a 3D rendering module for the Zanna runtime. It provides a software rasterizer (always available) with GPU-accelerated backends for Metal (macOS), Direct3D 11 (Windows), and OpenGL 3.3 (Linux). The GPU backend is selected automatically with software fallback.

**Namespace:** `Zanna.Graphics3D`

For code-first game projects that want a world/entity/input layer over these
primitives, use the runtime-backed `Zanna.Game3D` API documented in
[`docs/zannalib/graphics/game3d.md`](zannalib/graphics/game3d.md). Game3D is C
runtime code like the rest of the Zanna runtime, not a separate Zia helper
library.

Zanna Studio opens `.vscn` files in its built-in visual scene editor. The
viewport provides stable hierarchy multi-selection, primitive and asset import,
orbit/group-frame controls, numeric local transforms, and parent-aware Move,
Rotate, and Scale axis tools. A per-scene **Local / World** control switches
between the parent-relative axes used by local TRS fields and the absolute
world axes. The
primary row owns the visible gizmo; each selected node applies the same axis
delta around its own pivot. World-space edits ask the runtime to reproduce each
complete requested world matrix as exact parent-relative TRS. If one parent is
singular or the conversion would introduce shear, the whole group edit is
restored without a history entry. Optional snapping uses 0.5-unit, 15-degree,
and 0.1-scale steps; one completed group drag creates one undo entry and Escape
restores every origin. Move and Scale also draw XY/XZ/YZ plane squares; Scale
adds crossed diagonals so the operation does not depend on color. Dragging one
solves pointer motion against both projected axes and snaps from the immutable
primary origin. Move preserves group spacing in scene units, while one complete
handle width adds one scale unit on each Scale axis. Nearly edge-on planes are
hidden and cannot capture input. World plane transforms retain the same
exact-or-restore runtime contract and parent-before-child ordering as axis
transforms.
Rotate draws projected X/Y/Z rings in the active Local or World basis. Ring
input inverts the complete projected ellipse rather than treating angular
motion as movement along a line, and unwraps motion across the ±180-degree
seam. Nearly edge-on rings are hidden and cannot capture input. With snapping
enabled, the accumulated angle resolves to 15-degree steps; release commits the
group once and Escape restores every original transform.
Duplicate and delete operate once
per selected top-level subtree, even when a descendant is also selected. W/E/R
select Move/Rotate/Scale while the viewport or transform toolbar owns focus,
without intercepting text entry in other Studio surfaces. The primary selected
node's numeric fields become explicit relative batch controls for a group:
position and rotation values are added to every local transform while scale
values multiply each local scale, with one undo entry and neutral fields after
apply. Duplicate Selection (`Ctrl`/`Cmd`+`Shift`+`D`) and Delete are likewise
limited to hierarchy/viewport focus; inspector inputs keep their native keys.
The Parent chooser moves the selected top-level roots under Scene Root or a
valid existing node in one undoable transaction. It omits destinations inside
the moved subtrees or beyond the scene depth limit and restores the complete
selection after hierarchy preorder changes. **Keep world transform** is enabled
by default: Studio derives new local TRS through the runtime and rejects
singular or shear-producing conversions without changing the document. Clear
the option when intentionally keeping parent-relative local transforms.
The adjacent Earlier/Later controls move one contiguous same-parent selection
as a stable sibling block. They preserve its internal order, parent, local
transforms, and node-identity selection in one undoable VSCN transaction;
mixed-parent, gapped, and boundary requests are no-ops. The hierarchy itself
is a retained expandable TreeView. Dragging onto a row reparents the selected
roots; dragging into its top or bottom region combines reparenting with stable
placement before or after that row. These gestures use the same exact
preserve-world default, cycle/depth validation, one-step VSCN commit, selection
remapping, and canonical rollback as the explicit controls.
The primary selected node also exposes bounded Gameplay metadata for durable
roles, IDs, triggers, spawns, and component parameters. Null, Boolean, integer,
float, and string kinds survive VSCN exactly; create, rename, update, and remove
each use canonical one-step history. The selected nodes' Material components
can create, edit, or remove base RGB, alpha,
metallic, roughness, ambient occlusion, opaque/mask/blend, double-sided, and
unlit state. Multi-selection presents differing scalar, color, enum, and
Boolean values as Mixed. Applying resolves only concrete fields and preserves
each still-mixed value independently. Studio stages clones before assigning
nodes, so the batch is one undo step, shared imported siblings outside the
selection are unaffected, and unexposed maps/custom values survive.
The inspector also assigns or clears albedo, normal, metallic/roughness,
ambient-occlusion, and emissive maps from PNG, JPEG, BMP, GIF, or strictly
validated KTX2 files up to 16 MB; decoded raster maps are capped at 16,777,216
pixels. One chosen map can be assigned across the complete node selection and
cleared from every selected material that owns the slot. The model-import and
texture-map sections can search supported assets
from every open workspace root with bounded results and common-image previews,
while native file pickers remain available. Map replacement is clone-safe, each
accepted replace or clear creates one undo entry, and the image data is embedded
in VSCN. Standard Cut, Copy, Paste, and Select All commands follow the active
scene. A versioned clipboard envelope supports same-kind cross-document subtree
paste with unique root names, a one-unit local-X offset, restored hierarchy
selection, one-step undo, and exact rejection rollback. Studio serializes
accepted edits back to VSCN immediately, so ordinary
Save, Save As, session restore, and recovery remain authoritative.

Studio terrain uses a versioned, centered `Mesh3D` heightfield attached to an
ordinary `SceneNode`. The persisted mesh is the exact surface shown, raycast,
saved, and loaded by a game; typed `terrain.*` metadata only validates its
bounded grid interpretation for safe sculpting. This is distinct from the
standalone runtime `Terrain3D` object described below, which is not a
serializable `SceneNode` attachment. Runtime splatting, holes, chunk LOD, or
streaming therefore require an explicit game-side conversion until a future
scene attachment defines synchronization.

---

### Table of Contents

**Getting Started**
- [Quick Start](#quick-start)
- [Game3D](zannalib/graphics/game3d.md) — World/entity/input helpers for 3D games

**Core Classes**
- [Canvas3D](#canvas3d) — Window, frame loop, drawing, lighting
- [Mesh3D](#mesh3d) — Geometry: box, sphere, plane, OBJ/FBX/glTF loading
- [Camera3D](#camera3d) — Perspective and orthographic cameras
- [Material3D](#material3d) — Color, textures, shading models
- [Light3D](#light3d) — Directional, point, ambient, spot, area, and volume lights

**Rendering Infrastructure**
- [RenderTarget3D](#rendertarget3d) — Off-screen render targets
- [CubeMap3D](#cubemap3d) — Skybox and environment maps
- [PostFX3D](#postfx3d) — Post-processing effects
- [TextureAtlas3D](#textureatlas3d) — Texture atlas packing

**Scene Management**
- [SceneGraph](#scenegraph) — Scene graph with frustum culling
- [SceneNode](#scenenode) — Hierarchical scene nodes
- [Transform3D](#transform3d) — 3D transformation (position, rotation, scale)
- [SceneAsset](#sceneasset) — Unified imported asset container with instantiation

**Animation**
- [Skeleton3D, Animation3D, AnimPlayer3D](#skeleton3d) — Skeletal animation
- [AnimBlend3D](#animblend3d) — Animation blending
- [BlendTree3D](#blendtree3d) — Parametric animation blendspaces
- [IKSolver3D](#iksolver3d) — Two-bone, look-at, and FABRIK inverse kinematics
- [AnimController3D](#animcontroller3d) — Stateful animation control, events, and root motion
- [MorphTarget3D](#morphtarget3d) — Morph target (blend shape) animation

**Environment**
- [Terrain3D](#terrain3d) — Heightmap terrain with LOD and splatting
- [Water3D](#water3d) — Gerstner wave water simulation
- [Vegetation3D](#vegetation3d) — Instanced grass and foliage
- [InstanceBatch3D](#instancebatch3d) — GPU instanced rendering
- [Sprite3D](#sprite3d) — Billboard sprites in 3D space
- [Decal3D](#decal3d) — Projected decals

**Physics**
- [Physics3DWorld, PhysicsHit3D, CollisionEvent3D, Collider3D, Physics3DBody](#physics3dworld) — Rigid body physics, queries, and contacts
- [Character3D](#character3d) — Character controller
- [Trigger3D](#trigger3d) — Trigger volumes
- [DistanceJoint3D, SpringJoint3D](#distancejoint3d) — Constraints
- [Vehicle3D](#vehicle3d) — Raycast vehicle (suspension, drive, steering)

**Navigation**
- [NavMesh3D](#navmesh3d) — Navigation mesh pathfinding
- [NavAgent3D](#navagent3d) — Goal-driven agent path following and bindings
- [Path3D](#path3d) — 3D path waypoints

**Collision Queries**
- [Ray3D, RayHit3D, AABB3D, Sphere3D, Segment3D, Capsule3D](#particles3d) — Geometry queries

**Media**
- [VideoPlayer](#videoplayer) — Video playback (MJPEG/AVI, OGV)

**Format Loaders**
- [FBX](#fbx-loader) — Low-level FBX extractor API
- [GLTF](#gltf-loader) — Low-level glTF extractor API

**Operational Reference**
- [Backend Selection](#backend-selection) — GPU vs. software rendering
- [Performance Tips](#performance-tips)
- [Resource Limits](#resource-limits)
- [Error Handling](#error-handling)
- [Threading](#threading)

---

## Quick Start

### Zia

```zia
module HelloCube;

bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Graphics3D.Light3D;
bind Zanna.Math.Vec3;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("My 3D App", 640, 480);
    var cam = Camera3D.New(60.0, 640.0 / 480.0, 0.1, 100.0);
    var eye = Vec3.New(0.0, 2.0, 5.0);
    var target = Vec3.New(0.0, 0.0, 0.0);
    var up = Vec3.New(0.0, 1.0, 0.0);
    Camera3D.LookAt(cam, eye, target, up);

    var box = Mesh3D.Box(1.0, 1.0, 1.0);
    var mat = Material3D.FromColor(0.8, 0.2, 0.2);

    var light_dir = Vec3.New(-1.0, -1.0, -0.5);
    var light = Light3D.Directional(light_dir, 1.0, 1.0, 1.0);
    Canvas3D.SetLight(canvas, 0, light);
    Canvas3D.SetAmbient(canvas, 0.1, 0.1, 0.1);

    var angle = 0.0;
    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        var xform = Mat4.RotateY(angle);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawMesh(canvas, box, xform, mat);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
        angle = angle + 0.02;
    }
}
```

### BASIC

```basic
USING Zanna.Graphics3D
USING Zanna.Math

DIM canvas AS Canvas3D = Canvas3D.New("My 3D App", 640, 480)
DIM cam AS Camera3D = Camera3D.New(60.0, 640.0/480.0, 0.1, 100.0)
cam.LookAt(Vec3.New(0, 2, 5), Vec3.Zero(), Vec3.New(0, 1, 0))

DIM box AS Mesh3D = Mesh3D.Box(1.0, 1.0, 1.0)
DIM mat AS Material3D = Material3D.FromColor(0.8, 0.2, 0.2)
DIM light AS Light3D = Light3D.Directional(Vec3.New(-1,-1,-0.5), 1, 1, 1)

canvas.SetLight(0, light)
canvas.SetAmbient(0.1, 0.1, 0.1)

DIM angle AS DOUBLE = 0.0
DO WHILE NOT canvas.ShouldClose
    canvas.Poll()
    canvas.Clear(0.1, 0.1, 0.2)
    canvas.Begin(cam)
    canvas.DrawMesh(box, Mat4.RotateY(angle), mat)
    canvas.End()
    canvas.Flip()
    angle = angle + 0.02
LOOP
```

## Canvas3D

The rendering surface. It can own a platform window for an interactive render
loop or run windowless against an explicit `RenderTarget3D` for embedded tools,
thumbnails, tests, and offline previews.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `IsOffscreen` | Boolean | read | True when created windowless with `NewOffscreen` |
| `ShouldClose` | Boolean | read | True when user closes window |
| `Width` | Integer | read | Active output width in pixels (window, or current RenderTarget3D when bound) |
| `Height` | Integer | read | Active output height in pixels (window, or current RenderTarget3D when bound) |
| `WindowWidth` | Integer | read | Backing window width, ignoring any bound RenderTarget3D |
| `WindowHeight` | Integer | read | Backing window height, ignoring any bound RenderTarget3D |
| `ActiveOutputWidth` | Integer | read | Explicit alias for the active output width |
| `ActiveOutputHeight` | Integer | read | Explicit alias for the active output height |
| `Fps` | Integer | read | Frames per second |
| `DeltaTime` | Integer | read | Milliseconds since the last live `Poll()` or `Flip()`, or fixed synthetic dt when synthetic clock is selected (first live frame = 0, capped to 100ms by default) |
| `DeltaTimeSec` | Number | read | Seconds since last Flip or synthetic frame, using the same clamp as `DeltaTime` |
| `Backend` | String | read | Active renderer: "software", "metal", "d3d11", "opengl" |
| `BackendFallback` | Boolean | read | True when Canvas3D fell back from the selected GPU backend to software at creation |
| `BackendFallbackReason` | String | read | Human-readable reason for `BackendFallback`, or empty string |
| `BackendCapabilities` | Integer | read | 64-bit bitmask of `Canvas3D` backend capabilities |
| `InstancedFallbackCount` | Integer | read | Instances routed through the bounded per-instance fallback in the current/latest frame |
| `InstancedFallbackDroppedCount` | Integer | read | Instances skipped because that bounded fallback queue overflowed |
| `EventDropCount` | Integer | read | Window/input events dropped from the public `PollEvent()` ring since canvas creation |
| `MeshSnapshotBytes` | Integer | read | Deferred mesh snapshot bytes copied by the current/latest frame |
| `MeshSnapshotDropCount` | Integer | read | Mesh snapshot allocation or budget denials in the current/latest frame |
| `MeshSnapshotDroppedBytes` | Integer | read | Requested mesh snapshot bytes denied in the current/latest frame |
| `MeshSnapshotBudgetBytes` | Integer | read | Per-frame mesh snapshot byte budget |
| `QualityRequested` | Integer | read | Last requested quality profile (`0` performance, `1` balanced, `2` cinematic) |
| `QualityActive` | Integer | read | Active quality profile after backend fallback |
| `QualityFallback` | Boolean | read | True when quality setup degraded to stay backend-safe |
| `QualityFallbackReason` | String | read | Human-readable fallback reason, or empty string |
| `FrameFinalized` | Boolean | read | True after `FinalizeFrame()` or `ScreenshotFinal()` has applied post-FX/final overlays for the current frame |
| `Wireframe` | Boolean | write | Toggle wireframe rendering (default: off) |

### Constructors

| Constructor | Description |
|-------------|-------------|
| `IsAvailable()` | Return `true` when the Graphics3D Canvas runtime is compiled in |
| `New(title, w, h)` | Create canvas window (1-16384 pixels per dimension) |
| `NewFullscreen(title)` | Create a fullscreen canvas at desktop resolution without a windowed flash |
| `NewOffscreen(target)` | Create a windowless deterministic software canvas bound to a `RenderTarget3D` |

### Core Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Clear(r, g, b)` | `void(f64, f64, f64)` | Clear framebuffer and depth buffer (0.0-1.0 per channel) |
| `Resize(w, h)` | `void(i64, i64)` | Resize a windowed canvas; offscreen callers replace their explicit render target instead |
| `Begin(camera)` | `void(obj)` | Start 3D frame — must be called before DrawMesh |
| `Begin2D()` | `void()` | Start 2D overlay mode for the active output (closed by `End()`) |
| `BeginOverlay()` | `void()` | Start recording a final overlay pass composited after post-FX |
| `EndOverlay()` | `void()` | Finish final overlay recording |
| `ClearOverlay()` | `void()` | Discard recorded final overlay commands for the current frame |
| `End()` | `void()` | End the current 3D or 2D draw pass; this does not run post-FX or present |
| `FinalizeFrame()` | `void()` | Apply post-FX and replay the final overlay once, without presenting |
| `ScreenshotFinal()` | `obj()` | Finalize if needed, then capture the final frame as `Pixels` |
| `TryCopyScreenshotTo(pixels)` | `i1(obj)` | Copy the active output into a same-size reusable `Pixels`; return false on invalid size/handle or failed readback |
| `TryCopyScreenshotFinalTo(pixels)` | `i1(obj)` | Finalize if needed, then copy the final frame into a same-size reusable `Pixels` |
| `Flip()` | `void()` | Finalize if needed, present a windowed frame, and compute DeltaTime; inert for offscreen canvases |
| `Poll()` | `i64()` | Process window events and update `Keyboard`/`Mouse`/actions; returns `1` while open and `0` when closed, unavailable, or windowless |
| `PollEvent()` | `i64()` | Dequeue the next raw canvas event type, or `0` when none are pending |
| `BackendSupports(capability)` | `i1(str)` | Test a named backend capability such as `shadows`, `skybox`, `render_target`, `window_readback`, `hardware_instancing`, `postfx`, `gpu_postfx`, `postfx-overlay`, `final-screenshot`, `gpu-postfx-overlay`, `bc1`, `bc3`, `bc4`, `bc5`, or `bc7`; `runtime-fallback`, `backend-fallback`, and `software-fallback` report `BackendFallback` |

### Drawing Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `DrawMesh(mesh, transform, material)` | `void(obj, obj, obj)` | Draw a mesh with Mat4 transform and material |
| `DrawMeshSkinned(mesh, transform, material, animPlayer)` | `void(obj, obj, obj, obj)` | Draw with skeletal animation (CPU skinning) |
| `DrawMeshMorphed(mesh, transform, material, morphTarget)` | `void(obj, obj, obj, obj)` | Draw with morph target deformation |
| `DrawMeshBlended(mesh, transform, material, blender)` | `void(obj, obj, obj, obj)` | Draw with animation blend tree |
| `DrawInstanced(batch)` | `void(obj)` | Draw InstanceBatch3D (hardware instancing) |
| `DrawTerrain(terrain)` | `void(obj)` | Draw Terrain3D |
| `DrawTerrainAt(terrain, x, y, z)` | `void(obj,f64,f64,f64)` | Draw Terrain3D translated to a world-space offset (grid origin lands at `(x, y, z)`; culling/LOD track the translation) |
| `DrawDecal(decal)` | `void(obj)` | Draw Decal3D |
| `DrawSprite3D(sprite, camera)` | `void(obj, obj)` | Draw billboard Sprite3D |
| `DrawWater(water, camera)` | `void(obj, obj)` | Draw Water3D surface |
| `DrawVegetation(vegetation)` | `void(obj)` | Draw Vegetation3D |

### Lighting & Environment

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetLight(index, light)` | `void(i64, obj)` | Bind or clear a retained Light3D slot (0-15); invalid indices or non-Light3D handles trap |
| `ClearLights()` | `void()` | Clear every retained canvas light slot |
| `SetDefaultLighting()` | `void()` | Install a conservative directional key/fill plus readable ambient |
| `LightCount` | `i64` property | Count active enabled canvas-slot lights |
| `SetAmbient(r, g, b)` | `void(f64, f64, f64)` | Set ambient light color; values are clamped to `0..1` |
| `SetSkybox(cubemap)` | `void(obj)` | Set CubeMap3D skybox |
| `ClearSkybox()` | `void()` | Remove skybox |
| `SetFog(near, far, r, g, b)` | `void(f64, f64, f64, f64, f64)` | Enable linear distance fog; distances and RGB are sanitized |
| `ClearFog()` | `void()` | Disable fog |
| `EnableShadows(mapSize)` | `void(i64)` | Enable shadow mapping (mapSize = shadow map resolution; up to four directional-light shadow slots, or up to four primary-light cascades when CSM is enabled) |
| `DisableShadows()` | `void()` | Disable shadow mapping |
| `SetShadowBias(bias)` | `void(f64)` | Set shadow acne bias |

### Render Settings

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetBackfaceCull(enabled)` | `void(i1)` | Toggle backface culling (default: on) |
| `SetMaxDeltaTime(ms)` | `void(i64)` | Cap DeltaTime to prevent spiral-of-death (`ms <= 0` disables the cap; default cap is 100ms) |
| `SetQuality(profile)` | `void(i64)` | Apply a backend-safe post-FX profile: `0` performance, `1` balanced, `2` cinematic |
| `SetInputSource(mode)` | `void(i64)` | Select input source: `0` live, `1` synthetic, `2` live plus synthetic |
| `PushSyntheticKey(key, down)` | `void(i64, i1)` | Queue a synthetic keyboard transition for the next synthetic frame |
| `PushSyntheticMouse(dx, dy, buttons, wheel)` | `void(f64, f64, i64, f64)` | Queue synthetic mouse delta, button bitmask, and vertical wheel delta |
| `ClearSyntheticInput()` | `void()` | Clear queued synthetic input and release synthetic-held keys/buttons |
| `SetClockSource(mode)` | `void(i64)` | Select clock source: `0` live wall clock, `1` fixed synthetic dt |
| `SetSyntheticDeltaTimeSec(dt)` | `void(f64)` | Set fixed synthetic delta time in seconds |
| `AdvanceSyntheticFrame()` | `void()` | Advance one deterministic input/timing frame without pumping platform events |
| `SetRenderTarget(target)` | `void(obj)` | Redirect rendering to an explicit offscreen RenderTarget3D |
| `ResetRenderTarget()` | `void()` | Return a windowed canvas to window rendering; rejected for a windowless canvas because it has no fallback output |
| `SetPostFX(fx)` | `void(obj)` | Set PostFX3D chain applied during frame finalization to the window or active render target; SSAO/DOF/motion blur require GPU window postfx |
| `SetFrustumCulling(enabled)` | `void(i1)` | Toggle coarse CPU frustum rejection plus front-to-back opaque ordering |
| `SetOcclusionCulling(enabled)` | `void(i1)` | Toggle frustum rejection plus conservative CPU occlusion skips; SceneGraph feeds the grid from BVH candidates before Canvas3D sorting |

### Canvas Telemetry

| Property | Type | Description |
|----------|------|-------------|
| `DrawCount` | `i64` | Main 3D draw submissions queued by the latest ended frame |
| `OccludedDrawCount` | `i64` | Latest scene draw submissions skipped by visibility culling |
| `OcclusionCandidateCount` | `i64` | Opaque draw candidates tested by the CPU occlusion grid in the latest frame |
| `TextureUploadBytes` | `i64` | Texture bytes uploaded into backend storage during the latest ended frame |
| `TextureUploadPendingBytes` | `i64` | Texture bytes still waiting for backend texture or cubemap upload budget |

`TextureUploadBytes` reports real backend texture cache uploads/re-uploads for Metal, OpenGL, and
D3D11. Pixels-backed 2D material textures and cubemaps are row-sliced, while
native compressed `TextureAsset3D` mip blocks are submitted by resident mip, by
`Canvas3D.SetTextureUploadBudget(bytes)`; negative means unlimited, `0` pauses new upload rows, and
positive values cap per-frame upload bytes while preserving progress for sub-row budgets. Cache hits
and software/unsupported backends report `0`; non-overlay frame begin resets the counter. D3D11
validates row slices, native block rows, block layouts, and D3D11-sized upload byte fields before
issuing texture updates. If a D3D11 texture/cubemap cache table cannot grow, a valid upload may
still render through a one-draw temporary SRV that is released after the draw, but that fallback
must fit the remaining upload budget. Prefiltered IBL cubemap mips are validated as one atomic
payload before any destination mip changes; zero or exhausted budgets defer the payload without
decoding it, and a positive sub-payload budget may admit one indivisible IBL upload on an otherwise
empty frame so streaming cannot stall forever.
With a positive finite budget, an RGBA8 2D text or HUD texture no larger than 256 KiB may complete
even after larger scene uploads consume that frame's allowance. This latency exemption prevents
streaming from replacing newly rasterized labels and small interface images with a missing-texture
frame. A budget of `0` remains a strict pause, and negative/unlimited mode needs no exemption.
`TextureUploadPendingBytes` returns to `0` once all material/cubemap row slices
native compressed mip submissions, and validated IBL mip payloads drain. Use it
to correlate async asset commits and streaming movement with GPU texture upload pressure.

`Poll()` is the live-loop input boundary. It updates `Zanna.Input.Keyboard`,
`Zanna.Input.Mouse`, gamepad state, and action mappings; most gameplay code
should read those APIs instead of branching on raw event codes. `Poll()` now
returns only the open/closed status for the canvas. Low-level integrations can
call `PollEvent()` until it returns `0` to drain the queued event types collected
during the last poll.

For deterministic tests, select synthetic input and clock before the scripted
frames:

```zia
Canvas3D.SetInputSource(canvas, 1)
Canvas3D.SetClockSource(canvas, 1)
Canvas3D.SetSyntheticDeltaTimeSec(canvas, 1.0 / 60.0)
Canvas3D.PushSyntheticKey(canvas, Key.W, true)
Canvas3D.PushSyntheticMouse(canvas, 8.0, -2.0, 1, 0.0)
Canvas3D.AdvanceSyntheticFrame(canvas)
```

Synthetic keys and mouse samples flow through the normal `Keyboard` and `Mouse`
state paths, so `WasPressed`, `IsDown`, `Mouse.DeltaX`, button edges, and action
bindings observe the same state shape as live input. `ClearSyntheticInput()`
also releases keys/buttons held by the synthetic source so tests do not leak
state into the next run.

First-frame live timing can be zero because there is no previous live frame yet.
Code-first loops should seed or clamp their first `dt` before moving gameplay.
The synthetic clock reports the configured fixed dt after
`AdvanceSyntheticFrame()`, `Poll()` in synthetic mode, or `Flip()`.

`SetQuality()` currently configures the canvas post-FX chain. Performance and
Balanced use CPU/software-safe effects only. Cinematic adds SSAO, depth of field,
and motion blur only when the active canvas can present GPU post-FX to the
window; otherwise it uses a CPU-safe cinematic chain and sets
`QualityFallback`/`QualityFallbackReason` for debug overlays. Re-apply quality
after changing output mode if a game switches between a GPU window and a
render target.

Render targets must be bound or reset outside a `Begin`/`End` frame. Changing the active output
mid-frame is rejected so queued draws, overlays, post-processing, and readback state all target one
consistent surface.

### Debug Drawing

| Method | Signature | Description |
|--------|-----------|-------------|
| `DrawLine3D(from, to, color)` | `void(obj, obj, i64)` | Draw 3D line (color = 0xRRGGBB) |
| `DrawPoint3D(pos, color, size)` | `void(obj, i64, i64)` | Draw 3D point |
| `DrawAABBWire(min, max, color)` | `void(obj, obj, i64)` | Draw wireframe AABB |
| `DrawSphereWire(center, radius, color)` | `void(obj, f64, i64)` | Draw wireframe sphere |
| `DrawDebugRay(origin, dir, length, color)` | `void(obj, obj, f64, i64)` | Draw debug ray |
| `DrawAxis(transform, size)` | `void(obj, f64)` | Draw XYZ axes at a Mat4 position |
| `Screenshot()` | `obj()` | Capture the active output as `Pixels` without forcing finalization |
| `ScreenshotFinal()` | `obj()` | Finalize first, then capture post-FX plus final-overlay pixels as `Pixels` |
| `TryCopyScreenshotTo(pixels)` | `i1(obj)` | Allocation-reusing active-output capture into same-size `Pixels` |
| `TryCopyScreenshotFinalTo(pixels)` | `i1(obj)` | Allocation-reusing finalized capture into same-size `Pixels` |

Use the `TryCopy*` forms for repeated capture, streaming, or test loops. Create
the destination `Pixels` once at the active output dimensions and reuse it each
frame. A successful copy updates the destination generation so texture caches
observe the new pixels. The methods return false, without resizing or replacing
the destination, when either handle is invalid, dimensions differ, or backend
readback fails. GPU canvases also reuse canvas-owned staging storage after the
first sufficiently large readback.

### HUD Overlay (2D)

| Method | Signature | Description |
|--------|-----------|-------------|
| `DrawRect2D(x, y, w, h, color)` | `void(i64, i64, i64, i64, i64)` | Draw 2D rectangle on screen |
| `DrawText2D(x, y, text, color)` | `void(i64, i64, str, i64)` | Draw 2D text on screen |
| `DrawCrosshair(color, size)` | `void(i64, i64)` | Draw centered crosshair |

`DrawRect2D`, `DrawText2D`, and `DrawCrosshair` remain convenient immediate HUD helpers.
When called between `End()` and `Flip()`, they use the legacy overlay path and are part of
the frame before final post-processing. Use `BeginOverlay()` and `EndOverlay()` for HUD,
debug text, reticles, and capture overlays that must stay crisp after bloom, tonemapping,
or color grading.

### Zia Example

```zia
module Canvas3DDemo;

bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Graphics3D.Light3D;
bind Zanna.Math.Vec3;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("Demo", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);
    Camera3D.LookAt(cam, Vec3.New(0.0, 2.0, 5.0), Vec3.Zero(), Vec3.New(0.0, 1.0, 0.0));

    var box = Mesh3D.Box(1.0, 1.0, 1.0);
    var mat = Material3D.FromColor(0.8, 0.2, 0.2);

    var light = Light3D.Directional(Vec3.New(-1.0, -1.0, -0.5), 1.0, 1.0, 1.0);
    Canvas3D.SetLight(canvas, 0, light);
    Canvas3D.SetAmbient(canvas, 0.1, 0.1, 0.1);

    // Enable fog and shadows
    Canvas3D.SetFog(canvas, 10.0, 50.0, 0.5, 0.5, 0.6);
    Canvas3D.EnableShadows(canvas, 1024);

    var angle = 0.0;
    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawMesh(canvas, box, Mat4.RotateY(angle), mat);
        Canvas3D.End(canvas);

        // Final HUD overlay, composited after post-FX.
        Canvas3D.BeginOverlay(canvas);
        Canvas3D.DrawText2D(canvas, 10, 10, "Hello 3D!", 0xFFFFFF);
        Canvas3D.DrawCrosshair(canvas, 0xFFFFFF, 12);
        Canvas3D.EndOverlay(canvas);

        Canvas3D.Flip(canvas);
        angle = angle + 0.02;
    }
}
```

**Frame lifecycle:** `Poll → Clear → Begin → DrawMesh (repeated) → End → [BeginOverlay → HUD/debug draws → EndOverlay] → Flip`

`End()` only flushes queued geometry for the current 3D or 2D pass. `FinalizeFrame()`
is the idempotent boundary that applies post-FX and replays the final overlay. `Flip()`
calls `FinalizeFrame()` if needed, then presents. `ScreenshotFinal()` and
`TryCopyScreenshotFinalTo()` also call `FinalizeFrame()` if needed and capture
the finalized pixels without presenting, so a reusable capture path is
`EndOverlay → TryCopyScreenshotFinalTo → Flip`.

For a compact, executable example of this path, see
`examples/3d/walk_min.zia`. Its companion `walk_min_probe.zia` renders one
software-backend frame, captures with `ScreenshotFinal()`, checks the crisp final
overlay, and compares to the committed baseline in `examples/3d/baselines/`.

**Important:** `Begin`/`End` and `BeginOverlay`/`EndOverlay` must not nest. All 3D draw calls go between `Begin` and `End`; `DrawTerrain` and `DrawVegetation` are rejected during `Begin2D`. Legacy HUD overlay calls (`DrawRect2D`, `DrawText2D`, `DrawCrosshair`) may still be called between `End` and `Flip`, but final overlays should be grouped with `BeginOverlay`/`EndOverlay`. Public `DrawMesh` requires a valid heap `Mesh3D` handle and finite transform matrix; invalid matrices are rejected before they reach culling or backend submission. Draw submission clamps material colors and PBR scalars before narrowing to backend floats. Deferred heap `Mesh3D` draws retain immutable geometry revisions so submitted bytes remain stable through `Canvas3D.End()` and unchanged later frames avoid another whole-mesh copy; internal rebased, skinned, and morphed draws retain or snapshot their dynamic payloads as needed.

## Mesh3D

3D geometry with vertices and triangle indices.

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create empty mesh |
| `Box(sx, sy, sz)` | `obj(f64, f64, f64)` | Axis-aligned box (24 vertices, 12 triangles) |
| `Sphere(radius, segments)` | `obj(f64, i64)` | UV sphere (min 4 segments) |
| `Plane(sx, sz)` | `obj(f64, f64)` | XZ plane facing +Y (4 vertices, 2 triangles) |
| `Cylinder(radius, height, segments)` | `obj(f64, f64, i64)` | Cylinder with caps (min 3 segments) |
| `FromOBJ(path)` | `obj(str)` | Load Wavefront OBJ file |
| `FromSTL(path)` | `obj(str)` | Load STL file (binary or ASCII) |

`NewBox`, `NewSphere`, `NewPlane`, and `NewCylinder` remain available as
compatibility aliases for the shape factories.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `VertexCount` | Integer | read | Number of vertices |
| `TriangleCount` | Integer | read | Number of triangles |
| `Resident` | Boolean | read/write | Whether the mesh payload is resident and eligible for draw/LOD selection |
| `ResidentBytes` | Integer | read | Estimated resident vertex/index payload bytes; zero when `Resident` is false |
| `RetainedBytes` | Integer | read | Estimated retained CPU vertex/index payload bytes regardless of `Resident` |
| `SimplifyRequestedTriangles` | Integer | read | Sanitized target recorded on a mesh returned by `Simplify` (zero for ordinary or subsequently mutated meshes) |
| `SimplifyAchievedTriangles` | Integer | read | Exact triangle count achieved by `Simplify` (zero for ordinary or subsequently mutated meshes) |
| `SimplifyStatus` | Integer | read | `0` not run/stale, `1` complete, `2` valid partial result constrained by topology/boundaries |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Reserve(vertexCount, triangleCount)` | `void(i64, i64)` | Pre-size backing arrays for bulk mesh construction |
| `VertexPosition(index)` | `obj<Zanna.Math.Vec3>(i64)` | Return a fresh read-only snapshot of one mesh-local vertex position, or `null` when the index is invalid |
| `AddVertex(x, y, z, nx, ny, nz, u, v)` | `void(f64 x8)` | Add vertex with position, normal, and UV |
| `AddTriangle(i0, i1, i2)` | `void(i64, i64, i64)` | Add triangle from vertex indices (CCW winding) |
| `Clear()` | `void()` | Reset vertex/index counts to zero (reuse backing arrays) |
| `RecalcNormals()` | `void()` | Auto-compute vertex normals from face geometry |
| `CalcTangents()` | `void()` | Compute tangent vectors (required for normal mapping) |
| `Clone()` | `obj()` | Deep copy of mesh data, including attached morph targets |
| `Transform(mat4)` | `void(obj)` | Transform all vertices in-place by Mat4 |
| `BendArc(radius, arcDegrees)` | `void(f64, f64)` | Bend the mesh's X extent around a vertical circular arc in place, rotating normals and tangents with it |
| `Mesh3D.Simplify(mesh, targetTriangles)` | `obj(obj, i64)` | Return a new simplified mesh via quadric-error-metric edge collapse (static form; deterministic, manifold-, boundary-, material-, and attribute-seam-preserving; may return a valid partial result) |

`Simplify` keeps the existing object-returning API for both complete and partial
results. Inspect the three diagnostic properties when an exact budget matters.
Every surviving vertex uses subset placement: normals, tangents, UV sets,
colors, primary/extra skin influences, authoritative double positions, morph
deltas, material ranges, and retained skeleton/morph ownership remain aligned.
The simplifier stops with status `2` instead of collapsing a non-manifold or
bow-tie fan, pinching a classified boundary, or producing a duplicate,
degenerate, or inverted face. Any later geometry mutation clears these
diagnostics to status `0` so an old achieved count cannot describe new data.

`BendArc` is intended for straight modular-kit pieces such as seating, walls,
rails, and trim. The mesh must have a positive X extent, `radius` must be finite
and positive, and `arcDegrees` must be in `(0, 360]`. The mesh depth must remain
inside the bend radius. Validation completes before mutation, so an invalid
bend leaves the mesh unchanged; skinned and morph-target meshes are refused.
Pieces authored to the same width and bent with the same radius and arc share
their end sections without a gap. Rotate a piece 180 degrees around Y for the
opposite bend direction.

### Skeletal and Morph Extensions

These are available both as class methods and through their fully qualified static names.

| Method | Signature | Description |
|--------|-----------|-------------|
| `Mesh3D.SetSkeleton(mesh, skeleton)` | `void(obj, obj)` | Bind a Skeleton3D to the mesh |
| `Mesh3D.SetBoneWeights(mesh, vtx, b0, w0, b1, w1, b2, w2, b3, w3)` | `void(obj, i64, i64, f64, i64, f64, i64, f64, i64, f64)` | Set bone indices + weights for a vertex (4 bones max) |
| `Mesh3D.SetMorphTargets(mesh, morphTarget)` | `void(obj, obj)` | Bind a MorphTarget3D to the mesh |

`SetBoneWeights` drops invalid bone indices and non-positive or non-finite weights, normalizes the remaining positive weights, updates the mesh's skinned bone count, and invalidates cached geometry so renderer-side buffers refresh. `Clone()` also clones attached `MorphTarget3D` payloads so editing morph weights or deltas on the source mesh cannot mutate the clone.

`Resident` is a streaming/accounting hook: setting it to `false` keeps the
`Mesh3D` handle alive but removes its payload from resident-byte telemetry and
causes Canvas3D/SceneGraph draw paths to skip it. `SceneNode` LOD selection falls
back to the nearest resident mesh, so high-detail LODs can be demoted without
unloading the whole node or model template.

### Zia Example

```zia
module MeshDemo;

bind Zanna.Graphics3D.Mesh3D;

func start() {
    // Procedural triangle
    var mesh = Mesh3D.New();
    Mesh3D.AddVertex(mesh, -0.5, -0.5, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    Mesh3D.AddVertex(mesh,  0.5, -0.5, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    Mesh3D.AddVertex(mesh,  0.0,  0.5, 0.0, 0.0, 0.0, 1.0, 0.5, 1.0);
    Mesh3D.AddTriangle(mesh, 0, 1, 2);
    var first = mesh.VertexPosition(0);

    // Primitives
    var box = Mesh3D.Box(1.0, 1.0, 1.0);
    var sphere = Mesh3D.Sphere(0.5, 16);
    var plane = Mesh3D.Plane(10.0, 10.0);
    var cyl = Mesh3D.Cylinder(0.5, 2.0, 12);

    // File loading
    var model = Mesh3D.FromObj("assets/model.obj");

    // Compute tangents for normal mapping
    Mesh3D.CalculateTangents(model);
}
```

### Winding Order

All mesh generators and the OBJ loader produce **counter-clockwise (CCW)** winding for front faces. When constructing meshes programmatically, vertices must be ordered CCW when viewed from the front.

**Mesh validation:** Procedural generators reject non-finite and non-positive dimensions. `Box` takes full extents, while collider boxes use half-extents. `Plane` emits +Y-facing triangles, matching its vertex normals and backface-culling expectations. Sphere and cylinder segment counts are clamped to production-safe maxima to avoid accidental unbounded allocation. `Reserve()` can be called before many `AddVertex`/`AddTriangle` calls to avoid repeated reallocations; it changes capacity only, not counts or geometry revision. `AddVertex` traps on non-finite or out-of-float-range vertex data. `AddTriangle` traps on negative, out-of-range, duplicate-index, collinear, or otherwise degenerate triangles. These public append validation traps do not poison the mesh; valid later appends can continue without `Clear()`. Allocation failures and importer failures still mark the build failed until `Clear()` resets it. `RecalcNormals` accumulates in double precision before normalizing back to renderer floats. `CalcTangents` skips degenerate or overflowing face contributions instead of narrowing invalid double intermediates into renderer floats. Deferred heap draws retain an immutable geometry revision, so a source mutation after `DrawMesh` cannot change the queued bytes; unchanged later frames reuse that revision without another vertex/index copy. Camera-relative rebase and other dynamic geometry retain their explicit frame snapshots.

`VertexPosition(index)` is a bounded, read-only geometry query. It returns a
fresh mesh-local `Vec3`, preserving the authoritative double-precision position
sidecar when present, and returns `null` for negative or out-of-range indices.
Interactive tools should cap the number of vertices they scan per operation.

**Tangents:** `CalcTangents()` uses position/UV derivatives with Gram-Schmidt orthogonalization and `tangent.w` handedness for mirrored UVs. Degenerate UV islands get a normalized fallback tangent orthogonal to the vertex normal so normal maps never receive a tangent parallel to the normal. When a normal-mapped heap mesh has missing or degenerate tangents, Canvas3D generates a separate immutable tangent variant keyed by the mesh geometry revision. It is reused across frames and hardware backends without mutating the authored mesh; any position, normal, UV, or topology mutation forks a new raw/tangent revision.

**OBJ loader:** Supports v/vn/vt tuples, negative indices, inline face comments, locale-independent decimal parsing, and arbitrary n-gons through ear-clipping triangulation. The loader deduplicates identical `(position, uv, normal)` tuples so indexed assets do not balloon into one vertex per face corner. Invalid face indices trap and abort the load instead of emitting corrupt geometry. `Mesh3D.FromOBJ` is a geometry-only loader: `.mtl`, `usemtl`, `g`, and `o` directives are parsed and flattened into one mesh. Use `SceneAsset.LoadResult(".obj")` when you want `mtllib`/`usemtl` material groups preserved as separate model nodes and materials.

**STL loader:** Auto-detects binary vs ASCII format, streams exact binary STL payloads without buffering the full file, and computes normals for valid triangles.

## Camera3D

Perspective or orthographic camera with view and projection matrices.

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(fov, aspect, near, far)` | `obj(f64, f64, f64, f64)` | Create perspective camera with a vertical FOV in degrees |
| `NewHorizontalFov(fov, aspect, near, far)` | `obj(f64, f64, f64, f64)` | Create perspective camera from a horizontal FOV in degrees |
| `NewOrtho(size, aspect, near, far)` | `obj(f64, f64, f64, f64)` | Create orthographic camera (size = half-height in world units) |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Fov` | Float | read/write | Field of view in degrees (perspective only) |
| `Position` | Vec3 | read/write | Camera world position |
| `Forward` | Vec3 | read | Camera forward direction |
| `Right` | Vec3 | read | Camera right direction |
| `IsOrtho` | Boolean | read/write | Switch perspective/orthographic projection mode |
| `OrthoSize` | Float | read/write | Orthographic half-height in world units |
| `Yaw` | Float | read/write | Horizontal rotation angle (FPS mode, degrees) |
| `Pitch` | Float | read/write | Vertical rotation angle (FPS mode, degrees) |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `LookAt(eye, target, up)` | `void(obj, obj, obj)` | Point camera from eye toward target (Vec3 args) |
| `SetHorizontalFov(fov)` | `void(f64)` | Set perspective FOV from a horizontal aperture in degrees |
| `Orbit(target, distance, yaw, pitch)` | `void(obj, f64, f64, f64)` | Orbit around target (angles in degrees) |
| `ScreenToRay(sx, sy, sw, sh)` | `obj(i64, i64, i64, i64)` | Return a normalized world-space pick direction (Vec3). Perspective rays should pair it with `ScreenToRayOrigin()` or `GetPosition()`. Orthographic cameras return their forward direction. During active `Shake`, the ray matches the shaken render pose. |
| `ScreenToRayOrigin(sx, sy, sw, sh)` | `obj(i64, i64, i64, i64)` | Return the matching world-space pick origin (Vec3). Perspective cameras return the shaken render eye; orthographic cameras return the unprojected near-plane point for that screen pixel. |
| `Shake(intensity, duration, decay)` | `void(f64, f64, f64)` | Apply camera shake effect |
| `SmoothFollow(target, speed, height, distance, dt)` | `void(obj, f64, f64, f64, f64)` | Smoothly follow a Vec3 target position |
| `SmoothLookAt(target, speed, dt)` | `void(obj, f64, f64)` | Smoothly rotate toward a Vec3 target |
| `FPSInit()` | `void()` | Extract yaw/pitch from current view matrix |
| `FPSUpdate(mdx, mdy, fwd, right, up, speed, dt)` | `void(f64, f64, f64, f64, f64, f64, f64)` | FPS mouse look + WASD movement |

`Yaw`, `Pitch`, `Orbit`, and `Light3D.Spot` all use degrees. Writing `Yaw` or `Pitch` updates the camera view immediately.
Use `NewHorizontalFov` or `SetHorizontalFov` for game cameras authored with familiar horizontal FOV values; the runtime converts them to the vertical FOV stored in `Fov` using the camera aspect ratio, which avoids edge stretching from passing a horizontal value to `New`.
`Canvas3D.Begin(canvas, camera)` uses the active output's aspect ratio (window or bound `RenderTarget3D`) when building that frame's projection, so perspective remains correct across resizes and RTT passes without mutating the camera object's stored projection/aspect.
Camera constructors and control methods sanitize invalid numeric inputs at the API boundary: non-finite FOV/aspect/clip planes/orthographic sizes, degenerate `LookAt` vectors, invalid FPS deltas, and invalid shake/follow parameters fall back to finite defaults so view matrices, projection matrices, `ScreenToRay()`, and `ScreenToRayOrigin()` results remain usable. Imported camera animation can switch `IsOrtho` with step keys and animate `Fov`, aspect, clip planes, and `OrthoSize` through its attached `SceneNode`.

### Zia Example

```zia
module CameraDemo;

bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Math.Vec3;

func start() {
    var canvas = Canvas3D.New("Camera Demo", 800, 600);

    // Perspective camera
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);
    Camera3D.LookAt(cam, Vec3.New(0.0, 2.0, 5.0), Vec3.Zero(), Vec3.New(0.0, 1.0, 0.0));

    // Orbit camera around target
    Camera3D.Orbit(cam, Vec3.Zero(), 5.0, 45.0, 30.0);

    // Orthographic camera for UI/isometric
    var ortho = Camera3D.NewOrtho(10.0, 800.0 / 600.0, 0.1, 100.0);

    // Camera shake (e.g., on explosion)
    Camera3D.Shake(cam, 0.5, 0.3, 5.0);

    // Smooth follow for third-person
    var player_pos = Vec3.New(5.0, 0.0, 3.0);
    Camera3D.SmoothFollow(cam, player_pos, 4.0, 3.0, 5.0, 0.016);
}
```

**Orthographic cameras** have no perspective foreshortening. Use for isometric RPGs, strategy games, 2D-in-3D rendering, and UI overlays.

**Coordinate system:** Right-handed. +X right, +Y up, +Z toward viewer. Projection uses OpenGL NDC convention (Z: [-1,1]).

## Material3D

Surface appearance for meshes, models, decals, and other 3D drawables.

`Material3D` is now PBR-first. The default legacy Blinn-Phong path still exists for compatibility and for custom shading-model hooks, but new content should usually start with `PBR`.

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Default white material |
| `FromColor(r, g, b)` | `obj(f64, f64, f64)` | Colored material (0.0-1.0 per channel) |
| `Textured(texture)` | `obj(obj)` | Material with `Pixels` or `TextureAsset3D` texture |
| `PBR(r, g, b)` | `obj(f64, f64, f64)` | Metallic/roughness material with albedo color |

`NewColor`, `NewTextured`, and `NewPBR` remain available as compatibility
aliases for these factories.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Alpha` | Float | read/write | Opacity [0.0=invisible, 1.0=opaque]. Default 1.0. Setting alpha below 1.0 promotes `AlphaMode` from `Opaque` to `Blend`; returning to 1.0 restores `Opaque` only when that promotion was automatic. |
| `Metallic` | Float | read/write | PBR metallic factor [0.0=dielectric, 1.0=metal]. Default 0.0 |
| `Roughness` | Float | read/write | PBR roughness [0.0=smooth, 1.0=rough]. Default 0.5 |
| `AO` | Float | read/write | Ambient-occlusion multiplier. Default 1.0 |
| `EmissiveIntensity` | Float | read/write | Scalar applied to emissive color/map. Default 1.0 |
| `NormalScale` | Float | read/write | Tangent-space normal XY scale. Default 1.0 |
| `AlphaMode` | Integer | read/write | `0=Opaque`, `1=Mask`, `2=Blend` |
| `DoubleSided` | Bool | read/write | Disable backface culling when true |
| `Reflectivity` | Float | read/write | Environment reflection strength [0.0-1.0] |
| `SsrEnabled` | Bool | read/write | Opt into screen-space reflections (composited by a `PostFX3D.AddSSR` pass on GPU backends; misses keep the env-map term) |
| `Color` | Vec3 | read | Current diffuse/base color |
| `TexturePixels` | Pixels | read | Current decoded base-color/albedo map, or null |
| `NormalMapPixels` | Pixels | read | Current decoded normal map, or null |
| `SpecularMapPixels` | Pixels | read | Current decoded specular map, or null |
| `EmissiveMapPixels` | Pixels | read | Current decoded emissive map, or null |
| `MetallicRoughnessMapPixels` | Pixels | read | Current decoded packed PBR map, or null |
| `AmbientOcclusionMapPixels` | Pixels | read | Current decoded AO map, or null |
| `LightmapPixels` | Pixels | read | Current decoded baked lightmap, or null |
| `Unlit` | Bool | read | Whether lighting is ignored |
| `ShadingModel` | Integer | read | Current shading model index |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Clone()` | `obj()` | Duplicate the material state |
| `MakeInstance()` | `obj()` | Duplicate the material for per-object overrides |
| `SetColor(r, g, b)` | `void(f64, f64, f64)` | Change diffuse color |
| `SetTexture(texture)` | `void(obj)` | Set/change texture (`Pixels`, `TextureAsset3D`, or `RenderTarget3D`) |
| `SetAlbedoMap(texture)` | `void(obj)` | Set/change the PBR albedo map |
| `SetShininess(s)` | `void(f64)` | Specular exponent (default 32.0, higher = sharper highlights) |
| `SetUnlit(flag)` | `void(i1)` | Skip lighting (render flat color) |
| `SetMetallic(value)` | `void(f64)` | Set the metallic factor |
| `SetRoughness(value)` | `void(f64)` | Set the roughness factor |
| `SetAO(value)` | `void(f64)` | Set the AO multiplier |
| `SetEmissiveIntensity(value)` | `void(f64)` | Scale emissive output |
| `SetNormalMap(texture)` | `void(obj)` | Set tangent-space normal map (`Pixels` or `TextureAsset3D`) |
| `SetMetallicRoughnessMap(texture)` | `void(obj)` | Set the glTF-style metallic/roughness map (`G=roughness`, `B=metallic`) |
| `SetAOMap(texture)` | `void(obj)` | Set the ambient-occlusion map (`R=occlusion`) |
| `SetSpecularMap(texture)` | `void(obj)` | Set specular intensity map (`Pixels` or `TextureAsset3D`) |
| `SetEmissiveMap(texture)` | `void(obj)` | Set emissive color map (`Pixels` or `TextureAsset3D`) |
| `SetEmissiveColor(r, g, b)` | `void(f64, f64, f64)` | Set emissive color multiplier (additive glow) |
| `SetNormalScale(value)` | `void(f64)` | Scale tangent-space normal-map strength |
| `SetShadingModel(model)` | `void(i64)` | Set shading model (see table below) |
| `SetCustomParam(index, value)` | `void(i64, f64)` | Set custom shader parameter (index 0-11) |
| `SetEnvMap(cubemap)` | `void(obj)` | Set environment CubeMap3D for reflections |

### Workflow Notes

- `PBR` and `SetShadingModel(2)` select the metallic/roughness workflow directly.
- Calling `SetMetallic`, `SetRoughness`, `SetAO`, `SetMetallicRoughnessMap`, or `SetAOMap` on a legacy material promotes it into the PBR workflow.
- `Clone()` and `MakeInstance()` both return independent material objects. They eagerly copy scalar state and share the currently referenced texture/cubemap objects by pointer. After cloning, either material can replace its maps independently.
- Color and scalar setters sanitize input at the runtime boundary: colors and PBR factors are clamped to valid ranges, non-finite custom parameters become `0`, and non-finite shadow/fog/material values fall back to deterministic safe defaults. The draw path repeats finite/clamp validation before backend command submission.
- `Textured` and texture map setters accept `Pixels` handles or `TextureAsset3D` handles with either an RGBA8 fallback or retained native compressed mip blocks. Compressed-only assets render on backends that advertise the matching `bc7`, `astc`, or `etc2` capability and otherwise behave as an unbound texture until a fallback-capable mip is resident. `SetEnvMap` accepts `CubeMap3D` handles only; invalid cubemap handles are ignored rather than retained.
- The read-only `*Pixels` map properties resolve the current drawable decoded
  view for inspectors and previews without replacing the original
  `TextureAsset3D`/`RenderTarget3D` source or changing VSCN provenance.
- `AlphaMode` changes how texture alpha is interpreted for PBR materials:
  - `0`: opaque. Texture/material alpha does not enable blending, and surviving fragments write depth as opaque.
  - `1`: masked. Fragments below the cutoff are discarded; surviving fragments render as opaque coverage. Masked materials also cast alpha-tested shadows on the software, Metal, OpenGL, and D3D11 backends.
  - `2`: blended. Texture/material alpha participates in transparency and transparent sorting.
- Explicit `SetAlphaMode` calls take precedence over alpha auto-promotion. For example, a material
  explicitly set to `Blend` remains blended even if `Alpha` is later set back to `1.0`.
- When `AlphaMode` is left opaque, decoded textures with binary alpha auto-route to `Mask`, while
  decoded textures with fractional alpha auto-route to `Blend` so soft edges are preserved.
- `SetShadingModel` and `SetCustomParam` remain available as advanced escape hatches. They are not the main PBR API.
- All twelve custom parameters (`0..11`) are forwarded to every backend. Imported glTF material extensions use parameters through index 9 (including anisotropy strength/rotation at 8/9), so applications that override imported materials should preserve extension-owned slots they still need.

**Shading models:** `SetShadingModel` selects how the surface is shaded on the legacy path and can post-process the PBR result:
- **0 (BlinnPhong)**: Default. Diffuse + specular highlight.
- **1 (Toon)**: Quantized diffuse bands. `custom[0]` = number of bands (default 4).
- **2 (PBR)**: Selects the metallic/roughness workflow. Use the dedicated PBR setters for material data.
- **3 (Unlit)**: Same visual result as `SetUnlit(true)`.
- **4 (Fresnel)**: Angle-dependent alpha — edges glow brighter. `custom[0]` = power (default 3), `custom[1]` = bias.
- **5 (Emissive)**: Boosted emissive glow. `custom[0]` = strength multiplier (default 2).

See `examples/apiaudit/graphics3d/shading_demo.zia` for the legacy/custom-model path and `examples/apiaudit/graphics3d/material3d_pbr_demo.zia` / `examples/apiaudit/graphics3d/material3d_pbr_demo.bas` for the PBR workflow.

**Ownership:** `Material3D` retains `Pixels`, `TextureAsset3D`, and `CubeMap3D` references internally. When a `TextureAsset3D` is accepted, the material keeps the asset handle and resolves the currently resident RGBA8 fallback or native compressed mip blocks when drawing, so later `SetResidentMipRange` calls change the texture used by already-bound materials.

## TextureAsset3D

KTX2 texture asset metadata and material binding bridge.

| Constructor / Method | Signature | Description |
|----------------------|-----------|-------------|
| `LoadKTX2(path)` | `obj(str)` | Permissively load a KTX2 file; recognized but unsupported block modes may use a degraded checker fallback |
| `LoadKTX2Asset(assetPath)` | `obj(str)` | Permissively load KTX2 through the asset manager |
| `LoadKTX2Strict(path)` | `obj(str)` | Load from the filesystem and reject any input that would require checker substitution |
| `LoadKTX2AssetStrict(assetPath)` | `obj(str)` | Strictly load KTX2 through the asset manager |
| `SetResidentMipRange(firstMip, mipCount)` | `void(i64,i64)` | Request/count the resident mip-level range; count clamps to available mips |

| Property | Type | Description |
|----------|------|-------------|
| `Width` | Integer | Pixel width |
| `Height` | Integer | Pixel height |
| `MipCount` | Integer | Declared mip level count |
| `Format` | String | `rgba8`, `bc1`, `bc3`, `bc4`, `bc5`, `bc7`, `astc`, `etc2`, or `unknown` |
| `Compressed` | Bool | True for native block-compressed mip payloads |
| `Degraded` | Bool | True when the permissive loader substituted a checker for a recognized decode failure |
| `DegradedReason` | String | Stable decoder/format reason for degradation, or empty when complete |
| `ResidentMipStart` | Integer | First resident/requested mip level |
| `ResidentMipCount` | Integer | Number of resident/requested mip levels |
| `ResidentBytes` | Integer | Active native/fallback bytes for the resident mip window |
| `RetainedBytes` | Integer | Exact imported source container, canonical mip backing, and decoded fallbacks still retained in memory |

The runtime validates KTX2 headers, exact block geometry, mip ranges, ZLIB
framing/Adler-32, Zstandard destination length and trailing input, and
BasisLZ/UASTC payload bounds before publication. One table is authoritative for
CPU fallback quality (`none`, `partial`, or `full`), native backend capability,
block dimensions, and decoder selection. RGBA8, BC1/3/4/5/7 have full CPU
fallbacks; BC6H, ETC2, and ASTC intentionally report partial coverage because
their in-tree decoders accept only the implemented block modes. Permissive loads
retain native blocks and expose a checker plus `DegradedReason` when a recognized
mode cannot be decoded; strict loads return null for that same input.

`SetResidentMipRange` reconstructs every entering fallback from immutable
canonical backing before publishing the new window, then releases decoded
`Pixels` outside it. `ResidentBytes` therefore falls on eviction while a later
range change reproduces the same pixels; `RetainedBytes` includes that
reconstructable backing and, for PNG, JPEG, GIF, BMP, or KTX2 imports, the exact
bounded source container retained for lossless VSCN v5 bake. Negative arguments trap, `mipCount` clamps to the
available range, and a zero count releases all decoded resident fallbacks.
`Material3D.Textured`, `SetTexture`,
`SetAlbedoMap`, `SetNormalMap`, `SetMetallicRoughnessMap`, `SetAOMap`,
`SetSpecularMap`, and `SetEmissiveMap` accept texture assets directly when they
have an RGBA8 fallback or native compressed mip blocks. BC1/BC3/BC4/BC5/BC7/ASTC/ETC2
assets expose metadata, resident/retained byte counts, native mip payloads,
and software fallbacks for supported compressed blocks. Native mip block payloads upload through capable
GPU backends under `Canvas3D.SetTextureUploadBudget`;
`BackendSupports("native-texture:bc1"|"native-texture:bc3"|"native-texture:bc4"|"native-texture:bc5"|"native-texture:bc7"|"native-texture:astc"|"native-texture:etc2")`
advertises the device-specific native paths. Bare compressed-format names remain
accepted as legacy native-upload aliases, while `texture:*` names report CPU
fallback coverage. The open-world native-compressed hitch CTest records raw RGBA bytes,
compressed resident/upload bytes, RAM/VRAM reduction percentages, and a
final-frame tolerance check for the selected capable backend.

### Zia Example

```zia
module MaterialDemo;

bind Zanna.Graphics3D.Material3D;

func start() {
    var base = Material3D.PBR(0.8, 0.6, 0.4);
    Material3D.set_Metallic(base, 0.7);
    Material3D.set_Roughness(base, 0.3);
    Material3D.set_AmbientOcclusion(base, 0.9);
    Material3D.set_EmissiveIntensity(base, 1.4);
    Material3D.set_AlphaMode(base, 2); // blend
    Material3D.set_DoubleSided(base, 1);

    var inst = Material3D.MakeInstance(base);
    Material3D.set_Roughness(inst, 0.75);

    var legacy = Material3D.FromColor(0.4, 0.6, 1.0);
    Material3D.SetShadingModel(legacy, 1);     // Toon
    Material3D.SetCustomParam(legacy, 0, 3.0); // 3 bands
}
```

## Light3D

Light sources for the scene. The software backend applies up to 16 lights simultaneously; GPU backends (Metal, Direct3D 11, OpenGL) apply up to 64 via clustered forward+ lighting (see below).

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Directional(direction, r, g, b)` | `obj(obj, f64, f64, f64)` | Sun-like light from a direction (Vec3) |
| `Point(position, r, g, b, attenuation)` | `obj(obj, f64, f64, f64, f64)` | Local point light with distance falloff |
| `Spot(pos, dir, r, g, b, atten, inner, outer)` | `obj(obj, obj, f64, f64, f64, f64, f64, f64)` | Spot light with cone falloff (angles in degrees) |
| `Ambient(r, g, b)` | `obj(f64, f64, f64)` | Uniform ambient light |
| `AreaRectangle(pos, dir, width, height, r, g, b, atten, range)` | `obj(obj, obj, f64, f64, f64, f64, f64, f64, f64)` | One-sided oriented rectangle emitter |
| `AreaSphere(pos, radius, r, g, b, range)` | `obj(obj, f64, f64, f64, f64, f64)` | Omnidirectional spherical area emitter |
| `Volume(pos, radius, r, g, b, range)` | `obj(obj, f64, f64, f64, f64, f64)` | Isotropic volume emitter fading inside its radius |

`NewDirectional`, `NewPoint`, `NewSpot`, and `NewAmbient` remain available as
compatibility aliases.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetIntensity(value)` | `void(f64)` | Brightness multiplier (default 1.0) |
| `SetColor(r, g, b)` | `void(f64, f64, f64)` | Change light color |
| `SetAttenuation(value)` | `void(f64)` | Set point/spot distance attenuation |
| `SetSpotCone(innerDegrees, outerDegrees)` | `void(f64, f64)` | Atomically sanitize and replace both spot-cone angles |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Type` | Integer | read | `0=directional`, `1=point`, `2=ambient`, `3=spot`, `4=rectangle`, `5=sphere`, `6=volume` |
| `Color` | Vec3 | read | Current normalized RGB color |
| `Intensity` | Float | read | Current brightness multiplier |
| `IsEnabled` | Bool | read/write | Disabled lights are skipped by `Canvas3D` light submission |
| `CastsShadows` | Bool | read/write | Opt the light into its supported shadow path |
| `Attenuation` | Float | read | Current point/spot attenuation |
| `Direction` | Vec3 | read/write | Normalized direction and area-emitter normal |
| `Position` | Vec3 | read/write | World/local position for finite lights |
| `Width` | Float | read/write | Rectangle emitter width |
| `Height` | Float | read/write | Rectangle emitter height |
| `Radius` | Float | read/write | Sphere/volume radius |
| `DecayType` | Integer | read/write | FBX-compatible `0=none`, `1=linear`, `2=quadratic`, `3=cubic` falloff |
| `Range` | Float | read/write | Finite local-light range (`0` means constructor/importer default behavior) |
| `InnerConeDegrees` | Float | read | Sanitized inner spot-cone angle in degrees; zero for non-spot lights |
| `OuterConeDegrees` | Float | read | Sanitized outer spot-cone angle in degrees; zero for non-spot lights |

Light colors are clamped to `[0, 1]`, intensities and dimensions are clamped to
finite non-negative ranges, and point/spot attenuation uses a small non-zero
floor when callers pass zero, negative, or non-finite values. Non-finite
positions/directions fall back to finite defaults. Spot cone angles are clamped
to `0..89` degrees and reordered with at least `0.01` degree separation.
`SetSpotCone` sanitizes the pair together and advances the light mutation
generation once, so inspectors can retune a cone without publishing an
intermediate shape.
Rectangle and sphere shadows use a center-point approximation; volume lights do
not claim a shadow slot. Software, OpenGL, Metal, and D3D11 share the same native
area/volume light parameter contract.

### Zia Example

```zia
module LightDemo;

bind Zanna.Graphics3D.Light3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Math.Vec3;

func start() {
    var canvas = Canvas3D.New("Lights", 800, 600);

    // Directional sunlight
    var sun = Light3D.Directional(Vec3.New(-1.0, -1.0, -0.5), 1.0, 0.95, 0.8);

    // Point light at position with falloff
    var torch = Light3D.Point(Vec3.New(3.0, 2.0, 0.0), 1.0, 0.6, 0.2, 10.0);
    Light3D.SetIntensity(torch, 2.0);

    // Spot light (flashlight)
    var spot = Light3D.Spot(
        Vec3.New(0.0, 5.0, 0.0), Vec3.New(0.0, -1.0, 0.0),
        1.0, 1.0, 1.0, 15.0, 20.0, 35.0);

    // Ambient fill
    var ambient = Light3D.Ambient(0.1, 0.1, 0.15);

    // Bind lights to canvas
    Canvas3D.SetLight(canvas, 0, sun);
    Canvas3D.SetLight(canvas, 1, torch);
    Canvas3D.SetLight(canvas, 2, spot);
    Canvas3D.SetLight(canvas, 3, ambient);
    Canvas3D.SetAmbient(canvas, 0.05, 0.05, 0.05);
}
```

`Canvas3D.SetLight()` retains the assigned light until you replace that slot or clear it with `null`, and traps before mutation for invalid slot indices or non-`Light3D` objects.
Spot-light inner and outer cone angles are sanitized to remain finite and strictly separated before
their cosines are sent to the software and GPU backends, avoiding undefined falloff at equal cones.

For scene-owned lighting, assign a light through the typed read/write
`SceneNode.Light` property. The node retains the light, `null` clears it, and a
non-`Light3D` handle is rejected without changing the current component.
`SceneGraph.Draw` transforms a node-attached light's `Position` and `Direction`
from light-local space through the complete node hierarchy. Direct
`Canvas3D.SetLight` slots instead consume those fields in world space.

### Clustered forward+ lighting

GPU backends cull point and spot lights against a 16×9×24 view-space froxel grid each frame (CPU binning, deterministic), so each pixel only evaluates the lights that can actually reach it. Directional and ambient lights always apply globally. This raises the active light budget from 16 to 64 on GPU backends and keeps many-light scenes fast; the software backend keeps the flat 16-light path.

- Clustering is on by default wherever `canvas.BackendSupports("clustered-lighting")` is true. Toggle it with the strict `ClusteredLighting` property when lack of support should trap, or call `canvas.TrySetClusteredLighting(true)` to opt in and receive `false` on unsupported or disabled builds.
- Per-cluster light lists are capped at 8192 total entries per frame; pathological scenes (many unbounded point lights covering the whole view) truncate later lights per cluster deterministically rather than failing.
- Point/spot lights with zero attenuation are clamped to a small default falloff floor so clustered culling can still bound their influence.
- D3D11 verifies the light revision, global/binned counts, depth range, every prefix offset, and every referenced light index before upload. A malformed or stale table uses the bounded flat-light loop, and HLSL clamps packed-buffer reads as a second line of defense.
- The environment variable `ZANNA_3D_CLUSTERS=0` disables clustering process-wide (bisection escape hatch).

**Lighting model:** Blinn-Phong with per-vertex (software) or per-pixel (GPU) shading. Includes diffuse and specular components.

## RenderTarget3D

Offscreen rendering targets for render-to-texture effects (TV screens, mirrors, security cameras, post-processing).

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(width, height)` | `obj(i64, i64)` | Create offscreen target (1-16384 pixels per dimension) |
| `NewHdr(width, height)` | `obj(i64, i64)` | Create HDR offscreen target with RGBA16F internal color storage |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `IsHdr` | Bool | read | True when the target stores HDR color internally |
| `Width` | Integer | read | Target width |
| `Height` | Integer | read | Target height |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AsPixels()` | `obj()` | Read back color buffer as a new Pixels object |

### Zia Example

```zia
module RenderTargetDemo;

bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.RenderTarget3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("RTT Demo", 800, 600);
    var cam = Camera3D.New(60.0, 1.0, 0.1, 100.0);
    var target = RenderTarget3D.New(256, 256);
    var screen_mat = Material3D.New();
    var screen_mesh = Mesh3D.Plane(2.0, 2.0);

    // Render scene to offscreen target
    Canvas3D.SetRenderTarget(canvas, target);
    Canvas3D.Clear(canvas, 0.2, 0.0, 0.0);
    Canvas3D.Begin(canvas, cam);
    // ... draw objects to target ...
    Canvas3D.End(canvas);

    // Use result as texture on a quad
    Material3D.SetTexture(screen_mat, RenderTarget3D.AsPixels(target));
    Canvas3D.ResetRenderTarget(canvas);

    // Draw main scene with the textured quad
    Canvas3D.Begin(canvas, cam);
    Canvas3D.DrawMesh(canvas, screen_mesh, Mat4.Identity(), screen_mat);
    Canvas3D.End(canvas);
    Canvas3D.Flip(canvas);
}
```

### Windowless rendering

Use `Canvas3D.NewOffscreen(target)` when rendering should remain inside an
existing application window or no display server is available:

```zia
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.RenderTarget3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.SceneGraph;

var target = RenderTarget3D.New(640, 360);
var canvas = Canvas3D.NewOffscreen(target);
var camera = Camera3D.NewOrtho(10.0, 640.0 / 360.0, 0.1, 1000.0);
var scene = SceneGraph.New();

canvas.Clear(0.08, 0.10, 0.14);
scene.Draw(canvas, camera);
var pixels = target.AsPixels();
```

The constructor retains the target, always selects the portable software
backend, and creates no platform window or input state. `Width`/`Height` report
the active target while `WindowWidth`/`WindowHeight` are zero. Replace the
target with `SetRenderTarget` to resize; `Resize`, `ResetRenderTarget`,
`SetRenderTarget(null)`, live `Poll`, and presentation do not have windowless
equivalents. `Screenshot()` and `ScreenshotFinal()` read the same target when a
separate `AsPixels()` handle is not convenient.

**Note:** `AsPixels()` returns a fresh copy each call. The render target's CPU-side color/depth buffers are allocated lazily on first CPU access (or when the software backend binds the target), so GPU-only RTT passes do not pay the host-memory cost up front.
HDR targets created with `NewHdr()` keep their GPU color attachment in `RGBA16F`, but `AsPixels()` still returns standard `Pixels`. GPU readback keeps both a range-compressed RGBA8 mirror and a linear RGBA32F CPU mirror; render-target postfx consumes the linear HDR mirror for Bloom, Tonemap, FXAA, ColorGrade, and Vignette before final 8-bit conversion so highlights are not clamped before the chain runs.
**Sampling a target as a texture (ADR 0299 / 0301):** when a `RenderTarget3D` is bound to a material slot or passed to `DrawImage2D`, the sampled image is *display-referred* on every backend — the canvas's post-FX chain (tone curve, exposure, gamma, grade, LUT, FXAA, sharpen; not the window-sized or temporal passes, and not vignette) is applied to the target at its own size at the end of each frame rendered into it. Metal samples a native per-target display image; the other backends resolve the target's material mirror once per completed frame. `AsPixels()`, `CopyTo()` and `TryReadRgba()` stay scene-referred. A target rendered by a canvas with no chain keeps its raw scene colour.
When a render target is bound, `Canvas3D.Width`, `Canvas3D.Height`, `ActiveOutputWidth`, `ActiveOutputHeight`, `Begin2D()`, debug overlays, and `Screenshot()` all operate in that target's pixel space instead of the window's. Use `WindowWidth` / `WindowHeight` when gameplay or HUD layout must track the backing window regardless of the active render target.
`Canvas3D.Begin()` also uses the target's aspect ratio for that frame's projection while the render target is bound, so switching between the window and RTT views does not stretch perspective or rewrite the camera's stored projection.
`Canvas3D.SetRenderTarget()` accepts `RenderTarget3D` handles only and prepares the target's color/depth storage before handing it to the active backend, so a successful bind has a valid CPU mirror for software rendering and later readback.
**PostFX:** If a render target is active when you call `Flip()`, the canvas applies the current CPU-supported `PostFX3D` chain to that render target instead of the window backbuffer. SSAO, DOF, and motion blur require GPU window postfx because they need scene depth/history/velocity buffers; on a render target or software CPU path they trap with a clear error instead of silently no-oping.

## CubeMap3D

Six-face cube texture for skyboxes and environment reflections.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(right, left, top, bottom, front, back)` | `obj(obj, obj, obj, obj, obj, obj)` | Create cubemap from 6 Pixels faces |

CubeMap3D has no methods or properties — it is a data object used by `Canvas3D.SetSkybox` and `Material3D.SetEnvMap`.
CubeMap faces use the same top-left pixel origin as `Pixels`; the GPU backends normalize upload orientation so skyboxes and reflections sample consistently across backends.
Skyboxes honor the active camera projection across all backends. Perspective cameras reconstruct a per-pixel view ray; orthographic cameras sample the cubemap along the camera forward direction so the background stays stable instead of being distorted by perspective-only math. Degenerate camera-forward vectors fall back to the engine's conventional `-Z` direction before sampling; Metal also carries a shader-side safe-normalize fallback for this case. When a backend lacks a native skybox hook, the CPU fallback caches the generated RGBA skybox by cubemap generation, output size, and camera projection, then blits the cached image on stable frames instead of resampling the cubemap every frame.
Environment reflections are roughness-aware. Low-roughness materials keep a sharp cubemap reflection, while higher roughness values sample blurrier cubemap mips on GPU backends and a matching blur kernel in the software renderer.
CubeMap uploads and CPU fallback skybox caches are keyed by a stable internal cubemap identity plus the six face `Pixels` generations, so recreating or mutating cubemaps cannot accidentally reuse stale GPU skybox, CPU fallback, or reflection textures after allocator address reuse.
Seam handling is also more consistent now: the software sampler remaps bilinear taps across neighboring faces and OpenGL enables seamless cubemap filtering, which reduces visible face-edge seams when the artwork itself lines up.

### Zia Example

```zia
module CubeMapDemo;

bind Zanna.Graphics3D.CubeMap3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Graphics.Pixels;

func start() {
    var canvas = Canvas3D.New("Skybox Demo", 800, 600);

    // Load 6 face textures
    var right = Pixels.Load("skybox_right.png");
    var left  = Pixels.Load("skybox_left.png");
    var top   = Pixels.Load("skybox_top.png");
    var bot   = Pixels.Load("skybox_bottom.png");
    var front = Pixels.Load("skybox_front.png");
    var back  = Pixels.Load("skybox_back.png");

    var skybox = CubeMap3D.New(right, left, top, bot, front, back);
    Canvas3D.SetSkybox(canvas, skybox);

    // Use same cubemap for material environment reflections
    var chrome = Material3D.FromColor(0.9, 0.9, 0.9);
    Material3D.SetEnvMap(chrome, skybox);
    Material3D.set_Reflectivity(chrome, 0.8);
}
```

---

## SceneGraph

Hierarchical scene graph with frustum culling and LOD support.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create scene with root node |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Root` | SceneNode | read | Root node of the scene tree |
| `NodeCount` | Integer | read | Total nodes in tree |
| `CulledCount` | Integer | read | Nodes culled in last Draw |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(node)` | `void(obj)` | Add node to root |
| `TryAdd(node)` | `i1(obj)` | Add node to root and return whether the scene accepted it |
| `Remove(node)` | `void(obj)` | Detach node from parent |
| `Find(name)` | `obj(str)` | Recursive depth-first name search |
| `Draw(canvas, cam)` | `void(obj, obj)` | Traverse + render (with indexed frustum culling) |
| `QueryAABB(min, max)` | `obj(obj, obj)` | Return visible mesh nodes whose world AABB intersects the query box |
| `QuerySphere(center, radius)` | `obj(obj, f64)` | Return visible mesh nodes whose world AABB intersects the query sphere |
| `RaycastNodes(origin, dir, maxDist)` | `obj(obj, obj, f64)` | Return the closest visible mesh node hit by a ray |
| `Clear()` | `void()` | Remove all children from root |
| `Save(path)` | `i64(str)` | Write a VSCN scene snapshot (returns 1 on success) |
| `Load(path)` | `obj(str)` | Load and validate a VSCN scene, or return null on failure |
| `SyncBindings(dt)` | `void(f64)` | Apply scene-node body / animator bindings before draw |

---

## SceneNode

Individual node in a SceneGraph tree with transform, mesh, material, light,
camera, and child hierarchy.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create empty scene node |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Position` | Vec3 | read | Local position |
| `Rotation` | Quat | read/write | Local rotation |
| `Scale` | Vec3 | read | Local scale |
| `WorldMatrix` | Mat4 | read | Computed world transform (lazy) |
| `WorldPosition` | Vec3 | read | World-space position without manual matrix decomposition |
| `WorldRotation` | Quat | read | World-space rotation without manual matrix decomposition |
| `WorldScale` | Vec3 | read | World-space scale magnitudes without manual matrix decomposition |
| `ChildCount` | Integer | read | Number of child nodes |
| `Parent` | SceneNode | read | Parent node (null if root) |
| `Visible` | Boolean | read/write | Visibility (hides node + all descendants) |
| `Name` | String | read/write | Name for Find() lookup; native reads return an owned runtime-string reference |
| `Mesh` | Mesh3D | read/write | Mesh to render |
| `Material` | Material3D | read/write | Material for rendering |
| `Light` | Light3D | read/write | Node-local light component; assign `null` to clear |
| `Camera` | Camera3D | read/write | Camera attached to this node; imported camera animation updates this exact object |
| `BoundsMin` | Vec3 | read | Subtree axis-aligned bounding box minimum in this node's local space |
| `BoundsMax` | Vec3 | read | Subtree axis-aligned bounding box maximum in this node's local space |
| `Body` | Physics3DBody | read | Bound body used by `SyncBindings` |
| `Animator` | AnimController3D | read | Bound controller used for root motion and skinned draw submission |
| `SyncMode` | Integer | read/write | Transform sync policy used by `SceneGraph.SyncBindings` |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Set local position |
| `SetScale(x, y, z)` | `void(f64, f64, f64)` | Set local scale |
| `SetTransform(px,py,pz, qx,qy,qz,qw, sx,sy,sz)` | `void(f64 ×10)` | Set position, rotation (quaternion components), and scale in one call — the allocation-free hot-loop form |
| `TrySetWorldMatrix(worldMatrix)` | `i1(obj<Zanna.Math.Mat4>)` | Assign a complete world matrix only when it has an exact finite parent-relative TRS representation; reject invalid handles, singular parents, projective/degenerate matrices, shear, or decomposition drift before mutation |
| `AddChild(child)` | `void(obj)` | Attach child (auto-detaches from previous parent) |
| `TryAddChild(child)` | `i1(obj)` | Attach child and return whether the parent-child link was accepted |
| `TryAddChildPreserveWorld(child)` | `i1(obj)` | Attach child only when exact local TRS can preserve its complete world matrix; reject singular or shear-producing conversions before mutation |
| `TryMoveChild(child, index)` | `i1(obj, i64)` | Move an existing direct child to a strict zero-based sibling index while preserving every other sibling's relative order |
| `RemoveChild(child)` | `void(obj)` | Detach child node |
| `GetChild(index)` | `obj(i64)` | Get child by index |
| `Find(name)` | `obj(str)` | Recursive name search in subtree |
| `MetadataKeys()` | `obj()` | Return every gameplay-metadata key in deterministic lexicographic order |
| `MetadataKind(key)` | `str(str)` | Return `null`, `bool`, `int`, `float`, `string`, or empty when absent |
| `MetadataHas(key)` | `i1(str)` | Test whether an explicit metadata value exists |
| `MetadataGetInt(key, default)` | `i64(str, i64)` | Read integer metadata or the default |
| `MetadataGetFloat(key, default)` | `f64(str, f64)` | Read float metadata, promote an integer, or return the default |
| `MetadataGetBool(key, default)` | `i1(str, i1)` | Read Boolean metadata or the default |
| `MetadataGetString(key, default)` | `str(str, str)` | Read string metadata or the default |
| `MetadataSetNull(key)` | `i1(str)` | Create/replace explicit null metadata; false on rejected input |
| `MetadataSetInt(key, value)` | `i1(str, i64)` | Create/replace integer metadata; false on rejected input |
| `MetadataSetFloat(key, value)` | `i1(str, f64)` | Create/replace finite float metadata; false on rejected input |
| `MetadataSetBool(key, value)` | `i1(str, i1)` | Create/replace Boolean metadata; false on rejected input |
| `MetadataSetString(key, value)` | `i1(str, str)` | Create/replace bounded string metadata; false on rejected input |
| `MetadataRemove(key)` | `i1(str)` | Remove a value and report whether the key existed |
| `BindBody(body)` | `void(obj)` | Attach a `Physics3DBody` for transform sync |
| `ClearBodyBinding()` | `void()` | Remove the current body binding |
| `BindAnimator(controller)` | `void(obj)` | Attach an `AnimController3D` for root motion and animated draw submission |
| `ClearAnimatorBinding()` | `void()` | Remove the current animator binding |
| `AddLOD(distance, mesh)` | `void(f64, obj)` | Add or replace an LOD mesh at a distance threshold |
| `SetAutoLOD(enabled, screenErrorPx)` | `void(i1, f64)` | Select authored LODs by projected screen size |
| `SetImpostor(distance, pixels)` | `void(f64, obj)` | Generate or clear a distant textured impostor |
| `ClearLOD()` | `void()` | Remove all LOD levels |
| `GetLodMesh(index)` | `obj(i64)` | Borrow the mesh for an LOD entry |
| `GetLodDistance(index)` | `f64(i64)` | Get an LOD distance threshold |
| `SetLodResident(index, resident)` | `void(i64, i1)` | Mark the LOD mesh payload resident/nonresident |
| `GetLodResident(index)` | `i1(i64)` | Return whether the LOD mesh payload is resident |
| `GetLodResidentBytes(index)` | `i64(i64)` | Return resident bytes for the LOD mesh payload |

Gameplay metadata is limited to 256 values per node, 128 bytes per non-empty
key, and 64 KiB per string value. Float setters reject non-finite values. VSCN
integer payloads use canonical decimal text (`0`, no leading plus or zeroes,
and no `-0`) so the complete signed 64-bit range remains exact. Use
`MetadataKind` before a typed getter when the distinction between an integer
and an integral-looking float matters. These values are independent of `Name`,
transforms, and rendering components.

`TrySetWorldMatrix` is the transactional counterpart to the read-only
`WorldMatrix` property. It computes `inverse(parent.WorldMatrix) *
worldMatrix`, decomposes that prospective local matrix, and proves the
recomposed local and world matrices before publishing position, rotation, and
scale together. An already-satisfied matrix succeeds. A false result leaves
every local transform lane unchanged, so importers and authoring tools can
roll back a multi-node operation without accepting a closest-TRS
approximation.

### Zia Example

```zia
module SceneDemo;

bind Zanna.Graphics3D.SceneGraph;
bind Zanna.Graphics3D.SceneNode;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.Quat;

func start() {
    var canvas = Canvas3D.New("Scene Demo", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var scene = SceneGraph.New();

    // Create a tree node
    var trunk = SceneNode.New();
    SceneNode.set_Name(trunk, "trunk");
    SceneNode.SetPosition(trunk, 0.0, 0.0, 0.0);
    SceneNode.set_Mesh(trunk, Mesh3D.Cylinder(0.3, 3.0, 8));
    SceneNode.set_Material(trunk, Material3D.FromColor(0.4, 0.25, 0.1));

    // Child node (branches)
    var branch = SceneNode.New();
    SceneNode.set_Name(branch, "branch");
    SceneNode.SetPosition(branch, 0.0, 2.5, 0.0);
    SceneNode.MetadataSetString(branch, "game.role", "harvestable");
    SceneNode.MetadataSetInt(branch, "resource.amount", 3);
    SceneNode.MetadataSetBool(branch, "respawns", true);
    SceneNode.set_Mesh(branch, Mesh3D.Sphere(1.5, 12));
    SceneNode.set_Material(branch, Material3D.FromColor(0.1, 0.6, 0.1));

    // LOD: use low-poly sphere at distance
    SceneNode.AddLOD(branch, 20.0, Mesh3D.Sphere(1.5, 4));

    SceneNode.AddChild(trunk, branch);
    SceneGraph.Add(scene, trunk);

    // Find node by name
    var found = SceneGraph.FindOption(scene, "branch");

    // Render loop
    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        SceneGraph.Draw(scene, canvas, cam);
        Canvas3D.Flip(canvas);
    }
}
```

Transform order is
`world = parent_world * Translate * Rotate * Scale`. Dirty transform state is
lazy: local changes dirty the node, and descendants refresh automatically when
their cached parent world revision changes.
`TryAddChildPreserveWorld` evaluates
`inverse(new_parent_world) * child_world` before mutation and succeeds only when
that matrix decomposes and recomposes as exact finite non-degenerate local TRS.
It preserves reflected transforms, rejects singular destinations or required
shear, and leaves hierarchy and transforms unchanged on ordinary rejection.
`TryMoveChild` accepts only an existing direct child and an index in
`0..ChildCount-1`; it performs a stable move without detaching the child and
rejects invalid or corrupt hierarchy state before mutation.

LOD thresholds are kept sorted; adding the same threshold replaces that mesh,
and drawing uses the highest resident threshold that does not exceed camera
distance, falling back to the base mesh when the selected LOD has been demoted.
`SceneGraph.Draw`, `QueryAABB`, `QuerySphere`, and `RaycastNodes` use the
internal SceneGraph BVH spatial index, with an exact flat-walk fallback kept for
parity. Transform, geometry, and visibility changes refit the BVH; hierarchy,
mesh assignment, LOD, and impostor changes rebuild it lazily. Cached topology
and bounds are validated before use, malformed parent/BVH walks are bounded,
and deformation data that exceeds a bounded scan uses conservative scene-wide
bounds instead of risking an under-cull.
`SceneGraph.AddVisibilityZone(name, min, max)` and
`AddVisibilityPortal(from, to, bidirectional)` author an interior portal/PVS
graph; during `Draw`, nodes inside zones unreachable from the camera zone are
skipped, while unzoned nodes stay visible. `PvsCulledCount`,
`VisibilityZoneCount`, and `VisibilityPortalCount` expose that state and clamp
malformed counters to the live zone/portal allocations before traversal or
append. The normal runtime tests include a generated 10k drawable-node grid to
guard BVH shape, isolated-query reduction, frame-cull candidate reduction,
indexed CPU-occlusion candidate reduction, portal/PVS room culling, and parity
with the flat path. The open-world slice's `visibility_dense_probe.zia` adds a
named dense city/forest PVS fixture and records 169 authored drawables reduced
to 49 submitted draws with matching final-frame pixels on the local software
Release lane.

Finite zero scale is preserved on `Transform3D` and `SceneNode`; only
non-finite scale components are replaced. An attached `Camera` follows the node
hierarchy and is cloned with the node, while each mutable scene instance
receives an independent camera. `SceneGraph.Save` keeps the established VSCN
v2/v3/v5 selection for scenes without gameplay metadata. A scene with node
metadata writes VSCN v6, whose tagged values preserve
null/Boolean/integer/float/string kinds and encode integers as decimal strings
for exact `i64` round trips. Root-node metadata serializes as a
document-level `"metadata"` object in the same tagged format, carrying
scene-scoped conventions (`bake.*`, `env.*`; ADR 0188) through every load
path including `SceneAsset` instantiation. Embedded meshes, materials, exact source texture
containers or canonical RGBA fallbacks, cubemaps, cameras, node-attached
native lights, animation, and node hierarchy retain round-trip precision.
Each node retains its attached light reference, and public `SceneNode.Light`
replacement uses the same component slot serialized by VSCN. `SceneGraph.Load`
accepts VSCN v1-v7 and validates JSON, tagged metadata, bounds, base64 payloads,
mesh indices, asset references, and child nodes before returning a scene;
invalid partial assets fail the complete load instead of being skipped.
VSCN v7 adds prefab reference nodes: a node carrying `"prefab": "<portable
path>"` serializes its own identity only (name, local TRS, visibility, typed
metadata) and the loader grafts deep copies of the referenced scene's root
children beneath it, marking them transient instance content
(`SceneNode.IsInstanceContent`). Cycles, nesting beyond depth 8, per-load
fan-out beyond 4096 instantiations, and missing or invalid sources resolve to
empty placeholders that retain their reference and round-trip
byte-identically. `SceneNode.SetPrefabReference`/`ClearPrefabReference`
author and unpack references (absolute paths are rejected at authoring time),
and scenes without prefab nodes keep serializing at v6 or lower. See
[ADR 0159](adr/0159-typed-scenenode-metadata-and-vscn-v6.md) for the metadata
format contract,
[ADR 0187](adr/0187-vscn-v7-prefab-reference-nodes.md) for prefab references, [ADR 0161](adr/0161-stable-scenenode-sibling-reordering.md) for
sibling-order semantics, and
[ADR 0162](adr/0162-exact-preserve-world-scenenode-reparenting.md) for exact
reparent conversion and rejection. See
[ADR 0172](adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md)
for node-light ownership, spot-cone authoring, and Studio transaction rules.

### Binding Sync

`SceneGraph.SyncBindings(dt)` is the explicit bridge between simulation / animation systems and the scene graph. `SceneGraph.Draw` does not mutate bound bodies or controllers.

`SceneNode.SyncMode` values:

- `0` = `NodeFromBody`: pull the bound `Physics3DBody` world pose into the node.
- `1` = `BodyFromNode`: push the node world pose into the bound body.
- `2` = `NodeFromAnimatorRootMotion`: consume root motion from the bound `AnimController3D` into the node's local transform (translation plus rotation).
- `3` = `TwoWayKinematic`: push node-to-body while the body is kinematic, otherwise pull body-to-node.

Recommended frame order:

1. Step physics and update animation controllers.
2. Call `SceneGraph.SyncBindings(dt)`.
3. Call `SceneGraph.Draw(canvas, camera)`.

When `NodeFromAnimatorRootMotion` is active, `SceneGraph.SyncBindings(dt)` consumes both translation and rotation deltas from the controller's configured root-motion bone once per controller update.

Current scope:

- `SceneNode` bindings currently cover `Physics3DBody` and `AnimController3D`.
- `NavAgent3D` now provides its own `BindNode` / `BindCharacter` workflow for navigation-driven motion.
- `SoundListener3D` and `SoundSource3D` now use `SpatialAudio3D.SyncBindings(dt)`, and `SceneGraph.SyncBindings(dt)` forwards into that audio-binding pass after node/body/anim synchronization.

## SceneAsset

`SceneAsset` is the preferred high-level import surface for reusable 3D assets. It normalizes `.vscn`, `.fbx`, `.gltf`, `.glb`, `.obj`, and `.stl` files into one container that keeps shared meshes, materials, skeletons, animations, and a template node hierarchy together.

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `MeshCount` | Integer | read | Number of shared `Mesh3D` objects |
| `MaterialCount` | Integer | read | Number of shared `Material3D` objects |
| `SkeletonCount` | Integer | read | Number of imported `Skeleton3D` objects |
| `AnimationCount` | Integer | read | Number of imported `Animation3D` clips |
| `NodeCount` | Integer | read | Number of imported logical scene nodes (excluding the synthetic template root) |
| `SceneCount` | Integer | read | Number of immutable scenes addressable by scene-indexed APIs |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `LoadResult(path)` | `obj<Zanna.Result>(str)` | Load `.vscn`, `.fbx`, `.gltf`, `.glb`, `.obj`, or `.stl` as `Ok(SceneAsset)` or `Err(message)` |
| `LoadAssetResult(path)` | `obj<Zanna.Result>(str)` | Load through `Zanna.IO.Assets` as `Ok(SceneAsset)` or `Err(message)`; `.gltf` external buffers/images resolve relative to the model asset |
| `Load(path)` | `obj(str)` | Compatibility loader that returns `null` on routine content failure |
| `LoadAsset(path)` | `obj(str)` | Compatibility asset-store loader that returns `null` on routine content failure |
| `GetMesh(index)` | `obj(i64)` | Get a shared `Mesh3D` by index |
| `GetMaterial(index)` | `obj(i64)` | Get a shared `Material3D` by index |
| `GetSkeleton(index)` | `obj(i64)` | Get a shared `Skeleton3D` by index |
| `GetAnimation(index)` | `obj(i64)` | Get a shared `Animation3D` by index |
| `LoadAnimationResult(path, index)` | `obj<Zanna.Result>(str, i64)` | Load a skeletal animation clip as `Ok(Animation3D)` or `Err(message)` |
| `LoadAnimationAssetResult(path, index)` | `obj<Zanna.Result>(str, i64)` | Load a skeletal animation clip through `Zanna.IO.Assets` |
| `LoadNodeAnimationResult(path, index)` | `obj<Zanna.Result>(str, i64)` | Load a node animation clip as `Ok(NodeAnimation3D)` or `Err(message)` |
| `LoadNodeAnimationAssetResult(path, index)` | `obj<Zanna.Result>(str, i64)` | Load a node animation clip through `Zanna.IO.Assets` |
| `GetCameraCount(sceneIndex)` | `i64(i64)` | Number of imported cameras in a scene |
| `GetCamera(sceneIndex, index)` | `obj(i64, i64)` | Get an imported `Camera3D`, or `null` when absent/out of range |
| `GetSceneName(index)` | `str(i64)` | Get the immutable scene name, or `""` when out of range |
| `FindNode(name)` | `obj(str)` | Find a template `SceneNode` by name inside the imported hierarchy |
| `FindNodeOption(name)` | `obj<Zanna.Option>(str)` | Find a template `SceneNode` as `Some(node)`, or `None` |
| `Instantiate()` | `obj()` | Clone the template hierarchy into a fresh `SceneNode` subtree |
| `InstantiateScene()` | `obj()` | Create a fresh `SceneGraph` and attach cloned top-level imported nodes below its root |
| `InstantiateSceneAt(index)` | `obj(i64)` | Create a fresh `SceneGraph` for an immutable scene index |
| `FlattenStatic(rootName, transform)` | `obj<Zanna.Collections.Seq>(str, obj)` | Merge a static template subtree into one fresh `SceneNode` per material, applying an optional placement `Mat4` |

### Ownership and Instancing

- Imported meshes, materials, skeletons, and animations are shared across instances.
- OBJ-backed models preserve `mtllib`/`usemtl` material groups as synthesized template nodes with matching `Material3D` handles when the referenced `.mtl` is available; missing materials fall back to a default white material. Multiple `mtllib` entries on one line are supported, and common MTL maps such as `map_Kd`, `map_Ks`, `map_Ke`, and `map_Bump` / `bump` are resolved safely relative to the MTL file.
- `Instantiate()` clones nodes, transforms, attached cameras, and typed gameplay metadata. The returned node is a synthetic root group that owns the imported top-level nodes; meshes, materials, lights, skeletons, and animation resources remain shared. Metadata tables are deep copies, so changing an instance cannot change the template or another instance.
- Prefer `LoadResult()` / `LoadAssetResult()` for new code. `Load()` and `LoadAsset()` remain available for compatibility with existing `null` checks.
- Prefer `FindNodeOption()` for new code. `FindNode()` remains available for compatibility with existing `null` checks.
- Mutating an instantiated node does not mutate the template returned by `FindNode` / `FindNodeOption`.
- `InstantiateScene()` is the easiest way to drop an imported asset into a fresh scene while preserving node names and hierarchy.
- glTF imports order immutable scenes with the active/default scene at index `0` and secondary glTF scene roots after it. `GetSceneName(index)` preserves authored scene names where available and falls back to `"default"`/`"scene_N"`.
- glTF cameras reachable from each imported scene populate `GetCameraCount(sceneIndex)` and `GetCamera(sceneIndex, index)`. Each immutable table entry is the same `Camera3D` attached to its authored template node; mutable instances receive independent camera clones with node world transforms applied.
- Use `InstantiateSceneAt(index)` for scene-indexed code. Index `0` is equivalent to `InstantiateScene()`; invalid indices return `null` instead of mutating shared model state.
- `FlattenStatic(rootName, transform)` walks the named template subtree (`""` means the whole model) in authored order and returns one merged node per first-seen material. The source template is never mutated, materials remain shared, and each result keeps the first contributing source node's name so role conventions survive flattening. Pass `null` for an identity placement. Skinned, morph-target, and singularly transformed pieces are skipped and reported through `AssetDiagnostics3D`; an unknown root returns an empty `Seq`.

### Zia Example

```zia
module SceneAssetDemo;

bind Zanna.Graphics3D;
bind Zanna.Terminal;

func start() {
    var model = SceneAsset.LoadResult("tree.gltf").Unwrap();
    var templateNode = SceneAsset.FindNode(model, "Trunk").Unwrap();
    var instanceRoot = SceneAsset.Instantiate(model);
    var scene = SceneAsset.InstantiateScene(model);
    var indexedScene = SceneAsset.InstantiateSceneAt(model, 0);

    Say("Nodes = " + toString(SceneAsset.get_NodeCount(model)));
    Say("Meshes = " + toString(SceneAsset.get_MeshCount(model)));
    Say("Scenes = " + toString(SceneAsset.get_SceneCount(model)));
    Say("Default scene = " + SceneAsset.GetSceneName(model, 0));
    Say("Default scene cameras = " + toString(SceneAsset.GetCameraCount(model, 0)));
    Say("Template trunk found = " + toString(templateNode != null));
    Say("Instance root children = " + toString(SceneNode.get_ChildCount(instanceRoot)));
    Say("Scene nodes = " + toString(SceneGraph.get_NodeCount(scene)));
    Say("Indexed scene nodes = " + toString(SceneGraph.get_NodeCount(indexedScene)));
}
```

For game-facing asset loading, prefer `SceneAsset.LoadResult` for loose filesystem files during early development and `SceneAsset.LoadAssetResult` for code that should also work from embedded or mounted `.zpak` packages. `LoadAssetResult` accepts both plain asset paths such as `"assets/tree.glb"` and explicit URIs such as `"asset://tree.glb"`; mounted assets are checked before the development filesystem fallback. Use the lower-level `FBX` and `GLTF` helpers when you explicitly want extractor-style access to importer-native arrays.

Format note:
- `.vscn`, FBX, and glTF imports can populate shared skeletons and animation clips when the source format contains supported skin/animation data.
- FBX-backed `SceneAsset` assets preserve authored `Model` hierarchy and full transform stacks; polygon and tessellated NURBS/patch geometry; mesh/material/skin/morph attachments; composed animation layers; built-in constraints; node-coupled cameras and projection animation; native punctual/area/volume lights; external textures; and embedded Texture/Video image payloads. Numeric FBX IDs remain the binding identity throughout evaluation.
- OBJ-backed `SceneAsset` assets synthesize template nodes per material group, resolve relative `.mtl` files and texture maps safely beside the source OBJ/MTL, and reject absolute paths, URI schemes, traversal, and NUL-containing references.
- STL-backed `SceneAsset` assets synthesize a single mesh node and default material around the existing binary/ASCII STL geometry loader.
- glTF imports populate meshes, materials, active-scene and secondary scene hierarchies, scene-local cameras attached to their authored nodes, skins, morph targets, punctual lights, skeletal clips, and node/morph animation clips. Immutable camera tables share the node attachment; instantiated scenes clone each camera.
- Scalar entries in a glTF node's `extras` object import as typed `SceneNode` metadata: JSON strings, integral and fractional numbers, booleans, and null become the corresponding string, int, float, bool, and null entries. Nested arrays/objects and values beyond the metadata limits are skipped with one import warning per node, visible through `AssetDiagnostics3D` and `zanna asset validate`. A VSCN bake preserves the imported metadata.
- glTF skeletal tracks map to `Skeleton3D` / `Animation3D`; non-joint node translation, rotation, scale, and morph `weights` tracks are bound automatically on `SceneAsset.Instantiate()` and `InstantiateScene()`. Node animation channels reject non-finite sample data and non-increasing key times before playback; LINEAR rotation tracks use quaternion slerp, and CUBICSPLINE tracks use glTF Hermite tangents. Call `SceneGraph.SyncBindings(dt)` each frame to advance those imported node clips.
- glTF mesh extraction supports `POSITION`, `NORMAL`, `TEXCOORD_0`, `TEXCOORD_1`, `COLOR_0`, `TANGENT`, `JOINTS_0`/`WEIGHTS_0`, and `JOINTS_1`/`WEIGHTS_1`. Secondary joint sets are reduced to the four strongest supported influences and renormalized. Invalid optional attributes are dropped with normals regenerated when needed; invalid indices, sparse accessors, and skin references fail the import. Skins above the runtime 256-bone palette are rejected instead of silently dropping the rig.
- glTF morph targets import `POSITION`, `NORMAL`, and `TANGENT` deltas. Position/normal morphs can use the GPU path; tangent morphs currently route through the CPU morph path so tangent-space normal mapping stays correct.
- glTF node hierarchies are rejected if they contain invalid child references, duplicate parents, or cycles; valid meshes/materials still remain available to the asset container.
- Triangle-list, triangle-strip, and triangle-fan glTF primitives are triangulated on import. Points and line modes are skipped because the current renderer has no line/point primitive surface.
- Materialless glTF primitives receive a shared default white PBR material so valid assets render through `SceneGraph` / `SceneAsset` without manual material assignment.
- Complete `SceneAsset.Save` selects VSCN v5 when camera/light/source-container content needs it, v6 when typed node metadata is present, and v7 when prefab references are present. It writes the portable `vgfx3d_vertex_le_v3` vertex layout plus explicitly tagged little-endian index, bone-map, extra-influence, and keyframe streams; the loader keeps compatibility with VSCN versions 1–7 and the older v1/v2 binary layouts. It round-trips every immutable scene, camera-node attachment and scalar camera animation, native light state, typed node metadata, morph/animation/variant inventory, per-slot material metadata, lightmaps, and high-precision node transform. Texture entries retain exact KTX2/PNG/JPEG/GIF/BMP bytes and native KTX2 mips when available, otherwise canonical RGBA8. The loader rejects malformed JSON/Base64, invalid metadata tags or bounds, invalid source-image magic or decoding, invalid mesh indexes, broken table/node references, malformed present rig streams, and partial child subtrees transactionally.
- `.glb` files are validated as GLB 2.0 containers before JSON parse. External `.gltf` buffers and images are URI-decoded and resolved relative to the asset path; `./` relative paths are accepted, while absolute paths, URI schemes, `..` traversal, and NUL-containing references are rejected before opening files. In `LoadAsset`, those external dependencies are loaded through `Zanna.IO.Assets` first and missing-dependency diagnostics name both the parent model and dependency path.
- glTF matrix-authored node transforms are decomposed to runtime TRS. Reflections preserve negative scale sign, while unsupported shear is reduced to an orthonormal rotation basis instead of leaking into unstable quaternions.
- glTF `extensionsRequired` is enforced. Required `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_materials_unlit`, `KHR_materials_specular`, `KHR_lights_punctual`, `KHR_materials_variants`, `KHR_mesh_quantization`, `EXT_meshopt_compression`, `KHR_draco_mesh_compression`, and `KHR_texture_basisu` are accepted by their complete parser paths. Optional factor-level clearcoat, transmission, IOR, sheen, anisotropy, and volume features remain best-effort and are rejected when listed as required unless their full required semantics are representable. Required WebP, DDS, or unknown extensions fail load rather than rendering an incomplete fallback.

## Skeleton3D

Bone hierarchy for skeletal animation.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create empty skeleton |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `BoneCount` | Integer | read | Number of bones |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddBone(name, parentIdx, bindPose)` | `i64(str, i64, obj)` | Add a bone with an exact retained name (returns index). parentIdx=-1 for root. bindPose is Mat4 |
| `ComputeInverseBind()` | `void()` | Compute inverse bind matrices (call after all bones added) |
| `FindBone(name)` | `i64(str)` | Find bone index by name (-1 if not found) |
| `FindBoneOption(name)` | `obj<Zanna.Option>(str)` | Find bone index by name as `Some(index)`, or `None` |
| `GetBoneName(index)` | `str(i64)` | Get bone name by index |

Prefer `FindBoneOption()` for new code. `FindBone()` remains available for
compatibility with existing `-1` checks.

Bone names are length-carrying runtime strings: `AddBone`, `FindBone`,
`GetBoneName`, importer binding, retarget exact matching, and VSCN persistence do
not truncate long equal prefixes. Numeric source IDs—not names—remain the
identity used by FBX animation import.

Bones must be added in topological order (parent before child). Max 1,024 bones
per skeleton; each draw palette remains capped at 256 and imported meshes are
partitioned/remapped when necessary.
Add all bones before binding the skeleton to a mesh or creating an `AnimPlayer3D`, `AnimBlend3D`,
or `AnimController3D`; those runtime objects allocate fixed-size pose buffers and freeze the
skeleton topology.

---

## Animation3D

Keyframe animation clip with per-bone position, rotation, and scale tracks.
Key times use double precision internally. Arithmetic-noise duplicates replace
the prior sample, while adjacent FBX source ticks remain distinct so constant
segments can preserve an exact step boundary.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(name, duration)` | `obj(str, f64)` | Create animation clip with name and duration in seconds |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Looping` | Boolean | read/write | Loop playback |
| `Duration` | Float | read | Total duration in seconds |
| `Name` | String | read | Animation name |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddKeyframe(boneIdx, time, pos, rot, scale)` | `void(i64, f64, obj, obj, obj)` | Add keyframe: bone index, time, position Vec3, rotation Quat, scale Vec3 |
| `Retarget(srcSkeleton, dstSkeleton)` | `obj(obj, obj)` | Copy the animation onto matching bones in another skeleton |

Keyframes are kept sorted by time within each bone channel. Rotation keyframes use normalized
quaternions and SLERP; position/scale use linear interpolation. `pos`, `rot`, or `scale` may be
`null`; omitted or non-finite/out-of-float-range components fall back to the bone bind pose instead
of erasing that component to zero/identity.

`Retarget` preserves the clip name, duration, looping flag, and keyframe data while remapping
channels by exact bone name first, then humanoid role aliases, then matching bone index as a
fallback. It scales keyed translations by the source/destination bind-length ratio when both bones
expose a comparable segment, carries rotations/scales unchanged, and leaves destination-only bones
in bind pose.

---

## AnimPlayer3D

Playback controller for skeletal animation.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(skeleton)` | `obj(obj)` | Create player bound to a Skeleton3D |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Speed` | Float | read/write | Playback speed multiplier |
| `IsPlaying` | Boolean | read | Currently playing |
| `Time` | Float | read/write | Current playback time in seconds |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Play(animation)` | `void(obj)` | Start playing an Animation3D |
| `Crossfade(animation, duration)` | `void(obj, f64)` | Blend to new animation over duration (SLERP for rotation) |
| `Stop()` | `void()` | Stop playback and output the bind pose |
| `Update(dt)` | `void(f64)` | Advance animation by dt seconds |
| `GetBoneMatrix(boneIdx)` | `obj(i64)` | Get the current global/world Mat4 for a bone |

Negative playback speeds play clips in reverse. Looping clips wrap in both directions; non-looping
clips clamp at the start/end and stop when they hit the boundary. Player and animation handles are
validated before playback, so wrong graphics object types are ignored instead of being sampled as
animation memory.

### Zia Example

```zia
module SkeletonDemo;

bind Zanna.Graphics3D.Skeleton3D;
bind Zanna.Graphics3D.Animation3D;
bind Zanna.Graphics3D.AnimPlayer3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.Mat4;
bind Zanna.Math.Vec3;
bind Zanna.Math.Quat;

func start() {
    var canvas = Canvas3D.New("Skeleton", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    // Build skeleton
    var skel = Skeleton3D.New();
    Skeleton3D.AddBone(skel, "root", -1, Mat4.Identity());
    Skeleton3D.AddBone(skel, "arm", 0, Mat4.Translate(1.0, 0.0, 0.0));
    Skeleton3D.ComputeInverseBind(skel);

    // Create walk animation
    var walk = Animation3D.New("walk", 1.0);
    Animation3D.set_Looping(walk, true);
    var pos0 = Vec3.New(0.0, 0.0, 0.0);
    var pos1 = Vec3.New(0.0, 0.5, 0.0);
    var rot = Quat.Identity();
    var scl = Vec3.One();
    Animation3D.AddKeyframe(walk, 0, 0.0, pos0, rot, scl);
    Animation3D.AddKeyframe(walk, 0, 0.5, pos1, rot, scl);
    Animation3D.AddKeyframe(walk, 0, 1.0, pos0, rot, scl);

    // Play animation
    var player = AnimPlayer3D.New(skel);
    AnimPlayer3D.Play(player, walk);

    // Bind skeleton to mesh
    Mesh3D.SetSkeleton(mesh, skel);

    // Render loop
    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        var dt = Canvas3D.get_DeltaTime(canvas);
        AnimPlayer3D.Update(player, dt / 1000.0);

        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawMeshSkinned(canvas, mesh, Mat4.Identity(), mat, player);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

- `DrawMeshSkinned` applies CPU or GPU skinning via the internal skinning palette. Skinning weights are normalized consistently across CPU and GPU paths; missing palettes copy vertices through unchanged, and unused backend bone-palette slots behave as identity transforms.
- `Crossfade` blends every bone using TRS decomposition: position/scale linearly interpolated, rotation via quaternion SLERP. Channels present in only one clip blend against bind pose, and the fading-out clip keeps its own speed/looping settings during the transition.

## MorphTarget3D

Blend shapes for facial animation, muscle flex, and shape-based deformation.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(vertexCount)` | `obj(i64)` | Create morph target set for a mesh vertex count |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `ShapeCount` | Integer | read | Number of registered shapes |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddShape(name)` | `i64(str)` | Register a blend shape (returns shape index) |
| `SetDelta(shapeIdx, vertexIdx, dx, dy, dz)` | `void(i64, i64, f64, f64, f64)` | Set position delta for a vertex in a shape |
| `SetNormalDelta(shapeIdx, vertexIdx, dx, dy, dz)` | `void(i64, i64, f64, f64, f64)` | Set normal delta for a vertex in a shape |
| `SetWeight(shapeIdx, weight)` | `void(i64, f64)` | Set blend weight for a shape, clamped to `[-1, 1]` |
| `GetWeight(shapeIdx)` | `f64(i64)` | Get current blend weight |
| `SetWeightByName(name, weight)` | `void(str, f64)` | Set blend weight by shape name |

### Zia Example

```zia
module MorphDemo;

bind Zanna.Graphics3D.MorphTarget3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("Morph Demo", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var mesh = Mesh3D.Sphere(1.0, 16);
    var mat = Material3D.FromColor(0.8, 0.6, 0.5);

    // Create morph targets
    var morph = MorphTarget3D.New(Mesh3D.get_VertexCount(mesh));
    var smile = MorphTarget3D.AddShape(morph, "smile");
    MorphTarget3D.SetDelta(morph, smile, 10, 0.1, 0.05, 0.0);
    MorphTarget3D.SetDelta(morph, smile, 11, -0.1, 0.05, 0.0);

    // Bind to mesh
    Mesh3D.SetMorphTargets(mesh, morph);

    // Animate weight
    MorphTarget3D.SetWeight(morph, smile, 0.7);

    // Draw morphed mesh
    Canvas3D.Begin(canvas, cam);
    Canvas3D.DrawMeshMorphed(canvas, mesh, Mat4.Identity(), mat, morph);
    Canvas3D.End(canvas);
}
```

- Shape storage grows on demand
- Normal deltas optional (lazy-allocated on first `SetNormalDelta`)
- Weights can be negative (reverse deformation)
- `New(vertexCount)` bounds allocation size, deltas sanitize non-finite values to `0`, and non-finite weights become `0` before clamping to the supported range.
- `Canvas3D.DrawMeshMorphed` requires a `Mat4` transform, a `Mesh3D`, and a matching `MorphTarget3D`; mismatched vertex counts or invalid handles skip the draw without dereferencing the wrong object type.
- GPU-applied on Metal and on OpenGL/D3D11 while the active shape count fits backend shader limits; otherwise CPU-applied as `finalPos = basePos + sum(weight * delta)` per vertex. GPU backends clamp active shape counts to shader-indexable limits and disable the morph path on upload failure rather than reusing stale buffers.

## FBX Loader

Low-level extractor API for meshes, skeletons, materials, animations, and morph
targets from binary FBX files (v7100-7700) and brace-scoped ASCII FBX documents.
Both encodings populate the same typed node/property/connection graph and run
the same extraction pipeline. For instantiation-ready imported assets, prefer
`SceneAsset.LoadResult("asset.fbx")`.

FBX reads are capped at 256 MiB by default to avoid accidental whole-file
allocations on oversized content. Hosts that intentionally process larger files
can set `ZANNA_FBX_MAX_FILE_BYTES`; the runtime still clamps that value to the
1 GiB hard cap. Independently, the complete load has a 1 GiB aggregate budget
covering file bytes, typed nodes/properties, expanded arrays, indexes, and
generated animation samples. `ZANNA_FBX_MAX_LOAD_BYTES` may lower (never raise)
that budget for a host. Exceeding either limit returns null without publishing a
partial asset.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Load(path)` | `obj(str)` | Parse binary or ASCII FBX through the shared typed graph |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `MeshCount` | Integer | read | Number of geometry objects |
| `AnimationCount` | Integer | read | Number of animation stacks |
| `NodeAnimationCount` | Integer | read | Number of object/morph/camera animation stacks |
| `CameraCount` | Integer | read | Number of imported cameras |
| `MaterialCount` | Integer | read | Number of materials |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetMesh(index)` | `obj(i64)` | Get Mesh3D by index |
| `GetSkeleton()` | `obj()` | Get Skeleton3D (or null) |
| `GetAnimation(index)` | `obj(i64)` | Get Animation3D by index |
| `GetAnimationName(index)` | `str(i64)` | Get animation name by index |
| `GetNodeAnimation(index)` | `obj(i64)` | Get NodeAnimation3D by index |
| `GetNodeAnimationName(index)` | `str(i64)` | Get node animation name by index |
| `GetCamera(index)` | `obj(i64)` | Get Camera3D by index |
| `GetMaterial(index)` | `obj(i64)` | Get Material3D by index |
| `GetMorphTarget(index)` | `obj(i64)` | Get MorphTarget3D by index |

### Zia Example

```zia
module FBXDemo;

bind Zanna.Graphics3D.Fbx;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.AnimPlayer3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("FBX Demo", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var asset = Fbx.Load("character.fbx");
    var mesh = Fbx.GetMesh(asset, 0);
    var skel = Fbx.GetSkeleton(asset);
    var mat = Fbx.GetMaterial(asset, 0);

    // Play first animation
    var player = AnimPlayer3D.New(skel);
    var anim = Fbx.GetAnimation(asset, 0);
    AnimPlayer3D.Play(player, anim);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        AnimPlayer3D.Update(player, Canvas3D.get_DeltaTime(canvas) / 1000.0);
        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawMeshSkinned(canvas, mesh, Mat4.Identity(), mat, player);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

Supports zlib-compressed array properties, negative polygon indices, arbitrary
n-gon triangulation, LayerElementNormal/UV mapping modes,
LayerElementMaterial polygon assignments, default materials for materialless
meshes, complete FBX transform properties, embedded Texture/Video
PNG/JPEG/GIF/BMP/KTX2 payloads, external texture references, and Z-up to Y-up
coordinate conversion. Exact supported encoded texture bytes are retained for
VSCN v5 rather than being discarded after decode.
The ASCII lexer is length-bounded and brace-scoped, so identifiers in comments,
quoted strings, or closed sibling nodes never satisfy a lookup. Object identity
is always the numeric FBX ID: dynamically sized display names—including long
equal prefixes—remain distinct but never substitute for an ID.

Static nodes, bind extraction, and animation evaluate the same pivot,
pre/post-rotation, rotation-order, geometric-transform, and inheritance
composer. Every AnimationStack composes connected layers in order with
mute/solo, animated weight, additive/override behavior, rotation accumulation,
and scale accumulation. Constant curves preserve the immediately preceding FBX
tick, linear curves remain linear, and cubic Hermite curves subdivide adaptively
to the documented time/value error bound.

Open/closed/periodic rational NURBS surfaces and linear, Bézier, quadratic
Bézier, cardinal, and B-spline patches tessellate into bounded ordinary meshes.
Position, rotation, scale, parent, aim, and single-chain IK constraints feed the
same static/animated evaluator. Cameras remain attached to their model nodes and
accept animated FOV/aspect/clipping/orthographic/projection properties. FBX
rectangle/sphere area and volume lights become native Light3D types with their
oriented size, range, and decay. Invalid topology, active trim regions, cyclic
constraints, non-finite samples, or budget/capacity failure reject the complete
staged load instead of publishing partial output.

`SceneAsset.LoadResult("asset.fbx")` adapts these resources into an instantiable
scene asset and preserves the authored `Model` hierarchy, camera coupling,
lights, and animation through VSCN v5.

---

## GLTF Loader

Low-level extractor API for meshes and materials from glTF 2.0 files. `SceneAsset.LoadResult` uses the same loader internally and preserves the active-scene node hierarchy for instantiation.

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `Gltf.Load(path)` | `obj(str)` | Parse glTF file |
| `Gltf.LoadAsset(path)` | `obj(str)` | Parse glTF/GLB through `Zanna.IO.Assets`, including package-relative external dependencies |
| `GLTF.get_MeshCount(asset)` | `i64(obj)` | Number of meshes |
| `Gltf.GetMesh(asset, index)` | `obj(obj, i64)` | Get Mesh3D by index |
| `GLTF.get_MaterialCount(asset)` | `i64(obj)` | Number of materials |
| `Gltf.GetMaterial(asset, index)` | `obj(obj, i64)` | Get Material3D by index |

### Zia Example

```zia
module GLTFDemo;

bind Zanna.Graphics3D.Gltf;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("GLTF Demo", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var asset = Gltf.Load("scene.gltf");
    var mesh = Gltf.GetMesh(asset, 0);
    var mat = Gltf.GetMaterial(asset, 0);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawMesh(canvas, mesh, Mat4.Identity(), mat);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

**Note:** GLTF is class-backed in the runtime catalog and also works as a low-level extractor helper. It preserves the active-scene hierarchy, matrix-authored node transforms, extended mesh attributes, materials, skeletons, animations, and morph targets listed above. For preserved node hierarchies and scene instantiation, load `.gltf` or `.glb` through `SceneAsset.LoadResult`. Plain JSON is parsed with its exact byte length and rejects embedded NUL. Synchronous and preload GLB paths share one bounded chunk iterator, so malformed ordering, missing JSON/BIN chunks, and trailing bytes fail identically. Recognized integer/boolean-like fields require complete exact tokens, and malformed array separators invalidate the containing object rather than returning a prefix.

Supported glTF material fidelity:
- Core metallic-roughness PBR, base-color / normal / metallic-roughness / occlusion / emissive texture slots, alpha modes, `doubleSided`, and `KHR_materials_emissive_strength`. PBR base-color and emissive textures are decoded from sRGB to linear before lighting on software, Metal, D3D11, and OpenGL.
- `KHR_materials_unlit` and `KHR_materials_specular` are accepted in `extensionsRequired`; `KHR_materials_clearcoat` and `KHR_materials_transmission` are accepted only when optional (`extensionsUsed`) and mapped onto the current `Material3D` surface as best-effort material parameters.
- Optional `KHR_texture_basisu` KTX2 images are imported as `TextureAsset3D` handles, preserving compressed texture metadata for backends that can upload native payloads while still exposing the material texture slot through `Material3D`; required BasisU textures are rejected until the renderer can guarantee full required-extension fidelity.
- `KHR_texture_transform`, `textureInfo.texCoord`, wrap mode, and independent minification, magnification, and mip filter axes are preserved for base-color, normal, specular, emissive, metallic-roughness, and occlusion texture slots across software, Metal, D3D11, and OpenGL. UV0/UV1 are accepted only when the primitive carries the selected stream; missing streams and UV2+ reject the primitive instead of silently sampling UV0.
- `KHR_lights_punctual` directional, point, and spot lights attach to their authored scene nodes. `SceneGraph.Draw` transforms them by node world pose and includes them in the per-draw light snapshot; imported directional lights participate in shadow selection from that snapshot, and glTF `range` maps to the runtime quadratic attenuation coefficient.

## Particles3D

Emitter-based 3D particle effects with physics, lifetime, and billboard rendering.
Particle setters sanitize non-finite values: ranges are kept non-negative and ordered, alpha and
direction spread are clamped, invalid directions fall back to +Y, and invalid update deltas are ignored.
`Update` advances the simulation in 60 Hz fixed steps, carries an ordinary fractional step in
`ResidualTime`, and executes at most 60 steps (one simulated second) per call. Complete steps beyond
that safety budget are not hidden: `LastDroppedTime` reports the amount dropped by the latest call,
and `DroppedTime` accumulates it until `ResetDroppedTime()` is called.
Rendering is batched per emitter. Additive particles skip sorting, while alpha particles sort a
temporary key array back-to-front without reordering the live particle array. Metal, D3D11, and
OpenGL draw one process-retained unit quad with a compact 64-byte center/right/up/color instance for
each particle; repeated draws therefore do not rebuild or upload four expanded vertices and six
indices per particle. The software renderer reconstructs the same corners through its deterministic
CPU mesh path. Ribbon trails remain CPU-expanded on every backend and can be drawn alongside the
compact hardware billboard batch.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(maxParticles)` | `obj(i64)` | Create particle emitter with max capacity |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Count` | Integer | read | Number of live particles; terminal-frame snapshots are excluded |
| `Emitting` | Boolean | read | Whether emitter is active |
| `Additive` | Boolean | write | Additive particle blending mode (fire, sparks). Default: false. Preserves each particle's own alpha/intensity. |
| `Seed` | Integer | read/write | Deterministic per-emitter spawn-stream state |
| `RenderFinalFrame` | Boolean | read/write | Render an expired particle's exact endpoint until the next valid update. Default: true; the endpoint is not counted as live. |
| `DroppedTime` | Double | read | Cumulative complete-step time discarded by the per-call catch-up budget |
| `LastDroppedTime` | Double | read | Time discarded by the most recent update call, or zero when none was discarded |
| `ResidualTime` | Double | read | Fractional fixed-step time retained for a later update; always less than 1/60 second |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Set emitter world position |
| `SetDirection(dx, dy, dz, spreadDegrees)` | `void(f64, f64, f64, f64)` | Set emission direction + cone spread in degrees |
| `SetSpeed(min, max)` | `void(f64, f64)` | Set speed range |
| `SetLifetime(min, max)` | `void(f64, f64)` | Set lifetime range in seconds |
| `SetSize(start, end)` | `void(f64, f64)` | Set particle size (start/end interpolation) |
| `SetGravity(x, y, z)` | `void(f64, f64, f64)` | Set gravity vector |
| `SetColor(startColor, endColor)` | `void(i64, i64)` | Set color range (0xRRGGBB) |
| `SetAlpha(start, end)` | `void(f64, f64)` | Set alpha range (fade out: 1.0 → 0.0) |
| `SetRate(particlesPerSec)` | `void(f64)` | Set emission rate |
| `SetTexture(pixels)` | `void(obj)` | Set particle sprite (null for solid quads) |
| `SetEmitterShape(shape)` | `void(i64)` | 0=point, 1=sphere, 2=box |
| `SetEmitterSize(sx, sy, sz)` | `void(f64, f64, f64)` | Set emitter volume (for sphere/box shapes) |
| `SetSoftness(distance)` | `void(f64)` | Soft-particle fade distance in world units (0 = hard edges). Particles fade out where their quads approach opaque geometry, hiding intersection lines. Needs `BackendSupports("soft-particles")`; the `Effects3D` presets enable it automatically |
| `Start()` | `void()` | Start continuous emission |
| `Stop()` | `void()` | Stop emission |
| `Burst(count)` | `void(i64)` | Instantly spawn N particles |
| `Clear()` | `void()` | Remove all active particles |
| `ResetDroppedTime()` | `void()` | Reset cumulative and latest dropped-time telemetry; residual simulation time is unchanged |
| `Update(dt)` | `void(f64)` | Advance bounded 60 Hz simulation and update residual/dropped-time telemetry |
| `Draw(canvas, camera)` | `void(obj, obj)` | Render particles (between Begin/End) |

### Zia Example

```zia
module ParticleDemo;

bind Zanna.Graphics3D.Particles3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;

func start() {
    var canvas = Canvas3D.New("Particles", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var sparks = Particles3D.New(500);
    Particles3D.SetPosition(sparks, 0.0, 0.0, 0.0);
    Particles3D.SetDirection(sparks, 0.0, 1.0, 0.0, 23.0);
    Particles3D.SetSpeed(sparks, 2.0, 5.0);
    Particles3D.SetLifetime(sparks, 0.5, 1.5);
    Particles3D.SetSize(sparks, 0.2, 0.05);
    Particles3D.SetGravity(sparks, 0.0, -9.8, 0.0);
    Particles3D.SetColor(sparks, 0xFFAA22, 0xFF2200);
    Particles3D.SetAlpha(sparks, 1.0, 0.0);
    Particles3D.SetRate(sparks, 50.0);
    Particles3D.set_Additive(sparks, true);
    Particles3D.Start(sparks);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        var dt = Canvas3D.get_DeltaTime(canvas) / 1000.0;
        Particles3D.Update(sparks, dt);

        Canvas3D.Clear(canvas, 0.0, 0.0, 0.0);
        Canvas3D.Begin(canvas, cam);
        Particles3D.Draw(sparks, canvas, cam);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

- Particles are billboarded (camera-facing)
- Additive mode uses true additive blending and stays fully batched in one draw call
- Alpha blend mode sorts particles back-to-front and submits per-particle keyed draws so blending stays correct against the rest of the scene
- Expiration integrates only the remaining lifetime. With `RenderFinalFrame` enabled, `Draw` can
  show that exact terminal position during the update interval, while `Count` already reports the
  particle as expired.

## PostFX3D

Full-screen post-processing effect chain applied automatically in `Canvas3D.Flip()` to the active canvas output.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create empty post-processing chain |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Enabled` | Boolean | read/write | Enable/disable entire chain (bypass temporarily) |
| `EffectCount` | Integer | read | Number of effects in chain |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `NewQuality(canvas, profile)` | `obj(obj, i64)` | Create a backend-safe quality chain for a canvas (`0` performance, `1` balanced, `2` cinematic) |
| `AddBloom(threshold, intensity, passes)` | `void(f64, f64, i64)` | Bloom glow effect |
| `AddTonemap(mode, exposure)` | `void(i64, f64)` | Tone mapping (0=off, 1=Reinhard, 2=ACES) |
| `AddFXAA()` | `void()` | Fast approximate anti-aliasing |
| `AddColorGrade(brightnessOffset, contrast, saturation)` | `void(f64, f64, f64)` | Color grading: brightness is an additive offset centered on `0.0`; contrast and saturation are multipliers centered on `1.0` |
| `AddVignette(radius, softness)` | `void(f64, f64)` | Screen-edge darkening |
| `AddSSAO(radius, intensity, samples)` | `void(f64, f64, i64)` | Screen-space ambient occlusion |
| `AddDOF(focusDist, aperture, maxBlur)` | `void(f64, f64, f64)` | Depth of field |
| `AddMotionBlur(strength, samples)` | `void(f64, i64)` | Velocity-buffer motion blur |
| `AddTAA(blend)` | `void(f64)` | Temporal anti-aliasing resolve; `blend` is the history weight (0.5-0.98) |
| `AddSSR(intensity, maxRoughness)` | `void(f64, f64)` | Screen-space reflections for `SsrEnabled` materials (Water3D opts in automatically); ray misses fall back to the material's environment map |
| `Clear()` | `void()` | Remove all effects from chain |

Effects run strictly in append order. If you add the same effect type more than once, each pass is preserved instead of being collapsed into one combined backend setting. The GPU backends now follow that same ordered-chain behavior as the CPU path, so `Flip()`, GPU screenshots, and GPU readback all match the authored `PostFX3D` chain. Each D3D11 bloom entry builds its thresholded mip chain from that entry's current input, so effects before bloom and repeated bloom entries are preserved. Bloom `passes` is part of the backend snapshot so GPU paths can widen the bloom radius consistently with the authored quality setting.

PostFX parameters are bounded before they reach CPU or GPU shaders: bloom passes clamp to `0..32`, SSAO samples to `1..128`, motion-blur samples to `1..64`, color-grade brightness offsets clamp to `-1.0..1.0`, vignette softness has a non-zero floor, and non-finite exposure/radius/intensity values fall back to safe defaults.

Bloom, Tonemap, FXAA, ColorGrade, and Vignette run on both GPU outputs and CPU render-target/software fallback outputs. SSAO, DOF, MotionBlur, TAA, and SSR require `Canvas3D.BackendSupports(canvas, "gpu_postfx")` and a window-backed `Flip()` so the backend can provide depth, scene history, and motion vectors; attaching those effects to a render target or software CPU path raises an explicit runtime trap.

### Zia Example

```zia
module PostFXDemo;

bind Zanna.Graphics3D.PostFX3D;
bind Zanna.Graphics3D.Canvas3D;

func start() {
    var canvas = Canvas3D.New("PostFX Demo", 800, 600);

    var fx = PostFX3D.New();
    PostFX3D.AddBloom(fx, 0.8, 0.5, 5);
    PostFX3D.AddTonemap(fx, 1, 1.2);
    PostFX3D.AddFxaa(fx);
    PostFX3D.AddColorGrade(fx, 0.015, 1.1, 1.2);
    PostFX3D.AddVignette(fx, 0.6, 0.4);
    PostFX3D.AddSsao(fx, 0.5, 1.0, 16);
    PostFX3D.AddDof(fx, 10.0, 5.0, 1.0);
    Canvas3D.SetPostFX(canvas, fx);

    // Temporarily disable
    PostFX3D.set_IsEnabled(fx, false);

    // Re-enable
    PostFX3D.set_IsEnabled(fx, true);

    // Reset chain
    PostFX3D.Clear(fx);
}
```

Effects are applied in chain order (first added = first applied). Chain storage grows as needed instead of truncating at a fixed 8-effect limit.

## Ray3D / AABB3D / Sphere3D / Segment3D / Capsule3D / RayHit3D

3D raycasting and collision detection for picking, shooting, and physics. These are all standalone functions (no classes).

### Ray3D — Raycasting

| Function | Signature | Description |
|----------|-----------|-------------|
| `Ray3D.IntersectTriangle(o, d, v0, v1, v2)` | `f64(obj, obj, obj, obj, obj)` | Möller-Trumbore; returns distance or -1 |
| `Ray3D.IntersectTriangleCull(o, d, v0, v1, v2, frontOnly)` | `f64(obj, obj, obj, obj, obj, i1)` | As above; `frontOnly = true` rejects back-facing triangles (natural for picking and line-of-sight) |
| `Ray3D.IntersectMesh(o, d, mesh, transform)` | `obj(obj, obj, obj, obj)` | Traverse the mesh's retained triangle BVH, returns closest RayHit3D or null |
| `Ray3D.IntersectAABB(o, d, min, max)` | `f64(obj, obj, obj, obj)` | Slab method; returns distance or -1 |
| `Ray3D.IntersectSphere(o, d, center, radius)` | `f64(obj, obj, obj, f64)` | Quadratic formula; returns distance or -1 |

Ray directions are normalized internally for all `Ray3D` intersection helpers. A zero-length or non-finite direction is a miss, and returned distances are Euclidean world distances even when the input direction was not normalized. Sphere hits from inside the sphere return distance `0`. `IntersectMesh` retains a local-space BVH per mesh geometry revision and rebuilds it lazily after mutation. Identity and invertible transforms reuse that tree; singular transforms and allocation failure preserve an exact linear fallback. Equal-distance mesh hits choose the lowest source triangle index deterministically.

### RayHit3D — Hit result

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Distance` | Float | read | Distance from ray origin to hit point |
| `Point` | Vec3 | read | World-space hit position |
| `Normal` | Vec3 | read | Surface normal at hit point |
| `TriangleIndex` | Integer | read | Index of hit triangle in mesh |

### AABB3D — Box collision

| Function | Signature | Description |
|----------|-----------|-------------|
| `AABB3D.Overlaps(minA, maxA, minB, maxB)` | `i1(obj, obj, obj, obj)` | Boolean overlap test |
| `AABB3D.Penetration(minA, maxA, minB, maxB)` | `obj(obj, obj, obj, obj)` | Minimum push-out Vec3 |
| `AABB3D.ClosestPoint(min, max, point)` | `obj(obj, obj, obj)` | Closest point on AABB to a point |
| `AABB3D.SphereOverlaps(min, max, center, radius)` | `i1(obj, obj, obj, f64)` | AABB vs sphere overlap test |

### Sphere3D — Sphere collision

| Function | Signature | Description |
|----------|-----------|-------------|
| `Sphere3D.Overlaps(centerA, radiusA, centerB, radiusB)` | `i1(obj, f64, obj, f64)` | Sphere vs sphere overlap |
| `Sphere3D.Penetration(centerA, radiusA, centerB, radiusB)` | `obj(obj, f64, obj, f64)` | Push-out Vec3 |

### Segment3D — Line segment

| Function | Signature | Description |
|----------|-----------|-------------|
| `Segment3D.ClosestPoint(segA, segB, point)` | `obj(obj, obj, obj)` | Closest point on segment to a point |

### Capsule3D — Capsule collision

| Function | Signature | Description |
|----------|-----------|-------------|
| `Capsule3D.SphereOverlaps(capA, capB, capR, center, radius)` | `i1(obj, obj, f64, obj, f64)` | Capsule vs sphere overlap |
| `Capsule3D.AABBOverlaps(capA, capB, capR, min, max)` | `i1(obj, obj, f64, obj, obj)` | Capsule vs AABB overlap |

Ray/AABB/capsule helpers validate `Vec3`, `Mat4`, mesh, and hit handles. Non-finite rays or
dimensions return a miss or safe zero result. AABB helpers canonicalize inverted min/max bounds,
penetration vectors push the first shape out of the second, and capsule-vs-AABB uses exact
segment-to-box distance rather than only testing against the box center.

### Zia Example

```zia
module RaycastDemo;

bind Zanna.Graphics3D.Ray3D;
bind Zanna.Graphics3D.RayHit3D;
bind Zanna.Graphics3D.AABB3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Math.Vec3;

func start() {
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);
    var origin = Camera3D.get_Position(cam);
    var dir = Camera3D.get_Forward(cam);

    // Ray-sphere test
    var center = Vec3.New(5.0, 0.0, 0.0);
    var dist = Ray3D.IntersectSphere(origin, dir, center, 1.0);

    // Ray-mesh test
    var hit = Ray3D.IntersectMesh(origin, dir, mesh, transform);
    if (hit != null) {
        var point = RayHit3D.get_Point(hit);
        var normal = RayHit3D.get_Normal(hit);
        var tri = RayHit3D.get_TriangleIndex(hit);
    }

    // AABB overlap
    var minA = Vec3.New(-1.0, -1.0, -1.0);
    var maxA = Vec3.New(1.0, 1.0, 1.0);
    var minB = Vec3.New(0.0, 0.0, 0.0);
    var maxB = Vec3.New(2.0, 2.0, 2.0);
    var overlaps = AABB3D.Overlaps(minA, maxA, minB, maxB);
    var pushout = AABB3D.Penetration(minA, maxA, minB, maxB);
}
```

## FPS Camera

First-person camera controller with yaw/pitch mouse look and WASD movement. These methods are on the Camera3D class (documented above in the Camera3D section).

```zia
module FPSDemo;

bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Input.Mouse;
bind Zanna.Math.Vec3;

func start() {
    var canvas = Canvas3D.New("FPS", 800, 600);
    var cam = Camera3D.New(70.0, 800.0 / 600.0, 0.1, 200.0);
    Camera3D.LookAt(cam, Vec3.New(0.0, 1.7, 0.0), Vec3.New(0.0, 1.7, -1.0), Vec3.New(0.0, 1.0, 0.0));
    Camera3D.FirstPersonInit(cam);
    Mouse.Capture();

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        var dt = Canvas3D.get_DeltaTime(canvas) / 1000.0;
        var mdx = Mouse.DeltaX() * 0.1;
        var mdy = Mouse.DeltaY() * 0.1;
        Camera3D.FirstPersonUpdate(cam, mdx, -mdy, 0.0, 0.0, 0.0, 5.0, dt);

        Canvas3D.Clear(canvas, 0.1, 0.1, 0.2);
        Canvas3D.Begin(canvas, cam);
        // ... draw scene ...
        Canvas3D.End(canvas);
        Canvas3D.DrawCrosshair(canvas, 0xFFFFFF, 12);
        Canvas3D.Flip(canvas);
    }
}
```

- `FPSInit` decomposes the current view matrix to extract yaw/pitch
- `FPSUpdate(mdx, mdy, fwd, right, up, speed, dt)` accumulates yaw/pitch, clamps pitch to +/-89 degrees, applies WASD movement
- `Yaw`/`Pitch` properties allow reading/writing the current angles and rebuild the view immediately
- Use `Mouse.Capture()` to hide cursor and enable warp-to-center mouse tracking

## Spatial Audio

Spatial-audio math and low-level `Zanna.Audio.SpatialAudio3D` playback live in
the audio runtime. Graphics3D keeps only the `SoundListener3D` and
`SoundSource3D` wrappers that bind listeners/sources to `SceneNode` and
`Camera3D`.

Recommended frame order for scene-driven audio:

1. Move cameras and scene nodes.
2. Call `SceneGraph.SyncBindings(dt)`.
3. Trigger `SoundSource3D.Play()` or `SpatialAudio3D.PlayAt(...)` calls for the frame.

See [Audio: Spatial Audio](zannalib/audio.md#spatial-audio) for the full
spatial API and [Audio: Mix Group Effects](zannalib/audio.md#mix-group-effects)
for group-level low-pass, EQ, delay, and reverb.

### Zia Example

```zia
module Sound3DObjectsDemo;

bind Zanna.Graphics3D;
bind Zanna.Math;
bind Zanna.Audio;

func start() {
    var cam = Camera3D.New(60.0, 1.0, 0.1, 100.0);
    Camera3D.LookAt(
        cam, Vec3.New(0.0, 2.0, 6.0), Vec3.New(0.0, 1.5, 0.0), Vec3.New(0.0, 1.0, 0.0));

    var node = SceneNode.New();
    SceneNode.SetPosition(node, 3.0, 0.5, -1.0);

    var listener = SoundListener3D.New();
    listener.BindCamera(cam);
    listener.IsActive = true;

    var source = SoundSource3D.New(Synth.Tone(523, 220, 0));
    source.BindNode(node);
    source.RefDistance = 2.0;
    source.MaxDistance = 20.0;
    source.Volume = 75;

    SpatialAudio3D.SyncBindings(0.016);

    if (Audio.IsAvailable() && Audio.Init() != 0) {
        var voice = source.Play();
        source.Stop();
    }
}
```

- Linear distance attenuation stays at full volume through `refDist`, then falls to zero at `maxDist`
- Pan is derived from the listener's right vector and the source direction in world space
- `SpatialAudio3D.PlayAt` still records per-voice `max_distance`, and `UpdateVoice(..., 0.0)` reuses that stored value
- `SoundSource3D.DopplerFactor` exposes the latest factor computed from listener/source velocity; the current mixer applies volume and pan, with playback-rate application reserved for rate-capable backends

## Mouse Capture

For FPS-style games, capture the mouse to prevent it from leaving the window:

```zia
Mouse.Capture()   // hides cursor + warps to center each frame
Mouse.Release()   // restores cursor
```

When captured, `Mouse.DeltaX()`/`Mouse.DeltaY()` report movement from center. The cursor is hidden and warped to the window center each `Canvas3D.Poll()` call. Only active when the window has focus.

---

## Physics3D

Impulse-based 3D rigid body simulation with box, sphere, and capsule collision shapes.
Bodies now track quaternion orientation and angular velocity in addition to linear motion.
Shape-specific narrow-phase collision: sphere-sphere uses radial distance (not AABB),
box-sphere uses closest-point projection in the box's oriented local space. Collision detection uses a body-centric sweep-and-prune broadphase
before narrow-phase tests. That broadphase is intentionally separate from the render-facing `SceneGraph` BVH because it indexes all collider bodies, applies layer/mask and static-static solver filters, and preserves contact-event identity; mesh narrow-phase then traverses only the selected collider's per-mesh BVH for sphere, capsule, box, and convex-hull contacts. Non-trigger contacts apply impulses at the contact point, so off-center
hits update angular velocity as well as linear velocity. Coulomb friction and Baumgarte positional
correction are applied to non-trigger contacts.

Capsule primitive collision honors body orientation for capsule-vs-capsule, capsule-vs-sphere,
capsule-vs-box, mesh, and heightfield tests. Box primitives honor body and compound-child
orientation; the `NewAABB` name is kept as a compatibility factory for box bodies.
`Raycast` tests actual collider geometry for boxes, spheres, capsules, compound leaves, mesh/convex
triangles, and heightfields. Sphere and capsule sweeps use adaptive sampling so small-radius sweeps
and long capsules can hit thin geometry.
Mesh/convex colliders reuse a per-mesh BVH to prune candidate triangles for sphere, capsule, box,
and convex-hull contacts, falling back to a full triangle scan if the BVH path is unavailable.
SixDof joints measure angular limits as per-axis pose angles relative to the bodies' creation
orientation, then stop angular velocity that would push locked or already-limited axes farther out.

### Physics3DWorld

World storage for bodies, contacts, contact events, and joints grows on demand from production-sized initial capacities. Query result lists are still bounded for predictable allocation behavior.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(gx, gy, gz)` | `obj(f64, f64, f64)` | Create world with gravity vector |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `BodyCount` | Integer | read | Number of active bodies |
| `CollisionCount` | Integer | read | Number of contacts from last Step |
| `CollisionEventCount` | Integer | read | Number of current collision events from last Step |
| `EnterEventCount` | Integer | read | Number of collision pairs that began touching this step |
| `StayEventCount` | Integer | read | Number of collision pairs still touching this step |
| `ExitEventCount` | Integer | read | Number of collision pairs that stopped touching this step |
| `JointCount` | Integer | read | Number of active joints |
| `LastCcdRequestedSubsteps` | Integer | read | Unclamped CCD substeps requested by the last step |
| `LastCcdSubsteps` | Integer | read | CCD substeps actually used after capping |
| `CcdSubstepClampedCount` | Integer | read | Number of steps that exceeded the CCD cap |
| `SolverIterations` | Integer | read/write | Velocity contact/joint solver passes used by `Step()`; default `6`, clamped to `1..64` |
| `PositionIterations` | Integer | read/write | Contact position-correction passes; default `1`, clamped to `1..64` |
| `ContactBeta` | Float | read/write | Baumgarte contact recovery fraction; default `0.8`, clamped to `0..1` |
| `RestitutionThreshold` | Float | read/write | Minimum impact speed in m/s that bounces; default `0.5`, clamped non-negative |
| `FixedStepAlpha` | Float | read | Fixed-step accumulator interpolation fraction |
| `DroppedFixedSteps` | Integer | read | Fixed steps discarded by `StepFixed()` under the max-step guard |
| `LastSolverIslandCount` | Integer | read | Max active contact islands scheduled by the most recent `Step()` |
| `LastSolverActiveBodyCount` | Integer | read | Max awake dynamic bodies included in contact islands by the most recent `Step()` |
| `LastSolverContactCount` | Integer | read | Max non-trigger contacts scheduled through contact islands by the most recent `Step()` |

| Method | Signature | Description |
|--------|-----------|-------------|
| `Step(dt)` | `void(f64)` | Advance simulation by dt seconds |
| `StepFixed(dt, fixedDt, maxSteps)` | `i64(f64, f64, i64)` | Accumulate variable frame time and run up to `maxSteps` fixed `fixedDt` steps |
| `Add(body)` | `void(obj)` | Add body to world |
| `TryAdd(body)` | `i1(obj)` | Add body and return whether it is present afterward |
| `Remove(body)` | `void(obj)` | Remove body from world |
| `ContainsBody(body)` | `i1(obj)` | Return whether the body is currently registered in the world |
| `SetGravity(x, y, z)` | `void(f64, f64, f64)` | Update gravity |
| `AddJoint(joint, type)` | `void(obj, i64)` | Add joint (type: 0=distance, 1=spring) |
| `RemoveJoint(joint)` | `void(obj)` | Remove joint |
| `Raycast(origin, direction, maxDistance, mask)` | `obj(obj, obj, f64, i64)` | Return the nearest `PhysicsHit3D` or `none` |
| `RaycastAll(origin, direction, maxDistance, mask)` | `obj(obj, obj, f64, i64)` | Return a sorted `PhysicsHitList3D` or `none` |
| `SweepSphere(center, radius, delta, mask)` | `obj(obj, f64, obj, i64)` | Sweep a sphere and return the first `PhysicsHit3D` or `none` |
| `SweepCapsule(a, b, radius, delta, mask)` | `obj(obj, obj, f64, obj, i64)` | Sweep a capsule segment and return the first `PhysicsHit3D` or `none` |
| `OverlapSphere(center, radius, mask)` | `obj(obj, f64, i64)` | Return a `PhysicsHitList3D` of overlaps or `none` |
| `OverlapAABB(min, max, mask)` | `obj(obj, obj, i64)` | Return a `PhysicsHitList3D` of overlaps or `none` |
| `GetCollisionBodyA(index)` | `obj(i64)` | Get first body in contact pair |
| `GetCollisionBodyB(index)` | `obj(i64)` | Get second body in contact pair |
| `GetCollisionNormal(index)` | `obj(i64)` | Get contact normal Vec3 (A->B) |
| `GetCollisionDepth(index)` | `f64(i64)` | Get penetration depth |
| `GetCollisionEvent(index)` | `obj(i64)` | Get current `CollisionEvent3D` |
| `GetEnterEvent(index)` | `obj(i64)` | Get `CollisionEvent3D` from the enter bucket |
| `GetStayEvent(index)` | `obj(i64)` | Get `CollisionEvent3D` from the stay bucket |
| `GetExitEvent(index)` | `obj(i64)` | Get `CollisionEvent3D` from the exit bucket |

Notes:
- Query `mask` uses the same layer bit semantics as body collision layers. `0` matches no layers; use `-1`/all bits for "match any layer".
- Queries include trigger bodies and mark them through `PhysicsHit3D.IsTrigger`.
- Overlap, raycast, and sweep queries reuse the physics broadphase/query cache and include body collision scale before shape tests.
- CCD diagnostics are tuning counters; a non-zero clamp count means fast bodies requested more substeps than the runtime cap allows.
- `Raycast` and `RaycastAll` are true shape queries for boxes, spheres, capsules, compound leaves, mesh/convex triangles, and heightfields. Use `SweepSphere` or `SweepCapsule` for volume casts.
- `GetContactSeparation()` returns negative values while penetrating and positive values when separated.

### PhysicsHit3D

`PhysicsHit3D` is the result object returned by `Raycast`, `SweepSphere`, and `SweepCapsule`.

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Body` | Object | read | Hit `Physics3DBody` |
| `Collider` | Object | read | Hit `Collider3D` leaf collider |
| `Point` | Vec3 | read | Contact point approximation |
| `Normal` | Vec3 | read | Surface normal at the hit |
| `Distance` | Float | read | World-space distance travelled before the hit |
| `Fraction` | Float | read | `Distance / maxDistance` for sweeps and raycasts |
| `StartedPenetrating` | Boolean | read | Query began already overlapping the target |
| `IsTrigger` | Boolean | read | Hit body is trigger-only |

### PhysicsHitList3D

`PhysicsHitList3D` is returned by `RaycastAll`, `OverlapSphere`, and `OverlapAABB`.

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Count` | Integer | read | Number of hits in the list |

| Method | Signature | Description |
|--------|-----------|-------------|
| `Get(index)` | `obj(i64)` | Return hit `index` as a `PhysicsHit3D` |

### CollisionEvent3D

`CollisionEvent3D` is the structured per-pair contact event produced by `PhysicsWorld3D.Step()`.

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `BodyA` | Object | read | First body in the pair |
| `BodyB` | Object | read | Second body in the pair |
| `ColliderA` | Object | read | Leaf collider for body A |
| `ColliderB` | Object | read | Leaf collider for body B |
| `IsTrigger` | Boolean | read | Pair includes at least one trigger body |
| `ContactCount` | Integer | read | Number of contact points in the event; AABB and face-contact OBB box pairs can expose up to four manifold points, while other pairs currently expose one representative point |
| `RelativeSpeed` | Float | read | Relative speed along the contact normal before resolution |
| `NormalImpulse` | Float | read | Solver normal impulse from the last step (`0` for trigger pairs) |

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetContact(index)` | `obj(i64)` | Return `ContactPoint3D` for the manifold point |
| `GetContactPoint(index)` | `obj(i64)` | Return manifold point position as `Vec3` |
| `GetContactNormal(index)` | `obj(i64)` | Return manifold point normal as `Vec3` |
| `GetContactSeparation(index)` | `f64(i64)` | Return signed separation (`< 0` means penetration) |

### ContactPoint3D

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Point` | Vec3 | read | Contact point position |
| `Normal` | Vec3 | read | Contact normal |
| `Separation` | Float | read | Signed separation (`< 0` while penetrating) |

---

### Collider3D

`Collider3D` is the reusable shape object for 3D physics. Prefer authoring colliders first and
then attaching them to `Physics3DBody`; the old body shape constructors remain as convenience
wrappers for simple cases.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Box(hx, hy, hz)` | `obj(f64, f64, f64)` | Box collider with half-extents |
| `Sphere(radius)` | `obj(f64)` | Sphere collider |
| `Capsule(radius, height)` | `obj(f64, f64)` | Capsule collider authored along local Y; `height` is total height including caps, and values below `2*radius` collapse to a sphere-like capsule |
| `NewConvexHull(mesh)` | `obj(obj)` | Convex-hull collider sourced from a `Mesh3D` |
| `NewMesh(mesh)` | `obj(obj)` | Static triangle-mesh collider |
| `NewHeightfield(heightmap, sx, sy, sz)` | `obj(obj, f64, f64, f64)` | Static heightfield collider from `Pixels` |
| `NewCompound()` | `obj()` | Empty compound collider for child composition |

`NewBox`, `NewSphere`, and `NewCapsule` remain available as compatibility
aliases for collider factories.

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Type` | Integer | read | Collider kind: `0=box`, `1=sphere`, `2=capsule`, `3=convexHull`, `4=mesh`, `5=compound`, `6=heightfield` |

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddChild(child, localTransform)` | `void(obj, obj)` | Add a child collider to a compound collider |
| `GetLocalBoundsMin()` | `obj()` | Local-space AABB minimum as `Vec3` |
| `GetLocalBoundsMax()` | `obj()` | Local-space AABB maximum as `Vec3` |

Notes:
- `NewMesh` and `NewHeightfield` are static-only in v1. Attach them only to static bodies.
- `NewConvexHull` treats the mesh vertex cloud as a convex support set.
  Hull contacts against spheres, capsules, boxes, and other hulls use GJK/EPA,
  including contained primitive contacts. Triangle-mesh contacts against
  spheres, capsules, boxes, and convex hulls use BVH candidate pruning before
  triangle-level tests.
- Compound colliders are the preferred way to build complex dynamic bodies from simple children.

---

### Physics3DBody

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(mass)` | `obj(f64)` | Create an empty body and assign a collider later |
| `NewAABB(sx, sy, sz, mass)` | `obj(f64, f64, f64, f64)` | Box body (mass=0 for static); name retained for compatibility |
| `NewSphere(radius, mass)` | `obj(f64, f64)` | Sphere body |
| `NewCapsule(radius, height, mass)` | `obj(f64, f64, f64)` | Capsule body; `height` is total height including caps |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Collider` | Collider3D | read/write | Active collision shape for the body |
| `Position` | Vec3 | read | World position (set via `SetPosition`) |
| `Scale` | Vec3 | read | Collision scale applied to the collider |
| `Orientation` | Quat | read | World orientation (set via `SetOrientation`) |
| `Velocity` | Vec3 | read | Linear velocity (set via `SetVelocity`) |
| `AngularVelocity` | Vec3 | read | Angular velocity in radians/sec (set via `SetAngularVelocity`) |
| `Restitution` | Float | read/write | Bounciness, clamped to `0..1` |
| `Friction` | Float | read/write | Surface friction, clamped to finite non-negative values |
| `LinearDamping` | Float | read/write | Velocity damping per second |
| `AngularDamping` | Float | read/write | Spin damping per second |
| `CollisionLayer` | Integer | read/write | Bitmask layer |
| `CollisionMask` | Integer | read/write | Bitmask for which layers to collide with |
| `Static` | Boolean | read/write | Immovable body (mass-independent) |
| `Kinematic` | Boolean | read/write | Infinite-mass body driven by explicit linear/angular velocity |
| `Trigger` | Boolean | read/write | Overlap detection only, no physics response |
| `CanSleep` | Boolean | read/write | Allow automatic sleep when idle |
| `Sleeping` | Boolean | read | Body is asleep and skipped by dynamic integration |
| `UseCcd` | Boolean | read/write | Enable substep-based CCD for fast motion |
| `Grounded` | Boolean | read | Touching ground surface |
| `GroundNormal` | Vec3 | read | Surface normal of ground contact |
| `Mass` | Float | read | Body mass |

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Teleport body |
| `SetScale(x, y, z)` | `void(f64, f64, f64)` | Set per-body collider scale |
| `SetOrientation(quat)` | `void(obj)` | Set body orientation from a Quat |
| `SetVelocity(vx, vy, vz)` | `void(f64, f64, f64)` | Set linear velocity |
| `SetAngularVelocity(wx, wy, wz)` | `void(f64, f64, f64)` | Set angular velocity |
| `ApplyForce(fx, fy, fz)` | `void(f64, f64, f64)` | Accumulate force (applied per step) |
| `ApplyImpulse(ix, iy, iz)` | `void(f64, f64, f64)` | Instant velocity change |
| `ApplyTorque(tx, ty, tz)` | `void(f64, f64, f64)` | Accumulate torque (applied per step) |
| `ApplyAngularImpulse(ix, iy, iz)` | `void(f64, f64, f64)` | Instant angular velocity change |
| `Wake()` | `void()` | Wake a sleeping dynamic body |
| `Sleep()` | `void()` | Force a dynamic body into the sleeping state |

`NewAABB`, `NewSphere`, and `NewCapsule` now allocate a body, create the matching collider, and
attach it internally. Use `New(mass)` plus `body.Collider = collider` when you want reusable or advanced
shapes.

For a small headless example of the new rotation surface, see
`examples/apiaudit/graphics3d/physics3d_rotation_demo.zia`.
For the collider split and advanced-shape surface, see
`examples/apiaudit/graphics3d/collider3d_advanced_demo.zia`.
For world-space queries, see
`examples/apiaudit/graphics3d/physics3d_queries_demo.zia`.
For structured collision events, see
`examples/apiaudit/graphics3d/collisionevent3d_demo.zia`.

---

### Character3D

Controller-based character movement with slide-and-step collision response.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(radius, height, mass)` | `obj(f64, f64, f64)` | Create character controller |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `StepHeight` | Float | read/write | Max step-up height |
| `IsGrounded` | Boolean | read | On ground |
| `JustLanded` | Boolean | read | Landed this frame |
| `Position` | Vec3 | read | Current position |

| Method | Signature | Description |
|--------|-----------|-------------|
| `Move(direction, dt)` | `void(obj, f64)` | Move with collision response (Vec3 direction) |
| `SetSlopeLimit(degrees)` | `void(f64)` | Max climbable slope angle |
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Teleport |

---

### Trigger3D

AABB zone for enter/exit detection.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(x0, y0, z0, x1, y1, z1)` | `obj(f64 x6)` | Create AABB trigger zone |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `EnterCount` | Integer | read | Bodies that entered since last check |
| `ExitCount` | Integer | read | Bodies that exited since last check |

| Method | Signature | Description |
|--------|-----------|-------------|
| `Contains(position)` | `i1(obj)` | Point-in-zone test (Vec3) |
| `Update(world)` | `void(obj)` | Recompute occupancy once per frame against the physics world. Overlap tests each body's world AABB against the zone, so large bodies straddling the boundary register; tracked-body count is unbounded |
| `SetBounds(x0, y0, z0, x1, y1, z1)` | `void(f64 x6)` | Update zone bounds |

---

### DistanceJoint3D

Maintains a fixed distance between two body centers.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(bodyA, bodyB, distance)` | `obj(obj, obj, f64)` | Create distance joint |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Distance` | Float | read/write | Target distance |

Distance joints retain both body handles for the joint lifetime. Negative or non-finite target
distances are sanitized to zero, and the solver skips bodies with non-finite motion state.

---

### SpringJoint3D

Hooke's law spring with configurable stiffness and damping.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(bodyA, bodyB, restLen, stiffness, damping)` | `obj(obj, obj, f64, f64, f64)` | Create spring joint |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Stiffness` | Float | read/write | Spring constant |
| `Damping` | Float | read/write | Velocity damping factor |
| `RestLength` | Float | read | Equilibrium distance |

Spring joints retain both body handles for the joint lifetime. Rest length, stiffness, and damping
are non-negative finite values; invalid inputs become zero and very large values are clamped to keep
solver impulses finite.

### Vehicle3D

Raycast vehicle on a dynamic chassis body: wheels are suspension rays rather
than rigid bodies, so vehicles stay stable at speed. Lateral grip is a
load-scaled friction circle — unloaded wheels slide first.

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(world, chassisBody)` | `obj(obj, obj)` | Create a vehicle around a dynamic chassis |

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Speed` | Float | read | Signed m/s along the chassis local +Z forward axis |
| `WheelCount` | Integer | read | Wheels added so far |

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddWheel(x, y, z, radius, suspRest, stiffness, damping, steers, driven)` | `i64(f64 x7, i1, i1)` | Add a wheel at a chassis-local anchor; returns its index |
| `SetInput(throttle, brake, steer)` | `void(f64, f64, f64)` | Per-step controls: throttle/brake `0..1`, steer `-1..1` |
| `SetDriveForce(n)` / `SetBrakeForce(n)` | `void(f64)` | Force in newtons at driven/braked contact patches |
| `SetMaxSteer(degrees)` | `void(f64)` | Max steering angle for steerable wheels (clamped to 85°) |
| `SetGrip(longitudinal, lateral)` | `void(f64, f64)` | Tire friction-circle coefficients |
| `SetCollisionMask(mask)` | `void(i64)` | Layers the suspension rays treat as ground |
| `Step(dt)` | `void(f64)` | Cast suspension rays and apply forces; call before `PhysicsWorld3D.Step` |
| `WheelInContact(i)` / `WheelTravel(i)` / `WheelLoad(i)` | telemetry | Contact flag, current suspension length (m), suspension force (N) |

### Zia Example

```zia
module PhysicsDemo;

bind Zanna.Graphics3D.PhysicsWorld3D;
bind Zanna.Graphics3D.Physics3DBody;
bind Zanna.Graphics3D.Character3D;
bind Zanna.Graphics3D.Trigger3D;
bind Zanna.Graphics3D.DistanceJoint3D;
bind Zanna.Math.Vec3;

func start() {
    // Create physics world with gravity
    var world = PhysicsWorld3D.New(0.0, -9.8, 0.0);

    // Static ground
    var ground = Physics3DBody.NewAABB(100.0, 1.0, 100.0, 0.0);
    Physics3DBody.SetPosition(ground, 0.0, -0.5, 0.0);
    Physics3DBody.set_Static(ground, true);
    PhysicsWorld3D.Add(world, ground);

    // Dynamic sphere
    var ball = Physics3DBody.NewSphere(0.5, 1.0);
    Physics3DBody.SetPosition(ball, 0.0, 10.0, 0.0);
    Physics3DBody.set_Restitution(ball, 0.8);
    PhysicsWorld3D.Add(world, ball);

    // Distance joint between two bodies
    var anchor = Physics3DBody.NewSphere(0.2, 0.0);
    Physics3DBody.SetPosition(anchor, 0.0, 15.0, 0.0);
    Physics3DBody.set_Static(anchor, true);
    PhysicsWorld3D.Add(world, anchor);
    var joint = DistanceJoint3D.New(anchor, ball, 5.0);
    PhysicsWorld3D.AddJoint(world, joint, 0);

    // Character controller
    var player = Character3D.New(0.4, 1.8, 80.0);
    Character3D.SetPosition(player, 5.0, 1.0, 0.0);

    // Trigger zone
    var zone = Trigger3D.New(-2.0, 0.0, -2.0, 2.0, 3.0, 2.0);

    // Simulation loop
    PhysicsWorld3D.Step(world, 0.016);

    // Check collisions
    var n = Physics3DWorld.get_CollisionCount(world);
    for i in 0..n {
        var bodyA = Physics3DWorld.GetCollisionBodyA(world, i);
        var normal = Physics3DWorld.GetCollisionNormal(world, i);
    }

    // Check trigger
    Trigger3D.Update(zone, ball);
    var entered = Trigger3D.get_EnterCount(zone);
}
```

---

## Transform3D

Standalone 3D transform (position, rotation, scale) with lazy matrix computation.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create identity transform |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Position` | Vec3 | read | Current position |
| `Rotation` | Quat | read/write | Current rotation |
| `Scale` | Vec3 | read | Current scale |
| `Matrix` | Mat4 | read | Computed world matrix (lazy) |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Set position |
| `SetEuler(pitch, yaw, roll)` | `void(f64, f64, f64)` | Set rotation from Euler angles (degrees) |
| `SetScale(sx, sy, sz)` | `void(f64, f64, f64)` | Set scale |
| `Translate(delta)` | `void(obj)` | Move relative (Vec3) |
| `Rotate(axis, angle)` | `void(obj, f64)` | Rotate around axis (Vec3, angle in radians) |
| `LookAt(target, up)` | `void(obj, obj)` | Orient toward target (Vec3s) |

### Zia Example

```zia
module TransformDemo;

bind Zanna.Graphics3D.Transform3D;
bind Zanna.Math.Vec3;
bind Zanna.Math.Quat;

func start() {
    var xform = Transform3D.New();
    Transform3D.SetPosition(xform, 5.0, 0.0, 3.0);
    Transform3D.SetEuler(xform, 0.0, 45.0, 0.0);
    Transform3D.SetScale(xform, 2.0, 2.0, 2.0);

    // Incremental movement
    Transform3D.Translate(xform, Vec3.New(1.0, 0.0, 0.0));
    Transform3D.Rotate(xform, Vec3.New(0.0, 1.0, 0.0), 0.5);

    // Look at a target
    Transform3D.LookAt(xform, Vec3.New(0.0, 0.0, 0.0), Vec3.New(0.0, 1.0, 0.0));

    // Get computed matrix for rendering
    var mat = Transform3D.get_Matrix(xform);
}
```

---

## Sprite3D

Camera-facing billboard sprite in 3D space. Useful for particles, foliage, and NPC labels.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(texture)` | `obj(obj)` | Create billboard from Pixels texture |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `void(f64, f64, f64)` | Set world position |
| `SetScale(width, height)` | `void(f64, f64)` | Billboard size in world units |
| `SetAnchor(ax, ay)` | `void(f64, f64)` | Pivot point (0-1, default 0.5/0.5 = center) |
| `SetFrame(x, y, w, h)` | `void(i64, i64, i64, i64)` | Sprite sheet sub-rectangle (pixels) |

Draw via `Canvas3D.DrawSprite3D(sprite, camera)`. Mesh and material are cached internally.

### Zia Example

```zia
module Sprite3DDemo;

bind Zanna.Graphics3D.Sprite3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics.Pixels;

func start() {
    var canvas = Canvas3D.New("Sprite3D", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var tex = Pixels.Load("tree_billboard.png");
    var tree = Sprite3D.New(tex);
    Sprite3D.SetPosition(tree, 5.0, 1.0, -3.0);
    Sprite3D.SetScale(tree, 2.0, 3.0);
    Sprite3D.SetAnchor(tree, 0.5, 0.0); // bottom-center

    // In render loop:
    Canvas3D.Begin(canvas, cam);
    Canvas3D.DrawSprite3D(canvas, tree, cam);
    Canvas3D.End(canvas);
}
```

---

## Decal3D

Surface-projected quad for bullet holes, blood splatters, footprints.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(position, normal, size, material)` | `obj(obj, obj, f64, obj)` | Create decal aligned to surface (Vec3 pos, Vec3 normal, Float size, Material3D) |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Expired` | Boolean | read | True when lifetime reached |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetLifetime(seconds)` | `void(f64)` | Set auto-fade duration |
| `Update(dt)` | `void(f64)` | Advance lifetime |

Draw via `Canvas3D.DrawDecal(decal)`.

### Zia Example

```zia
module DecalDemo;

bind Zanna.Graphics3D.Decal3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.Vec3;

func start() {
    var canvas = Canvas3D.New("Decals", 800, 600);

    var bullet_mat = Material3D.Textured(bulletHolePixels);
    Material3D.set_Alpha(bullet_mat, 0.9);

    var decal = Decal3D.New(
        Vec3.New(2.0, 1.5, 0.01),  // position on wall
        Vec3.New(0.0, 0.0, 1.0),   // wall normal
        0.3,                         // size
        bullet_mat);
    Decal3D.SetLifetime(decal, 10.0);

    // In render loop:
    Decal3D.Update(decal, dt);
    if (Decal3D.get_IsExpired(decal) == false) {
        Canvas3D.DrawDecal(canvas, decal);
    }
}
```

---

## Water3D

Animated water surface with Gerstner wave simulation, texture support, and environment reflections.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(width, depth)` | `obj(f64, f64)` | Create water plane |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetHeight(y)` | `void(f64)` | Set world Y position |
| `SetWaveParams(speed, amplitude, frequency)` | `void(f64, f64, f64)` | Legacy single-wave parameters |
| `SetColor(r, g, b, a)` | `void(f64, f64, f64, f64)` | Water tint (0-1 float RGBA) |
| `SetTexture(pixels)` | `void(obj)` | Surface texture (Pixels) |
| `SetNormalMap(pixels)` | `void(obj)` | Wave normal map (Pixels) |
| `SetEnvMap(cubemap)` | `void(obj)` | Environment CubeMap3D for reflections |
| `SetReflectivity(r)` | `void(f64)` | Reflection strength [0.0-1.0] |
| `SetResolution(n)` | `void(i64)` | Grid resolution (8-256, default 64) |
| `AddWave(dirX, dirZ, speed, amplitude, wavelength)` | `void(f64, f64, f64, f64, f64)` | Add directional Gerstner wave (max 8) |
| `ClearWaves()` | `void()` | Remove all Gerstner waves |
| `Update(dt)` | `void(f64)` | Advance wave animation |

### Zia Example

```zia
module WaterDemo;

bind Zanna.Graphics3D.Water3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.CubeMap3D;

func start() {
    var canvas = Canvas3D.New("Water", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 200.0);

    var water = Water3D.New(100.0, 100.0);
    Water3D.SetHeight(water, 0.0);
    Water3D.SetColor(water, 0.0, 0.3, 0.5, 0.7);
    Water3D.SetResolution(water, 128);

    // Gerstner waves for realistic ocean
    Water3D.AddWave(water, 1.0, 0.0, 2.0, 0.3, 10.0);
    Water3D.AddWave(water, 0.7, 0.7, 1.5, 0.15, 7.0);
    Water3D.AddWave(water, -0.3, 1.0, 3.0, 0.1, 15.0);

    // Environment reflections
    Water3D.SetEnvMap(water, skyboxCubemap);
    Water3D.SetReflectivity(water, 0.6);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        var dt = Canvas3D.get_DeltaTime(canvas) / 1000.0;
        Water3D.Update(water, dt);

        Canvas3D.Clear(canvas, 0.5, 0.7, 0.9);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawWater(canvas, water, cam);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

**Gerstner waves:** When waves are added via `AddWave`, the water uses a sum of directional Gerstner waves instead of the legacy single sine wave. Each wave has a direction, speed, amplitude, and wavelength. Up to 8 waves can be combined for realistic ocean effects. Normals are computed from wave derivatives for correct lighting.

Procedural waves are represented internally as packed `MorphTarget3D` bases and
submitted through the same deformation path as ordinary morph animation. GPU
and software backends therefore render the same generated surface, reduced
graphics builds keep the same behavior, and the retained bounds conservatively
cover the moving water rather than describing only its rest plane.

`Water3D` clamps extreme sizes, heights, wave speeds, amplitudes, frequencies, and wavelengths before mesh generation so renderer-facing vertices and normals stay finite. The retained water mesh is rewritten in place across frames; only topology changes such as resolution/capacity changes rebuild the index buffer. If a generated mesh fails validation, the water surface clears the partial mesh and remains dirty so the next valid update can rebuild it.

`Update(0.0)` is valid: it rebuilds the mesh when resolution or wave settings are dirty without advancing animation time. `DrawWater` also performs that zero-delta rebuild if a surface has not been built yet or was invalidated by `SetResolution`.

Draw via `Canvas3D.DrawWater(water, camera)`.

See `examples/apiaudit/graphics3d/water_demo.zia` for a complete example.

---

## Terrain3D

Heightmap-based terrain with chunked rendering, LOD, and texture splatting.

This runtime object is drawn explicitly with `Canvas3D.DrawTerrain` and is not
currently persisted as a VSCN `SceneNode` attachment. Zanna Studio's visual
terrain tool instead sculpts the ordinary canonical scene mesh documented by
[ADR 0211](adr/0211-canonical-mesh-terrain-sculpting.md), ensuring that
authoring and normal scene loading use one exact surface.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(widthSegments, depthSegments)` | `obj(i64, i64)` | Create terrain grid |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetHeightmap(pixels)` | `void(obj)` | Load height from R+G channels of Pixels (16-bit) |
| `GeneratePerlin(noise, scale, octaves, persistence)` | `void(obj, f64, i64, f64)` | Generate heights from PerlinNoise (native fast path) |
| `SetMaterial(material)` | `void(obj)` | Set surface material |
| `SetScale(sx, sy, sz)` | `void(f64, f64, f64)` | Set terrain world size (Y = height scale) |
| `SetSplatMap(pixels)` | `void(obj)` | Set splat map (RGBA: weights for layers 0-3) |
| `SetLayerTexture(layer, pixels)` | `void(i64, obj)` | Set texture for splat layer (0-3) |
| `SetLayerScale(layer, scale)` | `void(i64, f64)` | Set UV tiling scale per layer |
| `GetHeightAt(x, z)` | `f64(f64, f64)` | Query height at world XZ position |
| `GetNormalAt(x, z)` | `obj(f64, f64)` | Query surface normal at world XZ (Vec3) |
| `SetLodDistances(near, far)` | `void(f64, f64)` | Set LOD transition distances (default 100/250) |
| `SetSkirtDepth(depth)` | `void(f64)` | Set skirt depth to hide LOD cracks |

### Zia Example

```zia
module TerrainDemo;

bind Zanna.Graphics3D.Terrain3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.PerlinNoise;

func start() {
    var canvas = Canvas3D.New("Terrain", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 500.0);

    var terrain = Terrain3D.New(128, 128);
    Terrain3D.SetScale(terrain, 200.0, 30.0, 200.0);

    // Generate heights from Perlin noise
    var noise = PerlinNoise.New(42);
    Terrain3D.GeneratePerlin(terrain, noise, 0.02, 6, 0.5);

    // Material and splatting
    Terrain3D.SetMaterial(terrain, Material3D.FromColor(0.3, 0.5, 0.2));
    Terrain3D.SetLayerTexture(terrain, 0, grassPixels);
    Terrain3D.SetLayerTexture(terrain, 1, rockPixels);
    Terrain3D.SetLayerScale(terrain, 0, 10.0);
    Terrain3D.SetLayerScale(terrain, 1, 5.0);
    Terrain3D.SetSplatMap(terrain, splatPixels);

    // LOD settings
    Terrain3D.SetLodDistances(terrain, 80.0, 200.0);
    Terrain3D.SetSkirtDepth(terrain, 2.0);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        Canvas3D.Clear(canvas, 0.5, 0.7, 0.9);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawTerrain(canvas, terrain);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

**Procedural generation:** Two approaches are supported:
1. **Zia-only:** Use `PerlinNoise.Octave2D()` to fill a `Pixels` buffer, then call `SetHeightmap()`. The heightmap uses 16-bit precision via R (high byte) + G (low byte) channels in `0xRRGGBBAA` pixel format.
2. **Native fast path:** Call `GeneratePerlin(noise, scale, octaves, persistence)` with a `PerlinNoise` object. This writes directly to the internal float heightmap, bypassing the Pixels intermediate for better performance on large terrains. The `noise` parameter is a `PerlinNoise` object, `scale` controls coordinate frequency, `octaves` sets detail layers (typically 4-8), and `persistence` controls amplitude decay (typically 0.4-0.6). Non-finite scale/persistence values are sanitized, octaves are clamped to `1..16`, and generated heights are clamped to `0..1`.

**Texture splatting:** When a splat map is set, the terrain blends 4 layer textures per-pixel during rasterization, weighted by the splat map RGBA channels. Each layer can have its own UV tiling scale for detail repetition. The software, Metal, OpenGL, and D3D11 backends all perform per-pixel splat sampling. Backend splatting is enabled only when the control map and all four layer textures are present; incomplete splat sets render with the base material/fallback texture instead of sampling missing layers. A `1x1` splat map is valid and acts as uniform coverage for the whole terrain. Any baked fallback texture is stored in the standard `Pixels` format, `0xRRGGBBAA`.

**LOD (Level of Detail):** Terrain chunks use 3 resolution levels based on distance from the camera:
- LOD 0 (full): 16x16 quads per chunk (nearest chunks)
- LOD 1 (half): 8x8 quads per chunk (mid-range)
- LOD 2 (quarter): 4x4 quads per chunk (distant)

Configure with `SetLodDistances(nearDist, farDist)` — chunks closer than `nearDist` use LOD 0, between `nearDist` and `farDist` use LOD 1, beyond `farDist` use LOD 2. Default: 100/250. Invalid distances are sanitized so `farDist` stays greater than `nearDist`. Chunks outside the camera frustum are culled entirely (not drawn). Skirt geometry (`SetSkirtDepth(depth)`) hides cracks at LOD transitions by extending chunk edges downward and is included in chunk bounds, so visible skirts are not clipped by frustum culling. Invalid or negative skirt depths disable skirts. Edge chunks always include their far row/column endpoints at coarser LODs, so partial edge chunks still produce triangles.

Draw via `Canvas3D.DrawTerrain(terrain)` during a normal 3D `Begin`/`End` pass, or `Canvas3D.DrawTerrainAt(terrain, x, y, z)` to place the terrain's grid origin at a world-space offset (e.g. `-half, 0, -half` to center an origin-symmetric playfield). Terrain is not valid inside `Begin2D()`.

See `examples/apiaudit/graphics3d/procedural_terrain_demo.zia` and `terrain_lod_demo.zia` for complete examples.

---

## InstanceBatch3D

Draw many copies of one mesh with different transforms in a single draw call.
Transforms passed to `Add` and `Set` are copied into finite float matrices; any non-finite element is
replaced with the corresponding identity-matrix value before culling or backend submission.
GPU instanced draws synthesize previous-instance matrices when the caller does not provide them, so
motion-vector consumers get stable no-streak first frames. Raw instanced submissions separate motion
history by batch buffer identity; reuse the same matrix buffer across frames for continuous history.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(mesh, material)` | `obj(obj, obj)` | Create batch for a Mesh3D+Material3D pair |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Count` | Integer | read | Number of instances |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(transform)` | `void(obj)` | Add instance with Mat4 transform |
| `Remove(index)` | `void(i64)` | Remove instance by index |
| `Set(index, transform)` | `void(i64, obj)` | Update instance Mat4 transform |
| `Clear()` | `void()` | Remove all instances |

Draw via `Canvas3D.DrawInstanced(batch)`.

Instanced motion-history keys use stable mesh/material/count/index identity rather than the
transient transform-buffer address, so reallocating an instance buffer does not reset motion vectors.

### Zia Example

```zia
module InstanceDemo;

bind Zanna.Graphics3D.InstanceBatch3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Graphics3D.Material3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("Instancing", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    var tree = Mesh3D.Cylinder(0.2, 2.0, 6);
    var mat = Material3D.FromColor(0.3, 0.5, 0.1);
    var batch = InstanceBatch3D.New(tree, mat);

    // Place 100 trees
    for i in 0..100 {
        var x = (i % 10) * 3.0;
        var z = (i / 10) * 3.0;
        InstanceBatch3D.Add(batch, Mat4.Translate(x, 0.0, z));
    }

    // Render loop
    Canvas3D.Begin(canvas, cam);
    Canvas3D.DrawInstanced(canvas, batch);
    Canvas3D.End(canvas);
}
```

---

## NavMesh3D

Navigation mesh with A* pathfinding for AI characters. `Build` requires manifold shared edges:
more than two triangles on one undirected edge is rejected because adjacency would be ambiguous.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Build(mesh, agentRadius, agentHeight)` | `obj(obj, f64, f64)` | Build navmesh from Mesh3D geometry |
| `Bake(scene, agentRadius, agentHeight, maxSlope, cellSize)` | `obj(obj, f64, f64, f64, f64)` | Build navmesh from Mesh3D-bearing SceneGraph nodes |
| `BakeTiled(scene, tileSize, agentRadius, agentHeight, maxSlope, cellSize)` | `obj(obj, f64, f64, f64, f64, f64)` | Build a tiled voxel navmesh with retained tile source data |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `TriangleCount` | Integer | read | Number of triangles in navmesh |
| `OffMeshLinkCount` | Integer | read | Number of authored traversal links |
| `ObstacleCount` | Integer | read | Number of authored coarse AABB obstacles |
| `LastPathCost` | Float | read | Weighted cost of the latest successful path query |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `FindPath(start, goal)` | `obj(obj, obj)` | A* pathfinding (Vec3 start/goal, returns waypoint list) |
| `FindPathOption(start, goal)` | `obj<Zanna.Option>(obj, obj)` | A* pathfinding as `Some(path)`, or `None` |
| `SamplePosition(position)` | `obj(obj)` | Snap position to nearest point on navmesh (Vec3) |
| `IsWalkable(position)` | `i1(obj)` | Check if Vec3 position is on the navmesh |
| `AddOffMeshLink(from, to, bidirectional)` | `i1(obj, obj, i1)` | Add a directed or bidirectional traversal edge between walkable points |
| `SetOffMeshLinkMetadata(index, kind, cost, state)` | `i1(i64, str, f64, i64)` | Attach kind/cost/state metadata to an off-mesh link |
| `GetOffMeshLinkKind(index)` | `str(i64)` | Return the off-mesh link kind string |
| `GetOffMeshLinkTraversalCost(index)` | `f64(i64)` | Return the off-mesh link traversal-cost multiplier |
| `GetOffMeshLinkState(index)` | `i64(i64)` | Return off-mesh link state flags |
| `AddObstacle(min, max)` | `i1(obj, obj)` | Add a coarse AABB obstacle and re-carve affected triangles |
| `RemoveObstacle(index)` | `i1(i64)` | Remove a coarse obstacle and re-carve affected triangles |
| `UpdateObstacle(index, min, max)` | `i1(i64, obj, obj)` | Move/resize a coarse obstacle and re-carve affected triangles |
| `SetArea(min, max, area, cost)` | `i1(obj, obj, str, f64)` | Assign area/cost metadata to polygons in a volume |
| `GetArea(position)` | `str(obj)` | Return the area name at a walkable position |
| `GetTraversalCost(position)` | `f64(obj)` | Return the traversal-cost multiplier at a walkable position |
| `RebuildTile(tileX, tileZ)` | `i1(i64, i64)` | Rebuild one retained tiled-bake voxel source tile |
| `SetMaxSlope(degrees)` | `void(f64)` | Update walkability slope threshold |
| `DebugDraw(canvas)` | `void(obj)` | Visualize navmesh wireframe on Canvas3D |

Prefer `FindPathOption()` for new code. `FindPath()` remains available for
compatibility with existing `null` checks.

`Build()` stores the source walkable geometry separately from the filtered navigation triangles. `Bake()` gathers every `Mesh3D` attached under a `SceneGraph`, applies each node's world transform, and runs the voxel baker. `BakeTiled()` keeps retained voxel-cell source data for each tile; `RebuildTile()` refreshes only the requested tile's geometry, heights, and blocked state from that retained source instead of running a whole-scene bake. `SetMaxSlope()` refilters preserved source triangles. Slope tests use upward-facing triangle planes. Shared-edge portals narrower than `agentRadius * 2` are not linked, so wider agents do not path through narrow authored passages. `SamplePosition()` projects to the closest point on the nearest walkable triangle instead of snapping to a centroid. Off-mesh samples search the retained spatial index in expanding cell rings and stop once a conservative distance bound proves that no unvisited cell can improve the result; a fixed cell/reference budget falls back to an exact triangle pass for pathological or unindexed meshes. `FindPath()` / `FindPathOption()` lease one of four independently mutable A* workspaces per navmesh, allowing ordinary worker queries to overlap while bounding retained scratch memory. Path and walkability queries require the query height to be near the triangle plane so stacked floors or points far above the mesh do not alias to the wrong layer.

`AddOffMeshLink()` stores authored endpoint pairs such as jumps, ladders, and drop-downs. Both endpoints must resolve to current walkable polygons; pathfinding treats the link as an extra graph edge and emits the link endpoints as waypoints. `SetOffMeshLinkMetadata()` records the link kind, traversal-cost multiplier, and state flags; link cost contributes to A* and the final `LastPathCost`. `SetArea()` tags polygons whose exact XZ footprint intersects a finite volume, `GetArea()` / `GetTraversalCost()` query that metadata, and polygon traversal costs weight A* edges. `AddObstacle()` stores a finite world-space AABB and removes polygons whose triangle footprint intersects the obstacle volume. On tiled bakes, obstacle adds/removes/updates re-carve only overlapped tiles; non-tiled meshes still refilter the preserved source mesh. This remains polygon-level AABB carving rather than clipped sub-polygons; `NavAgent3D` covers local crowd avoidance.

### Zia Example

```zia
module NavMeshDemo;

bind Zanna.Graphics3D.NavMesh3D;
bind Zanna.Graphics3D.Mesh3D;
bind Zanna.Math.Vec3;

func start() {
    var level_mesh = Mesh3D.FromObj("level.obj");
    var nav = NavMesh3D.Build(level_mesh, 0.4, 1.8);
    NavMesh3D.SetMaxSlope(nav, 45.0);
    NavMesh3D.AddObstacle(nav, Vec3.New(1.0, -0.1, 1.0), Vec3.New(2.0, 2.0, 2.0));
    NavMesh3D.UpdateObstacle(nav, 0, Vec3.New(1.5, -0.1, 1.0), Vec3.New(2.5, 2.0, 2.0));
    NavMesh3D.RebuildTile(nav, 0, 0);

    var start = Vec3.New(0.0, 0.0, 0.0);
    var goal = Vec3.New(20.0, 0.0, 15.0);
    NavMesh3D.AddOffMeshLink(nav, Vec3.New(4.0, 0.0, 2.0), Vec3.New(8.0, 0.0, 2.0), true);
    var path = NavMesh3D.FindPathOption(nav, start, goal);

    // Snap a position to the navmesh
    var snapped = NavMesh3D.SamplePosition(nav, Vec3.New(5.0, 2.0, 5.0));

    // Check walkability
    var ok = NavMesh3D.IsWalkable(nav, Vec3.New(10.0, 0.0, 10.0));
}
```

---

## NavAgent3D

Goal-driven navigation agent built on top of `NavMesh3D`. `NavAgent3D` owns a target, keeps an internal waypoint path, exposes steering state, and can either move a bound `Character3D` or directly reposition a bound `SceneNode`.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(navmesh, radius, height)` | `obj(obj, f64, f64)` | Create a navigation agent for a specific `NavMesh3D` |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Position` | `Vec3` | read | Current world-space agent position |
| `Velocity` | `Vec3` | read | Actual motion from the last update |
| `DesiredVelocity` | `Vec3` | read | Steering velocity requested by the path follower |
| `HasPath` | `Boolean` | read | Whether the agent currently has an active route |
| `RemainingDistance` | `Float` | read | Remaining linear distance along the current route |
| `StoppingDistance` | `Float` | read/write | Arrival radius around the final target |
| `DesiredSpeed` | `Float` | read/write | Preferred movement speed in world units per second |
| `AutoRepath` | `Boolean` | read/write | Periodically rebuild the path while a target is active |
| `AvoidanceEnabled` | `Boolean` | read/write | Enable same-NavMesh RVO-style steering against other enabled agents |
| `AvoidanceRadius` | `Float` | read/write | Radius used by local RVO avoidance; defaults to the agent radius and clamps to `>= 0` |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTarget(position)` | `void(obj)` | Set a new goal position and rebuild the path immediately |
| `ClearTarget()` | `void()` | Drop the current goal and clear steering state |
| `Update(dt)` | `void(f64)` | Advance path following and push motion into the current bindings |
| `Warp(position)` | `void(obj)` | Teleport the agent, clear stale motion, and rebuild if a target still exists |
| `BindCharacter(controller)` | `void(obj)` | Drive a `Character3D` with the agent's desired motion |
| `BindNode(node)` | `void(obj)` | Mirror the agent position into a `SceneNode` in world space |

### Recommended Pattern

1. Build or load a `NavMesh3D`.
2. Create a `NavAgent3D`.
3. Bind either a `Character3D` or a `SceneNode`.
4. Call `SetTarget(...)`.
5. Call `Update(dt)` every frame.

When both a `Character3D` and a `SceneNode` are bound, the character controller is authoritative and the node is updated to match it.

`AvoidanceEnabled` is opt-in. When enabled on nearby agents sharing the same `NavMesh3D`, the path follower solves a deterministic reciprocal-velocity-obstacle candidate set over nearby grid peers before moving the bound character or node. The solver predicts collisions over a bounded horizon, prefers the path-following velocity, and uses a stable right-side tie-break for symmetric passes. A named CTest fixture records a 200-agent pathing/avoidance baseline.

Native hosts that update crowds can call
`rt_navagent3d_update_batch(void *const *agents, int64_t count, double dt)`.
The additive C API filters invalid/duplicate handles, prepares all selected
agents, snapshots start-of-tick positions and velocities once, solves avoidance
without reading live mutations, and applies results in stable creation order.
Reversing the caller's array therefore produces the same desired velocities,
actual velocities, and positions. The scripting `Update(dt)` method remains the
compatible single-agent path.

### Zia Example

```zia
module NavAgentDemo;

bind Zanna.Graphics3D;
bind Zanna.Math;
bind Zanna.Terminal;

func start() {
    var mesh = Mesh3D.Plane(20.0, 20.0);
    var nav = NavMesh3D.Build(mesh, 0.4, 1.8);
    var agent = NavAgent3D.New(nav, 0.4, 1.8);
    var node = SceneNode.New();

    NavAgent3D.BindNode(agent, node);
    NavAgent3D.set_DesiredSpeed(agent, 5.0);
    NavAgent3D.set_AvoidanceEnabled(agent, true);
    NavAgent3D.set_AvoidanceRadius(agent, 0.6);
    NavAgent3D.Warp(agent, Vec3.New(0.0, 0.0, 0.0));
    NavAgent3D.SetTarget(agent, Vec3.New(4.0, 0.0, 3.0));

    var i = 0;
    while (i < 20) {
        NavAgent3D.Update(agent, 0.1);
        i = i + 1;
    }

    var pos = NavAgent3D.get_Position(agent);
    Say("Agent X = " + toString(Vec3.get_X(pos)));
    Say("Agent Z = " + toString(Vec3.get_Z(pos)));
    Say("RemainingDistance = " + toString(NavAgent3D.get_RemainingDistance(agent)));
}
```

---

## Path3D

Spline path for camera rails, patrol routes, and scripted movement.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New()` | `obj()` | Create empty path |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Length` | Float | read | Total path length |
| `PointCount` | Integer | read | Number of control points |
| `Looping` | Boolean | write | Connect last point to first |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddPoint(position)` | `void(obj)` | Add control point (Vec3) |
| `GetPositionAt(t)` | `obj(f64)` | Sample position at parameter t (0.0-1.0) |
| `GetDirectionAt(t)` | `obj(f64)` | Sample tangent direction at t |
| `Clear()` | `void()` | Remove all points |

Looping paths include the closing segment from the final control point back to the first point in both `Length` and `GetPositionAt`/`GetDirectionAt` sampling. Non-looping paths continue to clamp at the endpoints.

### Zia Example

```zia
module PathDemo;

bind Zanna.Graphics3D.Path3D;
bind Zanna.Math.Vec3;

func start() {
    var path = Path3D.New();
    Path3D.AddPoint(path, Vec3.New(0.0, 0.0, 0.0));
    Path3D.AddPoint(path, Vec3.New(5.0, 2.0, 0.0));
    Path3D.AddPoint(path, Vec3.New(10.0, 0.0, 5.0));
    Path3D.AddPoint(path, Vec3.New(15.0, 1.0, 10.0));
    Path3D.set_Looping(path, true);

    // Sample along path
    for i in 0..100 {
        var t = i / 100.0;
        var pos = Path3D.GetPositionAt(path, t);
        var dir = Path3D.GetDirectionAt(path, t);
    }
}
```

---

## AnimBlend3D

Weight-based animation blending for smooth transitions between clips.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(skeleton)` | `obj(obj)` | Create blender bound to a Skeleton3D |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `StateCount` | Integer | read | Number of registered states |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddState(name, animation)` | `i64(str, obj)` | Register animation clip (returns state index) |
| `SetWeight(stateIdx, weight)` | `void(i64, f64)` | Set blend weight (0.0-1.0) |
| `SetWeightByName(name, weight)` | `void(str, f64)` | Set blend weight by name |
| `GetWeight(stateIdx)` | `f64(i64)` | Query blend weight |
| `SetSpeed(stateIdx, speed)` | `void(i64, f64)` | Playback speed multiplier per state |
| `Update(dt)` | `void(f64)` | Advance all active animations |

Weights are clamped to `[0.0, 1.0]`, NaN weights become zero, and negative per-state speeds play the
state in reverse using the same loop/clamp behavior as `AnimPlayer3D`. `Update(0.0)` still
recomputes the blended pose, and newly added states inherit the source animation's looping flag.
Blending decomposes sampled bone matrices into TRS and uses quaternion slerp for rotation, which
avoids skewed matrices when rotations are mixed.

Draw blended mesh via `Canvas3D.DrawMeshBlended(canvas, mesh, transform, material, blender)`. The `AnimBlend3D` already owns its `Skeleton3D`, so no extra skeleton argument is required.

### Zia Example

```zia
module BlendDemo;

bind Zanna.Graphics3D.AnimBlend3D;
bind Zanna.Graphics3D.Skeleton3D;
bind Zanna.Graphics3D.Animation3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Math.Mat4;

func start() {
    var canvas = Canvas3D.New("Blend", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 100.0);

    // Assume skel, walkAnim, runAnim are loaded
    var blend = AnimBlend3D.New(skel);
    var walk_idx = AnimBlend3D.AddState(blend, "walk", walkAnim);
    var run_idx = AnimBlend3D.AddState(blend, "run", runAnim);

    // Blend 70% walk + 30% run
    AnimBlend3D.SetWeight(blend, walk_idx, 0.7);
    AnimBlend3D.SetWeight(blend, run_idx, 0.3);

    // In render loop:
    AnimBlend3D.Update(blend, dt);
    Canvas3D.Begin(canvas, cam);
    Canvas3D.DrawMeshBlended(canvas, mesh, Mat4.Identity(), mat, blend);
    Canvas3D.End(canvas);
}
```

---

## BlendTree3D

Parametric 1D/2D animation blendspaces over `AnimBlend3D`. Use this when a character should move
continuously across authored samples such as idle/walk/run speed or 2D locomotion/aim offsets.

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New1D(skeleton)` | `obj(obj)` | Create a 1D blend tree bound to a `Skeleton3D` |
| `New2D(skeleton)` | `obj(obj)` | Create a 2D blend tree bound to a `Skeleton3D` |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `SampleCount` | Integer | read | Number of animation samples in the tree |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddSample(animation, x, y)` | `i64(obj, f64, f64)` | Register an `Animation3D` sample at a parameter coordinate |
| `SetParam(x, y)` | `void(f64, f64)` | Set the current blend parameters and recompute weights |
| `Update(dt)` | `void(f64)` | Recompute weights and advance the underlying animation blend |

`New1D` uses the sample `x` coordinate and linearly interpolates between the closest lower/upper
samples, clamping outside the authored range. `New2D` selects exact-coordinate matches directly and
otherwise normalizes inverse-distance weights. Non-finite parameters are treated as zero.

`Canvas3D.DrawMeshBlended(canvas, mesh, transform, material, tree)` accepts either an `AnimBlend3D`
or a `BlendTree3D`; no separate extraction step is needed.

### Zia Example

```zia
module BlendTreeDemo;

bind Zanna.Graphics3D.BlendTree3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Math.Mat4;

func makeLocomotionTree(skel, idleAnim, walkAnim, runAnim) {
    var tree = BlendTree3D.New1D(skel);
    BlendTree3D.AddSample(tree, idleAnim, 0.0, 0.0);
    BlendTree3D.AddSample(tree, walkAnim, 1.5, 0.0);
    BlendTree3D.AddSample(tree, runAnim, 5.0, 0.0);
    return tree;
}

func render(canvas, mesh, mat, tree, speed, dt) {
    BlendTree3D.SetParam(tree, speed, 0.0);
    BlendTree3D.Update(tree, dt);
    Canvas3D.DrawMeshBlended(canvas, mesh, Mat4.Identity(), mat, tree);
}
```

---

## IKSolver3D

Inverse-kinematics solvers for final pose adjustment before skinning. Use these for simple
foot/hand target placement, look-at/aim bones, and short articulated chains that need to follow a
runtime target while still using authored animation as the base pose.

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `TwoBone(skeleton, root, mid, end)` | `obj(obj,i64,i64,i64)` | Create a parented three-bone chain solver |
| `LookAt(skeleton, bone)` | `obj(obj,i64)` | Aim one bone's local +Z axis toward a target |
| `FABRIK(skeleton, chain)` | `obj(obj,obj)` | Create a chain solver from a `Seq[Integer]` of parented bone indices |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTarget(pos)` | `void(obj)` | Set the target as a `Vec3` |
| `SetWeight(w)` | `void(f64)` | Blend solver output from `0.0` to `1.0`; non-finite values become zero |
| `SetPole(pos)` | `void(obj)` | Set a world-space pole target for `TwoBone` bend-plane control |
| `SetGroundNormal(normal)` | `void(obj)` | Set a ground normal; the end bone's animated rotation is tilted by the model-up-to-normal delta (flat ground is a no-op) |
| `SetTargetRotation(rotation)` | `void(obj)` | Set a model-space `Quat` orientation goal for the end bone, applied after the positional solve; wins over `SetGroundNormal` |
| `ClearTargetRotation()` | `void()` | Remove the end-bone orientation goal |
| `Solve()` | `void()` | Solve against the skeleton bind pose for standalone inspection |

Attach a solver to an animation controller with `AnimController3D.SetIKSolver(solver)` or the
Game3D wrapper `Animator3D.SetIKSolver(solver)`. Controller-bound IK is applied after the base
state/blend tree and overlay layers are composed, then before skinning palettes are generated.
`TwoBone` and `FABRIK` use a positional FABRIK-style chain solve and preserve the chain root;
`SetPole` swings a two-bone middle joint toward the requested bend plane, and `LookAt` aims the
selected bone's local +Z axis. End-bone orientation follows ADR 0286: `SetGroundNormal` composes
the shortest-arc tilt from model +Y to the surface normal onto the animated end-bone rotation
(so a flat surface changes nothing and no rig axis convention is assumed), while
`SetTargetRotation` slerps the end bone toward an explicit model-space quaternion goal by solver
weight and takes precedence over the ground hint. `Solve()` solves against bind pose, while a
bound controller solves against that controller's current animated pose.

---

## AnimController3D

Stateful skeletal animation controller for gameplay code. `AnimController3D` builds on the same sampling path as `AnimPlayer3D` and `AnimBlend3D`, but adds named states, transition defaults, clip events, root-motion extraction, and simple masked overlay layers.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(skeleton)` | `obj(obj)` | Create a controller bound to a `Skeleton3D` |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `CurrentState` | String | read | Active base-layer state name |
| `PreviousState` | String | read | Prior base-layer state name |
| `IsTransitioning` | Boolean | read | True while the base layer is inside a timed crossfade |
| `StateCount` | Integer | read | Number of registered states |
| `RootMotionDelta` | `Vec3` | read | Accumulated root-motion delta since the last consume/reset |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddState(name, animation)` | `i64(str,obj)` | Register a named `Animation3D` state |
| `AddTransition(fromState, toState, blendSeconds)` | `i1(str,str,f64)` | Register a default timed transition between two states |
| `Play(stateName)` | `i1(str)` | Play a state, using a registered transition if one exists from the current state |
| `Crossfade(stateName, blendSeconds)` | `i1(str,f64)` | Force a timed transition to another state |
| `Stop()` | `void()` | Stop the base layer and all overlay layers |
| `Update(dt)` | `void(f64)` | Advance all active layers by `dt` seconds |
| `SetStateSpeed(name, speed)` | `void(str,f64)` | Override playback speed for a named state |
| `SetStateLooping(name, loop)` | `void(str,i1)` | Override looping behavior for a named state |
| `SetAnimationLOD(distance, rateHz)` | `void(f64,f64)` | Enable deterministic batched updates at `rateHz` for distant/low-priority controllers; non-positive inputs disable it |
| `SetBoneLOD(maxBones)` | `void(i64)` | Freeze bones at or after `maxBones` for deterministic bone-count LOD; non-positive values restore full-pose output |
| `SetBlendTree(tree)` | `i1(obj)` | Use a compatible `BlendTree3D` as the base pose source; pass `Nothing`/`NULL` to clear |
| `SetBlendTreeFade(seconds)` | `void(f64)` | ADR 0302: ramp length `SetBlendTree` honours when a tree is attached or cleared (0 = instantaneous swap, the default); the cleared tree stays retained until its weight reaches zero |
| `SetTransitionContinuity(enabled)` | `void(i1)` | ADR 0302: crossfade out of finished/stopped clips, depart retriggered fades from the visible pose (frozen-pose source), and evaluate in-flight fades through the animation-LOD gate; off by default (historical palettes) |
| `SetIKSolver(solver)` | `i1(obj)` | Apply a compatible `IKSolver3D` after overlays and before skinning; pass `Nothing`/`NULL` to clear |
| `AddEvent(stateName, timeSeconds, eventName)` | `void(str,f64,str)` | Queue an event when playback crosses the specified state-local time |
| `PollEvent()` | `str()` | Dequeue the next event name, or `""` when none are pending |
| `SetRootMotionBone(boneIdx)` | `void(i64)` | Choose which bone contributes root motion; `-1` disables it |
| `ConsumeRootMotion()` | `obj()` | Return the accumulated `Vec3` delta and clear it |
| `SetLayerWeight(layer, weight)` | `void(i64,f64)` | Set overlay weight for layers `1..3` |
| `SetLayerMask(layer, rootBone)` | `void(i64,i64)` | Restrict an overlay layer to the subtree rooted at `rootBone` |
| `PlayLayer(layer, stateName)` | `i1(i64,str)` | Start a named state as a masked replace overlay |
| `PlayLayerAdditive(layer, stateName)` | `i1(i64,str)` | Start a named state as a true additive bind-pose delta overlay |
| `CrossfadeLayer(layer, stateName, blendSeconds)` | `i1(i64,str,f64)` | Crossfade an overlay layer to a new state |
| `CrossfadeLayerAdditive(layer, stateName, blendSeconds)` | `i1(i64,str,f64)` | Crossfade an overlay layer to a true additive bind-pose delta state |
| `StopLayer(layer)` | `void(i64)` | Stop one overlay layer |
| `GetBoneMatrix(boneIdx)` | `obj(i64)` | Read the controller's final global/world matrix for a bone |

Event times are clamped into the owning clip's duration and are fired when playback crosses them in
forward, reverse, exact-loop, or multi-loop updates. State speeds may be negative for reverse
playback; non-finite speeds fall back to `1.0`. Overlay weights are finite and clamped. `PlayLayer`
preserves the existing masked replace behavior, while `PlayLayerAdditive` composes
`(overlayPose - bindPose) * weight` onto the current base pose for true additive layers.
`CrossfadeLayerAdditive` keeps that additive composition while blending to another overlay state.
Both paths use TRS/quaternion composition so masked layers do not introduce matrix skew. Root motion
is disabled by default, preserves forward/reverse loop-wrap deltas, and can be reset with
`SetRootMotionBone(-1)`. `SetAnimationLOD(distance, rateHz)` accumulates elapsed time and samples
at the requested lower rate while preserving deterministic playback time; pass `0, 0` to restore
per-update sampling. `SetBoneLOD(maxBones)` limits palette updates to the retained bone-count
prefix while keeping ancestors valid for deterministic distant-character LOD; pass `0` to restore
full output. `SetBlendTree(tree)` updates the tree with the controller tick and uses its
blended local pose as the base layer before overlays are applied; root-motion extraction remains
state-player based. `SetIKSolver(solver)` applies the solver after overlays and before the final
skin palette; it only accepts solvers bound to the same skeleton. `Stop()` returns the output pose
to bind pose.

### When To Use Which API

- Use `AnimPlayer3D` when you just need to play one clip or crossfade directly between clips.
- Use `AnimBlend3D` when you want manual weight control over several simultaneously sampled clips.
- Use `BlendTree3D` directly for manually drawn blended meshes, or attach one to an
  `AnimController3D` with `SetBlendTree` when named overlay layers/events still need the controller.
- Use `IKSolver3D` when the current animated pose needs a final procedural target adjustment before
  skinning, such as a foot/hand target, aim bone, or short FABRIK chain.
- Use `AnimController3D` when gameplay code needs named states, default transitions, root motion, queued events, or masked upper-body/lower-body style overlays.

Current limitation:
- `AnimController3D` can now drive `SceneNode` root motion and skinned scene-node draws through `SceneGraph.SyncBindings` + `SceneGraph.Draw`.
- Direct standalone mesh submission accepts `AnimPlayer3D`, `AnimBlend3D`, and `BlendTree3D`; use scene-node binding when you want controller-driven scene composition.

### Zia Example

```zia
module AnimController3DDemo;

bind Zanna.Graphics3D;
bind Zanna.Math;
bind Zanna.Terminal;

func start() {
    var skel = Skeleton3D.New();
    var rootBone = Skeleton3D.AddBone(skel, "root", -1, Mat4.Identity());
    var armBone = Skeleton3D.AddBone(skel, "arm", rootBone, Mat4.Identity());
    Skeleton3D.ComputeInverseBind(skel);

    var rot = Quat.Identity();
    var scl = Vec3.One();

    var walk = Animation3D.New("walk", 1.0);
    Animation3D.set_Looping(walk, true);
    Animation3D.AddKeyframe(walk, rootBone, 0.0, Vec3.Zero(), rot, scl);
    Animation3D.AddKeyframe(walk, rootBone, 1.0, Vec3.New(10.0, 0.0, 0.0), rot, scl);

    var wave = Animation3D.New("wave", 1.0);
    Animation3D.set_Looping(wave, true);
    Animation3D.AddKeyframe(wave, armBone, 0.0, Vec3.Zero(), rot, scl);
    Animation3D.AddKeyframe(wave, armBone, 1.0, Vec3.New(0.0, 2.0, 0.0), rot, scl);

    var controller = AnimController3D.New(skel);
    AnimController3D.AddState(controller, "walk", walk);
    AnimController3D.AddState(controller, "wave", wave);
    AnimController3D.AddEvent(controller, "walk", 0.5, "step");
    AnimController3D.SetRootMotionBone(controller, rootBone);
    AnimController3D.SetLayerMask(controller, 1, armBone);
    AnimController3D.SetLayerWeight(controller, 1, 1.0);

    AnimController3D.Play(controller, "walk");
    AnimController3D.PlayLayer(controller, 1, "wave");
    AnimController3D.Update(controller, 0.5);

    var delta = AnimController3D.ConsumeRootMotion(controller);
    var armMat = AnimController3D.GetBoneMatrix(controller, armBone);

    Say("RootMotion X = " + toString(Vec3.get_X(delta)));
    Say("Event = " + AnimController3D.PollEvent(controller));
    Say("Arm Y = " + toString(Mat4.Get(armMat, 1, 3)));
}
```

See `examples/apiaudit/graphics3d/animcontroller3d_demo.zia` and `examples/apiaudit/graphics3d/animcontroller3d_demo.bas` for compact runnable samples.

---

## TextureAtlas3D

Texture array for efficient multi-texture rendering.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(width, height)` | `obj(i64, i64)` | Create atlas with layer dimensions |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Add(pixels)` | `i64(obj)` | Add Pixels texture layer (returns layer index) |
| `GetTexture()` | `obj()` | Export combined atlas as single Pixels |

Each added layer is copied into the atlas with a duplicated 1-pixel edge/corner border. The padding prevents bilinear filtering from bleeding neighboring layers into each other when atlas UVs land near tile edges.

### Zia Example

```zia
module AtlasDemo;

bind Zanna.Graphics3D.TextureAtlas3D;
bind Zanna.Graphics.Pixels;

func start() {
    var atlas = TextureAtlas3D.New(256, 256);
    var grass_idx = TextureAtlas3D.Add(atlas, Pixels.Load("grass.png"));
    var rock_idx = TextureAtlas3D.Add(atlas, Pixels.Load("rock.png"));
    var combined = TextureAtlas3D.GetTexture(atlas);
}
```

---

## Vegetation3D

Procedural grass/foliage rendering with wind animation and LOD.

### Constructor

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(terrain)` | `obj(obj)` | Create vegetation system bound to a Terrain3D |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetDensityMap(pixels)` | `void(obj)` | Set density map (Pixels grayscale, white = full density) |
| `SetWindParams(speed, strength, turbulence)` | `void(f64, f64, f64)` | Set wind animation parameters |
| `SetLodDistances(near, far)` | `void(f64, f64)` | Set LOD transition distances |
| `SetBladeSize(width, height, variance)` | `void(f64, f64, f64)` | Set grass blade dimensions |
| `SetSeed(seed)` | `void(i64)` | Set deterministic scatter seed for later `Populate` calls |
| `Populate(camera, maxBlades)` | `void(obj, i64)` | Generate blade instances around camera |
| `Update(dt, camX, camY, camZ)` | `void(f64, f64, f64, f64)` | Update wind animation and camera-relative LOD |

Draw via `Canvas3D.DrawVegetation(vegetation)`. `SetSeed(seed)` pins reproducible scatter for subsequent `Populate` calls; omitting it uses a per-instance default seed. `Update(0.0, camX, camY, camZ)` refreshes camera-relative visibility and LOD without advancing wind time, which is useful for paused scenes. `DrawVegetation` marks the blade material double-sided instead of mutating `Canvas3D`'s global backface-cull flag, and it must run inside the 3D `Canvas3D.Begin`/`End` section.

### Zia Example

```zia
module VegetationDemo;

bind Zanna.Graphics3D.Vegetation3D;
bind Zanna.Graphics3D.Terrain3D;
bind Zanna.Graphics3D.Canvas3D;
bind Zanna.Graphics3D.Camera3D;
bind Zanna.Graphics.Pixels;

func start() {
    var canvas = Canvas3D.New("Vegetation", 800, 600);
    var cam = Camera3D.New(60.0, 800.0 / 600.0, 0.1, 200.0);

    var terrain = Terrain3D.New(64, 64);
    // ... set up terrain ...

    var veg = Vegetation3D.New(terrain);
    Vegetation3D.SetDensityMap(veg, Pixels.Load("grass_density.png"));
    Vegetation3D.SetBladeSize(veg, 0.1, 0.4, 0.15);
    Vegetation3D.SetWindParams(veg, 1.2, 0.5, 0.3);
    Vegetation3D.SetLodDistances(veg, 30.0, 60.0);
    Vegetation3D.SetSeed(veg, 12345);
    Vegetation3D.Populate(veg, cam, 50000);

    while (Canvas3D.get_ShouldClose(canvas) == 0) {
        Canvas3D.Poll(canvas);
        var dt = Canvas3D.get_DeltaTime(canvas) / 1000.0;
        var pos = Camera3D.get_Position(cam);
        Vegetation3D.Update(veg, dt, Vec3.get_X(pos), Vec3.get_Y(pos), Vec3.get_Z(pos));

        Canvas3D.Clear(canvas, 0.5, 0.7, 0.9);
        Canvas3D.Begin(canvas, cam);
        Canvas3D.DrawTerrain(canvas, terrain);
        Canvas3D.DrawVegetation(canvas, veg);
        Canvas3D.End(canvas);
        Canvas3D.Flip(canvas);
    }
}
```

---

## VideoPlayer

Video playback for game cutscenes and GUI applications.

| Member | Description |
|--------|-------------|
| `Open(path)` | Load an AVI MJPEG or OGG/Theora video file |
| `Play()` | Start playback |
| `Pause()` | Pause playback |
| `Stop()` | Stop and rewind to start |
| `Seek(seconds)` | Seek to time position |
| `Update(dt)` | Advance by delta time in seconds (call each game frame) |
| `SetVolume(vol)` | Set audio volume [0.0-1.0] |
| `Width` | Frame width in pixels (read-only) |
| `Height` | Frame height in pixels (read-only) |
| `Duration` | Total duration in seconds (read-only) |
| `Position` | Current playback position in seconds (read-only) |
| `IsPlaying` | Whether playback is active (read-only) |
| `Frame` | Current decoded Pixels frame (read-only) |

### Supported Formats

| Container | Video Codec | Audio Codec | Extension |
|-----------|-------------|-------------|-----------|
| AVI (RIFF) | MJPEG | PCM WAV | `.avi` |
| OGG | Theora | Vorbis | `.ogv` |

**MJPEG notes:** MJPEG frames in AVI files often omit Huffman tables (DHT markers). The decoder automatically injects standard JPEG Annex K DHT tables when they are missing. MJPEG has no inter-frame compression — each frame is an independent JPEG image. File sizes are large (~100-200 MB per minute at 720p) but seeking is instant since every frame is a keyframe.

**Current limitation:** `.ogv` playback now handles mixed Theora/Vorbis containers through `VideoPlayer.Open`, including audio volume/seek integration and logical-stream demux, but video fidelity is still bounded by the current in-tree Theora frame decoder implementation.

### Game Cutscene Usage

```zia
var player = VideoPlayer.Open("cutscene.avi");
VideoPlayer.Play(player);

while (VideoPlayer.get_IsPlaying(player)) {
    Canvas.Poll(canvas);
    var dt = Canvas.get_DeltaTime(canvas) / 1000.0;
    VideoPlayer.Update(player, dt);
    Canvas.Blit(canvas, 0, 0, VideoPlayer.get_Frame(player));
    Canvas.Flip(canvas);
}
```

### 3D Video Texture Usage

```zia
VideoPlayer.Update(player, dt);
Material3D.SetTexture(screenMat, VideoPlayer.get_Frame(player));
Canvas3D.DrawMesh(canvas, screenMesh, screenXform, screenMat);
```

See `examples/apiaudit/graphics3d/video_demo.zia` for a complete example.

---

## Backend Selection

The GPU backend is selected automatically at startup:

| Platform | Primary | Fallback |
|----------|---------|----------|
| macOS | Metal | Software |
| Windows | Direct3D 11 | Software |
| Linux | OpenGL 3.3 | Software |

If the GPU backend fails to initialize (no GPU, driver issue), the software rasterizer is used automatically and Canvas3D emits one stderr notice for the process. Check `canvas.Backend` to see which renderer is active, and `canvas.BackendFallback`, `canvas.BackendFallbackReason`, or `canvas.BackendSupports("runtime-fallback")` to detect and explain a runtime software fallback.

For feature gating, prefer `canvas.BackendCapabilities` or `canvas.BackendSupports(name)` over string comparisons against `canvas.Backend`. Capability names currently include `software`, `gpu`, `render_target`, `window_readback`, `shadows`, `skybox`, `instancing`, `hardware_instancing`, `postfx`, `postfx-full`, `gpu_postfx`, `postfx-overlay`, `final-screenshot`, `gpu-postfx-overlay`, `clustered-lighting`, `soft-particles`, `ssr`, `shadow-csm`, `shadow-point`, `occlusion`, `hlod`, `hdr-scene`, `taa`, `native-texture:bc1`, `native-texture:bc3`, `native-texture:bc4`, `native-texture:bc5`, `native-texture:bc7`, `native-texture:astc`, and `native-texture:etc2`; `texture:*` names report CPU decoder/fallback coverage, and fallback-state aliases include `runtime-fallback`, `backend-fallback`, and `software-fallback`. The 64-bit bitmask values are:

| Bit | Capability |
|-----|------------|
| `0x0001` | Software backend |
| `0x0002` | GPU backend |
| `0x0004` | RenderTarget3D binding |
| `0x0008` | Window framebuffer readback / screenshots |
| `0x0010` | Shadow map passes |
| `0x0020` | Cubemap skybox path |
| `0x0040` | Hardware instancing backend hook |
| `0x0080` | PostFX support |
| `0x0100` | GPU-owned PostFX presentation |
| `0x0200` | Crisp final overlay composition |
| `0x0400` | Final screenshot after overlay composition |
| `0x0800` | Split GPU post-FX with final overlay after tonemap |
| `0x1000` | Clustered/forward+ lighting |
| `0x2000` | Cascaded shadow maps |
| `0x4000` | Occlusion culling |
| `0x8000` | Runtime HLOD / impostor support |
| `0x10000` | BC7 compressed texture upload |
| `0x20000` | ASTC compressed texture upload |
| `0x40000` | ETC2 compressed texture upload |
| `0x80000` | Sampler anisotropy |
| `0x100000` | PBR material path |
| `0x200000` | Normal maps |
| `0x400000` | Alpha masking |
| `0x800000` | Morph targets |
| `0x1000000` | Skinning |
| `0x2000000` | Terrain splatting |
| `0x4000000` | BC1 compressed texture upload |
| `0x8000000` | BC3 compressed texture upload |
| `0x10000000` | BC4 compressed texture upload |
| `0x20000000` | BC5 compressed texture upload |
| `0x40000000` | HDR scene color |
| `0x80000000` | Temporal anti-aliasing |
| `0x100000000` | Soft particles |
| `0x200000000` | Screen-space reflections |
| `0x400000000` | Instanced draw hook |
| `0x800000000` | Omnidirectional point-light shadow slots |
| `0x1000000000` | Full PostFX chain |

**Software renderer** — Always available. Gouraud shading by default, switches to per-pixel Blinn-Phong when a normal map is present. Supports nearest/bilinear material texture filtering with imported wrap modes, per-vertex colors, shadow mapping for up to four directional slots, primary-directional cascaded shadow maps with 3x3 PCF filtering, specular maps, normal maps, and per-pixel terrain splatting.

**Metal** (macOS) — Near-full feature parity (94%): lit/unlit textures, the shared `Material3D` PBR path (metallic/roughness, AO, alpha modes, emissive intensity, normal scale), spot light cone attenuation, linear fog, wireframe, per-frame texture caching, GPU skinning (4-bone), morph targets, up to four directional shadow slots or primary-directional CSM cascades, instanced rendering, terrain splatting, and post-processing (bloom, FXAA, tone mapping, vignette, color grading).

**OpenGL 3.3** (Linux) — Near-full feature parity (OGL-01 through OGL-20): all texture types, the shared `Material3D` PBR path (metallic/roughness, AO, alpha modes, emissive intensity, normal scale), spot lights, fog, wireframe, render-to-texture, up to four directional shadow slots or primary-directional CSM cascades, post-processing, instancing, skinning, morph targets, terrain splatting, cubemap skybox, environment reflections, and advanced post-FX (SSAO, depth of field, motion blur). Point/omni-directional shadow atlases are not implemented on this backend (`BackendSupports("shadow-point")` reports false; GAP-8 in `docs/cross-platform/platform-differences.md`).

**Direct3D 11** (Windows) — Full feature parity: same feature set as OpenGL, including the shared `Material3D` PBR path. On non-Windows hosts, validation depends on the Windows CI lane.

Backend correctness rules are shared where possible: skinning weights are normalized before application, oversized GPU bone palettes are clamped to backend shader limits, unused bone palette slots are identity transforms, terrain splatting uses the real-time backend payload when the control map and all four layer textures are resident and falls back to CPU baking only for incomplete sets, masked materials alpha-test shadow casters, shadow slots are advertised only after the indexed pass completes in the current frame, failed D3D11 swapchain presents invalidate their pre-present readback snapshot, and invalid draw/readback/texture/shadow inputs are rejected or treated conservatively instead of being dereferenced. D3D11 additionally clamps backend-uploaded material/light/post-FX scalars and enum IDs, forwards all twelve material custom parameters, validates complete clustered-light tables before upload, rejects malformed post-FX chain storage before indexed iteration, validates native BC mip block layouts and complete IBL layouts before GPU upload, rejects corrupt in-progress upload cursors instead of silently restarting partial resources, keeps deferred native-texture and IBL payload metadata independent of runtime-object lifetime, rebuilds partial post-FX target sets before reuse, selects depth-probe sources from the actual RTT/offscreen/swapchain route, and limits CPU staging readback to supported RGBA8/HDR16F source formats.

## Sky3D, TimeOfDay3D, ReflectionProbe3D, LensFlare3D, NodeAnimator3D

Five environment and presentation classes previously documented only in
the generated inventory:

**Sky3D** — a procedural analytic sky that renders into a cubemap.
Construct with `Sky3D.New()`, steer it with `SetSunDirection(vec3)`,
`SetGroundAlbedo(r, g, b)`, and the `Turbidity` (atmospheric haze) and
`Resolution` (cubemap face size) properties, then call `Update(canvas)`
whenever `Dirty` reports true. Read `Cubemap` and assign it as the
canvas skybox/IBL source. Cheaper than authoring HDR panoramas for
dynamic time-of-day scenes.

**TimeOfDay3D** — a day/night clock that drives a sun light, a `Sky3D`,
and optionally a `ReflectionProbe3D` together. Set `Hours` (0–24),
`DayLengthSeconds` (real seconds per in-game day), `LatitudeDegrees`,
and `RefreshDegrees` (how far the sun must move before bound targets
re-update), bind targets with `SetSunLight`, `SetSky`, and
`SetReflectionProbe`, then call `Advance(dt, canvas)` each frame.
`SunDirection` exposes the computed sun vector for gameplay.

**ReflectionProbe3D** — a box-bounded local reflection capture.
`ReflectionProbe3D.New(position, boundsMin, boundsMax)` places it;
`Capture(canvas, scene)` renders the surrounding scene into its
`Cubemap` when `CaptureDirty` is set. `InfluenceScale` sizes the blend
region, `Contains(point)` tests membership, and `Resolution` trades
sharpness for capture cost. Materials with reflectivity pick up the
nearest containing probe.

**LensFlare3D** — an occlusion-aware ghost chain along the light-to-
screen-center axis. Construct per light and add ghosts with
`AddElement(offset, size, color, intensity)`; flares fade automatically
when the source light is occluded.

**NodeAnimator3D** — playback for node TRS animation clips
(`NodeAnimation3D`), the non-skeletal counterpart of `AnimPlayer3D`.
Importers bind one automatically for assets with node tracks; drive it
with `Play(name)`, `Stop`, `SetSpeed`, and inspect `ClipCount`,
`CurrentClip`, `IsPlaying`, `Speed`, and `Time`, plus
`GetClip(i)`/`GetClipName(i)`. Bind to scene content through
`SceneNode.BindNodeAnimator` and advance via `SceneGraph.SyncBindings`.

## Scene Serialization, Precise Picking, and Readback (ADRs 0190–0227)

Late-cycle additions that previously appeared only in the generated
reference:

**In-memory scene text (ADR 0190, 0227).** `SceneGraph.SaveToText()`
returns the canonical VSCN text byte-identical to `Save`'s file output.
Its inverse is `SceneGraph.LoadTextResult(virtualPath, text)`, which
returns a `Zanna.Result` (ok wraps the scene; err carries the loader
diagnostic) and resolves relative prefab references against
`virtualPath`'s directory. `SceneGraph.LoadResult(path)` is the
Result-carrying peer of `Load` for files. `SceneAsset.LoadTextResult`
is the asset-document equivalent.

**Prefab diagnostics (ADR 0227).** Unresolved prefab references still
load as empty placeholders (ADR 0187), but each one now adds a warning
readable through `AssetDiagnostics3D` and increments the read-only
`SceneGraph.UnresolvedPrefabCount` — including occurrences nested inside
resolved prefabs — so a game can gate startup on a fully resolved world.

**Animation adoption (ADR 0227).** `SceneGraph.AdoptAnimations(source)`
retain-appends the source scene's baked animation clips onto the
receiver (idempotent per clip handle) so merging an instantiated model's
nodes into another scene no longer drops its clips on save.

**Triangle-accurate raycasts (ADR 0193).** Three siblings join the
AABB-based `RaycastNodes` on `SceneGraph`, sharing its argument
contract: `RaycastNodesPrecise` returns the visible mesh node containing
the nearest intersected triangle; `RaycastNodesPreciseAll` returns every
triangle-hit node nearest-first (the overlap-cycling primitive); and
`RaycastPreciseHit` returns the nearest `RayHit3D` itself (world point,
normal, distance, triangle index) for spawn-at-cursor and surface
snapping.

**GPU offscreen rendering (ADR 0191).**
`Canvas3D.NewOffscreenAccelerated(target)` creates a windowless canvas
that renders on the platform GPU backend into a `RenderTarget3D`,
falling back to the software rasterizer when no GPU backend is
available. Zero-copy presentation is explicitly out of scope.

**Camera readback (ADR 0227).** `Camera3D` exposes the retained state
the renderer actually uses: `ViewMatrix`, `ProjectionMatrix` (exactly as
last synced — including reversed-Z on backends that use it), `Up`
(completing the `Forward`/`Right` basis), and `AspectRatio`. Prefer
these over reconstructing projection math in Zia.

**Quaternion inverses (ADR 0227).** `Quat.ToEuler()` returns
(pitch, yaw, roll) radians as the exact algebraic inverse of
`FromEuler`; `Quat.FromMat4(m)` extracts a normalized rotation from a
matrix, tolerating scale via column normalization.
`Transform3D.GetEuler()` is the degree-valued inverse of `SetEuler`.

**Readback symmetry (ADR 0227).** Formerly write-only surfaces gained
readers: `TextureAtlas3D.GetUvMin/GetUvMax(id)` (packed region UVs),
`InstanceBatch3D.GetTransform(index)` plus `Mesh`/`Material` properties,
`Skeleton3D.GetBoneParent/GetBoneBindPose(index)`, `Canvas3D.Wireframe`,
`Particles3D.Additive`, and `Path3D.Looping` as read-write properties,
`SceneNode.TrySetWorldPosition(x, y, z)` (translation-only world edit
with ADR 0166 exact-or-reject semantics), writable `Vec2.X/Y`, and
inspection getters across `Terrain3D` (scale, material, layer
textures/scales, splat maps, LOD distances, heightmap dimensions, hole
rects) and `Water3D` (placement, wave parameters, color, alpha,
reflectivity, resolution, texture/normal/environment maps). `Gltf`
now registers the same skeleton/animation/camera/scene accessors as
`Fbx`.

## Performance Tips

- **Triangle budget:** Software renderer handles ~50K triangles at 30fps (640x480). GPU backends handle 1M+ at 60fps.
- **Mesh generators:** `Sphere(r, 8)` is adequate for most uses. Higher segments (16-32) for close-up objects.
- **Lights:** Each additional light adds computation. Use 1-3 lights for best performance.
- **Backface culling:** Enabled by default. Disable only for double-sided geometry (leaves, glass).
- **Non-uniform scaling:** All 3D backends use inverse-transpose normal transforms, so non-uniform scale preserves lighting direction consistently across software, Metal, OpenGL, and D3D11.

## Resource Limits

| Resource | Limit |
|----------|-------|
| Canvas dimensions | 16384 x 16384 |
| Texture dimensions | 8192 x 8192 |
| Mesh vertices | 16M (32-bit index buffer) |
| Authored node lights | Preserved by VSCN; draw traversal submits the first 64 enabled lights |
| Active software lights | 16 |
| Active clustered GPU lights | 64 |

Use `Canvas3D.MaxActiveLights`, `LightCount`, and `DroppedLightCount` to report
the live backend budget and any deterministic draw-time truncation. Authoring
tools should not discard scene lights merely because the current preview
backend has a smaller active budget.

## Error Handling

- Constructor failures (New/Load) trap with a descriptive message
- Out-of-bounds indices trap
- GPU allocation failure falls back to software (no trap)
- `ZANNA_ENABLE_GRAPHICS=OFF` builds: constructors trap, all other functions are silent no-ops

## Threading

All Graphics3D operations must be called from the main thread. `Begin`/`End` must not nest. Do not modify scene node transforms during `Draw()` traversal.
