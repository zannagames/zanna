# Graphics3D Deep Review — 2026-08-03

Status: proposed
Scope: full 3D stack — runtime (`src/runtime/graphics/3d`), public Zia API,
demos/examples, Zanna Studio 3D editing, tests, and cross-platform parity.
Method: five parallel audits (runtime completeness, API surface via live
`--dump-runtime-api`, demos, Studio, tests/platforms) with spot verification,
plus a local build + `ctest -L graphics3d` baseline (156/156 after one known
ridgebound perf-flake rerun).

Prior art: the 2026-07-29 review program (ADR 0227) landed tranches A–D
(stabilization fixes, test/repo hygiene, API symmetry, docs). This review
assesses the state after the 2026-08 hardening wave and scopes the deferred
E/F "next level" work.

---

## 1. Verdict

The 3D runtime itself is in unusually good shape. 147k lines with **zero
TODO/FIXME debt**, eight closed audit ledgers (~700 defects fixed July–August),
a truthful 38-bit backend-capability system, disciplined platform-macro usage
(8 raw macros, all justified), and a software backend that acts as a real
correctness oracle. Feature breadth is genuinely wide: clustered forward+,
CSM, TAA/HDR, baked GI + SH probe grids, full physics (CCD, joints, ragdoll,
vehicle, cloth), navmesh with dynamic obstacles and off-mesh links, IK,
animation layering, streaming with HLOD, and a complete gameplay tier
(combat, AI, dialogue, cutscenes, persistence).

The remaining problems are **not robustness problems**. They cluster in four
places:

1. **Developer-facing seams** — API asymmetries, a broken mouse-picking seam,
   no event callbacks, inconsistent error idioms, and narrative docs that
   drifted from the registry (including actively wrong facts).
2. **Proof** — real-GPU backends (~40k LOC) are nearly untested; Linux/OpenGL
   has one default-lane test; two GPU gates treat SKIP as PASS; perf budgets
   are effectively unguarded by default.
3. **Product surface** — 24 shipped gameplay classes have zero demos; demo
   assets are synthetic fixtures; Studio cannot author prefabs-from-selection,
   animation, or import settings.
4. **Architecture ceilings** — VSCN base64 embedding, a closed set of six
   shading models (no user shaders), no compute path, OpenGL as the weakest
   backend on the only platform where it is the sole GPU option.

---

## 2. Verified defects and quick fixes (Tranche 1 — days)

### Docs that are actively wrong (fix first; they mislead users today)
- `docs/zannalib/graphics/rendering3d.md:2136` documents
  `SetWaveParams(amplitude, frequency, speed)`; the C entry point is
  `rt_water3d_set_wave_params(obj, speed, amplitude, frequency)`
  (`rt_water3d.c:502`; getter order in `extras.def:760-762` confirms).
  **Documented argument order is reversed.**
- `physics3d.md:343,390` document `NewSphere(radius, mass)` factories that do
  not exist anywhere in the registry (`PhysicsBody3D.Sphere` is the real name).
- `game3d.md:409` documents `tick()` (real: `World3D.Update()`);
  `game3d.md:516-522` list `setName/setLayer/setMesh/setMaterial/
  setCollisionMask` as methods — all are properties.
- `rendering3d.md` front-matter says last-verified 2026-07-26 and omits ~100
  registered members (all Camera3D matrix getters, SceneGraph.AdoptAnimations/
  UnresolvedPrefabCount, all Water3D wave getters, 24 Terrain3D members, 6 of
  8 Sky3D members, 11 Mesh3D members). Root cause: ADR 0227's def-batch
  (commit 42261ab02) updated generated docs but not `docs/zannalib/graphics/`.
  Add a CTest (not CI) that fails when a def-batch lands without narrative-doc
  sync — extend the existing docs-snippets/manifest tripwire pattern.

### Test-integrity holes
- **SKIP passes as PASS**: `src/tests/CMakeLists.txt:3983`
  (`PASS_REGULAR_EXPRESSION "(PASS|SKIP):"`) and `:3945`. A machine whose GPU
  backend fails to initialize prints SKIP and goes green. Use
  `SKIP_REGULAR_EXPRESSION` so skips are reported as skips.
- ~~Hardcoded `/tmp` in default-lane probes~~ — **withdrawn on verification**:
  `src/tests/CMakeLists.txt:74-80` provisions `<SystemDrive>/tmp` on Windows at
  configure time, so `/tmp` paths in CTest-run probes are a deliberate,
  working convention (`walk_min_probe.zia:167` etc. are fine). Residual risk
  only when running probes by hand on a Windows box that never configured the
  test tree; not worth churn.
