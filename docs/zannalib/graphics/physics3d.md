---
status: active
audience: public
last-verified: 2026-07-26
---

# 3D Physics
> PhysicsWorld3D, PhysicsHit3D, PhysicsHitList3D, LedgeHit3D, Ragdoll3D, CollisionEvent3D, ContactPoint3D, Collider3D, PhysicsBody3D, Character3D, DistanceJoint3D, SpringJoint3D

**Part of [Zanna Runtime Library](../README.md) › [Graphics](README.md)**

---

This page covers the `Zanna.Graphics3D` physics and movement surface that is available
through the runtime library in both Zia and BASIC. For the broader 3D rendering guide,
asset pipeline, and scene examples, see [Graphics 3D Guide](../../graphics3d-guide.md).

---

## Zanna.Graphics3D.PhysicsWorld3D

3D rigid-body simulation world with manual stepping, gravity control, collision contact access,
and joint integration.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.PhysicsWorld3D(gx, gy, gz)`

### Properties

| Property         | Type    | Access | Description |
|------------------|---------|--------|-------------|
| `BodyCount`      | Integer | Read   | Number of active bodies in the world |
| `CollisionCount` | Integer | Read   | Number of contacts from the most recent `Step()` |
| `CollisionEventCount` | Integer | Read | Number of current collision events from the most recent `Step()` |
| `EnterEventCount` | Integer | Read | Number of collision pairs that began touching this step |
| `StayEventCount` | Integer | Read | Number of collision pairs that remained touching this step |
| `ExitEventCount` | Integer | Read | Number of collision pairs that stopped touching this step |
| `JointCount`     | Integer | Read   | Number of active joints |
| `LastCcdRequestedSubsteps` | Integer | Read | Unclamped CCD substep demand from the most recent `Step()` |
| `LastCcdSubsteps` | Integer | Read | Actual CCD substeps used after applying the runtime cap |
| `CcdSubstepClampedCount` | Integer | Read | Number of steps whose CCD demand exceeded the cap |
| `SolverIterations` | Integer | Read/Write | Velocity/contact/joint solver passes used by `Step()`; default `6`, clamped to `1..64` |
| `PositionIterations` | Integer | Read/Write | Contact position-correction passes; default `1`, clamped to `1..64` |
| `ContactBeta` | Double | Read/Write | Baumgarte contact recovery fraction; default `0.8`, clamped to `0.0..1.0` |
| `RestitutionThreshold` | Double | Read/Write | Minimum approach speed in m/s that applies bounce; default `0.5`, clamped to non-negative finite values |
| `FixedStepAlpha` | Double | Read | Fixed-step accumulator remainder divided by `fixedDt`, for render interpolation |
| `DroppedFixedSteps` | Integer | Read | Fixed steps discarded by `StepFixed()` when `maxSteps` caps a long frame |
| `LastSolverIslandCount` | Integer | Read | Max active contact islands scheduled by the most recent `Step()` |
| `LastSolverActiveBodyCount` | Integer | Read | Max awake dynamic bodies included in contact islands by the most recent `Step()` |
| `LastSolverContactCount` | Integer | Read | Max non-trigger contacts scheduled through contact islands by the most recent `Step()` |

### Methods

| Method                    | Signature             | Description |
|---------------------------|-----------------------|-------------|
| `Step(dt)`                | `Void(Double)`        | Advance simulation by `dt` seconds |
| `StepFixed(dt, fixedDt, maxSteps)` | `Integer(Double, Double, Integer)` | Accumulate variable frame time and run up to `maxSteps` fixed `fixedDt` steps, returning steps actually run |
| `Add(body)`               | `Void(Object)`        | Add a `PhysicsBody3D` to the world |
| `TryAdd(body)`            | `Boolean(Object)`     | Add a body and report allocation/validation failure without changing the world |
| `Remove(body)`            | `Void(Object)`        | Remove a body from the world |
| `ContainsBody(body)`      | `Boolean(Object)`     | Return whether the body is currently registered in the world |
| `SetGravity(x, y, z)`     | `Void(Double, Double, Double)` | Change the gravity vector |
| `AddJoint(joint, type)`   | `Void(Object, Integer)` | Add a joint (`0 = DistanceJoint3D`, `1 = SpringJoint3D`, `2 = HingeJoint3D`, `3 = RopeJoint3D`, `4 = SixDofJoint3D`) |
| `RemoveJoint(joint)`      | `Void(Object)`        | Remove a joint from the world |
| `Raycast(origin, direction, maxDistance, mask)` | `PhysicsHit3D(Object, Object, Double, Integer)` | Return the nearest `PhysicsHit3D` or `Nothing` |
| `RaycastAll(origin, direction, maxDistance, mask)` | `PhysicsHitList3D(Object, Object, Double, Integer)` | Return a sorted `PhysicsHitList3D` or `Nothing` |
| `SweepSphere(center, radius, delta, mask)` | `PhysicsHit3D(Object, Double, Object, Integer)` | Sweep a sphere and return the first `PhysicsHit3D` or `Nothing` |
| `SweepCapsule(a, b, radius, delta, mask)` | `Object(Object, Object, Double, Object, Integer)` | Sweep a capsule segment and return the first `PhysicsHit3D` or `Nothing` |
| `OverlapSphere(center, radius, mask)` | `PhysicsHitList3D(Object, Double, Integer)` | Return a `PhysicsHitList3D` of overlaps or `Nothing` |
| `ProbeClearance(position, radius, height, mask)` | `Boolean(Object, Double, Double, Integer)` | `true` when a capsule of the given dims fits at `position` without solid overlap |
| `ProbeLedge(origin, forward, radius, maxHeight, maxDepth, mask)` | `LedgeHit3D(Object, Object, Double, Double, Double, Integer)` | Find a grabbable ledge ahead of a foot-level origin; `Nothing` when no valid ledge exists |
| `ProbeVault(origin, forward, radius, maxHeight, maxThickness, mask)` | `LedgeHit3D(Object, Object, Double, Double, Double, Integer)` | Like `ProbeLedge` but also requires a near-origin-level landing on the far side |

`PhysicsHit3D.Body` returns a typed `PhysicsBody3D`; `Point`/`Normal` return `Vec3`; `PhysicsHitList3D.Get` returns a `PhysicsHit3D` — so hit results flow into typed bindings without casts.
| `OverlapAABB(min, max, mask)` | `Object(Object, Object, Integer)` | Return a `PhysicsHitList3D` of overlaps or `Nothing` |
| `RebaseOrigin(dx, dy, dz)` | `Void(Double, Double, Double)` | Shift registered bodies and contact/query state by `-delta` |
| `GetCollisionBodyA(i)`    | `Object(Integer)`     | Get the first body in contact pair `i` |
| `GetCollisionBodyB(i)`    | `Object(Integer)`     | Get the second body in contact pair `i` |
| `GetCollisionNormal(i)`   | `Object(Integer)`     | Get the contact normal as a `Vec3` |
| `GetCollisionDepth(i)`    | `Double(Integer)`     | Get the penetration depth for contact `i` |
| `GetCollisionEvent(i)`    | `Object(Integer)`     | Get the current `CollisionEvent3D` at index `i` |
| `GetEnterEvent(i)`        | `Object(Integer)`     | Get an enter `CollisionEvent3D` |
| `GetStayEvent(i)`         | `Object(Integer)`     | Get a stay `CollisionEvent3D` |
| `GetExitEvent(i)`         | `Object(Integer)`     | Get an exit `CollisionEvent3D` |
| `ClearCollisionEvents()`  | `Void()`              | Clear the collision events buffered from the most recent `Step()` |

### Notes

- `Step()` is explicit; the world does not simulate itself automatically.
- Contact queries reflect the latest completed step.
- Query `mask` uses the same layer bits as `PhysicsBody3D.CollisionLayer`. A mask of `0` matches no layers; use `-1` or an all-layers mask when you want any layer.
- Static bodies are immovable. Kinematic bodies move from explicit velocity but do not
  receive gravity or force integration.
- `Add(body)` keeps the historical void API. `TryAdd(body)` returns `false` for invalid handles or allocation failure, returns `true` for already-present bodies, and leaves the body count stable on duplicates.
- World storage for bodies, contacts, contact events, and joints grows on demand from production-sized initial capacities. Query result lists store a bounded nearest/result prefix for predictable allocation behavior, while `PhysicsHitList3D.TotalCount` and `Truncated` expose whether more matches existed.
- Collision detection uses a body-centric sweep-and-prune broadphase before shape-specific narrow-phase tests. This is intentionally separate from the render-facing `SceneGraph` BVH: physics indexes all collider bodies, including non-render bodies, and applies solver filters such as static-static rejection, layer/mask checks, trigger state, and contact-event identity.
- The unit lane includes a sparse 321-body step stress that exercises body
  storage growth, broadphase scratch growth, and dynamic integration without
  producing contacts, plus named island-batch and mesh-BVH body-candidate
  targets for larger Phase 8 coverage.
- World queries reuse the physics broadphase/query cache and honor each body's collision scale before running shape tests.
- `RebaseOrigin(dx, dy, dz)` is the low-level floating-origin hook. It shifts all
  registered body positions, live/previous collision contact points, and
  enter/stay/exit contact buffers by `-delta`, invalidates query broadphase
  caches, and clears cached boxed collision-event objects. Treat existing
  `PhysicsHitList3D`, `PhysicsHit3D`, and `CollisionEvent3D` objects as
  pre-rebase snapshots and query again after the boundary.
- `Raycast` and `RaycastAll` test collider geometry, not only broadphase bounds: boxes, spheres, capsules, compound leaves, mesh/convex triangles, and heightfields report nearest shape hits. Mesh raycasts build and reuse a per-mesh BVH, and heightfield raycasts adapt their step to the heightfield cell spacing.
- Mesh colliders use the per-mesh BVH to prune candidate triangles for sphere,
  capsule, box, and convex-hull contacts, with a full triangle scan retained as
  a correctness fallback. Convex hulls use support-point GJK/EPA for
  hull-vs-hull, hull-vs-simple, and mesh-triangle contacts. The physics
  body-centric broadphase feeds mesh candidates first; the `PHYSICS_MESH_BVH_TARGET`
  fixture covers 16 static mesh tiles while building only the one overlapping
  tile's per-mesh BVH.
- Sphere sweeps use analytic tests against primitive spheres and boxes before falling back to adaptive sampling. Capsule sweeps use adaptive sampling, so small-radius sweeps and long capsules can hit thin geometry without a fixed world-unit step floor.
- **CCD is a hard guarantee against static geometry**: bodies with `UseCcd`
  combine adaptive substep subdivision with a swept time-of-impact pass that
  clips each substep's translation at the first static/kinematic surface —
  a fast projectile cannot tunnel a thin wall at any speed/dt combination
  (covered by the `test_physics3d_ccd_toi` speed x thickness matrix). The TOI
  reflection honors the body's restitution; dynamic-vs-dynamic pairs stay on
  the substep path. `CcdToiCount` reports applied clips.
- `SetMaxQueryHits(n)` configures how many results `RaycastAll`/`OverlapSphere`/
  `OverlapAABB` return (16–4096, default 256). Lists always report `TotalCount`
  and `Truncated`, so raising the capacity is only needed when you must
  enumerate more than the nearest 256 matches.
- `LastCcdRequestedSubsteps`, `LastCcdSubsteps`, and `CcdSubstepClampedCount` are diagnostics for fast-body tuning; clamping is expected when very high velocity would require more substeps than the engine's safety cap.
- `SolverIterations` defaults to `6` and drives velocity contact solving plus joint
  constraint passes. `PositionIterations` defaults to `1` and controls the split
  contact position-correction pass. Both clamp to `1..64`; higher values can reduce
  penetration and make constraints stiffer at additional CPU cost.
- `ContactBeta` defaults to `0.8` and is clamped to `0.0..1.0`; lower values soften
  positional recovery and `0.0` disables it. `RestitutionThreshold` defaults to
  `0.5` m/s and is clamped to finite non-negative values; raising it suppresses bounce
  for slower impacts and helps resting stacks stay quiet.
- `StepFixed(dt, fixedDt, maxSteps)` is the raw `PhysicsWorld3D` fixed-step helper.
  Use a positive `fixedDt` such as `1.0 / 60.0` and a positive `maxSteps` guard.
  The world carries the accumulator remainder between calls, returns the number of
  fixed steps actually run, reports `FixedStepAlpha` for visual interpolation, and
  increments `DroppedFixedSteps` when a long frame overflows the `maxSteps` guard.

---

## Zanna.Graphics3D.PhysicsHit3D

Query result object returned by `Raycast`, `SweepSphere`, and `SweepCapsule`.

**Type:** Instance (obj)

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Body` | Object | Read | Hit `PhysicsBody3D` |
| `Collider` | Object | Read | Hit `Collider3D` leaf collider |
| `Point` | Object (`Vec3`) | Read | Contact point approximation |
| `Normal` | Object (`Vec3`) | Read | Surface normal |
| `Distance` | Double | Read | Distance travelled before the hit |
| `Fraction` | Double | Read | `Distance / maxDistance` |
| `StartedPenetrating` | Boolean | Read | Query began already overlapping the target |
| `IsTrigger` | Boolean | Read | Hit body is trigger-only |

