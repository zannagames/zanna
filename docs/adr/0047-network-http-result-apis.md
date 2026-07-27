---
status: active
audience: contributors
last-verified: 2026-07-02
---

# ADR 0047: Network HTTP Result APIs

## Status

Accepted

## Context

`HttpReq.Send` and the raw `RestClient` HTTP methods return `HttpRes` objects
when an HTTP response is received, but transport and setup failures are trapping
paths. `RestClient` also exposes `LastStatus`, `LastResponse`, and `LastOk`,
which makes examples easy to write but leaves production code dependent on
receiver-scoped mutable state.

The runtime already has `Zanna.Result`, and `HttpRes` already carries status,
headers, and body. The missing public shape is a result-returning request path
that keeps transport failures explicit while leaving HTTP status on the
response object.

## Decision

Add result-returning HTTP request APIs:

- `Zanna.Network.HttpReq.SendResult()`
- `Zanna.Network.RestClient.GetResult(path)`
- `Zanna.Network.RestClient.PostResult(path, body)`
- `Zanna.Network.RestClient.PutResult(path, body)`
- `Zanna.Network.RestClient.PatchResult(path, body)`
- `Zanna.Network.RestClient.DeleteResult(path)`
- `Zanna.Network.RestClient.HeadResult(path)`

These methods return `Result<HttpRes>`. Transport setup failures, connection
failures, timeouts, and recovered traps become `Result.ErrStr`. Any received
HTTP response, including 4xx and 5xx statuses, is `Result.Ok(HttpRes)` so
callers can inspect status, headers, and body directly.

Existing trapping methods remain available. `RestClient.LastStatus`,
`LastResponse`, and `LastOk` remain receiver-scoped compatibility diagnostics
and carry a migration target to `RestClient.GetResult` in the runtime API dump.

## Consequences

- Production HTTP code can handle transport failures without reading mutable
  last-state or relying on traps.
- HTTP status handling remains explicit on `HttpRes`; a 404 is not conflated
  with a failed TCP/TLS request.
- Existing scripts and demos that prefer trapping `Send`/`Get` behavior keep
  working unchanged.