- **Orphaned fixtures** (resolved during Tranche 1):
  `test_navagent3d.zia` / `test_sound3d_objects.zia` registered into
  `G3D_ZIA_TESTS`; `demo_sprite3d.zia` rescued to
  `examples/3d/sprite3d_demo.zia` (only Sprite3D sample in the tree);
  `demo_canvas3d.zia`, `test_canvas3d_backend.zia`, `test_canvas3d_render.zia`
  deleted (superseded by walk_min baseline + gpu_smoke/render_scale probes).
  `prefab_{source,world}_v7.scene3d` were NOT orphaned — live via
  `test_rt_scene3d.cpp:4913` (world references source internally); kept.
- `examples/3d/game3d_scenes/main.zia` — the README's designated GameBase3D
  reference demo — has **no CTest** (runs fine manually; 3 W001 warnings at
  lines 38, 97, 99). Register it; fix the warnings.

### Hygiene
- `misc/plans/3d_20260715/README.md:99` still says "no plan has begun
  implementation and both wave-A defects still reproduce" (2026-07-16). Wave A
  (overlay alpha, Metal AA-text) was fixed 2026-07-29 and is pinned by
  `zia_regress_overlay_alpha_blend` / `zia_regress_overlay_aa_text_identity`
  (`src/tests/CMakeLists.txt:2203-2213`). Refresh the README; reconcile the
  program (see §7).
- ~~Dangling plan breadcrumbs~~ — resolved in Tranche 1: 53 references to
  deleted plan dirs (`thirdpersonupgrade/`, `3d_overhaul/`, `fps/`, `game/`,
  `3d/`) across ~45 source/test headers repointed to living ADRs/docs or
  dropped; ashfall READMEs now note the material was retired. ADR reference
  sections left as historical record.
- `scripts/platform_policy_migration_baseline.txt:58-71` still budgets up to
  33 raw platform macros for 3D files that now use 0 (`rt_game3d.c` 9/0,
  `rt_canvas3d_overlay.c` 6/0, `vgfx3d_backend_sw.c` 7/0, …). Ratchet to 0.
- Two capability predicates hardcode backend identity behind platform ifdefs
  instead of vtable fields: `canvas3d_backend_supports_clustered_lighting`
  (`rt_canvas3d_overlay.c:1290-1295`) and `…_supports_shadow_csm`
  (`:1300-1319`). Promote to `int8_t clustered_lighting; int8_t shadow_csm;`
  vtable fields — this is the one concrete blocker to adding a 5th backend
  cheaply, and the pattern the architecture doc itself warns against.

---

## 3. Public API — "symmetry round two" + ergonomics (Tranche 2 — one ADR)

Registry ground truth: 130 3D classes, 1,890 members (1,177 methods, 713
properties), **all still `stability: preview`**. ADR 0227 landed 100% (all 22
promised additions verified in the registry; zero write-only *properties*
remain). What remains:

### Walls a real game hits (highest impact per line of code)
1. **Mouse picking is severed at the input seam.** `Camera3D.ScreenToRay` +
   `SceneGraph.RaycastPreciseHit` + `PhysicsWorld3D.Raycast` form a complete
   pipeline, but nothing in the 3D surface exposes an **absolute cursor
   position** — `Input3D` has only `MouseDelta()`, `Canvas3D` has no
   `MouseX/Y`. Click-to-select requires leaving the documented 3D surface for
   `Zanna.Input.Manager`. Fix is ~2 symbols on `Input3D` (or `Canvas3D`).
   Also: `ScreenToRayOrigin` is registered but undocumented.
2. **Physics week-one knobs**: `PhysicsBody3D.Mass` is read-only with **no
   `SetMass`** (blocks pickup/drop, fuel, damage-driven mass); no
   `LinearFactor`/`AngularFactor` (freeze rotation); `BodyCount`/`JointCount`
   exist with **no `GetBody(i)`/`GetJoint(i)`** (non-enumerable collections);
   `SetGravity` has no `Gravity` getter.
3. **`*Result` diagnostics are backwards**: the beginner path (`Prefab.Load`,
   all 8 `Assets3D.Load*`) is nullable-only with zero diagnostics, while the
   expert path (`SceneAsset`) has six `*Result` loaders. Propagate `*Result`
   variants down. Also `SceneGraph.Save -> i64` has no `SaveResult` peer, and
   `NavMesh3D.Export -> i1` vs `Import -> nullable` disagree within one class.