---

## Zanna.Graphics3D.PhysicsHitList3D

List of `PhysicsHit3D` results returned by `RaycastAll`, `OverlapSphere`, and `OverlapAABB`.

**Type:** Instance (obj)

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Count` | Integer | Read | Number of hits in the list |
| `TotalCount` | Integer | Read | Total matching hits before the bounded result prefix was applied |
| `Truncated` | Boolean | Read | True when more matches existed than are stored in the list |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Get(i)` | `Object(Integer)` | Return hit `i` as a `PhysicsHit3D` |

---

## Zanna.Graphics3D.LedgeHit3D

Traversal-probe result returned by `PhysicsWorld3D.ProbeLedge` and `ProbeVault`.
A plain snapshot handle (the `PhysicsHit3D` pattern): vectors are world-space at
probe time and are not live — re-probe after movement.

**Type:** Instance (obj), no public constructor

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `GrabPoint` | Object (`Vec3`) | Read | Ledge edge point (wall contact XZ at the ledge-top height) |
| `SurfaceNormal` | Object (`Vec3`) | Read | Ledge top surface normal (`y >= 0.6` guaranteed) |
| `WallNormal` | Object (`Vec3`) | Read | Front wall surface normal |
| `LandingPoint` | Object (`Vec3`) | Read | Far-side vault landing point; `Nothing` for ledge probes |
| `Height` | Double | Read | Ledge top height above the probe origin |
| `HasStandingRoom` | Boolean | Read | A capsule of the probe dims fits above the ledge top |
| `HasLanding` | Boolean | Read | A vault landing was found (always `false` for ledge probes) |

