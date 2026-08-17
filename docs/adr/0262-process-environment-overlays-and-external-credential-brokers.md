---
status: active
audience: contributors
last-verified: 2026-08-16
---

# ADR 0262: Overlay Process Environments and Delegate Credentials to External Brokers

## Status

Accepted (2026-08-16)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0016 — PTY Runtime Surface and Integrated Terminal
- ADR 0066 — Process and PTY Read Result APIs
- ADR 0260 — Ordered Nonblocking Process Streaming
- `src/runtime/system/rt_process.c` — process creation and environment blocks
- `src/zannastudio/src/scm/scm_git.zia` — Studio Git process policy

## Context

`Process.StartWithEnv` deliberately treats its map as the child's complete
environment. That replacement contract is useful for hermetic execution but is
unsafe as the default for Studio subprocesses: a short Git-specific map also
removes inherited `PATH`, locale, proxy, certificate, SSH-agent, askpass, and
credential-manager configuration.

Studio also carried an interactive PTY path that could read credential prompts
and collect secrets in application-owned controls. Besides being difficult to
make reliable across Git providers and operating systems, that makes Studio a
credential broker without the storage, redaction, and trust guarantees of the
host platform's existing helpers.

## Decision

Add the public runtime operation
`Zanna.System.Process.StartWithEnvOverlay(str, obj, str, obj)`. Its final map
overlays the inherited process environment rather than replacing it. Existing
`StartWithEnv` behavior remains unchanged for compatibility and hermetic use.

On POSIX, names use the platform's case-sensitive comparison and the child
receives inherited entries followed by explicit replacements. On Windows,
names compare case-insensitively, drive-current-directory pseudo entries are
preserved, and the resulting UTF-16 block is sorted as required by process
creation conventions. Both implementations retain all native storage through
the child-creation call and reject the same malformed names as replacement
mode.

Studio Git jobs use the overlay operation and set only deterministic,
non-secret policy such as `GIT_TERMINAL_PROMPT=0`, `GIT_MERGE_AUTOEDIT=no`, and
noninteractive pager values. Push and pull run as ordinary captured processes.
Studio does not display, retain, or forward passwords or tokens. Authentication
is delegated to configured Git credential helpers, platform credential
managers, SSH agents, and askpass brokers inherited from the host environment.

## Consequences

- Studio subprocesses retain user proxy, locale, certificate, PATH, SSH, and
  credential-helper configuration.
- Callers can choose explicitly between hermetic replacement and additive
  overlay semantics.
- Git operations fail with an actionable broker/configuration diagnostic when
  no noninteractive credential source is available; Studio never asks for the
  secret itself.
- The runtime gains an additive C ABI symbol and generated runtime member.
- Windows and POSIX overlay behavior intentionally differs only where their
  native environment-name rules differ.

## Alternatives Considered

- **Change `StartWithEnv` to merge.** Rejected because it would silently weaken
  existing hermetic callers and break a documented runtime contract.
- **Copy a hand-picked set of inherited variables in Studio.** Rejected because
  the set is open-ended and platform/tool specific.
- **Keep the Studio password prompt.** Rejected because it makes Studio handle
  secrets and bypasses established credential brokers.
- **Invoke Git through a login shell.** Rejected because quoting, startup files,
  and shell differences make execution less deterministic and less portable.
