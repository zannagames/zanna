---
status: accepted
audience: contributors
last-verified: 2026-09-11
---

# ADR 0350: Blend tree weight read

## Context

`AnimController3D.SetBlendTree` attaches a locomotion tree that replaces the
base layer's pose once its ADR 0302 fade completes, and `SetBlendTreeFade`
ramps that replacement in and out. A game that installs the tree on a moving
body has no way to confirm that the tree is actually driving the palette: the
controller reports `IsTransitioning` for the base layer's crossfade only, and
the ramped tree weight is private. Legacy Baseball's moving-body tripwire
(plan 113) needs to distinguish "the tree is attached" from "the tree drives
the pose", because a fade that never completes renders the layer-0 clip
under a route-anchored gait and reads as a slide.

## Decision

Add the read-only property `AnimController3D.BlendTreeWeight: Float`, runtime
function `rt_anim_controller3d_get_blend_tree_weight(void*) -> double`.

It returns 1 while an attached tree drives the palette exclusively (no fade
configured, or the fade complete), the ramped weight in (0, 1) while a fade
is in flight — including the ramp toward zero of a pending detach — and 0
with no tree attached or for an invalid handle. Non-finite stored weights
read as 1. The getter evaluates no pose, advances no clock and changes no
state. The disabled-graphics stub returns 0.

Registry delta: one function, one property. No payload, ownership, IL or
serialized-format change.

## Validation

`test_rt_animcontroller3d` reads the weight through an attach with a fade
(0 → 0.5 → 1), a pending detach (1 → 0.5 → 0, then the release), the no-tree
and invalid-handle cases; `RTGraphicsSurfaceLinkTests` links the symbol. The
Baseball rig's tripwire consumes it (a moving gait body whose tree weight
stays under 0.99 past the fade trips "tree attached but not driving").
