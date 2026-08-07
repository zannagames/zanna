---
status: active
audience: public
last-verified: 2026-08-03
---

# Game3D
> Runtime-first 3D game helpers for code-first Zanna and Zia projects.

**Part of [Zanna Runtime Library](../README.md) / [Graphics](README.md)**

`Zanna.Game3D` is implemented in the normal C runtime, beside the other
`Zanna.*` runtime namespaces. It is not a source-level Zia helper library. Thin
Zia samples and convenience imports may exist, but the authoritative API,
ownership rules, diagnostics, and tests live in the runtime.

Game3D composes the lower-level `Zanna.Graphics3D`, `Zanna.Input`, physics, and
audio surfaces so simple games do not need to hand-wire canvases, cameras,
scenes, physics worlds, final-frame capture, layer masks, and common input.

Writable `Zanna.Game3D` properties expose both property accessors and
method-call setters. A writable property such as `Speed` or `LookSensitivity`
has `get_Speed`/`set_Speed` runtime accessors and a matching `SetSpeed(...)`
class method. Every Game3D member uses PascalCase.

---

## Quick Start

```zia
module HelloGame3D;

bind Zanna.Game3D as Game3D;
bind Zanna.Graphics as Graphics;
bind Zanna.Math as Math;
bind Zanna.Terminal as Terminal;

func start() {
    var world = Game3D.World3D.New("Hello Game3D", 640, 480);
    var env = Game3D.Environment3D.Outdoor(world);
    Game3D.EnvHandle.WithWater(env, -0.05);

    var mat = Game3D.Materials.Metal(0.2, 0.7, 0.9);
    var cube = Game3D.Prefab.Box(1.0, mat);
    Game3D.Entity3D.set_Name(cube, "Cube");
    Game3D.Entity3D.SetPosition(cube, 0.0, 0.75, 0.0);
    Game3D.World3D.Spawn(world, cube);

    Game3D.PostFX.Crisp(world);
    Game3D.Quality.Apply(world, Game3D.QualityLevel.get_Balanced());
    Game3D.World3D.LookAt(world, new Math.Vec3(0.0, 0.0, 0.0));
    Game3D.World3D.BeginFrame(world);
    Game3D.World3D.DrawScene(world);
    Game3D.World3D.DrawEffects(world);
    Game3D.World3D.EndScene(world);

    var pixels = Game3D.World3D.CaptureFinalFrame(world);
    if (pixels == null || Graphics.Pixels.get_Width(pixels) != 640) {
        Terminal.Say("capture failed");
    }

    Game3D.World3D.Present(world);
    Game3D.World3D.Destroy(world);
}
```

This is intentionally free of common-case `Mat4` calls. `Entity3D` owns the raw
`SceneNode` transform, and `World3D` owns the normal frame order.

---

## Core Types

| Type | Purpose |
|------|---------|
| `World3D` | Owns the canvas, camera, scene, physics world, input wrapper, audio listener, post-FX registry, timing state, and entity registry |
| `Entity3D` | Spawnable game object with a scene node, optional mesh/material/body/anim handles, layer, collision mask, name, and child entities |
| `LayerMask` | Bitmask helper for collision and gameplay filtering |
| `BodyDef` | Runtime body recipe used by `Entity3D.attachBody` for common static, dynamic, trigger, and CCD bodies |
| `Collision3DEvent` | Entity-aware wrapper around raw `Graphics3D.CollisionEvent3D` enter/stay/exit records |
| `Assets3D` / `Prefab` / `SceneTemplate` | Filesystem and package-aware model loading with cached reusable prefab templates |
| `Animator3D` | Game3D wrapper over `Graphics3D.AnimController3D` for play/crossfade/state-time/events/root-motion attachment |
| `Input3D` | Named keyboard, mouse, movement-axis, and look-axis helper over the runtime input state |
| `Sound3D` | World-owned audio helper with camera-follow listener, loading, 2D, positional, and attached-source playback |
| `EffectRegistry3D` / `Effects3D` | World-owned post-FX plus runtime particle/decal registry and one-call VFX presets |
| `FreeFlyController` | Spectator camera controller with WASD/arrow movement, vertical movement, mouse look, and mouse capture |
| `FirstPersonController` | FPS camera controller that can either move the camera directly or drive a `CharacterController3D` |
| `OrbitController` | Target-orbit camera with drag orbit, wheel zoom, distance clamp, and pitch clamp |
| `FollowController` | Late-update camera follower that tracks an entity's post-physics pose with an offset and damping |
| `ThirdPersonController` | Collision-aware spring-arm over-the-shoulder camera with camera-relative character drive, aim mode, occluder fade, and lock-on framing |
| `TargetLock3D` | Lock-on target acquisition, cycling, auto-release, and soft input magnetism for third-person combat |
| `Hitbox3D` / `HitEvent3D` | Bone/entity-attached combat volumes with animation-window activation and a polled hit-event buffer |
| `Health3D` / `DamageEvent3D` | Per-entity hp with damage/i-frames/death lifecycle, knockback helper, and polled damage events |
| `RailCamera3D` | Constant-speed spline camera with FOV/roll keys, look modes, and damped progress |
| `Timeline3D` | Multi-track cutscene sequencer: cuts, spline moves, anim/audio/subtitle/letterbox/fade/marker tracks with skip semantics |
| `Dialogue3D` | Typewriter conversations with speaker anchoring, localization keys, and blocking choices |
| `LipSync3D` | Amplitude-envelope lip sync, seeded blinks, and LookAt-IK gaze |
| `CharacterController3D` | Game3D wrapper around `Zanna.Graphics3D.Character3D` with camera-relative movement, gravity, jump speed, grounding, and entity sync |
| `Lighting` | One-call readable lighting rigs: studio, outdoor, night, interior, and clear |
| `Materials` | PBR-oriented material presets for plastic, metal, rubber, glass, emissive, unlit, and albedo-map workflows |
| `PostFX` | Backend-safe post-processing presets: cinematic, crisp, and none |
| `Quality` | Applies backend-safe quality policy for post-FX, shadows, and culling |
| `Prefab` | Mesh+material entity factories for boxes, spheres, cylinders, planes, and ground |
| `Environment` / `EnvHandle` | Outdoor/sunset/overcast/night scene setup with terrain, water, and fog chaining |
| `Debug3D` | Final-overlay diagnostics plus axes and physics wire debug rendering |

Constant classes are runtime-backed too: `Layers`, `BodyShape`, `SyncMode`,
`AlphaMode`, `ShadingModel`, `QualityLevel`, `CollisionPhase`, `Keys`, and
`MouseButtons`.

All opaque Game3D and composed Graphics3D handles are validated by both runtime
class and complete implementation size before their private payload is read.
Wrong-class and same-class-but-undersized handles trap at mutating Game3D
boundaries and otherwise fail closed with the documented neutral result. A
trap handler is allowed to return, so every entry point also returns before
touching state after reporting an invalid or destroyed receiver.

`ShadingModel` mirrors `Zanna.Graphics3D.Material3D`: `Phong=0`, `Toon=1`,
`PBR=2`, `Unlit=3`, `Fresnel=4`, and `Emissive=5`.

---

## World3D Defaults

`World3D.New(title, width, height)` creates:

| Field | Backing runtime object |
|-------|------------------------|
| `canvas` | `Zanna.Graphics3D.Canvas3D` |
| `camera` | `Zanna.Graphics3D.Camera3D` |
| `scene` | `Zanna.Graphics3D.SceneGraph` |
| `physics` | `Zanna.Graphics3D.PhysicsWorld3D` |
| `input` | `Zanna.Game3D.Input3D` |
| `audio` | `Zanna.Game3D.Sound3D` with a camera-aligned listener |
| `effects` | `Zanna.Game3D.EffectRegistry3D` with a `PostFX3D` chain and particle/decal registry |
| `stream` | Lazily created `Zanna.Game3D.WorldStream3D` owned by the world |
| `droppedFixedSteps` | Integer counter for fixed-step updates discarded by the spiral-of-death guard |
| `fixedInterpolationAlpha` | Fixed-step accumulator fraction for render interpolation |
| `entityCount` | Spawned `Entity3D` objects currently owned by the world |
| `bodyCount` | Physics bodies currently registered through spawned entities |
| `drawCount` | Main 3D draw submissions queued by the latest ended frame |
| `visibleNodeCount` | Drawable scene nodes submitted by the latest scene draw |
| `occludedDrawCount` | Draw submissions skipped by latest visibility culling |
| `streamResidentBytes` | Resident byte estimate from the world-owned stream controller |
| `workerCount` | Internal deterministic job worker count, clamped to `1..64` |
| `jobsEnabled` | True when `workerCount > 1`; false when the world runs single-threaded |
| `FloatingOrigin` | Enables camera-relative rebasing for large-coordinate scenes |
| `worldOrigin` | Accumulated world-space offset applied by floating-origin rebases |

The constructor installs explicit default lighting, balanced backend-safe
quality, frustum culling, a readable camera, a neutral clear/ambient setup, and
the default audio listener. These are normal runtime choices, not hidden Zia
setup code.

`World3D.runFixed` caps the number of simulation steps processed by one rendered
frame. If a long frame would require more work than the cap, the dropped step
count is exposed through `World3D.droppedFixedSteps` for telemetry and tuning.
The `runFixed` update argument accepts a native-compiled function reference or
an interpreted Zia function reference with signature `(Float) -> Unit`.

`World3D.RenderInterpolation` (default off) makes fixed-step motion render
smoothly on displays faster than the simulation rate: during each rendered
frame, entity scene-node poses are blended between the previous and current
fixed steps by `FixedInterpolationAlpha` (position lerp, rotation
normalized-lerp) and restored immediately after drawing, so simulation state —
and determinism — are never touched. Poses captured before a floating-origin
rebase are discarded rather than lerped across the rebase delta.
`FixedInterpolationAlpha` remains available for games that blend visual-only
state manually.
Raw `Zanna.Graphics3D.PhysicsWorld3D` users can use the same fixed-step pattern
without the Game3D facade through `Physics3DWorld.StepFixed(dt, fixedDt,
maxSteps)`, `FixedStepAlpha`, and `DroppedFixedSteps`. `fixedDt` should be
positive (commonly `1.0 / 60.0`) and `maxSteps` should be a positive spiral
guard; `StepFixed` returns the fixed steps actually run and carries the
remainder on the world.

`Physics3DWorld` solver tuning is per-world: `SolverIterations` defaults to `6`
for velocity contacts and joints, `PositionIterations` defaults to `1`,
`ContactBeta` defaults to `0.8` and clamps to `0.0..1.0`, and
`RestitutionThreshold` defaults to `0.5` m/s and clamps to finite non-negative
values. Safe iteration ranges are `1..64`; higher values can improve stacked
contacts and constraints at additional CPU cost.
The editor/perf counters (`entityCount`, `bodyCount`, `drawCount`,
`visibleNodeCount`, `occludedDrawCount`, and `streamResidentBytes`) are
read-only wrappers over the owned scene, canvas, physics world, and stream, so a
tool can inspect one world handle without reaching through each subsystem.

`Game3D.Diagnostics` exposes process-wide degradation counters for rare runtime
fallbacks that keep execution correct but indicate pressure or lost fidelity:
`BroadphaseFallbackCount`, `CcdClampedFrames`, `CcdClampedBodies`,
`AnimEventsDropped`, `AudioVoicesEvicted`, `NavGridFallbacks`,
`StaleEntityCalls`, and `StaleAsyncLoadsDropped`, plus renderer/solver health
counters: `EpaFallbacks` (convex-pair penetration recovery hit its polytope
caps and emitted a zero-depth contact — persistent counts usually mean an
over-detailed hull collider worth decimating), `ShadowSlotsReused` (shadow maps
served from their previous-frame depth because the caster set, light transform,
and resolution were provably unchanged), and `AutoInstancedDraws` (opaque draws
folded into instanced batches by the renderer). The latter two are throughput
health metrics: they are property-only and never appear in `Summary()`, which
remains a pure degradation digest. `Reset()` clears the process
aggregates. `Summary()` returns stable `name=value` lines in that order,
omitting zero counters and returning `""` when clean. Smoke probes can print
`Game3D.Diagnostics.Summary()` and assert it is empty.

Diagnostic increments, reads, and resets are atomic across simulation,
streaming, animation, renderer, and audio-producing threads. Counters saturate
at `Int64.MaxValue`; concurrent producers cannot lose increments or invoke
signed-overflow behavior. `Summary()` snapshots through the public atomic
getters and emits complete lines only, including when every degradation counter
is at its maximum value.

`Zanna.Graphics3D.PhysicsWorld3D.BroadphaseFallbackCount` reports the matching
per-world broadphase fallback total. CCD inspection remains available through
`LastCcdRequestedSubsteps`, `LastCcdSubsteps`, `CcdSubstepClampedCount`, plus
`LastCcdClampedBodyCount` and `CcdSubstepClampedBodyCount` for affected-body
totals.