### Traversal probe recipe

Probes take a **foot-level** origin (the capsule is skin-lifted internally so
standing on the ground never reads as an initial penetration) and reason in the
horizontal plane of `forward`. `ProbeLedge` runs a wall sweep, then drops a
sphere onto the candidate top (rejecting overhangs whose normal has `y < 0.6`),
then reports standing room for a capsule of the probe radius and `maxHeight`.
`ProbeVault` additionally requires walkable ground on the far side of the
obstacle between roughly `0.3` above and `2.0` below the origin level — a thick
wall whose top is the only "landing" is rejected. Probes cost 2–3 sweeps per
call; use them event-driven (on jump press near a wall), not per-frame.

A minimal mantle: on jump press, `ProbeLedge`, check `HasStandingRoom`, then
either play a root-motion mantle clip toward `GrabPoint` (see the Game3D
guide's Animator3D root-motion section) or teleport the character so its feet
land on the ledge top and let the ground probe settle it.

---

## Zanna.Graphics3D.CollisionEvent3D

Structured per-pair contact snapshot from the most recent world step.

**Type:** Instance (obj)

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `BodyA` | Object | Read | First body in the pair |
| `BodyB` | Object | Read | Second body in the pair |
| `ColliderA` | Object | Read | Leaf collider for body A |
| `ColliderB` | Object | Read | Leaf collider for body B |
| `IsTrigger` | Boolean | Read | Pair includes a trigger body |
| `ContactCount` | Integer | Read | Manifold point count; AABB and face-contact OBB box pairs can expose up to four clipped points, other pairs currently expose one representative point |
| `RelativeSpeed` | Double | Read | Relative speed along the contact normal before resolution |
| `NormalImpulse` | Double | Read | Solver normal impulse (`0` for trigger pairs) |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetContact(i)` | `Object(Integer)` | Return `ContactPoint3D` for manifold point `i` |
| `GetContactPoint(i)` | `Object(Integer)` | Return point `i` as a `Vec3` |
| `GetContactNormal(i)` | `Object(Integer)` | Return point normal `i` as a `Vec3` |
| `GetContactSeparation(i)` | `Double(Integer)` | Signed separation (`< 0` means penetration) |

---

## Zanna.Graphics3D.ContactPoint3D

Contact manifold point returned by `CollisionEvent3D.GetContact`.

**Type:** Instance (obj)

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Point` | Object (`Vec3`) | Read | Contact position |
| `Normal` | Object (`Vec3`) | Read | Contact normal |
| `Separation` | Double | Read | Signed separation (`< 0` while penetrating) |

---

## Zanna.Graphics3D.Collider3D

Reusable 3D collision shape object. This is the preferred authoring path for advanced physics
content; bodies now own a collider instead of baking all shape state directly into the body.

**Type:** Instance (obj)

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `Box(hx, hy, hz)` | `Collider3D(Double, Double, Double)` | Box collider with half-extents |
| `Sphere(radius)` | `Collider3D(Double)` | Sphere collider |
| `Capsule(radius, height)` | `Collider3D(Double, Double)` | Upright capsule collider; `height` is total height including caps, and values below `2*radius` collapse to a sphere-like capsule |
| `NewConvexHull(mesh)` | `Collider3D(Object)` | Convex-hull collider sourced from a `Mesh3D` |
| `NewConvexHullReduced(mesh, maxVerts)` | `Collider3D(Object, Integer)` | Quickhull over the mesh's vertices (interior points removed), reduced to at most `maxVerts` (clamped 8–255) hull vertices, materialized as an owned hull mesh with faces |
| `NewMesh(mesh)` | `Collider3D(Object)` | Triangle-mesh collider (static or kinematic bodies) |
| `NewHeightfield(heightmap, sx, sy, sz)` | `Collider3D(Object, Double, Double, Double)` | Static heightfield collider from `Pixels` |
| `NewCompound()` | `Collider3D()` | Empty compound collider |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Type` | Integer | Read | `0=box`, `1=sphere`, `2=capsule`, `3=convexHull`, `4=mesh`, `5=compound`, `6=heightfield` |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `AddChild(child, localTransform)` | `Void(Object, Object)` | Add a child collider to a compound collider |
| `GetLocalBoundsMin()` | `Object()` | Local-space minimum bounds as a `Vec3` |
| `GetLocalBoundsMax()` | `Object()` | Local-space maximum bounds as a `Vec3` |

### Notes

- `NewMesh()` colliders attach to STATIC and KINEMATIC bodies (script-driven
  platforms, crushers, elevators); mass-driven DYNAMIC bodies remain rejected
  because dynamic-vs-mesh response is numerically unstable. `NewHeightfield()`
  stays static-only.
- `NewConvexHullReduced()` is the right constructor for downloaded art meshes:
  it removes interior vertices via quickhull (GJK support scans then touch only
  real hull corners) and re-hulls to the vertex budget when needed, trapping on
  degenerate (flat/collinear) input.
- Primitive collider constructors substitute a positive unit extent/radius for zero, negative-zero,
  or non-finite inputs; capsule height is still clamped to at least its diameter.
- `NewHeightfield()` requires a valid `Pixels` object, not just a matching class ID.
- `NewConvexHull()` treats the mesh vertex cloud as a convex support set.
  Hull-vs-hull and hull-vs-simple pairs use the GJK/EPA simplex narrow phase,
  including contained primitive contacts and separated-overlapping-AABB edge
  cases. The `PHYSICS_CONVEX_GJK_TARGET` fixture covers isolated hull contacts
  against spheres, capsules, boxes, and hulls.
- Compound colliders are the preferred way to build richer dynamic bodies from simple children.
- Heightfield contacts account for signed and non-uniform Y scale when computing sphere penetration depth and normals.

---

## Zanna.Graphics3D.PhysicsBody3D

3D rigid body with position, quaternion orientation, linear/angular velocity, collision filtering,
sleeping, and optional CCD.

**Type:** Instance (obj)

### Constructors

| Constructor | Signature | Description |
|-------------|-----------|-------------|
| `New(mass)` | `PhysicsBody3D(Double)` | Create an empty body and assign a collider later |
| `NewAABB(sx, sy, sz, mass)` | `PhysicsBody3D(Double, Double, Double, Double)` | Box body (`mass = 0` makes it static); name retained for compatibility |
| `Sphere(radius, mass)` | `PhysicsBody3D(Double, Double)` | Sphere body (note: named `Sphere`, not `NewSphere`) |
| `NewCapsule(radius, height, mass)` | `PhysicsBody3D(Double, Double, Double)` | Capsule body; `height` is total height including caps |

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Collider` | `Collider3D` | Read/Write | Active collision shape attached to the body |
| `Position` | Object (`Vec3`) | Read | World position |
| `Scale` | Object (`Vec3`) | Read | Collision scale applied to the attached collider |
| `Orientation` | Object (`Quat`) | Read | World orientation quaternion |
| `Velocity` | Object (`Vec3`) | Read | Linear velocity |
| `AngularVelocity` | Object (`Vec3`) | Read | Angular velocity in radians per second |
| `Restitution` | Double | Read/Write | Bounciness, clamped to `0.0` to `1.0` |
| `Friction` | Double | Read/Write | Surface friction, clamped to finite non-negative values |
| `LinearDamping` | Double | Read/Write | Per-step damping applied to linear motion |
| `AngularDamping` | Double | Read/Write | Per-step damping applied to spin |
| `CollisionLayer` | Integer | Read/Write | Layer bitmask for this body |
| `CollisionMask` | Integer | Read/Write | Which layers this body collides with |
| `IsStatic` | Boolean | Read/Write | Treat body as static |
| `IsKinematic` | Boolean | Read/Write | Treat body as kinematic |
| `IsTrigger` | Boolean | Read/Write | Trigger-only overlap body; no impulse response |
| `CanSleep` | Boolean | Read/Write | Allow the body to auto-sleep when idle |
| `IsSleeping` | Boolean | Read | Body is currently asleep |
| `UseCcd` | Boolean | Read/Write | Enable substep-based CCD for fast motion |
| `IsGrounded` | Boolean | Read | Body touched a ground-like contact in the last step |
| `GroundNormal` | Object (`Vec3`) | Read | Normal of the last ground contact |
| `Mass` | Double | Read | Body mass |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Teleport body position |
| `SetScale(x, y, z)` | `Void(Double, Double, Double)` | Set per-body collider scale |
| `SetOrientation(quat)` | `Void(Object)` | Set body orientation from a `Zanna.Math.Quat` |
| `SetVelocity(vx, vy, vz)` | `Void(Double, Double, Double)` | Set linear velocity |
| `SetAngularVelocity(wx, wy, wz)` | `Void(Double, Double, Double)` | Set angular velocity |
| `ApplyForce(fx, fy, fz)` | `Void(Double, Double, Double)` | Accumulate force for the next step |
| `ApplyImpulse(ix, iy, iz)` | `Void(Double, Double, Double)` | Apply an immediate linear impulse |
| `ApplyTorque(tx, ty, tz)` | `Void(Double, Double, Double)` | Accumulate torque for the next step |
| `ApplyAngularImpulse(ix, iy, iz)` | `Void(Double, Double, Double)` | Apply an immediate angular impulse |
| `Wake()` | `Void()` | Wake a sleeping dynamic body |
| `Sleep()` | `Void()` | Force a dynamic body into the sleeping state |

### Notes

- `NewAABB()`, `Sphere()`, and `NewCapsule()` are now convenience factories that create a body,
  create the matching collider, and attach it internally.
- Use `New(mass)` plus `body.Collider = collider` when you want reusable, mesh, compound, or heightfield shapes.
- `Scale` is a physics scale, not a renderer transform; Game3D keeps it synchronized from `Entity3D` scale when a body is attached to an entity.
- `Orientation` uses the runtime quaternion type `Zanna.Math.Quat`; `SetOrientation`
  traps when passed any other object type.
- `CollisionLayer` must be a positive bitmask. Use `CollisionMask = 0` when you
  need a body to collide with no layers.
- `Sleep()` and `Wake()` only affect dynamic bodies. Static and kinematic bodies do not enter
  the sleeping state. Contacts and joints both wake sleeping partners: a
  sleeping body attached to a moving one is re-woken by any non-trivial joint
  impulse or position correction, so jointed assemblies never freeze mid-air.
- `Kinematic = true` makes the body move from explicit `Velocity` / `AngularVelocity` only.
- `UseCcd` uses additional substeps to reduce tunneling for fast-moving bodies. It is
  **off by default**; enable it explicitly on small or fast bodies (projectiles, balls).
  Without it, a fast body can tunnel through thin geometry within a single step.
  When a body moves faster than the world substep cap can cover, it gets
  per-body swept catch-up segments on top of the world substeps, so a single
  bullet no longer multiplies the whole world's simulation cost and still
  cannot tunnel; the clamp diagnostics record requested-vs-applied demand.
- Position, velocity, angular velocity, force, torque, and integrated state are
  sanitized to finite values and saturated to the runtime state bounds, so
  extreme impulses or forces cannot create `NaN`/`Inf` body state. CCD keeps
  diagnostics for requested-vs-applied substeps when that demand is clamped.
- Body position and velocity integration use double precision. The regression
  lane covers a body at `1e9` world units retaining a `0.125` unit step delta.
- Rotational state is fully integrated for all body types. Non-trigger contacts apply impulses at
  the contact point, so off-center hits can generate angular velocity.
- Contacts are solved by a warm-started sequential-impulse solver: per-manifold-point
  normal and friction impulses are accumulated and carried across frames, so box stacks
  settle and rest stably rather than jittering or sinking. Awake contacts are batched into
  independent contact islands before the iterative solve; settled bodies auto-sleep and
  freeze out of those islands. `SolverIterations` raises support quality for tall stacks.
  Resting contacts apply no restitution (only genuine impacts bounce).
- Box collision honors body orientation. Axis-aligned and rotated face-contact box pairs publish
  clipped multi-point manifolds; edge-style box contacts and non-box pairs may still publish one point.

### Zia Example

```zia
module Physics3DBodyDemo;

bind Zanna.Graphics3D;
bind Zanna.Math.Quat as Quat;
bind Zanna.Terminal;

func start() {
    var world = PhysicsWorld3D.New(0.0, 0.0, 0.0);
    var body = PhysicsBody3D.Sphere(0.5, 1.0);
    world.Add(body);

    body.SetOrientation(Quat.Identity());
    body.ApplyTorque(0.0, 6.0, 0.0);
    world.Step(0.5);

    Say("AngularVelocity = " + toString(body.get_AngularVelocity()));
    Say("Orientation = " + toString(body.get_Orientation()));

    body.Sleep();
    Say("Sleeping = " + toString(body.get_IsSleeping()));
    body.Wake();

    body.set_IsKinematic(true);
    body.SetVelocity(1.0, 0.0, 0.0);
    body.SetAngularVelocity(0.0, 1.0, 0.0);
    world.Step(1.0);
}
```

### BASIC Example

```basic
DIM world AS OBJECT
DIM body AS OBJECT
DIM q AS OBJECT

world = Zanna.Graphics3D.PhysicsWorld3D.New(0.0, 0.0, 0.0)
body = Zanna.Graphics3D.PhysicsBody3D.Sphere(0.5, 1.0)
world.Add(body)

q = Zanna.Math.Quat.Identity()
body.SetOrientation(q)
body.ApplyTorque(0.0, 6.0, 0.0)
world.Step(0.5)

PRINT "Sleeping before: "; body.IsSleeping
body.Sleep()
PRINT "Sleeping after: "; body.IsSleeping
body.Wake()

body.IsKinematic = 1
body.SetVelocity(1.0, 0.0, 0.0)
body.SetAngularVelocity(0.0, 1.0, 0.0)
world.Step(1.0)
```

For a runnable headless example, see
[`examples/apiaudit/graphics3d/physics3d_rotation_demo.zia`](../../../examples/apiaudit/graphics3d/physics3d_rotation_demo.zia).
For the advanced collider surface, see
[`examples/apiaudit/graphics3d/collider3d_advanced_demo.zia`](../../../examples/apiaudit/graphics3d/collider3d_advanced_demo.zia).
For world-space query coverage, see
[`examples/apiaudit/graphics3d/physics3d_queries_demo.zia`](../../../examples/apiaudit/graphics3d/physics3d_queries_demo.zia).
For structured collision events, see
[`examples/apiaudit/graphics3d/collisionevent3d_demo.zia`](../../../examples/apiaudit/graphics3d/collisionevent3d_demo.zia).

---

## Zanna.Graphics3D.Character3D

Controller-based character movement with slide-and-step collision against a `PhysicsWorld3D`.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.Character3D(radius, height, mass)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `StepHeight` | Double | Read/Write | Maximum step-up height |
| `World` | Object | Read/Write | Attached `PhysicsWorld3D` |
| `IsGrounded` | Boolean | Read | Character is grounded |
| `JustLanded` | Boolean | Read | Character landed during the latest move |
| `Position` | Object (`Vec3`) | Read | Current world position |
| `Height` | Double | Read/Write | Capsule height including caps; the setter is `TrySetHeight` with the result ignored |
| `PushStrength` | Double | Read/Write | Impulse scale applied to blocking dynamic bodies (default `1.0`; `0` blocks without pushing) |
| `CollideDynamic` | Boolean | Read/Write | Dynamic bodies block and can be pushed (default `true`; `false` restores legacy ghost-through) |
| `RidePlatforms` | Boolean | Read/Write | Track moving kinematic/static ground each move (default `true`) |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `Move(direction, dt)` | `Void(Object, Double)` | Move with collision response using a `Vec3` direction |
| `SetSlopeLimit(degrees)` | `Void(Double)` | Set the maximum climbable slope angle |
| `SetPosition(x, y, z)` | `Void(Double, Double, Double)` | Teleport the character |
| `TrySetHeight(height)` | `Boolean(Double)` | Crouch/stand capsule resize: shrinking always succeeds with the feet planted; growing tests the stand pose first and returns `false` when blocked |
| `IsSliding()` | `Boolean()` | `true` while resting against a surface steeper than the slope limit (the controller slides instead of grounding) |
| `GetGroundBody()` | `Object()` | Body under the controller's feet, or `null` while airborne — the hook for conveyor logic and surface queries |

Dynamic-body interaction: blocking dynamic bodies receive one impulse per body
per move along the contact normal, proportional to the approach speed and
scaled by `min(1, controllerMass / bodyMass)` — light props yield and heavy
props wall the controller. Moving platforms: while grounded on a kinematic or
static body with velocity, the controller pre-displaces by the platform's step
displacement (linear plus yaw about the platform origin) *before* the swept
move, so walls on the platform still block. Platform rotation rides yaw only.

---

## Zanna.Graphics3D.Ragdoll3D

Auto-built ragdoll rigs: capsule bodies and limited 6-DoF joints fitted to a
`Skeleton3D`'s bind pose, with animation handoff and blend-back.

**Type:** Instance (obj)
**Constructor:** `Ragdoll3D.New(skeleton)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `TotalMass` | Double | Read/Write | Mass distributed across bodies by relative volume (default 70; configure before `Activate`) |
| `RadiusScale` | Double | Read/Write | Capsule radius as a fraction of bone length (default 0.22) |
| `MinBoneLength` | Double | Read/Write | Bones shorter than this collapse into their parent body (default 0.12) |
| `BodyCount` | Integer | Read | Number of rig bodies (builds the rig on first read) |
| `IsActive` | Boolean | Read | True between `Activate` and `Deactivate` |

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetJointLimits(boneName, swingDeg, twistDeg)` | `Void(String, Double, Double)` | Per-bone 6-DoF limit override (defaults ±30° swing / ±20° twist, relative to bind) |
| `Activate(world, controller, node)` | `Void(Object, Object, Object)` | Anim→ragdoll handoff: bodies posed from the current pose with finite-difference velocities, then added to the world with their joints |
| `Deactivate(blendSeconds)` | `Void(Double)` | Remove the rig and blend the palette back to live animation (play the get-up state at the same time) |
| `SetPowered(boneMask, stiffness)` | `Void(Integer, Double)` | PD-drive masked bodies (bit per rig slot; -1 = all) toward the animated pose — powered hit reactions |
| `Step(dt)` | `Void(Double)` | Per-step sync (palette write-back, root-follow, powered drive). Game3D worlds call this automatically between physics and scene sync; raw users call it manually |
| `GetBody(boneName)` | `Object(String)` | Per-bone body escape hatch (cameras, impulses) |

Rig bodies share a dedicated collision layer that excludes rig-vs-rig contacts
(joint anchors overlap by construction) while colliding with all world
geometry. The node root-follows the corpse: the entity origin tracks the root
body while skinned vertices stay world-stable. Requires an (approximately)
uniformly scaled entity. Game3D sugar: `Entity3D.EnableRagdoll()` builds and
activates from the entity's animator (cached), `DisableRagdoll(blendSeconds)`
blends out — the natural pairing with `Health3D.JustDied()`.

---

## Zanna.Graphics3D.DistanceJoint3D

Maintains a fixed distance between two `PhysicsBody3D` instances.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.DistanceJoint3D(bodyA, bodyB, distance)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Distance` | Double | Read/Write | Target separation distance |

---

## Zanna.Graphics3D.SpringJoint3D

Hooke's-law spring constraint between two `PhysicsBody3D` instances.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.SpringJoint3D(bodyA, bodyB, restLength, stiffness, damping)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `Stiffness` | Double | Read/Write | Spring constant |
| `Damping` | Double | Read/Write | Velocity damping factor |
| `RestLength` | Double | Read | Natural spring length |

---

## Zanna.Graphics3D.HingeJoint3D

Anchor constraint between two `PhysicsBody3D` instances that keeps authored
anchor points together while allowing relative angular velocity around the
given hinge axis.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.HingeJoint3D(bodyA, bodyB, anchor, axis)`

### Notes

- `anchor` and `axis` are `Vec3` values. The axis must be finite and non-zero.
- Register with `PhysicsWorld3D.AddJoint(joint, 2)`.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetMotor(enabled, targetVelocity, maxImpulse)` | `Void(Boolean, Double, Double)` | Drive rotation about the hinge axis toward a target angular velocity, bounded by a maximum impulse strength |
| `GetAngle()` | `Double()` | Current signed hinge angle, in radians |
| `SetLimits(min, max)` | `Void(Double, Double)` | Constrain the hinge to an angle range, in radians |

---

## Zanna.Graphics3D.RopeJoint3D

Maximum-distance constraint between two `PhysicsBody3D` instances. Bodies can
move closer than `MaxLength`; the solver only corrects separation beyond it.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.RopeJoint3D(bodyA, bodyB, maxLength)`

### Properties

| Property | Type | Access | Description |
|----------|------|--------|-------------|
| `MaxLength` | Double | Read/Write | Maximum allowed separation |

### Notes

- Register with `PhysicsWorld3D.AddJoint(joint, 3)`.

---

## Zanna.Graphics3D.SixDofJoint3D

Configurable frame-anchor constraint between two `PhysicsBody3D` instances. By
default it locks the two frame anchor translations together. `SetLinearLimits`
allows bounded frame-anchor separation. `SetAngularLimits` clamps relative
pose angle, in radians, around each joint-frame axis using the bodies' creation
relative orientation as the zero pose.

**Type:** Instance (obj)
**Constructor:** `NEW Zanna.Graphics3D.SixDofJoint3D(bodyA, bodyB, frameA, frameB)`

### Notes

- `frameA` and `frameB` are `Mat4` values. Their translation components define
  the local anchor points.
- Linear limits, locked linear axes, and the linear motor all operate in
  **body A's joint frame** (frameA composed with body A's rotation) — the
  same frame the angular limits use. A "local X" slider therefore keeps
  sliding along the base body's X after the base rotates.
