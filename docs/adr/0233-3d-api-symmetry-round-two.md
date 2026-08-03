---
status: active
audience: contributors
last-verified: 2026-08-03
---

# ADR 0233: 3D API Symmetry Round Two — Input, Physics Knobs, and Full Readback

## Status

Accepted (2026-08-03)

## Context

ADR 0227 established the principle that the registered Zia surface should not
stop short of state the runtime already retains, and fixed the worst cases
(`Terrain3D`, `Water3D`, `Camera3D`, `Skeleton3D`, `TextureAtlas3D`,
`InstanceBatch3D`). The 2026-08-03 full-stack review
(`docs/internals/graphics3d-deep-review-2026-08.md`) measured what remains:
204 `Set<X>` registrations still have no read peer, concentrated in exactly
the classes 0227 scoped out — `Canvas3D` (19 write-only render settings),
`Material3D` (every texture-map slot), `Particles3D` (every emitter
parameter), `Sky3D` (2 of 4 members), and all five joint classes (which
cannot even report the two bodies they connect). Three additional gaps sit
outside the setter/getter shape:

- **The mouse-picking pipeline is severed at the input seam.**
  `Camera3D.ScreenToRay` and the precise raycasts exist, but nothing in the
  3D surface exposes an absolute cursor position — `Input3D` carries only
  deltas. Click-to-select, RTS, and inventory interactions require leaving
  the documented 3D surface for `Zanna.Input.Manager`.
- **Physics lacks its week-one knobs.** `PhysicsBody3D.Mass` is read-only
  with no mutator (blocks pickup/drop, fuel burn, damage-driven mass);
  `PhysicsWorld3D` counts bodies and joints it cannot enumerate; gravity is
  constructor/setter input with no readback.
- **The beginner load path has the weakest diagnostics.** `Prefab.Load` and
  the `Assets3D` loaders are nullable-only, while the expert path
  (`SceneAsset`) carries six `*Result` loaders — exactly backwards, and the
  same defect 0227 fixed for `SceneGraph` without propagating to `Game3D`.

Runtime C ABI surface additions are ADR-gated; this document batches the
round-two pass so it is reviewed once and lands as one def-batch.

## Decision

All additions are strictly additive: no existing registration changes
signature or semantics. Read peers follow the enforced surface conventions:
a read-only property may coexist with its `Set<X>` method (the
`SceneNode.Position` shape); getters returning math objects return fresh
snapshots and join the `RuntimeOwnership.hpp` allowlists; getters returning
resource handles return the borrowed retained handle (or null when unbound),
matching the `Water3D.Texture` shape from 0227.

### Input3D absolute cursor position

`Zanna.Game3D.Input3D` gains, snapshotted in the same per-frame latch as the
existing mouse state (live and synthetic input sources alike):

- `MouseX -> i64`, `MouseY -> i64` — read-only properties, window-local
  pixel coordinates from `rt_mouse_x()`/`rt_mouse_y()`.
- `MousePosition() -> Vec2` — fresh snapshot, the method shape of
  `MouseDelta()`.

This closes the picking seam: `input.MousePosition()` feeds
`Camera3D.ScreenToRay` directly.

### Physics week-one knobs

- `PhysicsBody3D.SetMass(mass: f64)` — sanitizes to non-negative finite,
  stores, then refreshes derived state through the existing
  `body3d_refresh_motion_mode` path (recomputing `inv_mass` and the
  shape-derived inverse inertia exactly as construction does) and wakes the
  body. Mass near zero on a dynamic body yields infinite effective mass,
  identical to construction with mass 0. `Mass` stays a read-only property;
  `SetMass` is the canonical mutation path per the surface convention.
- `PhysicsWorld3D.Gravity -> Vec3` — read-only fresh snapshot of the
  retained gravity vector (`SetGravity` remains the mutator).
- `PhysicsWorld3D.GetBody(index: i64) -> PhysicsBody3D` and
  `GetJoint(index: i64) -> obj` — borrowed handles from the world's retained
  arrays; null for out-of-range indices. `GetJoint` returns an untyped
  object because the five joint classes are distinct registered classes;
  the handle carries its class id and downstream per-class calls validate
  it exactly as every other joint-typed argument does.