`World3D.SetWorkerCount(count)` controls the worker budget reserved for internal
3D jobs. `1` disables threaded jobs while preserving deterministic behavior;
values above `1` allow systems that opt into ordered internal jobs to use
workers. The runtime lazily creates a world-owned worker pool for eligible
batches; animator updates already use this path and are covered by single-worker
vs. multi-worker parity tests. User-authored job callbacks are not part of the
Game3D surface.

`World3D.SetOriginRebaseThreshold(meters)` configures the floating-origin
distance threshold. With `FloatingOrigin` enabled, the world recenters around
the active camera when it crosses that threshold, calls
`SceneGraph.RebaseOrigin()` for scene root subtrees, shifts physics bodies by the
same delta, shifts the explicit audio listener pose, and rebases world-owned
effect-registry particles and decals. The absolute offset accumulates in
`worldOrigin`. `World3D.RebaseOrigin(dx, dy, dz)` exposes the same cross-system
boundary for explicit world-streaming or sector handoff code and must be called
between frames; calling it during an active `BeginFrame`/`EndScene` pass traps
before state changes. During `World3D.BeginFrame`, the flag also enables Canvas3D
camera-relative upload for
double-precision `DrawMesh` model matrices, identity-matrix raw meshes built
through `Mesh3D.AddVertex`, generated `Particles3D` / `Sprite3D` / decal
billboard vertices, and point/spot light positions, so backend float payloads
stay near the camera. With the flag off, bounded scenes use the unchanged
absolute-upload path. The local Game3D CTests include a software final-frame
parity check between a near-origin scene and the same scene after a 50 km
floating-origin rebase, plus a byte-identical bounded-scene check after the
feature is toggled back off. `g3d_bounded_no_regression_probe` also runs the
existing `walk_min.zia` bounded sample with scale flags explicitly off and
checks exact final-frame pixel and captured-state parity.

`World3D.WithCamera(title, width, height, fov, near, far)` uses the same
defaults with custom vertical-FOV camera projection values.
`World3D.WithHorizontalCamera(title, width, height, fov, near, far)` accepts a
horizontal FOV and converts it for the runtime camera projection. `NewWithCamera`
and `NewWithHorizontalCamera` remain available as compatibility aliases.

`World3D.NewFullscreen(title)` and
`World3D.NewFullscreenWithHorizontalCamera(title, fov, near, far)` create the
window directly in fullscreen at the desktop resolution — it never appears at
a windowed size first. The world and camera adopt the actual display
dimensions (`world.Canvas.Width`/`Height` report them). Runtime toggling via
`Canvas3D.SetFullscreen`/`ToggleFullscreen` (e.g. an F11 binding) still works;
pass `--windowed`-style flags through your own game code by choosing the
constructor at startup. `Canvas3D.NewFullscreen(title)` is the low-level
equivalent when driving the canvas directly.

---

## Presets And Prefabs

Phase 3 adds runtime-backed convenience for the setup code that made small 3D
games noisy:

| API | Purpose |
|-----|---------|
| `Lighting.Studio(world)` | Balanced key/fill lighting for object inspection and samples |
| `Lighting.Outdoor(world, sunDir)` | Sunlight and ambient sky for outdoor scenes |
| `Lighting.Night(world)` / `Interior(world)` / `Clear(world)` | Night, indoor, or empty lighting rigs |
| `Materials.Plastic/Metal/Rubber(r, g, b)` | Common PBR material presets |
| `Materials.Glass(r, g, b, alpha)` | Transparent, double-sided reflective material |
| `Materials.Emissive(r, g, b, intensity)` | Emissive material for lights, pickups, and UI-like meshes |
| `Materials.Unlit(r, g, b)` | Flat unlit material |
| `Materials.FromAlbedoMap(pixels)` | Textured material from a `Pixels` albedo map |
| `PostFX.Cinematic(world)` / `Crisp(world)` / `None(world)` | Backend-safe effect chains; cinematic keeps vignette subtle so demos stay readable at the screen edges |
| `Quality.Apply(world, QualityLevel.*)` | Applies quality, post-FX, frustum culling, and shadow policy safely |

Prefab factories return normal spawnable `Entity3D` objects with mesh and
material already wired:

```zia
var metal = Game3D.Materials.Metal(0.65, 0.68, 0.72);
var crate = Game3D.Prefab.BoxXYZ(1.0, 0.8, 1.4, metal);
Game3D.Entity3D.SetPosition(crate, 0.0, 0.4, -2.0);
Game3D.World3D.Spawn(world, crate);
```

Available prefab factories are `Box`, `BoxXYZ`, `Sphere`, `Cylinder`, `Plane`,
and `Ground`. `Ground` also sets the entity layer to `Game3D.Layers.World`.
Sphere and cylinder segment requests are bounded to `8..256`, including their
fallback path. Optional materials must be complete `Material3D` objects and are
validated before a mesh is allocated. Lighting presets construct their entire
light rig before replacing the current one, so allocation failure leaves the
existing lighting intact; prefab and post-FX construction likewise fail
without publishing partial objects.

---

## Environment

`Environment3D.Outdoor(world)`, `Sunset(world)`, `Overcast(world)`, and
`Night(world)` apply clear color, ambient/light setup, fog, and a basic ground
entity immediately. The returned `EnvHandle` can refine the scene:

```zia
var env = Game3D.Environment3D.Outdoor(world);
Game3D.EnvHandle.WithTerrain(env, 96.0, 0.0);
Game3D.EnvHandle.WithWater(env, -0.05);
Game3D.EnvHandle.WithFog(env, 20.0, 160.0);
Game3D.EnvHandle.WithHeightFog(env, 0.05, 2.0, 0.25); // density, base height, falloff
```

`WithHeightFog` enables exponential **height fog** on the world's canvas:
density pools below the base height and thins above it (dense valleys, clear
peaks), combining with distance fog through joint transmittance. For
atmosphere looking toward the sun, `Canvas3D.SetHeightFogSun(r, g, b, power,
amount)` tints the fog toward a sun color by view-sun alignment (the
"silver lining" term; the sun direction follows the first enabled directional
light, and `amount` `0` — the default — keeps legacy fog output unchanged).
`Canvas3D.ClearHeightFog()` disables height fog alone;
`Canvas3D.HeightFogEnabled` reports the state.

Terrain is represented as a spawnable ground entity with a static body in this
phase, so character/physics samples get a useful floor without manual collider
setup. Water is a transparent plane prefab; `withWater()` uses the most recent
terrain size when terrain has been configured, otherwise it falls back to the
default water size.

When a skybox cubemap is installed (`World3D.SetSkybox`), image-based lighting
can replace the flat ambient term on PBR materials: set `World3D.IblEnabled`
to `true` (optionally scaling with `World3D.IblIntensity`, default `1.0`).
Diffuse ambient then follows the environment's directional irradiance and
specular reflections sample a prefiltered roughness chain — the standard
split-sum PBR environment model. The payload is computed once when IBL is
enabled or the skybox changes; frames pay nothing extra.

---

## Debug3D

`Debug3D` uses the same final overlay path as user HUDs, so its text is drawn
after post-FX rather than being bloomed, toned, or blurred.
On GPU post-FX backends, recorded final overlays are replayed into the backend
after `apply_postfx` and before `present` when the backend exposes the split
path. Legacy backends still use `present_postfx` with the overlay target, so
capture and presentation both composite crisp overlays over the post-FX scene.

```zia
Game3D.Debug3D.ShowOverlay(world, true);
Game3D.Debug3D.DrawAxes(world, new Math.Vec3(0.0, 0.0, 0.0), 1.5);
Game3D.Debug3D.DrawPhysics(world, true);
Game3D.Debug3D.DrawCameraInfo(world, true);
Game3D.Debug3D.DrawCapabilities(world, true);
```

The overlay reports backend, FPS, requested/active quality, fallback state,
scene node/cull counts, physics body count, optional camera position, backend
capability bits, and whether physics wire debug is enabled. `DrawEffects()`
draws axes and physics wire helpers during the 3D pass.

---

## Frame Order

The managed frame helpers use this order:

1. Poll or advance input/time.
2. Update `world.dt`, `world.elapsed`, and `world.frame`.
3. Run gameplay update if a callback loop is being used.
4. Run the installed Game3D camera/controller `Update`.
5. Advance spawned `Entity3D.Anim` controllers and collect animation events.
6. Step physics.
7. Sync physics-owned nodes, animation root motion, and 3D audio bindings.
8. Update the effect registry and expire finished particle/decal effects.
9. Run the installed Game3D camera/controller `LateUpdate`.
10. Begin the frame and draw the scene.
11. Draw the effects registry.
12. End the scene.
13. Draw the final overlay if an overlay callback is being used.
14. Finalize/present, or leave the finalized frame available for capture.

Controller `Update` runs inside `StepSimulation(step)` before the physics step,
which means manual loops can still do:

```zia
if (Game3D.World3D.Update(world)) {
    // game code may adjust controller properties, spawn entities, etc.
    Game3D.World3D.StepSimulation(world, Game3D.World3D.get_DeltaTime(world));
    Game3D.World3D.BeginFrame(world);
    Game3D.World3D.DrawScene(world);
    Game3D.World3D.EndScene(world);
    Game3D.World3D.Present(world);
}
```

`LateUpdate` runs after physics and binding sync, so follow cameras observe the
final entity pose for the frame rather than the previous frame's transform.

Manual code can use the same pieces directly:

| Method | Purpose |
|--------|---------|
| `Update()` | Poll live input, sync backing-window resize state, and advance world timing; returns false when the window should close |
| `StepSimulation(step)` | Dual contract. **Combined loop** (an `Update` ran earlier this frame — the pattern above): `step` is the ALREADY-SCALED simulation delta (`DeltaTime`); time-scale, hit-stop decay, and the frame/elapsed/unscaled counters are NOT re-applied (Update did them once), and a zero/paused delta performs a pure re-render frame. **Standalone** (no `Update` this frame — fixed-step drivers, tests): historical semantics — `step` is an unscaled real delta, clamped into the safe range, scaled by `TimeScale`/hit-stop here, with `frame`/`elapsed` advanced. Both paths then step controllers, animation, physics, scene/audio bindings, effect expiry, and late camera/controller work |
| `BeginFrame()` | Clear and begin drawing with the world camera |
| `DrawScene()` | Draw the world scene |
| `DrawEffects()` | Draw effect-registry particles/decals plus debug axes/physics wires |
| `EndScene()` | End the draw pass without presenting |
| `CaptureFinalFrame()` | Finalize post-FX/overlay and return `Pixels` |
| `Present()` | Finalize if needed and present the frame |

For deterministic tests, use `RunFrames(frameCount, stepSec, &update)`,
`RunFramesOnly(frameCount, stepSec)`, or call the manual methods explicitly.
`RunFramesOnly` uses the synthetic clock path and leaves the final frame
capturable. `RunFrames` / `RunFramesOnly` temporarily switch the backing canvas
to synthetic input and fixed-clock timing for callback, simulation, and render
work, keep synthetic-held keys/buttons down across the whole deterministic run,
then restore the previous canvas input source, clock source, and synthetic delta
after the run completes. If a `RunFrames` callback traps, the synthetic canvas
state is restored before the original trap is rethrown. The built-in run loops
avoid double-counting time because `Update()` / `RunFrames()` own frame and
elapsed-time accounting.

For worker-parity probes, set `World3D.SetWorkerCount(world, 1)` and a
multi-worker value before separate `RunFramesOnly` replays and compare the final
state. Internal jobs must merge results deterministically, so worker count should
not affect simulation results.

For future simulation-touching changes, rerun the full determinism gate: the
Game3D worker-count replay tests, the ordered-merge surface probe, the native
Zia promise tests, `test_codegen_env_is_native`, and `test_crosslayer_arith`.
Changes that alter IL, VM, or native codegen semantics used by simulation also
need the GATE-009 ADR/proof note.

The raw `Canvas3D` finalization calls map to the Game3D frame helpers this way:

| Raw call | Meaning |
|----------|---------|
| `Canvas3D.End()` | Close the 3D scene pass. This does not replay final overlays, read pixels, or present. |
| `Canvas3D.FinalizeFrame()` | Idempotently apply post-FX and replay recorded final overlays. |
| `Canvas3D.ScreenshotFinal()` | Finalize the frame, then read back the post-FX-plus-overlay pixels. |
| `Canvas3D.Flip()` | Finalize the frame if needed, then present it. |

`World3D.EndScene()`, `CaptureFinalFrame()`, and `Present()` are the Game3D
wrappers for that contract.

`World3D.OnResize(width, height)` updates the camera aspect and resizes the
owned `Canvas3D`, including backend render targets, so window callbacks and
manual resize paths keep the Game3D camera and Graphics3D output in sync.
`World3D.Update()` also observes native canvas size changes after polling so
platform window resizes update `world.Width`, `world.Height`, and the camera
aspect even when the app does not call `OnResize()` directly. Game3D tracks the
backing window size, not a temporary `RenderTarget3D` bound on the canvas.

---

## Callback Boundary

`World3D.run`, `runWithOverlay`, `runFixed`, `runFixedWithOverlay`,
`RunFrames`, and `DrawOverlay` accept script function references in both
interpreted and native-compiled Zia. Update callbacks must have signature
`(Float) -> Unit`; overlay callbacks must have signature `() -> Unit`.

