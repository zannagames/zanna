---
status: draft
audience: contributors
last-verified: 2026-08-03
---

# ADR 0239: VM Callback Policy for the 3D Surface

## Status

Proposed (decision recorded; implementation is its own program)

## Context

The entire 3D surface is polling-only: no `OnCollision`, no scriptable
camera controllers, no animation-event callbacks into Zia. The runtime's C
ABI supports native function pointers (`drawOverlay`), but Zia functions
are VM closures — invoking one from inside a runtime C call requires a
re-entrant VM trampoline. The docs honestly call this deferred; the review
asked for an explicit decision.

## Decision

**Adopt the event-queue model as the permanent public contract; build the
VM trampoline only as an internal dispatch aid, if ever.**

1. Gameplay reactions stay pull-based and framed: the runtime records
   typed events (collision enter/stay/exit, trigger, animation markers,
   agent arrivals, timeline cues) into bounded per-world queues that Zia
   drains once per tick (`GetEnterEvent(i)` et al. — the pattern collision
   events already use). This preserves VM/native determinism (events are
   observed at a defined point in the frame, in a defined order) and keeps
   the C runtime free of re-entrancy.
2. What look like "callbacks" in other engines become **registered
   behaviors**: named runtime-side policies (`Behavior3D` presets, camera
   controllers, `SyncMode`) selected from Zia, executed natively. New
   hooks grow the preset vocabulary rather than opening arbitrary
   re-entry.
3. A general "call Zia from C mid-simulation" trampoline is rejected as a
   public contract: it would make simulation order observable and
   divergent between VM and native builds, break the batch determinism
   work (e.g., NavAgent batch updates), and turn every runtime call site
   into a re-entrancy hazard.

## Consequences

- The near-term work is coverage, not architecture: extend the event
  queues to animation markers and agent arrivals, and document the
  drain-per-tick idiom as the canonical pattern.
- GameBase3D wraps queue drains into `onCollision(handler)`-style Zia-side
  dispatch so the ergonomics match callback engines without runtime
  re-entrancy.
