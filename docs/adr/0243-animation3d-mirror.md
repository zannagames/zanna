---
status: active
audience: contributors
last-verified: 2026-08-12
---

# ADR 0243: Animation3D.Mirror (Left/Right Clip Mirroring)

## Status

Accepted (2026-08-08). Amended (2026-08-12): the mirror is now computed in
model space relative to the bind pose, removing the bilaterally-symmetric
bind-pose precondition (auto-rigged skeletons violated it and skinned to a
deformed result; found by the Legacy Baseball left-handed batter).

## Context

Humanoid animation packs are overwhelmingly performed one-handed: a baseball
pack ships right-handed batting stances, swings, and takes only. A left-handed
performance is the same motion **mirrored across the character's sagittal
plane** — something no amount of entity rotation can produce (a yaw turn
preserves chirality; the "yaw the batter 180°" attempt leaves him facing the
backstop). The runtime's animation toolkit (`Retarget`, `StripRootMotion`,
`ExtractRange`) has no mirroring operation, so consumers are stuck either
shipping doubled clip sets from a DCC batch or presenting left-handers with
right-handed motion.

Runtime facts the design builds on:

- Clips store per-bone channels of **local-space** TRS keyframes
  (`vgfx3d_anim_channel_t` / `vgfx3d_keyframe_t`), optionally with cubic
  Hermite tangents.
- `animation3d_humanoid_role()` already classifies bones into canonical roles
  encoded as `base * 4 + side` (side: 0 center, 1 left, 2 right), and
  `Skeleton3D` carries exact bone names plus an alias table.
- Reflection across the model-space X=0 plane is the conjugation
  `T' = M·T·M` with `M = diag(-1, 1, 1)`. The original design applied that
  conjugation to every **local** keyframe while swapping Left/Right channels,
  which reflects the model-space pose exactly — but the **skinned** result is
  correct only when the bind pose is bilaterally symmetric (mirror bones' bind
  globals are reflections of each other). Real auto-rigs violate this badly:
  the Meshy baseball rig carries a ~90°-rotated pelvis frame (left–right is
  local Y, not local X) and laterally offset center bones, so local
  conjugation skinned to a mangled pelvis/torso while joint *positions* still
  landed correctly — the hardest kind of defect to catch structurally.
- The correct general form mirrors the **bind-relative displacement** in model
  space: with bind globals `B` and animated globals `G`, each output bone `b`
  takes `G'_b = M·(G_m·B_m⁻¹)·M·B_b` from its sagittal partner `m`. Skinning
  then sees `G'_b·B_b⁻¹ = M·(G_m·B_m⁻¹)·M` — the reflected displacement field
  of the partner — which reads correctly for **any** bind pose; the only
  remaining assumption is that the character mesh (and the skeleton's
  model-space joint positions) are bilaterally symmetric about X=0.

## Decision

Add one operation to the frontend-visible Animation3D surface (hence this
ADR):

- `Zanna.Graphics3D.Animation3D.Mirror(skeleton)` →
  `void *rt_animation3d_mirror(void *animation, void *skeleton)`

Semantics:

- Returns a **new** clip (name suffixed `"_mirror"`, duration/looping
  preserved); the source is never modified. `NULL` for invalid input or a
  clip with no keyed channels.
- The mirror is computed in model space relative to the bind pose. Per time
  on a **union key-time grid** (all channels' key times, deduplicated, with
  wide spans subdivided to ~30 Hz so cubic segments keep their shape), the
  source pose is composed to model space and each output bone `b` receives
  `G'_b = M·(G_{mirror(b)}·B_{mirror(b)}⁻¹)·M·B_b`; locals are recomputed
  against the mirrored parent globals, decomposed, and emitted as **linear**
  keys with hemisphere-aligned quaternions (cubic tangents do not survive the
  nonlinear local recompute; the densified grid carries the curvature).
- One output channel per skeleton bone that leaves bind; bones whose every
  resampled key stays at their bind local (float-noise tolerance) are
  dropped, so rigid riders of animated parents don't bloat the clip.
- `mirror(b)` resolution order, per bone of the supplied skeleton:
  1. **Exact-name side-token swap** — complete `Left`/`Right`, `left`/`right`,
     and `LEFT`/`RIGHT` words are swapped at delimiter or camel-case
     boundaries and looked up in the skeleton (handles `LeftForeArm` ↔
     `RightForeArm` and Mixamo/Biped-style conventions without interpreting
     substrings such as `Bright` or `Leftover`). Exact names are not capped by
     the stack scratch size; dynamic capacity is checked and truncation is
     never used for lookup.
  2. **Humanoid-role swap** — the bone's cached role with side 1↔2 flipped,
     matched against the other bones' roles (covers fused/abbreviated side
     spellings the tokenizer recognizes).
  3. **Self** — side-less (center) bones mirror onto themselves.
- Collisions are structurally impossible: output channels are constructed
  per **output** bone, each sourcing from its own resolved partner. A
  duplicate-role source whose partner is claimed by an exact-name match
  contributes nothing (its output bone samples its own partner instead) —
  degenerate rigs lose the duplicate rather than failing the whole clip.

## Consequences

- One right-handed clip set serves both handednesses at load time
  (milliseconds per clip), removing the DCC mirror batch and the doubled
  bake payload.
- Skinned correctness holds for any bind pose; the stated assumption is
  reduced to bilateral symmetry of the character mesh / model-space joint
  positions (the T-pose contract every humanoid asset already meets).
- Output keys are linear on a ~30 Hz-densified union grid: cubic tangents and
  original per-channel key layouts are not preserved. Unanimated rigid riders
  are compacted away; every animated subtree gains keys for coupled bones.
- Root-motion X travel is negated by the same rule, which is the desired
  behavior (a rightward drift mirrors to leftward). `StripRootMotion`
  composes before or after Mirror identically.
- Registry metadata is unchanged by the amendment (same method, same
  signature); no manifest re-pin.

## Tests

`src/tests/unit/test_rt_skeleton3d.cpp`:

- Left/right swap with reflected model-space bone matrices through the real
  player on a symmetric rig; center bone maps to itself; Mirror∘Mirror
  round-trips model-space results within float tolerance.
- **Asymmetric-bind regression** (the auto-rig pattern: ~90°-rotated root
  frame, left–right on local Y, laterally offset center bone): mirrored joint
  positions equal the X-reflection of the partner's source positions at every
  sampled time — including keyless bones riding animated parents; round-trip
  restores every joint; hand-computed endpoint pins; hemisphere-continuity
  probe across resampled keys.
- Degenerate inputs (`NULL` clip, `NULL`/empty skeleton) return `NULL`.
- Complete-token boundaries, uppercase tokens, exact names longer than the
  stack scratch buffer; a duplicate-role source whose partner is exact-name
  claimed is dropped rather than kept unmirrored.

`src/tests/unit/test_graphics3d_runtime_manifest.cpp`: manifest pin from the
original landing is unchanged by the amendment.