Native-compiled Zia lowers `&update` and `&overlay` to executable function
pointers. Interpreted Zia keeps those references as VM-managed callback handles;
the Game3D VM bridge resolves them against the active module and calls the C
runtime with native trampoline pointers, preserving the active VM state and
script globals during callback re-entry.

If a callback reference is not a function in the active VM module, or if its
arity/return shape does not match the required callback signature, the call traps
with an API-specific diagnostic such as
`Game3D.World3D.runFixed: update callback must have signature (Float) -> Unit`.
The lower C runtime still validates raw pointer calls so direct embedding code
gets a clear `native function pointer` diagnostic for garbage or non-executable
pointers.

This boundary is covered by `g3d_test_game3d_runframes_callback_probe`,
`g3d_test_game3d_runfixed_callback_probe`, and
`native_run_game3d_runfixed_callback_probe`.

---

## Entities, Layers, And Masks

`Entity3D.Of(mesh, material)` creates a spawnable entity with a raw
`SceneNode`. `Entity3D.FromNode(rootNode)` wraps an existing `SceneNode`
hierarchy, which is the preferred bridge for imported or procedurally assembled
subtrees that should move through Game3D spawning without cloning. If the root
node has a non-empty name, the wrapper entity inherits it so
`World3D.FindEntity(name)` and `World3D.FindNode(name)` agree on the imported
root; both return `Some(value)` or `None`. Raw child nodes stay raw scene nodes: they render and participate
in scene lookup as part of the subtree, but they are not separate Game3D
entities. When `rootNode` is a `SceneGraph`'s implicit root, `FromNode`
transactionally installs a new empty implicit root in that source graph and
transfers the complete former hierarchy into the entity. The source graph stays
valid and independent, animator/node bindings remain on the transferred nodes,
and `World3D.spawn` never reparents an implicit scene root. If the transfer
cannot preflight its complete owner update, it traps without changing the source
graph.
Transform helpers sanitize non-finite numbers before touching the node and update
an attached body only when the node sync mode is `SyncMode.BodyFromNode`:

| Method | Purpose |
|--------|---------|
| `FromNode(rootNode)` | Wrap a detached hierarchy, or transactionally transfer a `SceneGraph`'s implicit root hierarchy into a spawnable group entity while replacing the source root |
| `SetPosition(x, y, z)` / `SetPositionV(vec3)` | Set node position |
| `SetScale(s)` / `SetScaleXYZ(x, y, z)` | Set node scale |
| `SetRotationEuler(xDeg, yDeg, zDeg)` | Set node orientation in degrees |
| `Mesh` / `Material` | Read/write properties replacing the render resources |
| `SetMeshRecursive(mesh)` | Replace mesh handles on every drawable raw node in the entity subtree; if none are drawable, assign the root |
| `SetMaterialRecursive(material)` | Replace material handles on the root and every raw node in the entity subtree |
| `AddChild(child)` | Parent another Game3D entity; reparents from an old Game3D parent and rejects self/cycle parenting |
| `Name` | Read/write property naming the entity and backing node for lookup |
| `Layer` | Read/write property: gameplay/physics layer |
| `CollisionMask` | Read/write property: the layer mask used by attached bodies |
| `AttachBody(bodyDef)` | Create and attach a `Physics3DBody` from a `BodyDef` |
| `AttachAnimator(animator)` | Attach an `Animator3D` or raw `AnimController3D` to the entity node |
| `Position` / `WorldPosition` | Read local/world position (read-only properties) |
| `IsSpawned()` / `IsDestroyed()` | Inspect lifecycle state |
| `IsGroup()` | True when the entity wraps an imported/`FromNode` multi-node group rather than a single primitive |

`World3D.Spawn(entity)` attaches the entity node to the world scene and registers
the entity by name. `World3D.Despawn(entity)` removes it from the registry,
scene, and physics world, and the retained entity handle becomes stale.
Destroying a world also marks any retained entities from that world stale. Stale
entity getters return neutral values (`0`, `null`, or origin vectors), mutators
including `AttachAnimator` no-op, and each stale API call increments
`Game3D.Diagnostics.StaleEntityCalls`; invalid non-entity handles still trap.
`IsSpawned()` and `IsDestroyed()` remain available for lifecycle inspection.
Child Game3D entities are owned by their parent for despawn purposes; raw
imported child nodes remain part of the imported node subtree. Adding a child to
an already-spawned entity spawns that child into the same world; adding a
spawned child under an unspawned entity is rejected because it would detach the
child from its world scene.

Use `LayerMask.None()`, `LayerMask.All()`, `LayerMask.Of(layer)`,
`include(layer)`, and `includes(layer)` for readable filters. Layer values are
validated as single-bit masks. Physics query masks follow the same bit semantics:
`LayerMask.None()` matches no layers, while `LayerMask.All()` matches any layer.

`AttachBody` also accepts a raw `Zanna.Graphics3D.PhysicsBody3D` as an escape
hatch. The common path should use `BodyDef`, because it applies filters, node
binding, sync mode, and world registration consistently. The default
`BodyDef` sync mode is `NodeFromBody`: spawn seeds the body from the node once,
then simulation owns the body and writes the resulting pose back to the node.
Use `BodyFromNode` for scripted/authoritative transforms that should push into
physics on every transform setter. A raw body can only belong to one spawned
entity in a world; `spawn` and `AttachBody` reject attempts to share it so
collision events and physics ownership stay unambiguous. Reattaching the same
spawned raw body to the same entity is a no-op for world registration; it keeps
the existing physics-world body and body index stable.

---

## Assets3D

`Assets3D` loads `SceneAsset` assets into spawnable Game3D entities without manual
scene-node cloning:

```zia
var crate = Game3D.Assets3D.LoadEntity("assets/crate.glb");
Game3D.Entity3D.SetPosition(crate, 0.0, 0.5, -3.0);
Game3D.World3D.Spawn(world, crate);

var enemyTemplate = Game3D.Prefab.LoadAsset("models/enemy.glb");
var enemy = Game3D.SceneTemplate.Instantiate(enemyTemplate);
Game3D.World3D.Spawn(world, enemy);
```

| Method | Purpose |
|--------|---------|
| `LoadEntity(path)` | Load a filesystem/development scene asset and return a group `Entity3D` |
| `LoadEntityAsset(assetPath)` | Load through the asset resolver first, with filesystem fallback for development |
| `Prefab.Load(path)` | Load or reuse a cached filesystem prefab represented by a `SceneTemplate` |
| `Prefab.LoadAsset(assetPath)` | Load or reuse a cached package-aware prefab represented by a `SceneTemplate` |
| `LoadEntityResult(path)` / `LoadEntityAssetResult(assetPath)` | `Zanna.Result` peers of the entity loaders: ok wraps the entity, err carries the loader diagnostic instead of returning `null` (ADR 0233) |
| `LoadAnimationResult(path, index)` / `LoadAnimationAssetResult(assetPath, index)` | `Zanna.Result` peers of the skeletal-clip loaders |
| `LoadNodeAnimationResult(path, index)` / `LoadNodeAnimationAssetResult(assetPath, index)` | `Zanna.Result` peers of the node-animation loaders |
| `Prefab.LoadResult(path)` / `Prefab.LoadAssetResult(assetPath)` | `Zanna.Result` peers of the prefab loaders |
| `LoadEntityAsync(path)` | Return an `AssetHandle3D` for a filesystem/development entity |
| `LoadEntityAssetAsync(assetPath)` | Return an `AssetHandle3D` for a package-aware entity |
| `Prefab.LoadAsync(path)` | Return an `AssetHandle3D` for a cached filesystem prefab |
| `Prefab.LoadAssetAsync(assetPath)` | Return an `AssetHandle3D` for a cached package-aware prefab |
| `SetResidencyBudget(bytes)` | Bound the shared template cache by estimated resident bytes; negative means unlimited |
| `GetResidentBytes()` | Report estimated resident bytes held by the shared template cache |
| `SetResidencyHint(template, priority, distance)` | Bias cached-template eviction so higher-priority and nearer templates survive pressure first |
| `SetUploadBudget(bytes)` | Bound each async asset commit drain by decoded texture bytes; negative means unlimited |
| `Evict(handle)` | Drop the cached template entry backing a ready template `AssetHandle3D` |
| `Preload(path)` | Start a background filesystem template-cache warm |
| `PreloadAsset(assetPath)` | Start a background package-aware template-cache warm |
| `ClearCache()` | Release cached template entries and prevent older preload jobs from repopulating the cache |
| `SceneTemplate.Instantiate()` | Clone the template root subtree into a group `Entity3D` |

Prefabs load through `Prefab.Load*` and complete through `AssetHandle3D.GetPrefab()`,
so the object lifecycle reads as asset -> prefab -> entity -> world.

Loaded entities are groups whose backing node is the instantiated model root.
The raw imported child nodes remain under that root and are not separate
Game3D child entities. Despawning the group removes the whole subtree from the
scene. If the imported root has a skeletal animation controller, `entity.anim`
is populated with a `Game3D.Animator3D` wrapper.

`LoadEntityAsset` and `Prefab.LoadAsset` use the runtime asset resolver, so
mounted packages and `asset://` paths work the same way as lower-level
`SceneAsset.LoadAssetResult`. Relative glTF buffers and textures resolve
relative to the model asset.

Filesystem template cache keys are canonicalized to absolute paths when
possible, and concurrent requests for the same key share the in-flight load
instead of importing the same model more than once. Waiting threads sleep on the
cache condition variable rather than polling while another thread finishes an
import.

`AssetHandle3D` exposes `ready`, `progress`, `error`, `cancel()`,
`getEntity()`, and `GetPrefab()`. Handles start with `ready == false` and
`progress == 0.0`; first observation through `ready`, `progress`, `error`,
`getEntity()`, or `GetPrefab()` services the process-wide asset commit queue
and starts worker staging/loading for valid uncached model requests. While the
worker is running, `ready` remains false and repeated observations keep draining
committed results. On success, progress becomes `1.0` and `error == ""`. Entity loaders
return an entity from `getEntity()` and `null` from `GetPrefab()`; prefab
loaders do the inverse. Cached prefab handles can complete immediately on
first observation.

Missing filesystem paths and missing asset-manager paths become terminal
load-error handles on observation (`"cannot read file"` or `"asset not found"`)
rather than trapping. Existing files with unsupported model extensions complete
with `"unsupported file extension"`. Malformed glTF JSON or other importer
failures complete through the worker/commit path with `"failed to load model"` or
`"failed to load model asset"`. Calling `cancel()` before worker completion makes
the handle terminal with `error == "cancelled"` and no result. Calling `cancel()`
on a completed handle is a no-op, which keeps retrieved results stable.

Async handles accept the same model extensions as blocking `SceneAsset.LoadResult`:
`.gltf`, `.glb`, `.fbx`, `.obj`, `.stl`, and `.scene3d` (plus the legacy
`.vscn` alias, ADR 0182). glTF/GLB assets use the
worker preload path described below; the other formats are validated by the
handle and completed through the main-thread commit path so callers can use one
async API for mixed model libraries.

Current worker-backed async model loading stages glTF/GLB root bytes plus
external, data URI, and bufferView-backed buffer/image payloads on a runtime
worker. PNG, BMP, JPEG, and GIF image payloads in that preload bundle are decoded on the
worker into raw RGBA POD; the main-thread commit queue prepares those bytes into
`Pixels` objects in bounded slices, touching content generation once the image is
complete. Static, skinned, and morph-target glTF triangle topology
primitives (triangle lists, strips, and fans) with positions, optional normals,
sparse accessor overrides, and `JOINTS_0`/`WEIGHTS_0`/`JOINTS_1`/`WEIGHTS_1`
attributes are decoded into raw `Mesh3D` POD on the worker, then committed into
runtime mesh objects on the main thread; missing normals are regenerated during
commit, skin joint remapping still runs after skeleton import, and morph-target
delta payloads become attached `MorphTarget3D` objects during commit. The worker preload also
rejects missing required buffer
payloads, accessor ranges that overrun their declared bufferViews, and corrupt
required PNG/BMP/JPEG/GIF texture image payloads before the main-thread commit
queue builds the `SceneAsset`/`SceneTemplate`/`Entity3D` result. Blocking glTF
loads reject the same corrupt required data-URI texture images instead of
silently dropping the material map.
External `.bin` buffers can be loaded from the staged bundle even if the source
file disappears before commit. `Assets3D.Preload(path)` and
`Assets3D.PreloadAsset(assetPath)` schedule the same template-mode worker path as
background cache warms for filesystem and package-aware keys respectively. Async
template jobs snapshot the shared model-cache generation when scheduled; if
`Assets3D.ClearCache()` or another invalidation advances that generation before
the main-thread commit publishes the result, the stale staged model is discarded
and `Game3D.Diagnostics.StaleAsyncLoadsDropped` increments. A still-retained
handle is reloaded against the fresh generation; ownerless preload work is
dropped.
`World3D.Update` and simulation steps drain the asset commit queue with a fixed
per-frame item budget plus a decoded-texture byte budget so preload commits do
not require polling a returned handle. `Assets3D.SetUploadBudget(bytes)` changes
that byte budget: negative means unlimited, `0` pauses positive-cost decoded
texture slices, and positive budgets advance large decoded images across
multiple queue drains before the final model/template publish. Pair this with
`Canvas3D.SetTextureUploadBudget`, `TextureUploadBytes`, and
`TextureUploadPendingBytes` when profiling frames where decoded commits turn
into backend texture-cache uploads. That Canvas3D budget row-slices
Pixels-backed 2D material texture and cubemap uploads on Metal/OpenGL/D3D11 and
submits native compressed `TextureAsset3D` resident mip blocks on capable GPU
backends. The backend pending-byte counters return to zero when final row slices
and native mip submissions drain, and the open-world hitch probe verifies
resident-byte cache churn returns to zero after blocking and async clears. Its
GPU opt-in CTest also records native compressed upload pressure; the local
macOS/Metal lane uses ASTC and reports `native_zero_pending_bytes=16` followed
by `native_upload_bytes=16` once a positive texture-upload budget is restored.
The same run records `native_raw_rgba_bytes=64`,
`native_compressed_bytes=16`, `native_ram_reduction_pct=75`,
`native_vram_reduction_pct=75`, and a checked final-frame tolerance.
Non-glTF formats still enqueue a main-thread load request.

