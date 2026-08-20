---
status: active
audience: contributors
last-verified: 2026-08-20
---

# ADR 0275: Correlate and Bound Debug Adapter Queries

## Status

Accepted (2026-08-20)

## Consulted

- ADR 0006 — Spec Currency and ADR Triggers
- ADR 0009 — Debug Adapter Evaluate Protocol Extension
- ADR 0260 — Preserve Process Stream Order and Keep Windows Input Nonblocking
- `src/tools/zanna/DebugAdapter.cpp` — VM debug adapter protocol owner
- `src/zannastudio/src/build/debug_session*.zia` — Studio protocol client

## Context

The newline-JSON debug protocol identifies variable replies only by `varRef`
and evaluate replies only by expression text. Both values can repeat: variable
references are recycled at later stops, users can evaluate the same expression
as a watch and manually, and a timed-out reply can arrive after a newer request.
Studio can therefore attach a valid but stale reply to the wrong UI request.

Process input is deliberately nonblocking and may accept only a prefix. The
debug client currently ignores that byte count, so a saturated adapter pipe can
receive truncated JSON. Variable queries have a timeout, but watch/manual
evaluation can remain pending forever. The partial stderr control-line carry is
also unbounded.

## Decision

Every `evaluate` and `variables` command carries a positive, session-local,
monotonic integer `requestId`. The corresponding `evaluated` or `variables`
event echoes that exact value. Studio accepts a query response only when its ID
matches the currently outstanding owner; expression text and `varRef` remain
payload fields, not correlation keys. A missing, unknown, expired, or superseded
ID is ignored.

Studio queues complete newline-delimited commands behind a bounded one-megabyte
input carry and retries the unwritten UTF-8 suffix on later frame pumps. State
transitions that depend on a command occur only after the complete command is
accepted into this queue. Variables, watches, and manual evaluations have
bounded deadlines; timeout clears ownership and publishes an unavailable state
instead of wedging future requests. Debug control-line carry is capped at one
megabyte, and an oversized unterminated line is discarded with a visible
diagnostic.

The synchronous `FetchChildren` compatibility loop is removed. UI and probes
use the existing asynchronous request plus `Poll` contract.

## Consequences

- Late responses cannot populate a later stop, tree row, watch, or manual
  evaluation merely because their `varRef` or expression matches.
- Partial process writes no longer corrupt the adapter command stream.
- A nonresponsive adapter cannot leave Variables or watch evaluation permanently
  busy, and malformed output cannot grow Studio memory without bound.
- The internal Studio-to-adapter JSON protocol gains required correlation fields;
  it does not alter the runtime C ABI, IL, verifier, or VM execution semantics.

## Alternatives Considered

- **Continue matching by payload.** Rejected because payload identity is not
  request identity and is explicitly reused.
- **Allow one global query at a time.** Rejected because manual evaluate and
  variable/watch refresh are independent UI operations and global serialization
  still needs stale-response correlation after a timeout.
- **Block until `WriteStdin` accepts the command.** Rejected because adapter
  backpressure must never freeze the Studio frame loop.
- **Keep `FetchChildren` for headless callers.** Rejected because a public
  synchronous helper invites accidental UI-thread use and duplicates timeout
  policy.

