---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0097: Streamed-World Entity-State Persistence + Save Slots

Date: 2026-07-11

## Status

Accepted

## Context

An open world must remember itself — doors opened, pickups taken, enemies
dead — across cell unload/reload and across save files. Today a cell reload
re-instantiates the authored scene and every runtime mutation is lost, and
`Zanna.IO.SaveData` only covers scalar JSON progress, not world-shaped
state.

## Decision

- **Opt-in records:** `Entity3D.SetPersistent(key)` (fluent) registers a
  game-stable string key — duplicate live keys trap immediately, the
  plan-17 identity lesson. `Entity3D.StateTag` is a free-form persisted
  `i64`. The world owns a `key → {alive, position, rotation, scale, tag}`
  delta store: resident persistent entities refresh their record each step,
  `World3D.Despawn` marks the record dead with its final pose, and
  `World3D.GetPersistentAlive/GetPersistentPosition(key)` let spawn logic
  consult the store before re-creating anything.
- **Cell flags:** `WorldStream3D.SetCellFlag(cell, key, value)` /
  `GetCellFlag` hold coarse per-cell integers (door-opened, chest-looted)
  for authored content that is not entity-granular, and the stream buffers
  just-loaded cell names (`LoadedCellEventCount/LoadedCellEvent/
  ClearLoadedCellEvents`, pushed at the cell commit site) so games re-apply
  flags exactly when a cell arrives.
- **Snapshot:** `World3D.SaveState(appName, slot)` / `LoadState(appName,
  slot)` serialize the store + flags + world extras (floating-origin
  `worldOrigin`, elapsed) as `VW3DSAV1` — explicit little-endian, versioned,
  every count and key length validated — to
  `<per-user data dir>/<appName>/<slot>.vw3dsav` via the SaveData platform
  path helper, written atomically (temp file + rename). Corrupt or missing
  files return false with state untouched; the reader **never traps**.
  `LoadState` re-poses/kills resident keyed entities immediately; loaded
  records for entities not yet spawned apply when `SetPersistent` binds
  their key (streamed content re-registering on load).
- **Fuzz surface:** the validator is exposed as
  `rt_game3d_persistence_validate(bytes, size)` (internal-symbol policy
  entry) with a libFuzzer harness `fuzz_vw3dsav_reader` following the glTF
  loader harness pattern.
- API divergence from the plan sketch, recorded: `SaveState(appName, slot)`
  takes the app name explicitly (worlds don't carry a stable app identity;
  hidden globals would be worse), and cell-flag/loaded-event storage lives
  on the stream with world-side serialization.

## Consequences

- Deferred (recorded): engine-owned respawn suppression in template-spawn
  helpers (games check `GetPersistentAlive` before spawning — documented
  recipe), record rotation/scale application on load (captured and stored;
  position + tag apply in v1), and a streamed-cell end-to-end loaded-event
  probe (the event push rides the openworld_slice manifests; the probe
  asserts the polling surface).
- Physics velocities are not captured — a loaded save resumes at rest
  (genre convention, documented).
- Test: `g3d_test_game3d_persistence_probe` — per-step capture of a moved
  crate, despawn kills the record, cell flags round-trip, save → mutate →
  load restores pose/tag/flags, missing and corrupt slots load false with
  state untouched. VM == native.

## Links

- misc/plans/thirdpersonupgrade/17-world-persistence.md
- src/runtime/graphics/3d/rt_game3d_persistence.c
- docs/zannalib/game/persistence.md, ADR 0083 (async streaming)
