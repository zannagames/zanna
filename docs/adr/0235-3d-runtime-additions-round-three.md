---
status: active
audience: contributors
last-verified: 2026-08-03
---

# ADR 0235: 3D Runtime Additions Round Three

## Status

Accepted (2026-08-03)

## Context

Executing the 2026-08 deep review's remaining runtime items
(`docs/internals/graphics3d-deep-review-2026-08.md`) surfaced one real
rendering defect and four API gaps that ADR 0233 had deliberately deferred
or that fell out of the review's strategic list.

## Decisions

### Software capture idempotency (defect fix)

The CPU finalization chain (post-FX gamma-out plus final-overlay replay)
mutates the output framebuffer in place, and `Present` resets
`frame_finalized` so the next render can finalize. A capture after a
presented frame — or a second `Present` without re-rendering — re-ran the
chain on already-encoded pixels, double-applying display gamma and washing
every software screenshot toward gray (`x^(1/4)` instead of `x^(1/2.2)`).
Finalization now runs at most once per rendered frame: a
`cpu_finalized_this_render` latch sets at first finalize and re-arms only on
`Clear`/`Begin`. GPU backends were never affected (their post-FX reads scene
targets rather than mutating the presented buffer). Regression:
`g3d_test_g3d_capture_idempotent` (software-pinned).

### PostFX3D chain enumeration and removal

`GetEffectKind(index)` enumerates the chain in application order against the
new static `Zanna.Graphics3D.PostFXEffectKind` constants (thirteen kinds
mirroring the backend discriminator), and `RemoveEffectAt(index)` deletes
one entry while preserving order — closing ADR 0233's explicit exclusion now
that the enumeration design exists. The retained color-LUT strip follows
`Clear`'s precedent and stays for a later `AddColorLut`.

### NavAgent3D pause and path readback

`Stop()`/`Resume()`/`IsStopped` implement the Unity `isStopped` idiom: a
stopped agent keeps its target and path, syncs its bound pose, publishes
zero motion, and keeps `RemainingDistance` live. `Target`, `HasTarget`,
`PathCornerCount`, and `GetPathCorner(i)` expose the goal and the computed
corner list (fresh `Vec3`s, ownership-tracked) for debug drawing and patrol
previews.

### SceneGraph baked-clip readback

The clip carrier (`AdoptAnimations`, VSCN load) was write-only from Zia.
`AnimationCount`, `GetAnimationName(i)`, and `GetAnimationDuration(i)`
enumerate both clip classes; `GetAnimation(i)` returns rigid
`NodeAnimation3D` clips and yields null for skeletal `Animation3D` entries,
which are driven by `AnimController3D` at runtime rather than
`NodeAnimator3D`.

### PhysicsWorld3D.BuildSceneColliders

Materializes Studio's authored `collider.*` metadata convention (ADR 0185:
box/sphere/capsule/mesh-bounds dimensions plus `collider.trigger`) into
static bodies and triggers at each node's world pose, with world scale
applied to authored dimensions and mesh-bounds boxes recentered on subtree
bounds. This is the runtime read side of the convention that every game
previously hand-rolled (ashfall-scenes' `readBox` et al.). Bodies are always
static; calling twice adds duplicates by design — build into a fresh world.
The eventual typed-collider VSCN section (ADR 0185's deferral) supersedes
this convention when it lands; the API contract (scan scene → bodies) is
designed to survive that migration unchanged.

### Pixels return typings

`World3D.CaptureFinalFrame`, `Canvas3D.Screenshot`/`ScreenshotFinal`, and
`PostFX3D.MakeIdentityLut` method registrations declared untyped `obj()`
returns, which Zia's checker types as the declaring class (the ADR 0233
gotcha). All four now declare `obj<Zanna.Graphics.Pixels>`.

## Consequences

- ABI manifest counts/hash updated; narrative docs synced
  (`rendering3d.md` PostFX + NavAgent sections).
- New fixtures: `test_g3d_capture_idempotent`, `test_g3d_scene_colliders`;
  `test_navagent3d` and `test_g3d_api_symmetry2` extended in place.
- The `test_rt_navagent3d` C harness's byte-layout mirror gained the
  `stopped` flag (mirror discipline per ADR 0233's input-layout lesson).
