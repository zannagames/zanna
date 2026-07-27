---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0048: Runtime Unsafe Trap-State Hooks

## Status

Accepted

## Context

`Zanna.Diagnostics.CurrentTrap` is the application-facing way to read trap
metadata, but the runtime still exposes mutable trap-state hooks under
`Zanna.Error`. Those hooks are needed by generated code, runtime bridges, and
low-level tests, but names such as `Zanna.Error.SetTrapFields` make them look
like ordinary application APIs.

The overhaul policy keeps powerful low-level features available while naming
them according to their risk.

## Decision

Expose canonical unsafe names:

- `Zanna.Runtime.Unsafe.SetThrowMsg(message)`
- `Zanna.Runtime.Unsafe.ClearThrowMsg()`
- `Zanna.Runtime.Unsafe.SetTrapFields(kind, code, line)`
- `Zanna.Runtime.Unsafe.RaiseKind(kind, code, line)`

The existing `Zanna.Error.SetThrowMsg`, `ClearThrowMsg`, `SetTrapFields`, and
`RaiseKind` rows remain available for compatibility and carry migration targets
to the unsafe namespace.

Read-only trap inspection remains under `Zanna.Diagnostics.CurrentTrap()`.
Existing `Zanna.Error.GetThrowMsg` and `GetTrap*` getters continue to point
callers toward `CurrentTrap` in runtime API metadata.

## Consequences

- Generated code and low-level tests keep the full trap-state feature set.
- Application docs can present diagnostics as read-only by default.
- API tooling can flag direct trap-state mutation as unsafe and suggest the
  canonical namespace without breaking existing source.