4. **204 setters still lack any read peer.** Concentrated: `Canvas3D` 19
   (every shadow/fog/ambient/culling/streaming knob — an options menu cannot
   render its current state), `Material3D` 15 (every texture-map slot — an
   inspector cannot show what is bound), `Particles3D` 15 (every emitter
   param), `Sky3D` 2, all five joint classes (no body readback, no
   limits/motor readback; `SixDofJoint3D` has zero properties).
   `PostFX3D` has 12 `Add*` + `Clear()` but no per-effect readback or
   `RemoveEffect(i)`. Mechanically identical to ADR 0227: ~55 thin wrappers
   over already-retained state.
5. **No tween/timer story for 3D**: `Zanna.Game.Tween` interpolates only
   f64/i64 — no Vec3/Quat/Transform tween — and neither Timer nor Tween is
   mentioned in any 3D doc or used by any 3D example. Add `TweenVec3`/
   `TweenQuat` (or overloads) + a "Timers, tweens, and time" doc section.
6. **Entity3D asymmetry**: `SetScale`/`SetRotationEuler` exist but no `Scale`/
   `Rotation` property (readback is the two-hop `entity.Node.Scale`), while
   `Position` is first-class.
7. **NavAgent3D**: no `Stop()/Resume()/IsStopped`, no path-corner readback
   (cannot draw the planned path), no `Target` getter.
8. **Two disjoint prefab systems** (`Game3D.Prefab` factories vs VSCN
   `SetPrefabReference`) that never meet, and three unrelated instantiation
   APIs (`SceneAsset.Instantiate*`, `SceneTemplate.Instantiate*`,
   `World3D.Spawn`). At minimum document the seam; ideally bridge it
   (instantiate a VSCN prefab path as an `Entity3D`).
9. **Event callbacks** remain polling-only, honestly documented as deferred on
   VM callback-trampoline policy (`game3d.md:983,1191,1253`). This is a
   cross-cutting language/VM decision, not a 3D bug — but it gates
   `OnCollision`, custom Zia camera controllers, and animation-event sugar.
   Worth an explicit ADR decision rather than an indefinite deferral.

### Error-idiom consolidation
Eight failure idioms coexist (Result / nullable / i1 / i64-"infallible" /
trap / `PostFX3D.LastError` side-channel / AssetDiagnostics3D warnings /
sentinel `-1`). Pick the house rule (Result for loaders, i1+diagnostics for
saves, Option for finds), write it into the API guide, and converge new
surface on it. `SceneAsset.Save -> i64` tagged `infallible` is not credible.

---

## 4. Tests & platform trust (Tranche 3)

The unit story is strong (~133k test LOC; game3d gameplay tier best-covered).
The trust gaps are all at the GPU/platform boundary:

1. **Linux/OpenGL: 12.3k LOC guarded by exactly one default-lane test**
   (`g3d_test_canvas3d_render_scale_opengl`) plus two SKIP-tolerant probes.
   It is also the feature-weakest backend: no `shadow_atlas_slots` (GAP-8 —
   point/omni shadows dead on Linux), no 5–8-influence GPU skinning, probed
   HDR. Implementing the OpenGL shadow-atlas hook closes the biggest
   user-visible platform gap. *Tranche-3 execution note (2026-08-03): the
   OpenGL TU is compile-guarded to `__linux__`, so this item cannot be
   built or validated from a macOS host — deferred to a Linux session; the
   remainder of tranche 3 executed.*
2. **Metal debt**: no `get_backend_stats` (GAP-10 — all 7 `Backend*` stats
   read 0 on macOS), shared-helper test is 870 lines vs D3D11's 2,492, no
   Metal point-shadow probe despite the capability being advertised.
3. **Windows**: 6+ rounds of D3D11 hardening landed in device-touching code
   with no host-independent regression net, and the two best probes
   (`zia_smoke_ridgebound_d3d11`, `zia_smoke_3dbowling_title_postfx`) are in
   the Windows `slow` set — excluded from the default lane.
4. **Perf is unguarded by default**: `Graphics3DPerfHarness.cmake` asserts
   only `> 0` on every metric; the only real millisecond budgets
   (ridgebound 50 ms GPU / 5000 ms software) are `slow`-labelled on every
   platform. Add generous-but-real budgets to the default perf probe
   (mind the known battery/E-core variance — assert p95 with headroom).
