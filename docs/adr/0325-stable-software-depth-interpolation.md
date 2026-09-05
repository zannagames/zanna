---
status: active
audience: contributors
last-verified: 2026-09-04
---

# ADR 0325: Stable Software Depth Interpolation

## Status

Accepted for implementation under the owner-authorized Plan 95 renderer work.
Regression and full-suite results will be recorded after verification.

## Context

Legacy Baseball's field has continuous triangle geometry confirmed by precise
rays. Metal renders the infield continuously, but software captures show wedges
and stripes of the underlying field through it, even with flat diagnostic
materials. The view spans hundreds of feet, with NDC depths close to one.

The software rasterizer incrementally accumulates three single-precision edge
functions and computes depth as `b0*z0 + b1*z1 + b2*z2`. Rounding makes the
independently accumulated barycentric weights sum to values other than one.
The error multiplies the large common NDC depth rather than just its variation
across the triangle. Thus even a constant-depth plane can acquire a slope and
incorrectly occlude a genuinely closer surface. Long thin triangles are
particularly sensitive. The shadow rasterizer uses the same expression.

## Decision

Keep GPU-style affine interpolation of post-divide NDC depth. Evaluate the depth
plane using a vertex reference and differences, so a constant-depth triangle is
exactly constant regardless of edge-accumulator drift. Compute plane gradients
and evaluation in double precision from the projected triangle and pixel center;
store the final result in the existing float depth buffer. Evaluate each pixel
from its absolute center, not a worker-dependent accumulated depth.

Preserve constant clip coordinates during frustum clipping using an anchored
difference interpolation in double precision. Divide clip Z by W with one rounded
quotient for NDC depth; do not multiply by an already rounded reciprocal.

Use the same stable depth-plane helper for color/depth-only and shadow rendering.
Attribute interpolation remains perspective-correct, and the coverage top-left
rule, material depth bias, depth compare, normalized depth range, buffer layout,
and GPU backends remain unchanged. This is an internal numerical fix, with no
new IL opcode, runtime ABI, serialized format, or external dependency.

Degenerate/non-finite planes follow existing triangle rejection; do not invent
a finite depth for invalid geometry. The helper borrows scalar values only and
allocates no memory. Plan setup is per triangle and the pixel evaluation is a
small affine expression. Benchmark the software render path after the fix.

## Verification

- End-to-end software render of a segmented near surface over a larger far
  surface at stadium viewing distance. Compare against near-only coverage;
  neither draw order may reveal the far surface through the near surface.
- Constant-depth and sloped cases, both surface orders, serial and tiled paths,
  and shared-edge/coverage/worker determinism from existing regression fixtures.
- Re-render the field diagnostic and stadium crane; rays and visible surfaces
  must agree. Keep the separate game-side buried-layer fixes independently tested.
- Canonical parent build/tests, software shadow tests, and relevant game probes.

## Alternatives

Increasing the distance between the game's surfaces hides a renderer error and
produces floating markings at close range. Globally biasing all near-coplanar
draws makes visibility order-dependent. Merely normalizing three noisy weights
does not give a stable depth plane across different triangle tessellations.
Changing every buffer to double is a broader storage/performance change than
needed to eliminate the common-depth interpolation error.