`ClearCache()` advances the template-cache generation. Any `Preload` /
`PreloadAsset` job that was already in flight may still finish, but it publishes
only to its internal handle and does not reinsert a stale template into the
fresh cache generation. If another thread is actively publishing cache entries,
`ClearCache()` waits briefly for loading entries to settle; on timeout it leaves
the current cache intact rather than clearing entries while publishers still own
them.

`SetResidencyBudget(bytes)` applies to the shared `SceneTemplate` cache. The
budget uses a conservative CPU/GPU-resource estimate that includes mesh buffers,
model metadata, and decoded material texture pixels, treats a negative value as
unlimited, and considers only non-loading entries for eviction. A budget of `0`
keeps returned templates alive for callers that hold them, but prevents them
from remaining in the shared cache. `SetResidencyHint(template, priority,
distance)` annotates cached templates for streaming policy: higher priority
survives pressure before lower priority, and nearer templates survive farther
templates when priority ties; unhinted templates keep the previous LRU behavior.
`GetResidentBytes()` returns the same cache counter, so tests and tooling can
assert that blocking and async load/clear churn returns resident template bytes
to zero. `Evict(handle)` is explicit cache eviction for ready template handles;
entity handles are accepted as a stable no-op.

---

## WorldStream3D

`WorldStream3D` is the Game3D handle for world-partition and terrain-streaming
state. `World3D.stream` lazily creates a stable world-owned stream handle for
the common case; `WorldStream3D.New(world)` still creates a separate stream
controller when tooling or tests need an isolated handle:

```zia
var stream = world.stream;
Game3D.WorldStream3D.MountCells(stream, "assets/world/cells.vscn");
Game3D.WorldStream3D.MountTiledTerrain(stream, "assets/world/terrain.vscn");
Game3D.WorldStream3D.SetRadii(stream, 256.0, 320.0);
Game3D.WorldStream3D.SetCenter(stream, player.worldPosition);
Game3D.WorldStream3D.Update(stream, world.dt);
```

`SetCenter`, `SetRadii`, `setResidencyBudget`, `MountCells`,
`MountTiledTerrain`, and `Update` are registered and tested. `MountCells`
parses a VSCN streaming manifest with a `cells` array:

```json
{"cells":[{"name":"town_00","path":"cells/town_00.vscn","center":[0,0,0],"radius":64,"bytes":65536}]}
```

Cell paths are resolved relative to the manifest. Each resident cell loads its
`.vscn`, transactionally transfers the loader scene's root hierarchy into a
detached Game3D entity, and then spawns that entity into the world. The retained
loader scene remains valid with a replacement empty root for load metadata and
teardown compatibility. Residency measurement, HLOD proxy baking, and impostor
generation follow the entity-owned hierarchy, not that replacement root.
`Update` loads/unloads cells around the current center using load/unload radii.
`MountTiledTerrain` parses a
terrain manifest with a `tiles` array using the same `name`, `path`, `center`,
`radius`, and `bytes` fields, plus optional `width`, `depth`, `scale`, and
`heightmap` for the `Terrain3D` payload:

```json
{"tiles":[{"name":"terrain_00","path":"terrain/terrain_00.tile","heightmap":"terrain/terrain_00.height","center":[0,0,0],"radius":128,"bytes":262144,"width":129,"depth":129,"scale":[4,32,4],"material":"grass","collision":{"enabled":true,"layer":1,"mask":-1},"navArea":"meadow","traversalCost":1.0,"sidecar":"terrain/terrain_00.bin"}]}
```

Terrain and heightmap paths are resolved relative to the manifest and terrain
tile telemetry loads/unloads deterministically around the current center.
Resident terrain tiles instantiate `Terrain3D` payloads using the manifest
dimensions and scale; an optional `heightmap` sidecar uses a small text format,
`zanna-heightmap-v1 <width> <depth>` followed by normalized height values, and
is applied through the existing 16-bit `Terrain3D.SetHeightmap` path. The
world-owned stream renders resident terrain tiles during `World3D.DrawScene`
using their manifest-centered world positions. Each resident terrain tile also
owns an invisible `*_heightfield_collider` entity
with a static `Collider3D.NewHeightfield` body, so physics residency follows
terrain residency and unloads with the tile. Resident tiles also spawn hidden
mesh-only `*_navmesh_source` nodes so `World3D.bakeNavMesh` includes streamed
terrain through the existing scene bake path. When two resident terrain tiles
share a full manifest edge, `WorldStream3D` stitches the adjacent border samples
in world-height space, invalidates cached terrain LOD meshes, and rebuilds the
tile collider/nav sources from the stitched height grid. This keeps the
single-`Terrain3D.New` 4096-sample cap intact while allowing larger streamed
worlds to be composed from multiple tiles.

Both `cells[]` and `tiles[]` may carry authored metadata for runtime/editor
handoff: `material` names the authored surface/material, `layer` or
`collisionLayer` selects a Game3D layer bit, `collisionMask` or nested
`collision.mask` selects the collision mask, nested `collision.enabled=false`
suppresses generated terrain heightfield colliders, `navArea` and
`traversalCost` describe navigation semantics, and `sidecar` / `binarySidecar`
points at optional binary metadata resolved relative to the manifest. Cell
layer/mask metadata is applied to the spawned root entity; terrain collision
metadata is applied to generated heightfield bodies. The metadata is also
available through typed `getCell*` and `getTerrainTile*` inspection methods, each
taking the manifest index returned by `getCellCount()` / `getTerrainTileCount()`
and mirroring the manifest fields. Resident-only payload access uses the
explicit `getResidentTerrainTile(index)` method:

| Method | Signature | Description |
|--------|-----------|-------------|
| `getCellMaterial(i)` / `getTerrainTileMaterial(i)` | `String(Integer)` | Authored surface/material name (`""` if unset) |
| `getCellLayer(i)` / `getTerrainTileLayer(i)` | `Integer(Integer)` | Game3D collision layer bit |
| `getCellCollisionMask(i)` / `getTerrainTileCollisionMask(i)` | `Integer(Integer)` | Collision mask |
| `getCellCollisionEnabled(i)` / `getTerrainTileCollisionEnabled(i)` | `Boolean(Integer)` | Whether generated colliders are enabled |
| `getCellNavArea(i)` / `getTerrainTileNavArea(i)` | `String(Integer)` | Navigation area name |
| `getCellTraversalCost(i)` / `getTerrainTileTraversalCost(i)` | `Double(Integer)` | Navigation traversal cost |
| `getCellSidecar(i)` / `getTerrainTileSidecar(i)` | `String(Integer)` | Optional binary sidecar path (`""` if unset) |
| `getCellSidecarBytes(i)` | `Integer(Integer)` | Resident bytes of the cell's loaded binary sidecar payload (0 if none or unloaded) |

When a resident cell declares a `sidecar` path, the world stream loads that file's
bytes as opaque cell metadata, counts them in the cell's resident-byte budget, and
frees them when the cell unloads. A missing, empty, or oversized sidecar is
recoverable (zero bytes, no error).

`getResidentTerrainTile(index)` returns the nth currently resident terrain
payload or `null`; the `getCell*` and `getTerrainTile*` metadata methods remain
manifest-indexed even when earlier entries are unloaded. The telemetry
properties (`residentCellCount`,
`residentTerrainTileCount`, `pendingRequestCount`, and `residentBytes`) reflect
resident cell payloads plus manifest-backed terrain tile residency. Loaded VSCN
cells are measured from their authored scene resources, including base meshes,
resident LOD meshes, impostor meshes, materials, and resident material textures.
`Mesh3D.Resident` / `SceneNode.SetLodResident` changes are remeasured on
subsequent `Update(dt)` calls so a cell can shed mesh detail without unloading
its whole scene subtree; unloaded cells continue to report the manifest `bytes`
estimate for planning and editor inspection. `Update(dt)` advances a
deterministic per-frame load budget; when
the stream center jumps across multiple desired cell/tile payloads, stale
payloads unload immediately, one or more new payloads are admitted by the frame
budget, any payload whose measured post-load size exceeds the active residency
budget is evicted before telemetry is published, and
`pendingRequestCount` reports deferred desired payloads until later updates
drain them (including cells that are staging or staged but not yet committed).
A budget of `0` unloads cells and reports no resident cells or tiles; a
negative budget is unlimited.

### Worker-backed streaming

Streaming is worker-backed by default: `Update(dt)` never does cell/tile file
IO on the main thread. Workers read `.vscn` text, sidecar bytes, and heightmap
text (parsed to a plain height grid off-thread), and the main thread commits
staged payloads — VSCN parse, scene spawn, `Terrain3D` build, collider/nav
registration, seam stitching — inside the deterministic nearest-first
recompute pass. Commits are **order-gated**: a farther cell never becomes
resident before a nearer desired cell, so the resident sequence matches
blocking mode at any worker count; async merely spreads the work across
updates. Configuration and telemetry:

| Member | Description |
|--------|-------------|
| `setAsyncStreaming(on)` / `getAsyncStreaming()` | Toggle worker staging (default on). Off restores single-update inline loads for bisection and determinism debugging. |
| `setCommitBudget(bytes)` | Max staged bytes committed per `Update` (`-1` unlimited, `0` holds all commits pending; an oversized first payload commits alone). |
| `setPrefetchLookahead(seconds)` | Stage cells/tiles along the smoothed center velocity this far ahead (default 2 s; `0` disables). Teleports — jumps larger than the unload radius — reset the velocity estimate so nothing prefetches along the jump vector. |
| `streamStallMs` | Worst single staged-commit slice in wall milliseconds since mount (budget-tuning observability). |
| `prefetchedCellCount` | Cells currently staged/staging purely from prefetch. |

Worker staging failures are recoverable: a missing or corrupt cell payload is
skipped with a reload cooldown and counts
`Game3D.Diagnostics3D.StreamStagingErrors`; staged payloads dropped without
committing (center reversed, remount) count `StreamStaleStagesDropped`. A
missing tile heightmap still loads a blank tile, matching the blocking loader.
Because staging is asynchronous, code that needs settled residency should loop
`Update(dt)` until `pendingRequestCount` reaches `0` rather than assuming one
update suffices.

### HLOD proxies and impostors

Cells may carry an optional `"proxy"` manifest field (plus `"proxyBytes"`)
naming a merged low-poly `.vscn` stand-in. Proxy-bearing cells add a third
residency ring: inside `setProxyRadius` (default 4x the load radius) but
outside the load radius, the stream attaches only the proxy subtree
(render-only — no colliders or nav sources); crossing the load radius swaps
proxy→full, and receding swaps back, with **no gap frame in either
direction** — the outgoing representation stays attached until the incoming
one commits. Proxy payloads ride the same worker staging pipeline as cells.
Telemetry: `proxyResidentCount` / `proxyResidentBytes` (also included in
`residentBytes`); `getCellProxy(i)` inspects the resolved path.

`bakeCellProxy(i)` is the authoring hook (run it after editing a cell, like a
navmesh bake — never during play): it merges the resident cell's drawable
meshes in cell-local space, simplifies to an 800-triangle budget, bakes each
source material's diffuse color into a small block atlas (flat-shaded, unlit),
and saves `<cell>_proxy.vscn` next to the cell (or at the authored `proxy`
path), updating the session's cell record so the ring works immediately.

`generateImpostors(distance)` renders each proxy-resident cell's proxy mesh
from 8 yaw angles into a horizontal strip (off-screen `RenderTarget3D`) and
installs it via `SceneNode.SetImpostorFrames(distance, strip, 8)` — beyond
`distance` the cell degrades to a single yaw-selected textured quad
(`GetImpostorFrameIndex` exposes the last frame the draw path picked). This
gives three rings: full cell → proxy mesh → impostor quad. Impostor strips
capture against the canvas background, so they read best at distances where
fog and sky dominate.