5. **`rt_textureasset3d` (6.5k LOC of KTX2/BC6H/UASTC/Basis codecs) has no
   dedicated unit test**; only Clang-only opt-in fuzz. Same for the FBX
   loader family. Add decode-fixture unit tests (small committed containers).
6. **Fuzz lane never runs on Windows/MSVC** (harness requires clang++).
   Document the gap; consider a clang-cl lane later.
7. Modules with zero direct test references: `rt_canvas3d_motion.c`,
   `rt_scene3d_spatial.c`, `rt_scene3d_metadata.c`, `rt_scene3d_nodeanim.c`,
   `rt_physics3d_character.c`, `rt_physics3d_probes.c`, `rt_canvas3d_tempmgr.c`.
   Thinnest public APIs: `Vehicle3D`, `LensFlare3D`, `InstanceBatch3D`,
   `TextureAtlas3D`.
8. **No CI executes CTest** (installer workflows only). Per project policy CI
   changes are out of scope during the zanna phase — flagged as a standing
   risk, not an action item: cross-platform 3D validation is 100% manual.

---

## 5. Demos & onboarding (Tranche 4 — the adoption story)

All 8 registered `examples/3d` demo tests pass and every demo compiles clean.
But:

1. **The 2026-07-11 action-game tier (ADRs 0074–0100) has zero demos** —
   24 classes including `ThirdPersonController`, `Health3D`/`Hitbox3D`/
   `TargetLock3D`, `Interactable3D`, `Dialogue3D`, `BehaviorTree3D`/
   `Perception3D`, `Footsteps3D`/`SurfaceTable3D`, `Sky3D`, `TimeOfDay3D`,
   `LightBaker3D`, `ReflectionProbe3D`, `Minimap3D`, `RailCamera3D`.
   Ridgebound proves the demand by **hand-rolling** sky, time-of-day, minimap,
   interaction, and surface audio that the runtime already ships.
   → One **third-person action slice** demo covers most of the tier.
2. **Persistence is demonstrated nowhere**: `World3D.SaveState`/`LoadState`/
   `Entity3D.SetPersistent` have zero uses in the entire repo.
3. **The learning path cliffs**: hello (18 lines) → starter (131) is good;
   then the "next step" (`game3d_showcase`) is a 128×96 self-asserting CI
   probe, and the next complete game is 6,868 lines (ridgebound). Fill the
   gap with a **complete small game template** (~600–1,000 lines: menu →
   gameplay → pause → save/load → win/lose, music/SFX, built on the existing
   `examples/games/lib/gamebase3d.zia`).
4. **Assets are the visible weakness**: everything in `examples/3d` is a
   synthetic fixture (450-byte triangle "model", empty streaming cells,
   one-bone procedural rig, 7.9 KB max asset). Zero demo screenshots exist
   anywhere in docs/. Only ashfall-scenes carries real licensed art
   (Quaternius/Kenney) and nothing reuses it. → Build a small **shared demo
   asset pack** (one rigged character with 3–4 clips, a few props, one HDR
   panorama, one splat set) and reuse it across demos; capture screenshots.
5. Registration: add `game3d_starter`, `game3d_scenes`, `overhaul_showcase`
   to `scripts/demo_projects.list` (native-binary + Windows launch coverage);
   reorder `examples/3d/README.md` into an explicit ladder with engine probes
   segregated from tutorials.

---

## 6. Studio 3D (Tranche 5 — each item ADR-led)

Studio's 3D editor is a competent, transactionally-correct scene-assembly
tool (byte-exact undo, three snap systems, camera bookmarks, view cube,
marquee select, batch editing, collider/light/camera overlays, bake pipeline
with exact-bytes cancel, prefab instances, cross-scene clipboard, zero TODO
debt, ~30 registered probes). Verified still-open gaps, in product-impact
order:

1. **Prefab-from-selection** — prefabs are consume-only (import-as-instance
   from an existing `.scene3d`). Assembling a lamppost in-scene and promoting
   it to a reusable asset is impossible without hand-authoring a second file.
   The single biggest workflow hole vs Unity/Godot.
2. **Animation UI** — none (no timeline, no clip assignment/preview). Clips
   survive import/save but are invisible. Start with clip list + preview.
3. **Import settings** — `ImportAssetPath` takes a path and nothing else: no
   scale, up-axis, unit, or material policy. Also **asset drops ignore the
   drop point** (`ImportAssetDrop` discards X/Y; material drops use them) —
   you cannot drop a prop where you point.