### Canvas3D render-settings readback

Read-only properties over retained state, one per write-only setter:
`ShadowBias`, `ShadowSlopeBias`, `ShadowStrength` (`f64`); `ShadowQuality`,
`ShadowCascades`, `ShadowBudget`, `ClusterLightBudget`, `MaxDeltaTime`
(`i64`); `BackfaceCull`, `FrustumCulling`, `OcclusionCulling`,
`TextureStreaming`, `ForceCpuSkinning` (`i1`); `TextureStreamingBias`
(`f64`); `FogEnabled` (`i1`), `FogNear`, `FogFar` (`f64`), `FogColor`
(`Vec3`, fresh); `AmbientColor` (`Vec3`, fresh); `Skybox`, `RenderTarget`,
`PostFX` (borrowed handles, null when unbound); `ClipRectX`, `ClipRectY`,
`ClipRectWidth`, `ClipRectHeight` (`i64`, zeros when no clip rect is
active) plus `ClipRectActive` (`i1`). An options menu can now render its
own current state.

### Material3D readback

- Borrowed-handle read-only properties for every retained map slot:
  `Texture`, `NormalMap`, `SpecularMap`, `EmissiveMap`,
  `MetallicRoughnessMap`, `AmbientOcclusionMap`, `Lightmap`, `EnvMap`.
  The eleven setters store into eight slots — `SetAlbedoMap` and
  `SetAlbedoRenderTarget` alias the diffuse slot read by `Texture`, and
  `SetEmissiveRenderTarget` aliases `EmissiveMap` — so eight getters give
  complete coverage and each returns the exact bound object (`Pixels`,
  `TextureAsset3D`, or `RenderTarget3D`). A material inspector can now show
  which map is bound.
- `EmissiveColor -> Vec3` (fresh), `Shininess -> f64`, `DepthBias -> f64`
  (constant bias), `DepthSlopeBias -> f64` (slope-scaled bias).
- `GetCustomParam(index: i64) -> f64` mirroring
  `SetCustomParam(index, value)`.

### Particles3D emitter readback

Read-only properties named for the setter parameters they mirror: `Rate`
(`f64`); `LifetimeMin`/`LifetimeMax`, `SpeedMin`/`SpeedMax`,
`SizeStart`/`SizeEnd`, `AlphaStart`/`AlphaEnd` (`f64`);
`ColorStart`/`ColorEnd` (`i64`); `Gravity`, `Position`, `EmitterSize`,
`Direction` (`Vec3`, fresh); `Spread` (`f64`, the fourth `SetDirection`
parameter); `EmitterShape` (`i64`); `Stretch`, `Softness` (`f64`);
`TrailLifetime` (`f64`), `TrailSegments` (`i64`); `Texture` (borrowed).

### Sky3D readback

`SunDirection -> Vec3` and `GroundAlbedo -> Vec3` (fresh snapshots),
completing the class alongside the existing read-write `Turbidity` and
`Resolution`.

### Joint readback

- All five joint classes gain `BodyA -> PhysicsBody3D` and
  `BodyB -> PhysicsBody3D` read-only borrowed properties over the existing
  internal `rt_joint3d_get_bodies` discriminated accessor.
- `HingeJoint3D`: `MotorEnabled -> i1`, `MotorTargetVelocity -> f64`,
  `MotorMaxImpulse -> f64`, `LimitsEnabled -> i1`, `LimitMin -> f64`,
  `LimitMax -> f64` — named for the `SetMotor`/`SetLimits` parameters they
  mirror (the solver bounds motors by impulse, not torque).
- `SixDofJoint3D`: `LinearLimitMin`/`LinearLimitMax`,
  `AngularLimitMin`/`AngularLimitMax` (`Vec3`, fresh),
  `LinearMotorEnabled -> i1`, `LinearMotorVelocity -> Vec3` (fresh),
  `LinearMotorMaxImpulse -> f64`.

Break/reaction-force telemetry is deliberately excluded: joints do not
retain per-step reaction impulses today, so exposing them is solver work,
not surface symmetry.

