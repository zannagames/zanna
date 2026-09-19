---
status: accepted
audience: contributors
last-verified: 2026-09-18
---

# ADR 0375: `trap.err` and `trap.from_err` Take Runtime Error Codes

## Status

Accepted (2026-09-18). A correction of the IL specification text to match
every existing implementation; no opcode, verifier rule, backend or runtime
behaviour changes.

## Context

`docs/il/il-guide.md` described the operand of `trap.from_err` as "the given
i32 trap-kind code" and that of `trap.err` as an "i32 kind". Every
implementation treats both as a **runtime error code** — the `Err_*` numbering
of `vm/err_bridge.hpp` — and maps it to a trap kind: the IL VM through
`map_err_to_trap`, the bytecode VM through the same bridge, and native code
through the runtime's `rt_err_to_trap_kind`. The two numberings differ
(`Err_Overflow` is 4, the Overflow trap kind is 1), so code written from the
specification raises the wrong trap: `trap.from_err i32 1` raises
FileNotFound, not Overflow. The Zia frontend already emitted error codes
(`throw` uses 9, RuntimeError); the mismatch surfaced when it began emitting an
Overflow trap for a failed `as Byte` (Legacy Baseball ledger ZB-50).

## Decision

- The operand of `trap.from_err` and the first operand of `trap.err` are
  runtime error codes. The specification states so and lists the mapping to
  trap kinds; `trap.kind` continues to return the trap kind.
- The mapping is: 1 FileNotFound, 2 EOF, 3 IOError, 4 Overflow, 5 InvalidCast,
  6 DomainError, 7 Bounds, 8 InvalidOperation, 9 RuntimeError, 10–19
  NetworkError; any other code is a RuntimeError.

## Consequences

- Front ends and hand-written IL use the error-code numbering with these two
  opcodes and the trap-kind numbering with `trap.kind` and typed handlers.
- No implementation changes; existing IL keeps its meaning.