Editor/debug inspection uses method-style lower/camel calls:
`getCellCount()`, `getCellName(i)`, `getCellCenter(i)`,
`getCellResident(i)`, `getCellBytes(i)`, `getTerrainTileCount()`,
`getTerrainTileName(i)`, `getTerrainTileHeightmap(i)`, `getTerrainTileCenter(i)`,
`getTerrainTileResident(i)`, and `getTerrainTileBytes(i)`. Invalid indexes
return the usual safe defaults (`0`, `false`, `""`, or `null`).

`examples/3d/openworld_slice/` is the current end-to-end streaming smoke
project. Its CTest moves the stream through all four far-apart quadrants over
budgeted stream ticks,
asserts one resident cell/tile at a time, verifies bounded resident bytes,
validates heightmapped terrain-payload swaps, rendered stream terrain, and
inspection hooks, checks
the async `AssetHandle3D` model path, loads RGBA8 and BC7 KTX2
`TextureAsset3D` fixtures, renders a BC7 texture panel in the main scene,
loads a synthetic skinned glTF agent through
`Assets3D.LoadEntityAsset`, crossfades its auto-bound animator from `Idle` to
`Wave`, binds `IKSolver3D.LookAt` through `Animator3D.setIKSolver`, loads a
committed GLB through `Assets3D.LoadEntityAsset`, loads a committed WAV through
`Sound3D.loadAsset`, validates a terrain-sampled `IKSolver3D.TwoBone` foot
target, renders a visible foot marker/leg pair for that proof, reads the
`World3D` runtime counters, runs physics/character/nav-agent simulation,
compares the software final frame to the committed
`assets/baselines/openworld_slice_software.png` baseline, and repeats the same
fixed-step sequence to prove deterministic replay.
`streaming_hitch_probe.zia` records blocking-vs-async template load timing,
proves a zero upload budget keeps positive-cost async commit work pending until
the budget is restored, checks `Assets3D.GetResidentBytes()` returns to zero
after cache churn, and in its native-compressed CTest lane proves backend
texture-upload budget telemetry on a capable GPU backend.

---

## World-Scoped NavMesh Baking

`World3D.BakeNavMesh(agentRadius, agentHeight, maxSlope, cellSize)` and
`World3D.BakeTiledNavMesh(tileSize, agentRadius, agentHeight, maxSlope,
cellSize)` are Game3D editor hooks over the lower-level
`Zanna.Graphics3D.NavMesh3D.Bake*` APIs. They bake from the world's current
`SceneGraph`, including hidden streamed-terrain nav source nodes, preserve
world-space transforms, and return a
`Zanna.Graphics3D.NavMesh3D` for `NavAgent3D` use. Both entries produce tiled
bakes (the non-tiled hook auto-derives a tile size from the scene extent), so
`NavMesh3D.RebuildTile(tileX, tileZ)` can always refresh one tile's geometry
and obstacle state without a whole-scene bake; the tiled entry just pins an
explicit tile size.

---

## Animator3D

`Animator3D` wraps a lower-level `Zanna.Graphics3D.AnimController3D` while
keeping the common game loop readable:

```zia
var controller = AnimController3D.New(skeleton);
AnimController3D.AddState(controller, "idle", idleClip);
AnimController3D.AddState(controller, "run", runClip);
AnimController3D.AddEvent(controller, "run", 0.25, "footstep");
AnimController3D.SetRootMotionBone(controller, rootBone);

var anim = Game3D.Animator3D.New(controller);
Game3D.Entity3D.AttachAnimator(player, anim);
SceneNode.set_SyncMode(Game3D.Entity3D.get_Node(player),
                         Game3D.SyncMode.get_NodeFromAnimRootMotion());
Game3D.Animator3D.Play(anim, "run");
```

| Method / property | Purpose |
|-------------------|---------|
| `controller` | Raw `AnimController3D` escape hatch |
| `Play(name)` | Play a named controller state |
| `crossfade(name, seconds)` | Blend to another named state |
| `playLayerAdditive(layer, name)` | Play a named controller state as a true additive overlay layer |
| `crossfadeLayerAdditive(layer, name, seconds)` | Blend a named controller state as a true additive overlay layer |
| `setBlendTree(tree)` | Use a compatible `BlendTree3D` as the wrapped controller's base pose source |
| `setIKSolver(solver)` | Apply a compatible `IKSolver3D` after overlays and before skinning |
| `setSpeed(name, speed)` | Change a state's playback speed |
| `isPlaying(name)` | Check the active base-layer state |
| `stateTime()` | Current base-layer playback time |
| `EventCount()` / `eventName(index)` | Events captured during the latest play/update frame |
| `Update(dt)` | Manually advance an animator that is not spawned in a world |

Spawned entity animators are advanced automatically by `World3D.stepSimulation`
after camera/controller `Update` and before physics. If the entity node uses
`SyncMode.NodeFromAnimRootMotion`, the normal scene binding sync consumes the
controller root-motion delta and moves the entity node deterministically in the
same frame.

`Entity3D.attachAnimator` accepts either a `Game3D.Animator3D` wrapper or a raw
`AnimController3D`. Raw controllers are wrapped automatically so `entity.anim`
always exposes the Game3D helper surface. Imported model templates with a root
animation controller use the same wrapper path.

`Animator3D.EventCount()` and `eventName(index)` are the supported
interpreted-Zia event path. Optional callback sugar such as `onAnimEvent` is
deferred until the VM has a callback trampoline for managed function objects.
Layer entry events from `playLayerAdditive` are captured through the same event
buffer; `crossfadeLayerAdditive` uses the same event capture path. Layer masks
and weights remain controlled on the wrapped `AnimController3D` through the
`controller` escape hatch. `setBlendTree(tree)`
forwards to `AnimController3D.SetBlendTree`; it is useful when a movement
blendspace should supply the base pose while the controller still owns overlay
layers and event polling. `setIKSolver(solver)` forwards to
`AnimController3D.SetIKSolver` for foot/hand targets, look-at bones, and short
FABRIK chains driven from gameplay.

`Animator3D.FindBone(name)` resolves a bone index by name (-1 when unknown) and
`Animator3D.GetBoneMatrix(index)` returns the model-space composited bone
matrix — useful for custom effects anchored to skeletal joints.

### Bone Sockets

`Entity3D.AttachToBone(child, boneName)` (and the `AttachToBoneOffset`
variant with a bone-space positional offset) parents `child` under the entity
and drives the child's world transform from the named bone's composited pose
every simulation step — the standard way to put a weapon in a hand or a hat on
a head. The entity must have an attached `Animator3D` whose controller has a
skeleton containing the bone; unknown names trap with a clear message.
`Entity3D.DetachFromBone()` stops the tracking (the child keeps its last pose
and stays parented). The lower-level form is
`Graphics3D.SceneNode.AttachToBone(animator, boneIndex, ox, oy, oz)`.

---

## Behavior3D

`Behavior3D` gives entities composable, engine-ticked motion without per-frame
script code — the 3D counterpart of `Zanna.Game.Behavior`. Build one fluently,
attach it, and the world updates it every simulation step (before physics and
binding sync):

```zia
var pickup = Prefab.Sphere(0.3, 20, Materials.Emissive(1.0, 0.8, 0.2, 2.0));
pickup.AttachBehavior(
    Behavior3D.New().AddSpin(0.0, 1.0, 0.0, 90.0).AddSineFloat(0.25, 2.0));
world.Spawn(pickup);
```

Presets (all fluent, freely composable): `AddSpin(axis, degPerSec)`,
`AddOrbit(center, radius, degPerSec)` (XZ circle), `AddSineFloat(amplitude,
speed)`, `AddFaceTarget(entity)` (yaw toward a target), `AddChase(entity,
speed, range)` (direct XZ steering, or navmesh-routed when a `NavAgent3D` is
bound via `SetNavAgent`), `AddFollowPath(path, speed, loop)` (constant-speed
`Path3D` traversal by arc length, with speed measured in world units per
simulation second), and `AddLifetime(seconds)` (despawns the entity).
Presets apply in a fixed order each tick (lifetime, path, chase, orbit, sine,
spin, face) so composed behaviors stay deterministic.

---

## GameBase3D Scene Framework

`examples/games/lib/gamebase3d.zia` + `iscene3d.zia` provide the application
tier every 3D game otherwise re-implements: a driven frame loop with delta-time
clamping, an `IScene3D` lifecycle (`onEnter`/`onExit`/`Update`/`drawOverlay`),
deferred scene switches applied at frame boundaries, and fade transitions
rendered with `Canvas3D.DrawRect2DAlpha`. Subclass `GameBase3D`, build scenes
in `onInit()`, and call `setScene`/`transitionTo` — see
`examples/3d/game3d_scenes/` for a complete two-scene reference.

For input, load the `"fps3d"` action preset
(`Zanna.Input.Action.LoadPreset("fps3d")`) to get named, rebindable
`move_x`/`move_y`/`jump`/`sprint`/`crouch`/`interact`/`fire`/`aim`/`pause`
actions across keyboard, mouse, and gamepad instead of hard-coded key polling.

---

## Timers, Tweens, And Time

Game3D has no dedicated tween classes because the primitives already exist and
compose with the world clock:

- `Zanna.Game.Timer` — countdowns, repeating intervals, and cooldowns. Advance
  it with the same `dt` your update callback receives so timers respect
  `World3D` pause/step semantics.
- `Zanna.Game.Tween` — scalar interpolation with easing
  (`Start(from, to, durationMs, easing)`, then read `Value` per frame).
- `Zanna.Math.Vec3.Lerp(a, b, t)` — positional tweening. Drive `t` from a
  `Tween` (or accumulate `dt / duration`) and assign the result with
  `entity.SetPositionV(...)`.
- `Zanna.Math.Quat.Slerp(a, b, t)` — rotational tweening on the shortest arc;
  pair with `SceneNode.Rotation` for smooth turns, door swings, and camera
  blends.

A camera dolly, a door swing, or a pickup bob is one `Tween` for `t` plus one
`Lerp`/`Slerp` at the target — no per-component bookkeeping needed.

---

## Sound3D And Effects3D

`World3D.audio` owns a runtime `SoundListener3D` that follows the world camera
by default. The listener can be detached for cutscenes, replays, or split-view
tests:

```zia
var audio = Game3D.World3D.get_Audio(world);
Game3D.Sound3D.ListenerFollowCamera(audio, false);
Game3D.Sound3D.SetListenerPose(
    audio,
    new Math.Vec3(0.0, 2.0, 6.0),
    new Math.Vec3(0.0, 0.0, -1.0),
    new Math.Vec3(0.0, 1.0, 0.0));
```

| Audio API | Purpose |
|-----------|---------|
| `listener` | Raw `SoundListener3D` escape hatch |
| `ListenerFollowCamera(enabled)` | Bind/unbind the listener from the world camera |
| `SetListenerPose(pos, forward, up)` | Set a manual listener pose; `up` is reserved for future orientation support |
| `setAttenuation(refDist, maxDist)` | Store and apply Game3D playback attenuation defaults; sources stay full-volume through `refDist`, then fall linearly to silence at `maxDist` |
| `volume` | Default source/playback volume, clamped to 0..100 |
| `load(path)` / `loadAsset(assetPath)` | Load a `Zanna.Audio.Sound` clip from filesystem or asset resolver |
| `PlayAt(clip, pos)` | Create and play a positional `SoundSource3D` at a `Vec3` |
| `playAttached(clip, entity)` | Create an `SoundSource3D` bound to the entity node, so it follows after scene/body sync |
| `play2D(clip)` | Play a non-positional clip and return the voice id |
| `clearSources()` | Stop and release sources created through this `Sound3D` helper |

`World3D.stepSimulation` syncs audio bindings after physics and scene sync, so
attached audio observes the same final entity transforms as follow cameras.

`World3D.effects` owns the existing `PostFX3D` chain plus a particle/decal
registry. The registry can be driven manually, but world stepping and
`DrawEffects()` handle the normal update/draw path. When `World3D.FloatingOrigin`
performs a rebase, registered particle emitters, live particles, and decals are
shifted by the same delta; decal mesh caches are rebuilt on the next draw so
their vertices stay near the rebased camera:

```zia
var effects = Game3D.World3D.get_Effects(world);
Game3D.Effects3D.Explosion(world, hitPoint);
Game3D.Effects3D.ImpactDecal(world, hitPoint, hitNormal);
```

| Effects API | Purpose |
|-------------|---------|
| `postfx` | Raw `PostFX3D` escape hatch |
| `count` / `particlesCount` / `decalCount` | Registry diagnostics |
| `addParticles(particles, lifetime)` | Retain a `Particles3D` emitter and auto-remove it after `lifetime` seconds |
| `addDecal(decal)` | Retain a `Decal3D` until its own lifetime expires |
| `Update(dt)` / `draw(canvas, camera)` / `clear()` | Manual registry control |
| `Effects3D.Explosion(world, pos)` | Additive burst with warm fire colors |
| `Effects3D.Sparks(world, pos, dir)` | Directional additive sparks |
| `Effects3D.Dust(world, pos)` | Short ground-impact dust puff |
| `Effects3D.Smoke(world, pos)` | Slower rising smoke puff |
| `Effects3D.ImpactDecal(world, pos, normal)` | Fading projected impact decal |

