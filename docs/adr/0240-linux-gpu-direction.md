---
status: draft
audience: contributors
last-verified: 2026-08-03
---

# ADR 0240: Linux GPU Direction — OpenGL Parity over Vulkan

## Status

Proposed (decision recorded)

## Context

Linux's GPU story trails macOS/Windows: the OpenGL backend (12.3k LOC) has
one default-lane test, no point-shadow atlas (GAP-8), and thinner
capability coverage. The strategic question: invest OpenGL to parity, or
start a Vulkan backend.

## Decision

**Bring OpenGL to capability parity; do not start Vulkan.**

- The backend contract is the vtable capability surface (ADR era:
  `clustered_lighting`, `shadow_csm`, `BackendSupports`), and OpenGL 3.3+
  covers every capability the engine actually uses — clustered forward+,
  CSM/point shadows, HDR targets, TAA. Nothing Zanna renders needs
  Vulkan-only features.
- A Vulkan backend is a multi-quarter, ~30k-LOC undertaking (explicit
  sync, memory allocation, pipeline caches) whose payoff is performance
  headroom Zanna's scenes don't yet demand — while doubling the Linux test
  matrix and violating the "every feature complete on all platforms"
  principle for its entire buildout.
- Parity work is enumerable and small by comparison: the point-shadow
  atlas (GAP-8), capability-flag truthfulness, a default-lane test batch
  mirroring the Metal set, and perf baselines. All of it requires a Linux
  host and is queued for the next Linux session.

Revisit only if a concrete scene demonstrates an OpenGL-bound bottleneck
that clustered forward+ cannot mitigate, or if a target platform drops GL.

## Consequences

- GAP-8 and the GL test batch become ordinary backlog items, not a
  platform decision.
- Wayland/EGL work (already landed) remains the presentation layer for
  both current and any hypothetical future backend.
