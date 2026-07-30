---
status: active
audience: contributors
last-verified: 2026-07-29
---

# ADR 0228: Use One End-to-End Deadline for Each HTTP Request

## Status

Accepted (2026-07-29)

## Context

The HTTP runtime historically interpreted `HttpReq.SetTimeout`,
`HttpClient.SetTimeout`, and `RestClient.SetTimeout` as a fresh timeout for
each resolved address and each socket-I/O or TLS phase. A peer could therefore
send one byte before every socket timeout and keep a response alive
indefinitely. Redirects also restarted the complete timeout, and a host with
several resolved addresses multiplied the configured connect budget.

That behavior made the configured value unsuitable as a request resource
bound. It also diverged from the shared connect budgets already used by other
network paths.

Implementing one request deadline requires HTTP to carry the deadline into
the private TLS record engine. This is a new cross-translation-unit dependency
inside the network runtime, so it is recorded here even though no public
runtime C ABI entry point or signature changes.

## Decision

A positive HTTP timeout is one monotonic, end-to-end budget for a public
request or download. The runtime computes a saturating absolute deadline when
the operation begins and preserves it through:

- synchronous name resolution and every resolved-address connection attempt;
- connection-pool reuse and any stale-connection retry;
- the TLS handshake and all TLS record reads and writes;
- HTTP/1 request sending, response headers, and framed response bodies;
- HTTP/2 transport work;
- every redirect hop; and
- response decompression and other bounded response transformations.

Every blocking stage receives only the remaining time. Readiness waits use
rounded-up millisecond values so a positive sub-millisecond remainder is not
misinterpreted as an unbounded wait. Expiry is reported as `Err_Timeout` by
Result-returning HTTP APIs.

A timeout of zero remains an explicitly unbounded request. Existing native
socket timeouts remain a secondary I/O safeguard, but the monotonic deadline
is authoritative and is checked again between stages.

The platform `getaddrinfo` call remains synchronous and cannot be portably
cancelled by this change. Time spent resolving nevertheless consumes the
request budget: if resolution returns after the deadline, no connection
attempt begins and the request reports timeout.

The TLS session's existing private `io_deadline_us` state is set through a
network-internal inline helper declared in `rt_tls_internal.h`. TLS
record-layer readiness observes that absolute deadline even when the session
has no cancellation callback. HTTP clears request-local TLS deadline state
before closing or returning a connection to an idle pool so one request cannot
poison the next lease. This private hook adds no exported symbol and does not
change the runtime C ABI.

## Consequences

- Slow trickle responses and TLS records can no longer extend a request
  indefinitely.
- Redirects and multi-address hosts consume one predictable budget instead of
  multiplying it.
- Code that relied on a long sequence of individually fast phases may now
  time out sooner. Callers must configure a budget for the complete operation,
  or use zero deliberately when no deadline is appropriate.
- DNS resolution can still return later than the configured deadline because
  the system resolver call is not preemptible; the runtime prevents subsequent
  network work once it returns.
- Pooled connections remain reusable because deadline state belongs to a
  lease/request, not to the transport's idle lifetime.

## Alternatives Considered

- **Keep per-operation timeouts.** Rejected because progress by occasional
  bytes defeats the timeout as a resource bound.
- **Restart the budget for each redirect.** Rejected because a redirect chain
  would still multiply the configured duration.
- **Move name resolution to a cancellable worker immediately.** Deferred. It
  requires a cross-platform resolver lifecycle and cancellation contract; the
  current decision still counts resolver elapsed time and prevents late
  connection attempts.

## Validation

Targeted runtime tests cover a plain HTTP body trickled one byte at a time, a
redirect chain that exhausts one shared budget, and a TLS record whose bytes
arrive slowly. Existing focused HTTP, TLS, and high-level network suites remain
green.
