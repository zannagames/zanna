---
status: active
audience: contributors
last-verified: 2026-08-15
---

# Memory, Threads, and Core Runtime Audit (August 2026)

This ledger records ten correctness defects found and fixed in the runtime C
implementations behind the `Zanna.Memory.*`, `Zanna.Threads.*`, and
`Zanna.Core.*` surfaces. Older code and issue reports may call these the
`Viper.*` surfaces; the public namespace is now `Zanna.*`.

No public runtime C ABI signature, IL opcode, grammar, verifier rule, or
cross-layer dependency changed in this work, so no ADR was required.

## Resolved findings

| ID | Surface | Defect | Resolution and regression coverage |
|---|---|---|---|
| MTC-001 | `Zanna.Memory.WeakRef` | A registered payload with a forged or unsupported heap-kind tag could be installed as a weak target. | Weak targets now admit only object and array payload kinds; `RTGCTests` covers rejection. |
| MTC-002 | `Zanna.Memory.WeakRef` | An object or array payload whose reference count was already zero could be installed as a supposedly live weak target. | Heap targets must now have a nonzero reference count; `RTGCTests` covers the zero-count boundary. |
| MTC-003 | `Zanna.Memory.WeakRef` | A registered string handle whose inline or heap reference count was zero could be installed as a weak target. | String target validation now checks the applicable live reference count; `RTGCTests` covers the inline-string case. |
| MTC-004 | `Zanna.Core.Box` | The default mixed integer/floating comparator converted `i64` values to `double`, collapsing distinct integers above `2^53` and violating its total-order contract. | Mixed comparison now preserves the exact signed integer and handles the `i64` range boundaries before conversion; `RTSeqBoxTests` covers precision and antisymmetry. |
| MTC-005 | `Zanna.Core.Diagnostics` | With a returning trap hook, an invalid assertion-message handle raised its validation trap and then fell through to raise a second fallback assertion trap. | Message formatting now reports invalidity separately and every assertion returns after that trap; `RTDiagTests` verifies exactly one trap. |
| MTC-006 | `Zanna.Core.MessageBus` | Receiver-retain overflow was ignored when the trap hook returned, allowing the operation to continue and later release a reference it never acquired. | Receiver validation uses the non-trapping live-retain primitive and stops after reporting overflow or a dead receiver; `RTMsgBusTests` verifies no refcount change. |
| MTC-007 | `Zanna.Threads.Channel` | Public operations could continue after receiver-retain overflow and decrement the channel's saturated reference count on exit. | All receiver-retaining Channel entry points use a checked retain helper; `RTChannelTests` covers returning-trap behavior. |
| MTC-008 | `Zanna.Threads.ConcurrentQueue` | Public operations could continue after receiver-retain overflow and perform an unmatched receiver release. | All receiver-retaining ConcurrentQueue entry points use a checked retain helper; `RTConcQueueTests` covers returning-trap behavior. |
| MTC-009 | `Zanna.Threads.ConcurrentMap` | Public operations could continue after receiver-retain overflow and perform an unmatched receiver release. | All receiver-retaining ConcurrentMap entry points use a checked retain helper; `RTConcMapTests` covers returning-trap behavior. |
| MTC-010 | `Zanna.Threads.Scheduler` | Public operations could continue after receiver-retain overflow and perform an unmatched receiver release. | All receiver-retaining Scheduler entry points use a checked retain helper; `RTSchedulerTests` covers returning-trap behavior. |

## Validation

The regression cases intentionally use both non-local trap recovery and trap
hooks that return. The latter is essential: an aborting or `longjmp`-based hook
cannot observe erroneous fallthrough after a failed retain. The normal Windows
build script, targeted runtime CTests, formatting, and platform-policy lint are
the validation gates for this ledger.