- Angular limits are per-axis pose-angle bounds. Equal min/max values lock that
  rotary axis; the solver also removes angular velocity that would drive a
  locked axis or already-limited pose farther out of range.
- Register with `PhysicsWorld3D.AddJoint(joint, 4)`.

### Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetLinearLimits(min, max)` | `Void(Object, Object)` | Set per-axis minimum and maximum anchor separation as `Vec3` values |
| `SetAngularLimits(min, max)` | `Void(Object, Object)` | Set per-axis relative pose-angle limits in radians as `Vec3` values |
| `SetLinearMotor(enabled, velocity, maxForce)` | `Void(Boolean, Object, Double)` | Drive relative linear velocity along unlocked axes toward a `Vec3` target, bounded by a maximum force |

---

## Zanna.Graphics3D.Cloth3D

From-scratch verlet cloth for secondary motion: **chains** (capes, hair
tails) and **patches** (banners, flags). Fixed 1/120 substeps behind an
accumulator (max 8/step, remainder carried) make replay bit-identical; the
same simulated time sliced into different frame dts produces identical
states.

```zia
var cape = Cloth3D.NewChain(8, 1.6).Pin(0);         // segments, total length
var flag = Cloth3D.NewPatch(6, 4, 1.2, 0.6);        // grid w,h, size w,h
flag.Pin(0).Pin(1).Pin(2).Pin(3).Pin(4).Pin(5);     // pin the top row
flag.SetWind(Zanna.Math.Vec3.New(0.0, 0.0, 1.0), 6.0);
flag.BindMesh(mesh);                                 // rewritten in place per step
cape.BindBoneChain(controller, "cape_root");         // linear bone chain
world.AddCloth(cape);
world.AddCloth(flag);
```

