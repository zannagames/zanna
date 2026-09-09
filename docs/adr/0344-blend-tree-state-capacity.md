---
status: accepted
audience: contributors
last-verified: 2026-09-09
---

# ADR 0344: Match Blender Capacity to Blend-Tree Samples

## Context

BlendTree3D has an explicit 16-sample ceiling, but its owned AnimBlend3D
accepts only eight states. A directional walk/jog/sprint tree therefore
silently loses half its requested samples, despite remaining otherwise valid.

## Decision

Raise AnimBlend3D's bounded inline state capacity from eight to sixteen.
Keep BlendTree3D at sixteen and assert at compile time that its blender can
hold every sample. Existing count repair, retained clip ownership, finalizer
and state iteration use the shared blender limit; no unbounded allocation or
new dependency is introduced. Public function signatures/registry counts
and animation formats are unchanged. This changes an internal object size;
consumers must link the matching rebuilt runtime.

## Validation

Register all sixteen tree samples, verify each is selectable, and reject
the seventeenth without changing either count. Run animation/graphics tests
and Baseball's real directional-tree phase tests in VM and native.
