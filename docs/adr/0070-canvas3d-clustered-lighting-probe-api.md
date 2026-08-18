---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0070: Canvas3D Clustered Lighting Probe API

Date: 2026-07-09
Status: Accepted

## Context

`Canvas3D.ClusteredLighting = true` is intentionally strict: enabling it on a
backend that cannot run clustered/forward+ lighting traps. That keeps accidental
quality regressions visible, but it makes portable runtime code awkward because
scripts must either pre-check capabilities perfectly or use a trap-prone setter
as a probe.

Graphics3D already exposes `BackendSupports("clustered-lighting")`, but backend
selection, environment kill switches, and disabled-build stubs are runtime
conditions. A non-trapping API is needed for fallback-oriented code while keeping
the existing property semantics unchanged.

## Decision

Add `Canvas3D.TrySetClusteredLighting(enabled: Boolean) -> Boolean`.

The method applies the requested state and returns `true` when it succeeds. It
returns `false` instead of trapping when the canvas is invalid, Graphics3D is not
compiled in, clustered lighting is blocked by `ZANNA_3D_CLUSTERS=0`, or the
active backend does not support clustered lighting. Disabling clustered lighting
on a valid canvas succeeds.

The existing `ClusteredLighting` property setter remains strict and continues to
trap when enabling is requested on an unsupported backend.

## Consequences

- Portable games can opt into clustered lighting without using exceptions or
  traps for capability probing.
- Existing code that relies on strict quality enforcement remains compatible.
- Disabled-build behavior matches other non-trapping diagnostic/probe APIs by
  returning a neutral failure value.
