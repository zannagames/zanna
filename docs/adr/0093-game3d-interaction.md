---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0093: Focus-and-Use Interaction (Interactable3D + Interactor3D)

Date: 2026-07-11

## Status

Accepted

## Context

Every adventure verb — doors, chests, levers, NPC talk — re-implements the
same loop: find the best target in front of the player, show a prompt, fire
on use. The runtime had trigger zones and raycasts but no candidate
selection.

## Decision

- **`Interactable3D.New(entity)`** installs on the entity component slot with
  fluent `WithPrompt`/`WithKind`/`WithRadius` plus `Enabled` and
  `FocusPriority`.
- **`Interactor3D.New(entity)`** installs on the player entity and scans in
  the world step (same component-tick pass as footsteps): candidates come
  from the world entity list (deterministic order, stale entities fail
  closed), filtered by per-interactable radius, the owner-forward view cone
  (`ConeDegrees`, default 70), and an optional line-of-sight raycast
  (`RequireLineOfSight`/`LosMask`; a hit on any body other than the target
  blocks). Score = distance term + 0.5 x alignment term + priority, with a
  10% hysteresis bonus for the current focus so ties never flicker.
- **Polled state:** `Focused` (retained), `FocusChanged()` one-shot,
  `Interact()` (true when focused; bumps `InteractCount` and latches
  `LastInteracted` for game dispatch by `Kind`).

## Consequences

- Deferred (recorded): camera-facing mode (owner-forward only in v1), the
  emissive focus-highlight sugar, and a world-level interaction event
  buffer — `InteractCount`/`LastInteracted` polling covers the loop
  meanwhile.
- Test: `g3d_test_game3d_interact_probe` — in-cone door beats behind-cone
  chest, prompt/kind round-trip, interact fires, disabling the target drops
  focus with the one-shot raised.
