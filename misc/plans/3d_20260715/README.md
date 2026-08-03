# Zanna 3D Application-Layer Improvement Program

Date prepared: 2026-07-15  
Evidence baseline: current working tree and build outputs inspected on 2026-07-15  
Baseline reverified: 2026-07-16 against the committed tree (runtime API dump,
cited files/tests, and both wave-A defect repros re-run; see plan 00)  
Status: implementation plans; no implementation is authorized by this package alone

## Purpose

This package converts the review of `3dbowling`, `ridgebound`, `ashfall`, the
complete `Zanna.Game3D` surface, and the underlying Graphics3D runtime into an
ordered implementation program. It is intentionally explicit enough for an
execution agent that has not performed the original architectural review.

The runtime is already a capable low-level 3D engine. The program therefore
does not propose another renderer rewrite or duplicate systems that already
exist. It closes two confirmed overlay defects, then builds the missing
application tier: a flexible frame driver, resource scopes, registered
environment renderables, reusable motor/camera composition, asset and quality
policy, entity-aware queries/events, pooled feedback, a stronger scene/UI
framework, ranged combat, save-game composition, deterministic scenario tests,
and clearer starters. The three games are migrated only after their required
foundations are proven.

## How to use this package

An implementation owner must:

1. Read [00-baseline-and-program-contract.md](00-baseline-and-program-contract.md).
2. Read the selected plan and every dependency named in its `Dependencies`
   section.
3. Read the shared registers under `appendices/`.
4. Reconfirm the cited code against the then-current branch. Line numbers and
   free class IDs can drift; contracts and ownership boundaries are the
   authority.
5. Create or amend the required ADR before changing a public runtime surface,
   C ABI, cross-layer dependency, IL reference, or verifier rule.
6. Implement one acceptance slice at a time, keeping the tree green after each
   slice. Do not combine unrelated plans in one change.
7. Record evidence named in the plan's handoff checklist.

The proposed API names are design inputs, not pre-approved ABI. The canonical
draft register is [appendices/proposed-api-register.md](appendices/proposed-api-register.md).
If an ADR changes a name or signature, update the register and all dependent
plans in the same documentation change.

## Current conclusion

Today a Zanna 3D game normally follows this pattern:

1. Construct a `Game3D.World3D` or independently construct Canvas3D,
   SceneGraph, Camera3D, and PhysicsWorld3D.
2. Configure lighting, post-processing, materials, terrain/water, audio, input,
   and quality with a mixture of Game3D helpers and low-level Graphics3D calls.
3. Own game entities and raw render/physics resources in script fields and
   arrays.
4. Poll input and window events in a manual loop.
5. Run game logic and a fixed or variable simulation step.
6. Render the scene, manually inserted terrain/water/instance passes, effects,
   overlay, and presentation in the correct order.
7. Build scene state, UI flow, event routing, asset fallback, effect pooling,
   quality propagation, save schemas, and test probes in the game.

`World3D.Run*` is concise when its fixed render order is sufficient. The real
demos need custom world passes, fixed-step gameplay, viewmodels, replay cameras,
HUD/menu states, or deterministic probes, so all three retain meaningful
manual orchestration. This program preserves that flexibility while making the
common responsibilities first-class and composable.

## Plan inventory

