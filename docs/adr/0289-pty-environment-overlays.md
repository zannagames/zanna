---
status: active
audience: contributors
last-verified: 2026-08-21
---

# ADR 0289: Add PTY Environment Overlays

## Status

Accepted

## Context

The PTY API accepted only a complete replacement environment. Zanna Studio
therefore had to choose between inheriting essential variables such as `PATH`,
`HOME`, locale, and shell configuration, or supplying terminal capability
variables such as `TERM` and `COLORTERM`. Process already distinguishes complete
replacement from native environment overlay, but PTY did not.

Adding PTY factory methods changes the runtime C ABI and registry surface.

## Decision

Add `Zanna.System.Pty.OpenWithEnvOverlay` and
`OpenWithEnvOverlayResult`, backed by
`rt_pty_open_with_env_overlay` and its Result-returning counterpart. Overrides
are validated as `NAME=value`, replace inherited names case-insensitively on
Windows and case-sensitively on POSIX, and preserve every other inherited
entry. Existing `Open` and `OpenResult` retain complete-replacement semantics.

Studio opens integrated terminals with `TERM=xterm-256color` and
`COLORTERM=truecolor` through the overlay API.

## Consequences

- Interactive children retain executable lookup, home, locale, and user shell
  variables while receiving deterministic terminal capabilities.
- Existing callers that intentionally construct a hermetic environment are not
  changed.
- The runtime maintains native name-comparison and environment-block ordering
  rules on each platform.

## Alternatives Considered

Changing `Open` to overlay implicitly would silently break callers relying on
an empty or hermetic environment. Wrapping shells with platform-specific
commands would reintroduce quoting problems and would not work uniformly on
Windows.
