---
status: active
audience: contributors
last-verified: 2026-08-13
---

# ADR 0246: 3D Display-Transfer Contract (Scene-Linear Targets, Single Encode)

## Status

Accepted (2026-08-13)

## Context

The 3D pipeline had no written contract for where the display transfer (the
sRGB-style gamma encode) happens. Each backend answered the question
differently, and a 2026-08-10 change (`fix(graphics): restore texture and
postfx contrast`) codified the wrong answer in two places:

- The software post-FX path gained an sRGB *decode* (`pow(v, 2.2)`) applied to
  the 8-bit scene buffer before any chain that carried a real tone curve. But
  the software scene buffer is scene-referred linear by construction: the
  rasterizer writes `clamp01(f) * 255` with no encode, and albedo textures are
  linearized at sample time. The decode therefore cancelled the tonemap's own
  `pow(1/2.2)` display encode, ran ACES on display-referred data, and crushed
  everything below ~0.06 into the toe. Measured on a mid-grey frame: byte 128
  should tonemap to ~207 and instead produced ~152.
- The Metal tonemap shader decided whether to linearize its input from the
  *pixel format* of the source texture (`sceneIsHdr` = "is the source
  RGBA16F"). The chain ping-pongs each effect through intermediate textures,
  so any effect ordered before the tonemap handed it a BGRA8 texture holding
  linear data, which the shader then wrongly linearized again. The live
  broadcast chain (bloom before tonemap) hit exactly this: mid-grey 205 via
  tonemap-only versus 154 via bloom-then-tonemap. The BGRA8 hop also clamped
  HDR values before the curve, so the ACES shoulder never saw them.

The same commit re-imaged two engine visual baselines and re-centered the
Zanna Studio gameplay-preview luma envelope from [180, 215] to [85, 145],
recording the crushed output as intended.

## Decision

1. **Scene targets are scene-referred linear on every backend.** The software
   8-bit scene buffer, the Metal RGBA16F scene target, and the D3D11/OpenGL
   equivalents all hold linear radiance. Nothing in the post-FX chain may
   decode its input as display-referred.
2. **The tone curve performs the single display encode.** A chain with a real
   tonemap (Reinhard/ACES) maps linear to display exactly once, in that pass.
   A tonemap-free chain passes bytes through unchanged (display encoding of
   raw output is a separate, pre-existing behavior outside this contract).
3. **`sceneIsHdr` is chain state, not pixel format.** A pass input is
   display-referred only after a tonemap pass has already run in the same
   chain. Backends thread this as an explicit flag through the pass loop; the
   Metal implementation passes `!tonemapped_yet`. Deriving it from the texture
   format is forbidden.
4. **GPU ping-pong intermediates carry HDR.** Metal chain intermediates are
   RGBA16F with a BGRA8 resolve only at the end of the chain, so pre-tonemap
   passes cannot clamp or quantize linear data. D3D11 already hardcodes
   `scene_is_hdr = 1` and OpenGL never linearized; both comply as-is.
5. **Baseline re-images must state measured deltas.** Any commit that
   re-images a visual baseline or moves a luma/chroma envelope must record the
   measured mean-luma/mean-chroma delta and the reason the new value is
   correct, in the probe comment or commit body. The 2026-08-10 re-centering
   contained a rationale that restated the regression as a fix; a numeric
   before/after plus the transfer-contract reference makes that class of error
   reviewable.

## Consequences

- `rt_postfx3d.c` no longer decodes chain input; the software transfer tests
  in `test_rt_postfx3d_cpu.c` pin the contract numerically (mid-grey anchor,
  saturated-patch anchor, toe monotone lift, tonemap-free passthrough).
- `test_canvas3d_postfx_parity.zia` pins the contract per backend: ctest runs
  it under the software backend and the platform GPU backend, asserting the
  same absolute ACES anchors and that bloom-before-tonemap matches
  tonemap-only. The pre-fix binary fails both (152 anchor on software;
  205-vs-154 ordering divergence on Metal).
- The Studio gameplay-preview envelope returns to [180, 215] (measured 191.8
  on the restored transfer). The `walk_min` / `openworld_slice` baselines are
  re-imaged on the restored transfer, with measured deltas per rule 5:
  `walk_min_software.png` mean luma 37 → 77 (mean chroma 21 → 20) and
  `openworld_slice_software.png` mean luma 58 → 85 (mean chroma 45 → 45) —
  reversing the crushed 2026-08-10 re-images of the same scenes.
- Effects ordered before the tonemap now read/write RGBA16F on Metal; the
  final chain output resolves to BGRA8 for present/readback.
- No IL opcode, runtime registry method, language grammar, or serialized
  format changes. Rebuild required; no data migration.

## Links

- `src/runtime/graphics/3d/render/rt_postfx3d.c` — software chain entry
- `src/runtime/graphics/3d/backend/vgfx3d_backend_metal_draw.inc` — chain-state
  threading and HDR ping-pong resolve
- `src/tests/unit/test_rt_postfx3d_cpu.c` — CPU transfer anchors
- `src/tests/fixtures/runtime/test_canvas3d_postfx_parity.zia` — per-backend
  anchors and ordering parity
- ADR 0245 — post-FX chain storage ownership