Collision events expose `Point()` and `normal()` as `Vec3`, which makes
impact audio/VFX a direct event-buffer workflow:

```zia
var evt = Game3D.World3D.CollisionEvent(world, Game3D.CollisionPhase.get_Enter(), 0);
Game3D.Sound3D.PlayAt(audio, bounceClip, Game3D.Collision3DEvent.Point(evt));
Game3D.Effects3D.Dust(world, Game3D.Collision3DEvent.Point(evt));
```

---

## BodyDef And Collision Events

`BodyDef` describes the body that `Entity3D.attachBody` should create:

```zia
var ground = Game3D.Prefab.Ground(40.0, Game3D.Materials.Rubber(0.25, 0.35, 0.25));
Game3D.Entity3D.AttachBody(ground, Game3D.BodyDef.StaticPlane(40.0));
Game3D.World3D.Spawn(world, ground);

var ball = Game3D.Prefab.Sphere(0.5, 24, Game3D.Materials.Plastic(0.9, 0.2, 0.2));
var body = Game3D.BodyDef.Sphere(0.5, 1.0);
Game3D.BodyDef.set_Restitution(body, 0.35);
Game3D.BodyDef.set_UseCcd(body, true);
Game3D.BodyDef.WithMask(body, Game3D.LayerMask.Of(Game3D.Layers.get_World()));
Game3D.Entity3D.AttachBody(ball, body);
Game3D.World3D.Spawn(world, ball);
```

| API | Purpose |
|-----|---------|
| `BodyDef.Box(halfX, halfY, halfZ, mass)` | Dynamic box body |
| `BodyDef.Sphere(radius, mass)` | Dynamic sphere body |
| `BodyDef.Capsule(radius, height, mass)` | Dynamic capsule body |
| `BodyDef.StaticBox(halfX, halfY, halfZ)` | Static world-layer box |
| `BodyDef.StaticPlane(size)` | Static world-layer floor, implemented as a shallow box |
| `withLayer(layer)` / `WithMask(mask)` | Apply collision layer/filter bits |
| `asTrigger()` | Mark the body trigger-only |
| `withSync(mode)` | Set the node/body sync policy |

`BodyDef` exposes `shape`, `mass`, `friction`, `restitution`, `isStatic`,
`isKinematic`, `isTrigger`, `useCCD`, `layer`, `mask`, and `syncMode`
properties. Dynamic body definitions inherit the entity's current layer unless
`withLayer` or `set_layer` is used. `StaticBox` and `StaticPlane` default to
`Game3D.Layers.World`. `StaticPlane(size)` is centered on the entity node, spans
`size` on X/Z, and uses a total thickness of `0.1` world units.
Setting `isStatic` true clears `isKinematic` and sets mass to zero. Setting it
back to false restores a positive default mass when the definition does not
already have one, so a static definition can be toggled back to a usable dynamic
body without an extra `set_mass` call.

`World3D.CollisionEventCount(phase)` and `CollisionEvent(phase, index)` expose
the runtime enter/stay/exit buffers through `Collision3DEvent`;
`World3D.ClearCollisionEvents()` empties those buffers manually:

```zia
Game3D.World3D.StepSimulation(world, 0.016);
var count = Game3D.World3D.CollisionEventCount(world, Game3D.CollisionPhase.get_Enter());
if (count > 0) {
    var evt = Game3D.World3D.CollisionEvent(world, Game3D.CollisionPhase.get_Enter(), 0);
    var other = Game3D.Collision3DEvent.Other(evt, ball);
    var point = Game3D.Collision3DEvent.Point(evt);
    var firstNormal = Game3D.Collision3DEvent.ContactNormal(evt, 0);
}
```

The wrapper exposes `phase`, `a`, `b`, `raw`, `isTrigger`, `relativeSpeed`,
`normalImpulse`, `contactCount`, `Point()`, `normal()`, `contactPoint(i)`,
`ContactNormal(i)`, `contactSeparation(i)`, and `Other(entity)`.
`Point()` and `normal()` are first-contact convenience methods. Raw physics
events can expose multiple contact points for AABB and face-contact OBB box pairs;
other shapes currently carry one representative point. The indexed wrapper methods mirror the raw
`Graphics3D.CollisionEvent3D` surface so broader manifolds can extend behavior
without another Game3D API rename.
If a wrapped raw event or entity reference is invalid, the wrapper fails closed:
`phase` defaults to `Any`, entity and raw handles return `Nothing`, scalar
metrics and contact counts return zero, contact points return the origin, and
contact normals use the documented `+Y` fallback.
`CollisionPhase.Any` iterates enter, stay, and exit records. Optional
world/entity collision callback sugar remains deferred until the VM callback
trampoline policy is implemented; polling the event buffers is the supported
interpreted-Zia path.

---

## Input3D

`Input3D` reads the same keyboard and mouse state updated by `Canvas3D.Poll()` or
the synthetic input path. Calling `Update()` snapshots key/button transitions,
mouse deltas, and wheel motion for the current Game3D frame, so later polling or
synthetic input changes do not mutate controller decisions already made for that
frame.