### Result-based Game3D loaders

Mirroring the `SceneAsset` result contract (ADR 0190):

- `Prefab.LoadResult(path: str) -> Zanna.Result` and
  `Prefab.LoadAssetResult(name: str) -> Zanna.Result` — ok carries the
  entity; err carries the loader diagnostic.
- `Assets3D.LoadEntityResult(path)`, `LoadEntityAssetResult(name)`,
  `LoadAnimationResult(path, index)`, `LoadAnimationAssetResult(name,
  index)`, `LoadNodeAnimationResult(path, index)`,
  `LoadNodeAnimationAssetResult(name, index)` — `Zanna.Result` peers of the
  six synchronous loaders. The async variants already report through
  `AssetHandle3D.Progress`/`Error` and are unchanged.

The bare nullable loaders remain for compatibility.

### Documentation sync (no new API)

`Vec3.Lerp` and `Quat.Slerp` already exist; the review's "no 3D tween"
finding is a visibility gap. `game3d.md` gains a "Timers, tweens, and time"
section documenting `Zanna.Game.Timer`, `Zanna.Game.Tween`, `Vec3.Lerp`,
and `Quat.Slerp` for 3D use. The narrative docs
(`docs/zannalib/graphics/rendering3d.md`, `physics3d.md`, `game3d.md`)
document every member this ADR adds plus the ADR 0227 members the 0227
def-batch never propagated to them (Camera3D matrices, SceneGraph
diagnostics, Water3D/Terrain3D/Sky3D readback, Mesh3D helpers), and their
`last-verified` front matter is bumped. A full generated-vs-narrative
coverage gate is deliberately not added: narrative docs are curated (26%
member coverage today), and a hard gate would either fail permanently or
freeze curation; the per-def-batch review checklist gains a "narrative docs
updated" line instead.

### Deliberate exclusions

- `PostFX3D` per-effect readback/removal needs an effect-stack enumeration
  design (per-kind parameter schemas), not a wrapper — deferred to its own
  ADR.
- `NavAgent3D` path-corner readback and `Stop`/`Resume` are agent-behavior
  surface, grouped with future nav work.
- Event callbacks remain polling-only pending the VM callback-trampoline
  policy decision (documented boundary in `game3d.md`).

## Consequences

- One def-batch touches `src/il/runtime/defs/` (graphics3d + game3d),
  `RuntimeOwnership.hpp` fresh-snapshot allowlists (every new Vec2/Vec3
  return), the ABI surface tests (`test_graphics3d_abi_surface`,
  `test_graphics3d_runtime_manifest`, `test_runtime_registry`,
  `test_rt_core_ownership`), and the generated reference docs via rtgen.
- New C entry points are thin readers over retained state plus
  `rt_body3d_set_mass`, the world enumerators, the Input3D snapshot fields,
  and six Result wrapper loaders — no new invariants.
- Every addition ships with a set→get round-trip (or snapshot) test that
  fails before registration; `SetMass` additionally ships a dynamics test
  (halving mass doubles impulse response) so the derived-state refresh is
  pinned, and the Result loaders ship ok/err path tests.
- `--dump-runtime-api` grows by ~85 members; consumers keying on counts
  regenerate in the same change.

## Alternatives considered

- **Read-write properties instead of read-only + `Set<X>`.** Rejected: the
  surface audit enforces one canonical mutation path per property; the
  setters are established API and several sanitize or have multi-parameter
  shapes a property assignment cannot express.
- **A `Joint3D` base class for shared readback.** Rejected: the registry
  has no inheritance for runtime classes; five small per-class additions
  are cheaper than introducing one.
- **Typed `GetJoint` via per-class enumerators
  (`GetHingeJoint(i)`, …).** Rejected: five enumerators with disjoint
  index spaces are harder to use than one untyped enumerator whose result
  self-validates downstream.
- **Retrofitting `Zanna.Game.Tween` with Vec3/Quat channels.** Rejected for
  this batch: `Vec3.Lerp`/`Quat.Slerp` already compose with `Tween.Value`
  for every authoring case raised; a typed tween object is additive later
  if demand appears.
