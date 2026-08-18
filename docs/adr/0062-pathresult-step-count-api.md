---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0062: PathResult StepCount API

## Status

Superseded — the public-surface standardization retired the `PathResult.Length`
compatibility alias from the registry; `PathResult.StepCount` is now the sole
public property for the cell-to-cell step count. The backing C symbol
`rt_path_result_length` is retained as an internal (non-registered) ABI alias of
`rt_path_result_step_count`, so native/embedding callers keep working, but the
scripting surface no longer exposes `Length`. New code must use `StepCount`
(VDOC-260).

## Context

`Zanna.Game.PathResult.Length` was added as part of the game snapshot result
work to expose the number of cell-to-cell steps in a path. The value is useful,
but the name is ambiguous beside `PathResult.Path.Length`, weighted path `Cost`,
and collection/string `Length` conventions.

## Decision

Add `Zanna.Game.PathResult.StepCount` as the canonical property for the
cell-to-cell step count. Keep `PathResult.Length` registered as a compatibility
alias, with runtime API metadata pointing callers to `PathResult.StepCount`.

## Consequences

- New pathfinding code can read `StepCount` without confusing it for waypoint
  list length or weighted path cost.
- Existing `Length` users remain source-compatible.
- Runtime docs, examples, and API metadata should teach `StepCount` first and
  describe `Length` as a legacy alias.
