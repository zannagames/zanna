---
status: active
audience: contributors
last-verified: 2026-08-08
---

# ADR 0243: Animation3D.Mirror (Left/Right Clip Mirroring)

## Status

Accepted (2026-08-08)

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
  `T' = M·T·M` with `M = diag(-1, 1, 1)`. For a local TRS this maps
  translation `(x, y, z) → (-x, y, z)` and rotation quaternion
  `(x, y, z, w) → (x, -y, -z, w)`, leaving scale untouched. By induction from
  the root, applying the conjugation to every **local** transform while
  swapping each Left bone's channel onto its Right counterpart (and vice
  versa) yields exactly the reflected model-space pose — no global pose
  composition is required.
- Skinning the reflected pose reads correctly when the skeleton's bind pose is
  bilaterally symmetric (mirror bones' bind globals are reflections of each
  other) — true of humanoid auto-rigs and required by any mirroring scheme.

## Decision

Add one operation to the frontend-visible Animation3D surface (hence this
ADR):

- `Zanna.Graphics3D.Animation3D.Mirror(skeleton)` →
  `void *rt_animation3d_mirror(void *animation, void *skeleton)`

Semantics:

- Returns a **new** clip (name suffixed `"_mirror"`, duration/looping/key
  times preserved); the source is never modified. `NULL` for invalid input.
- Per source channel targeting bone `b`, the output channel targets
  `mirror(b)` and every keyframe (and its cubic tangents) is conjugated:
  position X negated, quaternion Y/Z negated, scale copied.
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
- Collisions are defensive without silently losing animation: if an inferred
  role targets an already claimed output, the channel remains on its own
  still-free source bone. If both targets are already claimed, the entire
  mirror operation fails closed and releases the partial result (never two
  channels driving one bone and never a silently incomplete clip).

## Consequences

- One right-handed clip set serves both handednesses at load time
  (milliseconds per clip), removing the DCC mirror batch and the doubled
  bake payload.
- The operation is exact for the animation itself on any rig; visual
  correctness of the *skinned* result requires a bilaterally symmetric bind
  pose, which is stated in the API documentation.
- Root-motion X travel is negated by the same rule, which is the desired
  behavior (a rightward drift mirrors to leftward). `StripRootMotion`
  composes before or after Mirror identically.
- Registry metadata grows by one method; the runtime API manifest is
  re-pinned in the same change.

## Tests

`src/tests/unit/test_rt_skeleton3d.cpp`:

- Left/right channel swap on a named L/R pair with X-negated positions and
  Y/Z-negated quaternions; center bone maps to itself.
- Mirror∘Mirror round-trips the original keys within float tolerance.
- Degenerate inputs (`NULL` clip, `NULL`/empty skeleton) return `NULL`.
- Cubic tangents conjugate with their lanes.
- Complete-token boundaries, uppercase tokens, exact names longer than the
  stack scratch buffer, and inferred-role collisions preserve all channels.

`src/tests/unit/test_graphics3d_runtime_manifest.cpp`: manifest re-pin
covering the new method.
