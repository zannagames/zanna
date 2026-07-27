---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0044: Crypto Module Process Policy API

## Status

Accepted

## Context

`Zanna.Crypto.Module.EnableApprovedMode()` and
`DisableApprovedMode()` mutate process-wide crypto module policy. The old names
were short and accurate enough for early tests, but they did not make the global
scope obvious at call sites or in generated API listings.

The runtime must preserve existing calls while making policy-changing operations
stand out as advanced process configuration.

## Decision

Add process-scoped names:

- `Zanna.Crypto.Module.EnableApprovedModeForProcess()`
- `Zanna.Crypto.Module.DisableApprovedModeForProcess()`
- `Zanna.Crypto.Module.IsApprovedModeForProcess()`

The old `EnableApprovedMode`, `DisableApprovedMode`, and `IsApprovedMode` names
remain available as compatibility rows with migration targets. The mutating
enable/disable rows are classified as `unsafe` in runtime API metadata because
they change global process policy and affect which crypto services are allowed.

## Consequences

- New code shows that approved-mode policy is process-wide.
- Existing code keeps compiling and running.
- API consumers can flag policy-changing crypto toggles during audits.
- Documentation can teach approved mode as deployment/process configuration
  instead of a casual per-operation option.
