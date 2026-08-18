---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0061: Zia SemanticJob Error Option API

Date: 2026-07-02

## Status

Superseded — the public-surface standardization folded the Option-returning
accessor into the canonical name instead of registering a second one.
`Zanna.Zia.SemanticJob.Error` is now registered as `obj<Zanna.Option>(obj)`
backed by `rt_zia_semantic_job_error_option`; there is no separate
`ErrorOption` row and no string-returning `Error` row (VDOC-288).

## Context

`Zanna.Zia.SemanticJob.Error(job)` returns a string side channel for background
language-service failures. An empty string means "no error", but an empty string
is also a valid string value and does not communicate absence as clearly as the
runtime's newer public APIs.

Semantic jobs already expose `IsDone` and `IsError`, and their result accessors
return typed values. The error accessor should follow the same explicit absence
model used by other lookup/status APIs.

## Decision

Add `Zanna.Zia.SemanticJob.ErrorOption(job) -> Option[String]`.

The API returns `SomeStr(message)` when the job handle contains a non-empty
error payload. It returns `None` when the handle is invalid, the job has no
error payload, or the weak runtime stub is in use because `fe_zia` is not linked.

The existing `Error(job)` method remains available for compatibility and is
marked as a legacy side-channel row in the runtime API metadata with a migration
target to `ErrorOption`.

## Consequences

- Editor and IDE code can handle semantic-job failures without comparing against
  `""`.
- Weak-stub builds preserve the same shape by returning `None`.
- Existing callers that already read `Error(job)` keep working while generated
  docs and API dumps now recommend the Option-returning method.