| ID | Plan | Primary outcome | Depends on |
|---:|---|---|---|
| 00 | [Baseline and program contract](00-baseline-and-program-contract.md) | Shared evidence, invariants, and execution rules | None |
| 01 | [Final-overlay alpha correctness](01-final-overlay-alpha-correctness.md) | Correct scalar overlay opacity (`DrawRect2DAlpha` family) on every backend | 00 |
| 02 | [Metal AA-text resource identity](02-metal-aa-text-resource-identity.md) | Eliminate same-size AA-text texture aliasing | 00 |
| 03 | [Fixed-step frame driver and render phases](03-fixed-step-frame-driver-and-render-phases.md) | Poll-based fixed-step scheduling with custom render insertion points | 01, 02 |
| 04 | [Scene and resource scopes](04-scene-and-resource-scopes.md) | Idempotent ownership and teardown for mixed 3D resources | 03 |
| 05 | [World environment renderables](05-world-environment-renderables.md) | Register terrain, water, vegetation, sky, and time-of-day with World3D | 04 |
| 06 | [Composable character motor](06-composable-character-motor.md) | Intent-driven movement independent of hard-coded input/camera policy | 03, 10 |
| 07 | [Composable camera rig](07-composable-camera-rig.md) | Reusable base camera modes plus additive camera effects | 03, 10 |
| 08 | [Asset location and fallback policy](08-asset-location-and-fallback-policy.md) | One source/package/runtime resolver with explicit fallback diagnostics | 00 |
| 09 | [Quality profile system](09-quality-profile-system.md) | Capability-resolved policy shared by renderer and gameplay subsystems | 05, 08 |
| 10 | [Entity-aware queries and event stream](10-entity-aware-queries-and-event-stream.md) | World queries/events that return Game3D identities | 00 |
| 11 | [Pooled effects and audio cues](11-pooled-effects-and-audio-cues.md) | Named, bounded, reusable visual/audio feedback | 04, 09, 10 |
| 12 | [Game application, scene, and UI framework](12-game-application-scene-ui-framework.md) | Production-grade `GameBase3D` without duplicating `Zanna.Game.UI` | 03, 04, 01, 02 |
| 13 | [Ranged combat and hurt regions](13-ranged-combat-and-hurt-regions.md) | Hitscan, penetration, radial damage, and hurt-region resolution | 10, 11 |
| 14 | [Versioned save-game composition](14-versioned-savegame-composition.md) | Atomic slots combining custom data and `World3D.SaveState` | 04, 08 |
| 15 | [Deterministic 3D scenario harness](15-deterministic-3d-scenario-harness.md) | Shared input, stepping, capture, and assertion conventions | 03, 10 |
| 16 | [Documentation, starters, and API navigation](16-documentation-starters-and-api-navigation.md) | A discoverable path from hello-world to custom game | 03–15 as applicable |
| 17 | [3dbowling migration](17-3dbowling-migration.md) | Prove low-level physics-heavy migration without behavior regression | 01–04, 08–12, 14, 15 |
| 18 | [Ridgebound migration](18-ridgebound-migration.md) | Prove environment, traversal, quality, and scoped ownership | 03–12, 14, 15 |
| 19 | [Ashfall migration](19-ashfall-migration.md) | Prove FPS/custom-render/ranged-combat application composition | 03–15 |
| 20 | [Roadmap, release gates, and rollout](20-roadmap-release-gates-and-rollout.md) | Integration sequence, compatibility policy, and completion gates | All |

## Where to start

As of the 2026-08-03 reverification:

- **Wave A is complete, outside this program.** Both wave-A defects were fixed
  by the 2026-07-29 3D stabilization pass (overlay alpha in
  `canvas3d_queue_screen_geometry`; Metal AA-text resource identity) and are
  pinned by the default-lane CTests `zia_regress_overlay_alpha_blend` and
  `zia_regress_overlay_aa_text_identity`. Plans 01–02 are obsolete; keep them
  only as historical context.
- **Plan 12 is partially overtaken:** a `GameBase3D`/`IScene3D` scene
  framework now exists as an example-level library
  (`examples/games/lib/gamebase3d.zia`, consumed by `examples/3d/game3d_scenes`
  and `overhaul_showcase`). The plan's engine-level framework remains unbuilt;
  any restart should begin by evaluating whether promoting that library is the
  right shape.
- **No other plan has begun implementation.**

Before restarting any wave, reconcile this program against the current
full-stack review in `docs/internals/graphics3d-deep-review-2026-08.md` —
plans 13–15 (ranged combat, versioned saves, deterministic harness) and 16–19
(docs/starters, demo migrations) overlap directly with that review's Tranche 4
recommendations. The next actionable work is plan 00's baseline capture,
skipping the wave-A repro steps.

## Program waves

The dependency graph is intentionally conservative. Parallel work is allowed
only where the plans do not touch the same runtime structures.

