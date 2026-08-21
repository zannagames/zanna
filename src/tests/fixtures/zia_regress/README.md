# Zanna issues observed during the Neon Lanes upgrade

This log records engine/runtime behavior found while upgrading the 3D Bowling
demo. It intentionally does **not** change Zanna, rebuild Zanna, or run CTest.
Each confirmed item has a small Zia reproduction that runs against the existing
binary.

Environment used for the observations:

- Zanna `v0.3.0` (`0.2.7-dev-60-gef74b30b1-dirty`)
- IL `0.3.0`
- macOS `26.5.2`, Apple Silicon (`arm64`)
- Canvas3D backend: Metal

## NL-ZANNA-001 — Canvas3D overlay primitive opacity is ignored on Metal

Status: **fixed (2026-07-29)** — screen-space overlay geometry now queues
with vertex alpha as its single opacity source and classifies translucent
draws as BLEND (`rt_canvas3d_draw.inc`), which repairs both the opaque
composite on GPU backends and a double-darkening of in-frame overlay alpha
on every backend. Registered as CTest `zia_regress_overlay_alpha_blend`.

Original report:

Reproduction: [overlay_alpha_repro.zia](overlay_alpha_repro.zia)

Run from the repository root:

```sh
./build/src/tools/zanna/zanna run \
  src/tests/fixtures/zia_regress/overlay_alpha_repro.zia
```

The probe draws an opaque `Pixels` image, then a 50%-opaque black rectangle and
rounded rectangle over it. The source pixel is `(192,128,64)`, so conventional
source-over blending should produce approximately `(96,64,32)`.

Observed output:

```text
backend=metal
base=192,128,64
alpha50=0,0,0
rounded-alpha50=0,0,0
expected-alpha50-approximately=96,64,32
```

Both `DrawRect2DAlpha(..., 0.50)` and
`DrawRoundRect2D(..., 0.50)` render fully opaque. This affects modal scrims,
selection highlights, HUD glass panels, fades, and any overlay design that
depends on partial opacity.

Temporary demo impact: Neon Lanes avoids a full-screen alpha scrim over its
title art. Panels remain readable, but their authored alpha values currently
look more opaque than intended on Metal.

## NL-ZANNA-002 — same-size DrawText2DAA textures alias on Metal

Status: **no longer reproduces (2026-07-29)** — the Metal texture-lifetime
hardening that landed with the G3D-237..350 audit tranche resolved the
aliasing; the reproduction now renders both rows correctly. Registered as
CTest `zia_regress_overlay_aa_text_identity` to guard against regression.

Original report:

Reproduction: [overlay_aa_text_repro.zia](overlay_aa_text_repro.zia)

```sh
./build/src/tools/zanna/zanna run \
  src/tests/fixtures/zia_regress/overlay_aa_text_repro.zia
```

The final frame requests green `MMMMMMMM` on the first row and blue
`ABCDEFGH` on the second row. Both strings have the same length and scale, so
their transient AA `Pixels` have identical dimensions.

Observed result: the first row contains no green pixels and instead contains
the later row's blue texture. A saved capture shows the later `ABCDEFGH`
texture in both draw locations. Representative metrics:

```text
final-frame-expected=green-MMMMMMMM-plus-blue-ABCDEFGH
top-red-pixels=0
top-green-pixels=0
top-blue-pixels=1762
bottom-blue-pixels=1906
```

This appears to be a final-overlay texture-cache identity/lifetime collision;
that cause is an inference, not yet proven. The visible contract failure is
that two independent AA text draws with equal output dimensions do not retain
their own content.

Temporary demo impact: release menus and HUDs use geometry-based
`DrawText2DScaled`, which does not create one transient texture per label.
[overlay_image_text_control.zia](overlay_image_text_control.zia) records the
control case: a large image, twelve `DrawText2DScaled` rows, and a second image
remain distinct.

## NL-ZANNA-003 — intermittent material/overlay Pixels interference

Status: **observed, not yet a stable reproduction**

Reproduction harness: [material_overlay_repro.zia](material_overlay_repro.zia)

The bowling lane initially bound the authored maple `Pixels` as a 3D albedo
map and reused the same object as a small 2D oil-map background. During one
captured run, the final frame contained only the last wood overlay image; the
textured mesh, different title-art overlay, panel, and geometry text were all
absent. The same symptom had appeared while developing the full game scene.

A second variant appeared after configuring the lane's oil sheen with the
documented `Material3D.AlphaMode = 2` blend path: one 1280x720 release capture
was covered by large black regions that replaced most HUD and scene content.
Running the same probe again immediately produced the correct frame. This
reinforces that the failure is timing/allocation-sensitive rather than a
deterministic material-configuration error.

After adding pixel-region instrumentation, twelve consecutive fresh-process
runs rendered correctly:

```text
left-3d-visible-samples=127
title-art-visible-samples=546
labels-visible-samples=453
wood-thumbnail-visible-samples=270
```

Because the failure is allocation/timing-sensitive and no longer deterministic,
this item is recorded separately from the two confirmed defects. It should not
be treated as fixed or as a reliable regression gate yet.

Temporary demo impact: the Metal path keeps the stable procedural regulation
boards and uses the separately decoded maple image only for sampled HUD colors.
Composition-safe backends may bind a second, material-only decode to the active
lane. The shipping path therefore does not depend on this defect being fixed.

## Compiler status

No Zia parser, semantic-analysis, IL-generation, or verifier defect was
confirmed during this upgrade. All compiler diagnostics encountered in the
issue probes were correct (for example, a missing `Zanna.Math` binding was
reported as an unknown `Vec3`/`Mat4` type and fixed in the probe).