4. **Play-mode input ceiling** — mouse button 0 only, no wheel, 21 hardcoded
   keys, no capture, no pause/step, no audio routing
   (`scene_play_controller.zia:136-228`). FPS-style games are not testable
   embedded. Generalize forwarding; add wheel + right/middle buttons first.
5. **Group pivot** — multi-rotate always orbits the primary node
   (no centroid/individual-origins toggle), and "Local" is parent space.
6. **Cheap 2D-parity ports**: align/distribute (`scene_editor_2d_hierarchy_
   clipboard.zia:650`), arrow-key nudge, solo. All absent in 3D.
7. **Bake off the UI pump** — chunked on the pump today (well-mitigated:
   progress + cancel + edit-lock); `Zanna.Threads.Async` exists and is unused
   here.
8. **Navmesh/shadow visualization** — bake writes a sidecar you cannot see;
   no shadow preview in-viewport.

---

## 7. Strategic programs (Tranche 6 — the "next level")

Each is its own ADR-led program; recommended order by leverage:

1. **VSCN v8 asset references.** Still fully open (verified: save path caps
   at v7, meshes/textures embedded base64 — `rt_scene3d_vscn_save.c:2419`).
   Drives file bloat, the editing cap, undo pinning, texture duplication,
   and "re-import doesn't update placements". The single largest
   architectural gap vs Unity-class tooling, and prerequisite-adjacent to a
   binary/mmap scene path later.
2. **Typed scene sections.** `collider.*`/`env.*`/`bake.*`/`nav.*` still ride
   untyped metadata conventions only Studio parses (ADR 0185 explicitly
   deferred promotion). Promote so `SceneGraph.Load` materializes colliders/
   environment for games directly — this is what makes Studio-authored
   scenes fully *playable* without game-side convention parsing.
3. **Custom shader / material extensibility.** Six fixed shading models is
   the hard ceiling on how any Zanna game can look; all shaders are
   compiled-in strings. The defining product gap vs Unity/Godot. Even a
   constrained "uber-shader parameter blocks + user hooks" v1 beats the
   closed set. (Requires the backend-capability vtable cleanup from §2.)
4. **Event callbacks / VM trampoline policy.** Unlocks OnCollision, custom
   Zia camera controllers, animation-event sugar — the whole reactive style
   every engine tutorial assumes. Decide it once as an ADR.
5. **OpenGL modernization or successor decision.** Either bring OpenGL to
   parity (shadow atlas, skinning extras) or make the explicit call that
   Linux's future is a Vulkan backend and scope it. Today Linux is the only
   platform whose sole GPU path is the weakest backend.
6. **Compute/GPU-driven pipeline + MSAA** — later; unblocks GPU particles,
   Hi-Z occlusion, and post-FX quality, and is the payoff case for any
   Vulkan/D3D12 work.

**Reconcile `misc/plans/3d_20260715/`** (20-plan application-tier program,
README stale): wave A (plans 01–02) is done via the July-29 fixes; plan 12's
framework exists as `examples/games/lib/gamebase3d.zia` (example-level, not
engine); plans 13–15 (ranged combat, versioned savegame composition,
deterministic scenario harness) remain relevant and align with §5/§3; plans
16–19 (docs/starters, demo migrations) largely overlap Tranche 4 here.
Either refresh that program to this review's state or fold its live items in
and retire it.

---

## 8. Suggested execution order

| Tranche | Content | Size |
|---|---|---|
| 1 | §2 quick fixes: wrong docs, SKIP-as-PASS, orphaned fixtures, plan-README refresh, baseline ratchet, capability vtable fields | days |
| 2 | §3 API round two (one ADR): mouse position, SetMass/GetBody/GetJoint, ~55 getters, Result propagation, Vec3/Quat tween, narrative-doc sync + tripwire test | 1–2 wks |
| 3 | §4 platform trust: OpenGL shadow atlas, Metal stats + probe, texture-codec unit tests, default-lane GPU tests, real perf budgets | 1–2 wks |
| 4 | §5 adoption: third-person action slice demo, save/load demo, small-game template, shared asset pack + screenshots, demo registration | 2–3 wks |
| 5 | §6 Studio: prefab-from-selection → import settings + drop-at-point → animation UI v1 → play-input generalization → pivot/align/nudge | ADR each |
| 6 | §7 strategic: VSCN v8 → typed sections → custom shaders → callbacks → OpenGL/Vulkan decision | program each |

Tranches 1–3 stabilize ("polish what exists"); 4–6 are the "next level."
