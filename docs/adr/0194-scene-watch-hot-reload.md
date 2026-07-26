---
status: active
audience: contributors
last-verified: 2026-07-26
---

# ADR 0194: The --scene-watch Hot-Reload Contract

## Status

Accepted (2026-07-26)

## Context

Studio's Run Scene command launches the owning project with
`-- --scene <path>` (ADR 0181), and the project's entry point decides
what the scene path means. Iterating on a scene today means stopping and
relaunching the game per edit. The fast play loop (program decision #3)
wants saves to reach a running game without process churn — and without
promising live state migration the engine cannot honestly keep.

The runtime already has everything a watching game needs:
`Zanna.IO.File.Modified(path)` returns a file's modification stamp and
`Zanna.IO.Watcher` provides event-driven watching. What is missing is a
shared convention, which this ADR fixes so Studio, templates, and games
agree on behavior.

## Decision

`--scene-watch` is an **opt-in convention argument**, passed by games and
honored by their own entry points; the engine does not intercept it.

- A game that accepts `--scene-watch` re-runs its own scene-load path
  (the same code that consumed `--scene` at startup) whenever the scene
  file's modification stamp changes. Polling `File.Modified` at a bounded
  interval (>= 250 ms) or using `Watcher` are both conforming.
- A reload is a **fresh load, not a migration**: the game re-enters its
  scene-load path and rebuilds whatever that path builds. Any state the
  load path does not restore is expected to reset. Games wanting to keep
  state across reloads own that logic entirely.
- A failed reload (unreadable or invalid file mid-save) must leave the
  previous scene running and may retry on the next change; conforming
  games never crash on a torn write. Studio saves scenes atomically
  (write-then-replace), which keeps torn reads rare but not impossible.
- Studio's contribution is the save itself plus visibility: saving a
  scene while a Run Scene job is alive reports that a watching game will
  pick the change up. Studio does not signal the process directly — the
  file is the channel.

## Consequences

- No engine or ABI changes: the contract composes existing runtime
  surface, so headless tests can pin it with a fixture script that
  watches, counts reloads, and exits.
- Games that ignore the argument behave exactly as before.
- Live-state preservation is explicitly out of scope; recording that
  here prevents the editor from ever implying otherwise.
