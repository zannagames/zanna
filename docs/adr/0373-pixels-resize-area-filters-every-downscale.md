---
status: accepted
audience: contributors
last-verified: 2026-09-18
---

# ADR 0373: Pixels.Resize Area-Filters Every Downscale

## Status

Accepted (2026-09-18). Changes the documented behaviour of
`Zanna.Graphics.Pixels.Resize` for shrinking axes. The C ABI, signature, and
growing-axis behaviour are unchanged.

## Context

`rt_pixels_resize` area-averaged an axis only when it shrank by more than 2:1
(`pixels_is_heavy_downscale`). Every milder shrink, including exactly 2:1, used
endpoint-aligned bilinear interpolation, which at 2:1 reads almost exactly
every other source pixel: fine detail was skipped rather than filtered. The
documentation promised an "endpoint-preserving" resize and
`test_resize_preserves_source_endpoints` pinned it (4 → 2 kept the first and
last pixels). The heavy path itself used whole-pixel boxes
(`pixels_map_boundary`), so at non-integer ratios some outputs averaged one
more source pixel than others.

Real callers shrink by exactly 2:1. Legacy Baseball renders portraits at
twice their size (`PS_SS = 2`) and shrinks them, so the supersampling mostly
went to waste; Cat 'n' Mouse shrinks its 128 px crate skins and header badge to
64. The packager's icon resampler moved to an exact-coverage area filter in
[ADR 0372](0372-package-icon-sources-and-dmg-volume-icons.md) for the same
reason.

## Decision

- Every axis whose destination is smaller than its source is area-filtered with
  exact integer coverage weights: in units of 1/destination, source pixel *s*
  spans [*s*·D, (*s*+1)·D) and output pixel *x* spans [*x*·S, (*x*+1)·S), so
  each weight is an integer overlap and the weights of one output sum to S.
- Colour is the alpha-weighted mean and alpha the plain mean of the covered
  pixels, both rounded, as before; a fully transparent footprint stays
  transparent black.
- A growing axis keeps endpoint-aligned, premultiplied bilinear interpolation,
  and a shrinking axis is still processed before a growing one.
- The accumulator bound becomes `Sx·Sy·65025 < 2^64` (taking a factor of 1 for
  an unchanged axis); a larger footprint traps with
  `Pixels.Resize: source footprint too large`, as before.
- All arithmetic stays integer, so the VM and native builds, which share this
  runtime code, produce identical pixels on every host.

## Consequences

- Shrinks up to 2:1 now average instead of decimating; shrinks at non-integer
  ratios above 2:1 change slightly (fractional coverage instead of uneven
  whole-pixel boxes). Integer ratios above 2:1 (for example 4096 → 512) are
  byte-identical to before.
- `test_resize_preserves_source_endpoints` became
  `test_resize_two_to_one_area_averages` (4 → 2 gives 45 and 205);
  `test_resize_odd_ratio_uses_rounded_area_filter` was re-pinned to exact
  coverage (5 → 2 gives 40 and 1); `test_resize_mild_shrink_uses_coverage` and
  `test_resize_checkerboard_two_to_one_is_gray` were added.
- `docs/zannalib/graphics/pixels.md` describes the new contract.
- Legacy Baseball bumps its portrait cache version so cached portraits
  re-render with the averaged downscale.
