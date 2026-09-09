---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0346: Preserve authored foot orientation during ground following

## Context

ADR 0286 composes the ground tilt onto the post-position-solve end rotation.
Its zero-displacement tests preserve a standing foot, but moving an ankle
also rotates its parent shin and therefore the shoe. A full bag-height
correction in Baseball visibly pitches the toes upward on flat ground.

## Decision

For a ground-normal hint, snapshot the incoming end-bone model rotation
before the positional chain solve. Apply the shortest-arc model-up-to-normal
delta, blended from identity by solver weight, to that snapshot. Install the
result in the solved parent's local frame, preserving the solved endpoint
translation and scale. Thus flat ground retains the incoming authored shoe
orientation while the knee bends; slopes add only their weighted tilt.

This supersedes ADR 0286's post-solve rotation basis for ground hints.
Explicit target rotation retains its existing post-solve weighted goal and
precedence. No hint retains the ordinary positional chain behavior. Zero
weight remains a complete no-op. Two-bone and FABRIK share this policy.
No public API, ABI, allocation, dependency or platform-specific code changes.

## Validation

Use a rotated-parent skeleton with a nonzero ankle displacement. Test flat
and sloped normals at zero, partial and full weights in both chain types;
assert authored axes plus weighted terrain tilt and the positional endpoint.
Retain explicit-goal precedence and other animation/IK tests. Build and test
through the platform script; review native Baseball stance and re-run foot,
field-style and possession gates after rebuilding its executables.