| Method / property | Purpose |
|-------------------|---------|
| `LookSensitivity` | Multiplier for `LookAxis()` |
| `Update()` | Synchronize input helper state |
| `IsDown(key)` / `Pressed(key)` / `Released(key)` | Keyboard queries |
| `MouseDelta()` | Current mouse movement as `Vec2` (sub-pixel in relative mode) |
| `MouseX` / `MouseY` | Absolute window-local cursor position in pixels, snapshotted with the deltas (ADR 0233) |
| `MousePosition()` | Absolute cursor position as a fresh `Vec2`; feed it to `Camera3D.ScreenToRay`/`ScreenToRayOrigin` for picking |
| `MouseButton(button)` / `MousePressed(button)` | Mouse button queries |
| `WheelY()` | Vertical wheel delta |
| `MoveAxis()` | WASD/arrow movement as a normalized `Vec3` (merges the bound pad's left stick) |
| `LookAxis()` | Mouse look as `Vec2`, scaled by `LookSensitivity` (merges the bound pad's right stick) |
| `CaptureMouse()` / `ReleaseMouse()` | Forward to the active mouse capture policy |
| `SetRelativeLook(on)` | Enable raw relative mouse-look: captures the cursor and switches `MouseDelta()`/`LookAxis()` to unbounded sub-pixel deltas (see `Zanna.Input.Mouse.SetRelativeMode`) |
| `BindPad(index)` / `PadBound` | Merge gamepad `index`'s sticks into `MoveAxis()`/`LookAxis()` (`-1` unbinds; poll buttons/triggers via `Zanna.Input.Pad`) |
| `PadLookSensitivity` | Right-stick look speed (degrees per frame at full tilt, response curve x^1.8, radial deadzone 0.18) |

`BindPad` accepts exactly `-1` or a supported controller slot; another negative
value or an index beyond the backend slot count traps and preserves the prior
binding. Rebinding and unbinding clear the cached pad snapshot immediately.
Every query repairs corrupted/non-finite sensitivities, mouse deltas, wheel
state, flags, and stick axes to finite documented bounds. An invalid `Input3D`
receiver returns neutral input and never falls through to live process-wide
keyboard/mouse state or changes cursor capture.

Use `Zanna.Input.Key` and `Game3D.MouseButtons` instead of hard-coded integer
input codes in game code. `Zanna.Input.Key` is the canonical key-code namespace
for all runtime input APIs:

| Group | Members |
| --- | --- |
| Letters | `A`-`Z` |
| Digits | `Digit0`-`Digit9` |
| Function | `F1`-`F12` |
| Arrows | `Up`, `Down`, `Left`, `Right` |
| Navigation / editing | `Space`, `Escape`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown` |
| Modifiers | `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt` |
| Punctuation | `Quote`, `Comma`, `Minus`, `Period`, `Slash`, `Semicolon`, `Equals`, `LeftBracket`, `RightBracket`, `Backslash`, `Grave` |
| Numpad | `Numpad0`-`Numpad9`, `NumpadAdd`, `NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadDecimal`, `NumpadEnter` |

The same key codes remain available through `Zanna.Input.Keyboard.Key*` and
`Zanna.Input.Key` for compatibility. New examples should import
`Zanna.Input.Key` and reserve `Keyboard` for key state queries.

---

## Camera And Character Controllers

Install a built-in controller with `World3D.SetCameraController(controller)`.
The current runtime accepts the built-in Game3D controller objects listed here;
it does not yet invoke interpreted Zia interface objects as native C callbacks.
A controller can be installed on only one world at a time. Installing the same
controller on a second world detaches it from the previous world's controller
slot before binding it to the new world. Built-in controllers validate their
stored world refs during rebinding, so stale or wrong-class private slots do not
detach unrelated worlds.

```zia
var world = Game3D.World3D.New("Controller Demo", 960, 540);
var freeFly = Game3D.FreeFlyController.New(world);
Game3D.FreeFlyController.set_Speed(freeFly, 8.0);
Game3D.World3D.SetCameraController(world, freeFly);
Game3D.World3D.RunFramesOnly(world, 1, 0.016);
```

`FreeFlyController.New(world)` is the quickest camera for debug views and
editor-like movement. It reads `Input3D.MoveAxis()` and the frame's snapshotted
mouse deltas, moves through the camera basis, and can capture or release mouse input with
`captureMouse()` / `releaseMouse()`.

`FirstPersonController.New(world)` uses the same look controls. Without a
character controller it moves the camera directly. With `character` set, it
updates camera orientation first, drives `CharacterController3D.Update(...)`
before physics, and late-updates the camera eye to the character position.

```zia
var player = Game3D.Entity3D.New();
Game3D.Entity3D.SetPosition(player, 0.0, 1.0, 4.0);
Game3D.World3D.Spawn(world, player);

var character = Game3D.CharacterController3D.New(world, player, 0.32, 1.8, 70.0);
var fps = Game3D.FirstPersonController.New(world);
Game3D.FirstPersonController.set_Character(fps, character);
Game3D.FirstPersonController.set_Speed(fps, 5.0);
Game3D.World3D.SetCameraController(world, fps);
```

`CharacterController3D` exposes `speed`, `jumpSpeed`, `gravity`, `teleport(x,y,z)`,
and `grounded()`. `gravity` is a positive downward acceleration magnitude;
negative values are accepted as the same magnitude for compatibility. The
controller owns a lower-level `Zanna.Graphics3D.Character3D`, binds it to the
world's physics world, moves it with swept-slide collision, and mirrors the
character position back to its Game3D entity. Walking movement uses W/A/S/D,
arrow keys, and the bound pad's left stick for planar X/Z movement; Space is
jump, and Shift/Ctrl are left available for application actions such as sprint.

Controller updates treat `dt <= 0`, NaN, and infinity as a paused update: no
look, movement, damping, lock timer, rail progress, or transition state is
advanced. Positive deltas still clamp to the runtime frame-step ceiling. The
built-in controllers also repair retained scalar state and reject incomplete or
cross-world entity/controller bindings transactionally, preserving the prior
valid binding after a failed setter.

`OrbitController.New(world, target)` takes either a `Vec3` target position or an
`Entity3D` target. Entity targets are resolved to world position during
`LateUpdate`. Holding the left mouse button and dragging changes yaw/pitch,
wheel input changes distance, and `LateUpdate` places the camera on the orbit.

`FollowController.New(world, entity, offset)` tracks an entity after physics.
Set `damping` to `0.0` for a snap follow, or a positive value for exponential
smoothing.

`ThirdPersonController.New(world, targetEntity)` is the over-the-shoulder camera
for third-person games: a collision-aware spring arm that orbits the target and
optionally drives a `CharacterController3D` camera-relatively. During `Update`
it consumes `Input3D.LookAxis()` into `Yaw`/`Pitch` (pitch clamps to
-60..75 by default; positive pitch places the camera above the pivot), advances
the aim blend, and — when `Character` is set — moves the character along the
yaw basis (yaw `0` faces `-Z`, yaw `90` faces `-X`). During `LateUpdate` it
sphere-sweeps the camera boom from the pivot (entity position + `PivotHeight` +
yaw-rotated `ShoulderOffset`): hits pull the camera in instantly (it never
clips), release smooths back at `Damping`. `MinDistance`/`MaxDistance` bound the
boom, and a sweep that starts inside geometry snaps to `MinDistance`.

```zia
var tp = Game3D.ThirdPersonController.New(world, player);
tp.Character = character;      // camera-relative WASD drive
tp.CollisionMask = 1;          // boom collides with static scenery only
tp.OcclusionFade = true;       // fade props that block the view
world.SetCameraController(tp);
```

Aim mode: setting `Aiming = true` blends the boom toward `AimDistance` and the
camera FOV toward `AimFov` (the pre-aim FOV is captured and restored when the
blend releases). `OcclusionFade` raycasts pivot→camera each late update across
**all** layers — fadeable props are typically excluded from `CollisionMask` so
the boom ignores them while they still fade to a translucent material instance;
originals are restored when clear, on disable, on detach, and at finalization.

`TargetLock3D.New(world, ownerEntity)` adds lock-on targeting. `Acquire()`
scores overlap candidates inside `MaxDistance` and the `ConeDegrees` half-angle
around the camera forward (angle-weighted 2:1 over distance, sticky toward the
current target), gated by origin-to-origin line of sight when
`RequireLineOfSight` is set. `Cycle(±1)` steps to the nearest candidate left or
right in the camera basis. `Update(dt)` (ticked automatically while installed as
a `ThirdPersonController.LockTarget`) auto-releases on death, `BreakDistance`,
or line of sight broken longer than `LosGraceSeconds` (set `0` for instant
break). Poll `JustAcquired()`/`JustLost()` for one-shot transitions, and use
`LockedMoveBias(move)` to bend a planar move vector up to 12° toward the target
for melee approach assist. Only entities managed through `Entity3D.attachBody`
resolve as candidates, and layer policy is entity-owned — use `Entity3D.Layer`
to place targetables on a dedicated bit and match it with `CandidateMask`.

A third-person controller accepts only a `TargetLock3D` from its own world.
Target-lock and third-person retained world/entity/controller slots are
full-payload validated before use; corrupt numeric ranges and flags are repaired
to their documented bounds. Occluder-fade storage carries a private ownership
identity, so corrupted bookkeeping is quarantined without traversing or freeing
foreign memory, while valid entries still restore their original materials on
detach and finalization.

While a lock target is engaged, the third-person controller ignores look input,
eases yaw/pitch onto the player→target bearing, and pulls the look point 40%
toward the target; releasing resumes free look from the framed angles.

`CharacterController3D` also exposes crouch and environment interaction:
`SetCrouching(true)` swaps the capsule to `CrouchHeight` (feet stay planted;
always succeeds) and `SetCrouching(false)` stands back up only when the
headroom test passes (returns `false` under a low ceiling). `PushStrength`
scales the impulse applied to blocking dynamic bodies (0 = block without
pushing), `RidePlatforms` keeps the character tracking moving kinematic ground,
`IsSliding()` reports too-steep-slope descent, `GroundEntity()` resolves the
body underfoot to its owning entity, and `ProbeLedge(maxHeight)` /
`ProbeVault(maxHeight, maxThickness)` forward to the world traversal probes
using the character pose and entity facing (see the Physics3D guide's
Traversal Probes section).

---

## Cinematics

`RailCamera3D.New(world, path)` is the gameplay-installable spline camera:
`Progress` [0,1] rides the path at **constant speed** (an arclength-normalized
centripetal Catmull-Rom evaluation — `Path3D.GetPositionAt` keeps its
historical uniform parameterization), `Speed` auto-advances in units/sec, and
`PositionDamping` smooths progress jumps. Look modes: `SetLookEntity`
(post-physics pose), `SetLookPoint`, `SetLookPath` (second path at the same t),
or tangent-facing by default. `AddFovKey(t, fov)` / `AddRollKey(t, degrees)`
add piecewise keys (`KeyEase` selects smoothstep); roll composes an explicit
up vector into the look-at basis.

Each FOV and roll track stores at most 16 keys. Times are clamped to `[0,1]`,
FOV to `[1,179]`, and roll to `[-720,720]`; keys are kept strictly sorted.
Adding a key at an existing time replaces its value instead of consuming
another slot. Corrupt counts, ordering, duplicate times, values, and unused
slots are repaired before evaluation. Entity look targets must belong to the
rail camera's world, and a zero/non-finite update delta preserves progress.

`Timeline3D.New(world)` is the cutscene sequencer. Build tracks fluently —
`AddCameraCut`, `AddCameraMove` (spline over a `Path3D` with a Vec3/Entity3D/
Path3D look target and an ease), `AddFovRamp`, `AddAnim(entityName, state,
crossfade)`, `AddAudio`, `AddSubtitle`, `AddLetterbox`, `AddFade`,
`AddMarker(t, id)` — then `World3D.PlayTimeline(tl)`. While any camera track
exists, the installed camera controller is suspended (not detached) and the
timeline writes the camera in the late-update slot; `StopTimeline()` (or the
end of playback plus `StopTimeline`) restores it. Fire-once tracks fire exactly
once per play regardless of step size; `Skip()` applies final anim states,
never replays audio, fires pending markers, and applies the end camera
(`Skippable` gates it). Poll markers via `EventsFiredCount()`/`EventFiredId(i)`
and completion via `Finished`/`JustFinished()`. Letterbox/fade/subtitle draw
after user overlays, and timelines advance by the world's **scaled** time, so
pause and hit-stop behave sensibly inside cutscenes.
`World3D.SetDofFocus(distance)` drives a live DOF focus pull through the
post-FX chain (false when the chain has no DOF effect).

## Dialogue And Facial

`Dialogue3D.New(world)` queues typewriter conversations over the world
overlay: `Say(speaker, textOrKey)` / `SayVoiced(speaker, textOrKey, clip)`,
`Show()`, then `Advance()` on the interact key — the first press completes the
reveal, the next moves on (two-stage skip). Bind a `MessageBundle` with
`SetLocale`: strings that resolve as keys are localized, otherwise the literal
is used (missing keys never trap). `SetSpeakerEntity` + `SetAnchored(true)`
float a compact bubble above the speaker via `Camera3D.WorldToScreen`, falling
back to the bottom panel when off-screen. `AskChoice(options)` blocks advance
until `MoveChoice(±1)` + `ConfirmChoice()`; poll `ChoiceMade()` (one-shot) and
`LastChoice`. Dialogue input is game-mapped — gate gameplay interactions on
`Dialogue3D.Active`.

`LipSync3D.New(entity)` makes speakers look alive: bind a `MorphTarget3D`
(`BindMorph`) plus up to four mouth shapes (`BindMouthShape(name, scale)`),
then `Drive(voiceId)` — the mixer's per-voice RMS meter
(`Zanna.Audio.Voice.EnableMetering/GetLevel`, pre-attenuation so distance
never closes the mouth) feeds an envelope follower (0.04 s attack / 0.12 s
release, soft-knee curve). `DriveLevel(level)` injects levels directly.
`SetBlink(true, shape, minInterval, maxInterval)` adds seeded-deterministic
blinks (additive-max with same-shape lip sync); `BindHeadBone(name)` +
`SetGaze(entityOrVec3)` ease conversational eye contact through LookAt IK.
Amplitude sync reads best at conversation camera distance — that is the
design target, not close-ups.

---

## Combat Volumes And Health

`Hitbox3D` gives melee combat first-class hit detection without physics bodies:
combat volumes are collider shapes posed on entities (or bones) and tested by a
dedicated world combat pass each step — raycasts and sweeps never see them.
`Hitbox3D.New(entity, collider)` registers an entity-space volume (default kind
`HitboxKind.Hurt`); `Hitbox3D.NewOnBone(entity, boneName, collider)` poses the
volume from the animator's bone palette after animation updates. `Kind` selects
attack (`Hit`) or damageable (`Hurt`); `Team` pairs are skipped when equal
unless the attacker sets `FriendlyFire`; `Channel` bitmasks must overlap.

Activation is manual (`Active = true` for scripted swings) or bound to
animation windows: `BindWindow(stateName, t0, t1)` (up to four) makes the
volume live while the owner's animator plays that state within the time range.
Each activation reports **one hit per victim** — rehit suppression resets when
the volume goes inactive, so multi-hit moves are modeled as multiple windows.

The combat pass runs after animation and scene sync inside `StepSimulation` and
buffers polled events, cleared at the next step:

```zia
var n = world.HitEventCount();
for (var i = 0; i < n; i = i + 1) {
    var hit = world.HitEvent(i);
    var victimHealth = Game3D.Entity3D.get_Health(Game3D.HitEvent3D.get_Victim(hit));
    victimHealth.Damage(25.0, Game3D.HitEvent3D.get_Attacker(hit), DMG_SLASH);
}
```

`Health3D.New(maxHp)` + `Entity3D.AttachHealth(h)` add the bookkeeping half:
`Damage(amount, source, tag)` returns the applied amount (0 while dead or
inside `InvulnSeconds` i-frames, which each applied hit grants), latches
`IsDead` when hp crosses zero, and buffers a `DamageEvent3D`
(victim/source/amount/tag/`WasLethal`) on the world. `Heal`/`Revive` restore;
`JustDamaged()`/`JustDied()` are one-shot flags cleared at the next step.
`ApplyKnockback(direction, strength, point)` impulses the entity's dynamic body
and returns `false` for kinematic/character entities so gameplay can route the
push through its controller instead. Damage amounts are game data — the runtime
never auto-damages from hit events. Death does not despawn: pair `JustDied()`
with `Entity3D.EnableRagdoll()` for corpse handoff.

---

## Time Control

`World3D.TimeScale` (clamped 0–4, default 1) scales simulated time at one choke
point: controllers, behaviors, animation, physics, scene sync, and effects all
see the scaled `Dt`, while `UnscaledDt`/`UnscaledElapsed` keep real time for
menus and UI. `Paused = true` latches the effective scale to zero: bodies,
animators, particles, and `Elapsed` freeze while rendering continues (the
camera late-update still runs so resize/aspect stays live). `HitStop(seconds)`
freezes simulated time for a real-time window — max-latched, so repeated hits
never extend past the longest request — the standard combat-impact feel.

In the fixed-step loop (`runFixed`) the accumulator gains scaled time, so at
`TimeScale 0.5` fixed steps fire half as often while the fixed step *size*
never changes — simulation determinism is unaffected. Raw `Physics3DWorld`
users are untouched: scaling lives entirely in the Game3D facade. Audio voices
keep playing under pause (menu music continues; positional one-shots finish).

---

## Gameplay Systems (Surfaces, Interaction, AI, Persistence, HUD)

The adventure-game systems layer (ADRs 0091–0100). Each component is polled
state — no VM callbacks — and every world-registered piece ticks inside
`StepSimulation` in a fixed, deterministic order.

| Class | Purpose |
|---|---|
| `Game3D.Surfaces` | Process-global surface-tag registry: `Register(name)` → stable id (1-based, 255 budget), `NameOf`/`IdOf`/`Count`. Colliders carry `SurfaceType` plus per-side `MaterialFriction`/`MaterialRestitution`; raycast hits and contact events report the tag. |
| `SurfaceTable3D` / `Footsteps3D` | Per-surface footstep clip sets (≤8 variants, deterministic LCG pick) consumed by animator events (prefix match, 1.5 m ground probe resolves the surface, 0.12 s cooldown). `StepCount`/`LastSurface` telemetry. |
| `Interactable3D` / `Interactor3D` | Focus-and-use loop: fluent prompt/kind/radius on targets; the scanner scores candidates in the owner's view cone (distance + alignment + priority, 10% focus hysteresis, optional LoS). `Focused`, `FocusChanged()` one-shot, `Interact()`, `LastInteracted`. |
| `Perception3D` | Sight cones with LoS raycasts and reaction hysteresis (0.3 s to see, 2.0 s to lose), plus hearing via `World3D.ReportSound(pos, loudness, tag)` (heard events live one step). `SeenCount/SeenTarget/LastKnownPosition/SeenChanged`. |
| `BehaviorTree3D` / `BehaviorTreeInstance3D` | Shared immutable node arena (`Sequence`/`Selector`/`Inverter`, `TargetVisible`, `Wait`, `MoveToTarget`, `MoveToLastKnown`, `Custom(id)`); per-entity instances hold all mutable state. `Custom` leaves park the tree and expose `PendingCustom` for the script to `Resolve(success)`. |
| `ReverbZone3D` / `AmbientBed3D` | Listener-selected AABB reverb zones easing a dedicated `"g3d_reverb"` group insert (`Sound3D.get_ReverbWet` telemetry); zone ambient beds crossfade looping clips on `"g3d_ambience"`. `Sound3D.SetOcclusion` runs budgeted listener→source raycasts into the mixer's smoothed per-voice occlusion tap; `PlayDialogue` routes speech to the `"g3d_dialogue"` ducking trigger group. |
| `Cloth3D` | Verlet chains/patches for capes and banners — see the Physics3D page. Registered via `World3D.AddCloth`, ticked after the facial pass. |
| Persistence | `Entity3D.SetPersistent(key)` + `StateTag` opt into the world's delta store (alive/pose/tag, refreshed per step, killed on despawn); `WorldStream3D.SetCellFlag/GetCellFlag` + `LoadedCellEvent*` cover authored-cell state (cell and flag keys are non-empty and at most 255 bytes); `World3D.SaveState/LoadState(app, slot)` snapshot everything as an exact, finite, at-most-64-MiB `VW3DSAV1` under the per-user SaveData dir. Loading validates and stages the complete replacement before changing live state. `GetPersistentAlive/Position(key)` steer respawn logic. |
| `Minimap3D` | Authored north-up map with world-rect affine (`MapX/MapY` exposed), ≤64 entity/point markers with rim clamping, compass strip, and `WorldToScreen` objective indicators. Explicit `Draw()` from the HUD pass. |
| Profiling | `Canvas3D.PassDrawCount/PassInstanceCount(Game3D.RenderPass.*)` per-pass attribution plus the `World3D` hitch ring (`SetHitchThresholdMs`, `HitchCount/HitchFrame/HitchSource/HitchMs`, `Game3D.HitchSource` constants: StreamCommit, FrameTotal). |

Quest/objective tracking is 2D/3D-agnostic and lives at `Zanna.Game.Quests`
(see the Game library docs).

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `LoadEntityAsset` or `Sound3D.loadAsset` returns `null` | Run from the project root or package the asset path in `zanna.project`; source-tree samples use `asset assets assets` or repository-relative fixture paths. |
| Final overlay pixels look post-processed | Draw HUD after `World3D.EndScene()` with `Canvas3D.BeginOverlay()` / `EndOverlay()`, then call `CaptureFinalFrame()` or `Present()`. |
| A callback loop traps before the callback runs | Check that the update callback is `(Float) -> Unit`, the overlay callback is `() -> Unit`, and the function reference comes from the active script module. |
| Software backend disables a requested quality feature | Inspect `Canvas3D.get_QualityActive()` and `get_QualityFallback()`; `Quality.Apply` avoids unsupported shadow/post-FX paths. |
| A body does not collide with another body | Verify the entity layer, `BodyDef.withLayer`, and `BodyDef.withMask`; masks must include the other body layer. |
| A handle fails after teardown | Destroy the `World3D` last. Spawned entities, effects, and audio source bindings are owned by the world. Destroyed-handle diagnostics use `Game3D.<Type>.<method>: <reason>` so the failing API call is visible. |

---

## Tests

The Game3D runtime is covered by:

| Test | Coverage |
|------|----------|
| `test_rt_game3d` | C runtime contracts for constants, masks, repaired input snapshots, exact opaque-handle layouts, returning traps, world defaults/component quarantine, hierarchy ownership and transactional reparenting, spawn/despawn, stale-entity no-op diagnostics, shared-body rejection, collision-event clearing, native callback loops, fixed-loop accumulator/spiral-guard behavior, overlay hooks, final capture, packaged glTF hierarchy loading through `Assets3D.LoadEntityAsset`, pause-safe controller input, orbit/follow late update, first-person character and bound-pad movement, transactional material/prefab/lighting setup, quality, environment, post-FX, debug helpers, streamed terrain metadata inspection and LOD seam stitching beyond the single-heightmap cap, Animator3D root motion/events, Sound3D helpers, and Effects3D presets/expiry |
| `g3d_test_game3d_world_probe` | Zia construction, default subsystems, layer masks, entity spawn/find/despawn, direct `Entity3D.FromNode` subtree wrapping, synthetic `tick`, clamped `StepSimulation`, resize/aspect, manual frame path, final capture, and destroy |
| `g3d_test_game3d_runframes_probe` | Zia deterministic `RunFramesOnly`, dt/elapsed/frame accounting, and final capture |
| `g3d_test_game3d_runframes_callback_probe` | Interpreted Zia `RunFrames` update callback bridge, fixed dt delivery, and frame accounting |
| `g3d_test_game3d_runfixed_callback_probe` | Interpreted Zia `runFixed` and `runFixedWithOverlay` callback bridges, re-entrant global mutation, fixed dt delivery, overlay invocation, and callback-driven teardown |
| `native_run_game3d_runfixed_callback_probe` | Native-compiled Zia parity for the same fixed-step callback script |
| `g3d_test_game3d_destroyed_handle_reject` | Interpreted Zia destroyed-`World3D` diagnostic for post-teardown handle use |
| `g3d_test_game3d_destroyed_entity_reject` | Interpreted Zia stale-`Entity3D` neutral getter/no-op diagnostics for post-teardown handle use |
| `g3d_test_game3d_double_despawn_reject` | Interpreted Zia double-despawn stale-handle no-op diagnostic |
| `g3d_test_game3d_camera_controllers_probe` | Zia free-fly synthetic input, orbit drag/zoom, and follow camera post-physics tracking |
| `g3d_test_game3d_character_controller_probe` | Zia first-person character movement and late-update camera alignment |
| `g3d_test_game3d_thirdperson_probe` | Zia third-person spring-arm drive, crouch, lock-on acquire, and traversal probes |
| `test_rt_game3d_thirdperson` | ThirdPersonController boom/aim/fade, private-state repair and fade-storage ownership, cross-world lock rejection, pause-safe TargetLock3D scoring/cycling/transitions, Character3D crouch/push/platform/slide, and traversal probes |
| `g3d_test_game3d_combat_probe` | Zia hitbox swing → hit event → damage → hit-stop/pause/time-scale |
| `test_rt_game3d_combat` | Hitbox overlap/rehit/filters/windows, Health3D lifecycle/i-frames/knockback, stale safety |
| `test_rt_game3d_ragdoll_time` | Ragdoll3D builder/settle/blend + World3D time-scale/pause/hit-stop |
| `test_rt_game3d_cinematics` | Spline evaluator constant-speed/continuity, RailCamera3D key/state repair and pause behavior, DOF focus, Timeline3D firing/ownership/skip |
| `test_rt_game3d_dialogue_facial` | WorldToScreen projection, Dialogue3D reveal/choices/localization, LipSync3D envelope + blink determinism |
| `test_rt_game3d_streaming_async` | Worker-backed streaming: async/blocking parity, staging-error recovery, cancellation drops, commit-budget pacing, prefetch + teleport reset |
| `test_rt_game3d_hlod` | HLOD: proxy bake (merge/simplify/atlas + round-trip), no-gap proxy ring swaps (blocking + async), multi-frame impostor install and generation |
| `g3d_test_game3d_presets_probe` | Zia material/prefab presets, lighting, quality fallback, post-FX, environment chaining, physics body setup, and final-overlay debug capture |
| `g3d_test_game3d_assets_probe` | Zia `Assets3D` filesystem/asset loading, imported-group spawn/despawn churn without registry retention, cached templates, cache clear/reload, missing package-asset error handles, and model-template instantiation |
| `g3d_test_game3d_physics_probe` | Zia `BodyDef` static/dynamic body attachment, CCD/filter flags, gravity, and collision production |
| `g3d_test_game3d_collision_probe` | Zia entity-aware collision wrappers for enter/stay/exit and trigger events |
| `g3d_test_game3d_anim_probe` | Zia Animator3D play/crossfade/state-time/events, entity attachment, raw controller wrapping, and root-motion world stepping |
| `g3d_test_game3d_sound_probe` | Zia listener follow/manual pose, attenuation defaults, positional playback, attached-source sync, 2D playback, and source cleanup |
| `g3d_test_game3d_effects_probe` | Zia Effects3D presets, registry diagnostics, draw path, auto-expiry, manual particle/decal registration, and collision-triggered VFX |
| `g3d_test_game3d_docs_snippets` | Copy-paste docs surfaces for setup, presets, assets, physics, audio/VFX, deterministic frame helpers, and manual final-frame capture |
| `g3d_test_graphics3d_docs_snippets` | Copy-paste Graphics3D animation docs surface for retargeting, IK pole/ground-normal controls, animation LOD, and bone-count LOD |
| `g3d_walk_min_visual_probe` | Game3D sample final-frame baseline, crisp overlay, directional lighting, and grounded synthetic first-person movement |
| `g3d_bounded_no_regression_probe` | Existing `walk_min.zia` bounded sample run with scale flags off, proving exact final-frame pixel parity and exact player/body/draw/visibility/stream state parity against the default bounded path |
| `g3d_game3d_hello` | <=20-line hello-world scene with lighting, walkable ground, first-person character, and no `Mat4` |
| `g3d_game3d_common_no_mat4` | CMake guard that common Game3D samples/probes avoid direct `Mat4.` calls |
| `g3d_game3d_starter_probe` | Starter project deterministic movement, package-aware model asset, final capture, and grounded character path |
| `g3d_game3d_starter_package_dry_run` | Starter `zanna.project` asset packaging layout |
| `g3d_openworld_slice_probe` | Open-world slice stream in/out, rendered heightmapped terrain payload access, visible KTX2/BC7 texture panel, async asset handle completion, skinned glTF play/crossfade plus LookAt IK binding, committed GLB/WAV asset fixture loading, visible terrain-sampled TwoBone foot IK proof, character/physics/nav stepping, software final-frame baseline comparison, and deterministic replay |
| `g3d_openworld_slice_perf_probe` | Open-world slice deterministic software frame-loop perf probe; emits setup, elapsed, average-frame, FPS, optional backend GPU frame time, draw, visibility, entity, body, and stream residency metrics |
| `g3d_openworld_slice_perf_harness` | Reusable CTest perf harness wrapper that runs `perf_probe.zia`, parses the `PERF:` metrics, validates required counters, and emits a stable `HARNESS:` summary |
| `g3d_openworld_slice_streaming_hitch_probe` | Phase 4 async asset probe that records blocking-vs-async load timing, verifies zero-upload-budget pending behavior, releases work under a positive budget, and checks resident bytes return to zero |
| `g3d_openworld_slice_streaming_hitch_native_compressed_probe` | GPU opt-in run of the same hitch probe that binds native compressed texture content, verifies zero texture-upload budget leaves backend bytes pending, then records budgeted native upload bytes, raw-vs-compressed RAM/VRAM reduction, and final-frame texture tolerance once released |
| `g3d_openworld_slice_long_traversal` | Open-world slice repeated all-quadrant stream churn with bounded residency, zero pending requests after settled visits, traversal hitch/memory/seam telemetry, terrain collider checks, render telemetry, and deterministic replay |
| `g3d_openworld_slice_visibility_dense_probe` | Authored dense city/forest visibility fixture that records PVS draw-call/fill-proxy reduction and compares optimized software pixels against the no-PVS baseline |
| `g3d_openworld_slice_gpu_smoke` | Capability-gated platform GPU backend smoke for the open-world slice, including a degenerate-basis normal-map robustness pass, 24-light clustered/forward+ draw, 3-cascade primary directional CSM fixture, and optional backend GPU frame-time telemetry; reports `SKIP` when the requested GPU backend is unavailable |
| `g3d_openworld_slice_package_dry_run` | Open-world slice `zanna.project` asset packaging layout |
| `g3d_3dnext2_surface_probe` | Phase-0 3D next-level surface probe covering worker-backed ordered `Zanna.Threads.Parallel` map output and `World3D.RunFramesOnly` worker-count replay parity |
| `test_rt_g3d_commit_queue` | Internal Graphics3D main-thread commit queue FIFO/budgeted drain, worker-enqueue/main-thread-commit behavior, and worker-drain rejection |
| `scripts/g3d_tsan_concurrency_lane.sh` | Focused ThreadSanitizer lane for the worker pool, ordered map/reduce, runtime concurrency stress, asset-worker decode paths, Game3D worker parity, open-world streaming hitch probe, and the Graphics3D commit queue |
| NL3-031 determinism gate | `g3d_3dnext2_surface_probe`, `test_rt_game3d`, `test_codegen_env_is_native`, native-run Zia promise tests, and `test_crosslayer_arith` prove worker-count replay, ordered merge, and VM/native parity |
| NL3-033 software-baseline gate | `test_rt_canvas3d`, `test_rt_canvas3d_gpu_paths`, `test_rt_canvas3d_production`, `test_rt_scene3d`, `test_vgfx3d_backend_utils`, and open-world slice probes keep software visual correctness as the baseline before capability-gated GPU upload/lighting/shadow/visibility paths |
| NL3-034 policy audit gate | `lint_platform_policy.sh --strict --changed-only`, `run_cross_platform_smoke.sh`, the zero-new-dependency audit, and ADR 0004 cover platform policy, dependency policy, and registry-only IL runtime surface edits |
| `g3d_game3d_showcase` | Full-stack sample smoke, software final-frame structural/HUD assertion, asset/audio/VFX/camera/physics/animation integration, and deterministic replay |
| `g3d_game3d_bowling_setup` | Bowling setup migration smoke and deterministic replay |

Run the focused set with:

```sh
ctest --test-dir build -R 'test_rt_game3d|g3d_test_game3d_' --output-on-failure
```

Run the broader 3D regression set with:

```sh
ctest --test-dir build -L graphics3d --output-on-failure
```

---

## Current Boundaries

The core world/entity/input layer, built-in camera/character controllers,
presets, prefabs, environment/debug helpers, `Assets3D`, `BodyDef`,
entity-aware collision event wrappers, `Animator3D`, `Sound3D`, and `Effects3D`
now live in the C runtime.
`examples/3d/walk_min.zia`, `examples/3d/game3d_starter/`, and
`examples/3d/game3d_showcase/` are the current code-first samples.
`examples/3d/openworld_slice/` is the streaming vertical-slice smoke project.
The bowling setup migration lives at `zannademos/games/3dbowling/game3d/`.

Use the lower-level `Zanna.Graphics3D` and `Zanna.Audio` APIs as escape hatches
when a sample needs behavior outside the Game3D convenience layer.
