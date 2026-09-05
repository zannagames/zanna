---
status: active
audience: contributors
last-verified: 2026-09-05
---

# ADR 0330: Observe Scene Node Count on Demand

## Status

Accepted for implementation; validation results are recorded separately.

## Summary and scope

Avoid an unrelated whole-scene count traversal after each `SceneGraph.Add`,
`TryAdd`, or `Remove`. A native stadium startup profile attributes 1,199 of
4,269 main-thread samples to scene attachment, primarily to that recount.
Repeated independent insertion makes this extra work quadratic in scene size.

This changes no runtime C ABI, scripting property, IL rule, serialized format,
backend capability, hierarchy ownership, or dependency. World stable-ID
preflight and renderer batching are explicitly outside this increment.

## Decision

- The private `rt_scene3d.node_count` is the last observed count, not a maintained
  hierarchy invariant. This was already true after direct nested node edits.
- Remove its eager refresh from successful top-level insertion and removal.
  Do not replace it with an approximate increment, which would be wrong for
  subtrees, in-scene reparenting, duplicate insertion, and nested removal.
- Keep `rt_scene3d_get_node_count`'s existing fresh, checked traversal. Creation,
  clear, and loading retain their existing initialization/validation behavior.
- Keep all parenting validation, owner transactions, spatial invalidation,
  scene-root protection, and error handling unchanged. Invalid handles remain
  a rejected `TryAdd`/no-op `Remove`; this change adds no diagnostic or trap.
- No feature toggle or configuration is required: no public observable result
  depends on the removed intermediate count. No allocation is added.
- Performance contract: Add/TryAdd/Remove must not invoke the unrelated full
  count walk. Public count queries remain linear and exact. This does not claim
  all hierarchy operations or world spawning are constant time.

## Tests

Given repeated independent insertions, the last observed private count must
remain unchanged until queried, while root ownership and public counts stay
correct. Cover a multi-node subtree, duplicate add, promotion of a nested child
to the root, nested removal, an unrelated scene's node, and invalid input.
Keep the existing root-transfer, corrupted-input, nested-count, load/save,
query, and world spawn/rollback tests. Retain a failing pre-change regression,
then run the canonical build and scene/Game3D suites plus native startup timing.

## References and alternatives

`rt_scene3d_api.inc` already refreshes the count in its getter;
`test_node_count_tracks_nested_hierarchy_edits` already requires that freshness.
Scene clear and VSCN loading are examples of deliberate whole-scene boundaries.
Maintaining a second incremental subtree-count system would add invalidation
and rollback obligations without removing the getter's corruption-safe walk.
Using render-only chairs instead would change gameplay/query ownership and
would leave this generally applicable redundant traversal in place.
