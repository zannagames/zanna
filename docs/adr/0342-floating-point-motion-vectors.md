---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0342: Floating-Point Motion Vectors

## Context

The native Baseball pan fixture exposes misplaced temporal history and lost
chalk contrast. Offset velocity in an 8-bit UNORM attachment cannot represent
zero: 128/255 decodes to 1/255 UV, approximately five pixels at width 1280.
The quantization step is approximately ten pixels. This defeats subpixel TAA.

## Decision

Metal and D3D11 motion attachments use RGBA16F. RG stores signed UV velocity
(current minus previous), clamped to [-1,1], with exact zero as the clear.
TAA and motion blur consume signed RG directly. Attachment B retains ADR
0341's validity/weight encoding, and A retains its SSR mask. All mesh,
instanced, unlit, and target pipeline variants use matching formats.

OpenGL uses the same format and encoding on its float scene route. Its
existing non-float/LDR fallback retains offset UNORM encoding; explicit
uniforms select encoding and decoding together, including compact unlit
and TAA shaders. The existing HDR/TAA capability probe remains authoritative.
Allocation failure must retain the previous target until replacement succeeds.

Render-target reservations include native motion storage where allocated:
25 bytes per LDR texel and 45 per HDR texel, including the software weight
plane from ADR 0341. This conservatively covers Metal's target-owned motion
attachment on every platform. No public runtime ABI or dependency changes.
Jitter semantics are unchanged in this increment.

## Validation

Exercise the existing graphics label and native shader compilation. Add a
precision regression for signed binary16 zero and small positive/negative
subpixel velocities, alongside independent validity/weight/SSR checks.
Repeat the matched native pan, pitch, and HUD captures; passing arithmetic
or shader tests alone does not establish visual acceptance.
