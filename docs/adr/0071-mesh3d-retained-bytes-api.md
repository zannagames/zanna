---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0071: Mesh3D Retained Bytes API

Date: 2026-07-09
Status: Accepted

## Context

`Mesh3D.Resident` controls whether a mesh payload is eligible for draw and upload
submission. Nonresident meshes deliberately keep their CPU vertex/index payload
so streaming systems can restore residency without reloading or rebuilding
procedural geometry.

The existing `ResidentBytes` property correctly reports zero when a mesh is
nonresident, but that hides retained RAM from tooling and budget logic.

## Decision

Add read-only `Mesh3D.RetainedBytes -> Integer`.

`RetainedBytes` reports the estimated CPU vertex/index payload size regardless
of the `Resident` flag. `ResidentBytes` remains unchanged and continues to
report only currently resident/drawable payload bytes.

## Consequences

- Streaming systems can separately track drawable residency and retained RAM.
- Existing code that treats `ResidentBytes == 0` as nonresident remains
  compatible.
- Disabled Graphics3D builds return the neutral value `0`.