| Wave | Scope | Exit condition |
|---|---|---|
| A: correctness | 01–02 | Both minimal repros pass on software and the affected GPU backend; regression tests are automated |
| B: orchestration foundation | 03–04, 08, 10 | Fixed scheduling, lifecycle ownership, asset resolution, and entity-aware queries are stable |
| C: world composition | 05–07, 09, 11–12 | Environment, movement, camera, feedback, quality, and app framework compose without a mandatory monolith |
| D: gameplay/data/testing | 13–16 | Ranged combat, save slots, deterministic harness, docs, and starters are ready |
| E: proof by migration | 17–19 | Every reviewed game adopts the useful layers while retaining its release gates |
| F: release | 20 | Cross-platform, disabled-graphics, documentation, surface-audit, and full-build gates pass |

Within a wave, consult
[appendices/dependency-and-risk-register.md](appendices/dependency-and-risk-register.md)
before assigning concurrent owners. Plans 03, 04, 05, 10, 11, and 13 all touch
the World3D internals and should normally land serially or through an explicitly
coordinated integration branch.

## Non-negotiable program rules

- Preserve the low-level Graphics3D escape hatches. The application layer is
  additive; custom games must still be able to own their frame phases.
- Do not replace already-landed systems merely to fit this plan. Compose
  `ThirdPersonController`, `CharacterController3D`, persistence,
  `EffectRegistry3D`, `Environment3D`, `Zanna.Game.UI`, and
  `Zanna.Assets.Resolver` where possible.
- Public classes receive permanent appended class IDs, real and
  disabled-graphics implementations, registry definitions, docs, surface
  audits, and VM/native coverage as applicable. Never renumber an existing ID.
- Avoid script callbacks in per-entity or worker-thread hot paths. Prefer
  pollable objects and explicit phase methods. Any callback surface must use the
  established VM bridge and prove VM/native behavior parity.
- Fixed-step behavior, phase ordering, event lifetime, ownership, and error
  behavior are public contracts. Document and test them before demo migration.
- Software is the portable correctness oracle, but it cannot replace Metal,
  D3D11, and OpenGL validation for backend-specific resource and blend defects.
- Do not add dependencies. Do not introduce raw platform preprocessor checks
  outside approved adapters.
- Preserve unrelated uncommitted changes. Each owner must inspect
  `git status --short` before editing. The historical plan directories removed
  in the 2026-07 documentation reorganization stay deleted; do not restore them
  as a side effect of any plan.

## Shared appendices

- [Proposed API register](appendices/proposed-api-register.md): ownership,
  namespace, draft signatures, and compatibility decisions.
- [Shared validation matrix](appendices/shared-validation-matrix.md): required
  tests by change type and platform/backend.
- [Dependency and risk register](appendices/dependency-and-risk-register.md):
  overlap map, failure modes, mitigations, and escalation rules.

## Program-level definition of done

The program is complete only when all of the following are true:

- The two confirmed overlay bugs have automated regressions and pass on all
  applicable backends.
- A custom-render game can use fixed-step scheduling without rebuilding an
  accumulator or surrendering custom pre-scene/post-scene/overlay passes.
- A scene or level can release all of its mixed resources through one
  idempotent scope operation.
- World3D can automatically tick and draw registered environment renderables.
- Character movement and camera behavior can be driven by game intent rather
  than hard-coded device policy.
- Asset fallback and quality policy are defined once per game, not copied into
  each subsystem.
- World queries resolve `Entity3D` and hurt-region identity without a game-side
  body registry.
- Effects and audio cues have bounded, observable pooling behavior.
- The scene/UI framework supports fixed update, variable update, custom world
  draws, overlay, pause, resize, and transitions.
- Ranged combat and save slots are reusable without forcing one genre's weapon
  or data model.
- Deterministic scenario tests cover all three demo migrations.
- `3dbowling`, `ridgebound`, and `ashfall` retain their existing gameplay and
  visual release gates after migration.
- Full platform build scripts, tests, lints, runtime surface audits,
  disabled-graphics builds, docs, and packaging checks are green.