- Knobs: `Damping` (0..1), `Iterations` (default 4), `GravityScale`,
  `WindResponse`; tune stiffness in that order. `GetPoint(i)` inspects the
  simulation; `Step(dt)` drives an unregistered cloth manually.
- Colliders: `AddSphere(center, r)` / `AddCapsule(a, b, r)` (≤16, static).
  Bone-bound chains simulate in the skeleton's **model space**, so a static
  torso capsule is exactly right for a cape.
- `BindBoneChain` walks single-child links from the root bone (branching
  traps), pins point 0 to the animated root pose each step, and writes
  simulated directions back as aim rotations through the same masked
  pose-override slot ragdolls use (positions preserved — length-safe).
  Anchor jumps beyond half the rest length rigid-translate the whole cloth,
  so teleports never inject phantom velocity.
- One-way coupling by design: cloth never feeds back into rigid physics.

## Zanna.Graphics3D.Vehicle3D

Raycast vehicle on top of a dynamic chassis body: each wheel is a suspension
ray (not a rigid body), so there is nothing to tip over or joint-explode at
speed. Suspension is a spring/damper along the ray, drive and brake are
longitudinal forces at the contact patch, and lateral grip is a friction
circle scaled by the per-wheel suspension load — unloaded inner wheels slide
first, exactly like a real car.

```zia
var chassis = PhysicsBody3D.NewAABB(0.9, 0.4, 1.8, 1200.0);
PhysicsBody3D.SetPosition(chassis, 0.0, 1.0, 0.0);
world.Add(chassis);

var car = Vehicle3D.New(world, chassis);
// AddWheel(x, y, z, radius, suspensionRest, stiffness, damping, steers, driven)
car.AddWheel(-0.8, -0.3,  1.4, 0.34, 0.35, 42000.0, 3200.0, true,  false);
car.AddWheel( 0.8, -0.3,  1.4, 0.34, 0.35, 42000.0, 3200.0, true,  false);
car.AddWheel(-0.8, -0.3, -1.4, 0.34, 0.35, 42000.0, 3200.0, false, true);
car.AddWheel( 0.8, -0.3, -1.4, 0.34, 0.35, 42000.0, 3200.0, false, true);

// Per fixed step: throttle 0..1, brake 0..1, steer -1..1.
car.SetInput(throttle, brake, steer);
car.Step(dt);           // before world.Step(dt)
world.Step(dt);
```

- Wheel anchors are in **chassis-local** space; the ray casts down the
  chassis's local -Y through `suspensionRest + radius`. Suspension rays
  ignore the chassis itself, and `SetCollisionMask` filters what counts as
  ground.
- Tuning: `SetDriveForce` / `SetBrakeForce` (newtons at the patch),
  `SetMaxSteer` (degrees, clamped to 85, applied to `steers` wheels),
  `SetGrip(longitudinal, lateral)` scales the friction circle. Stiffness
  around `35 * mass` and damping near `0.1 * stiffness` are sensible
  starting points.
- Telemetry: `Speed` (signed m/s along the chassis forward axis, +Z local),
  `WheelCount`, and per wheel `WheelInContact(i)`, `WheelTravel(i)` (current
  suspension length in meters — `suspensionRest` when airborne, smaller as
  it compresses), `WheelLoad(i)` (newtons) — enough for skid audio,
  suspension meshes, and dust effects without touching the solver.
- `Step` applies forces only; the world integrates them on its next step, so
  vehicles are deterministic under the same fixed-dt replay contract as the
  rest of the world.

## See Also

- [Graphics](README.md) - 2D graphics, scene graph, fonts, and sprites
- [Mathematics](../math.md) - `Vec3`, `Mat4`, and `Quaternion` / `Quat`
- [Graphics 3D Guide](../../graphics3d-guide.md) - broader 3D rendering, materials, scenes, and asset loading
