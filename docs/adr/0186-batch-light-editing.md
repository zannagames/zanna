---
status: active
audience: contributors
last-verified: 2026-07-24
---

# ADR 0186: Add Mixed-Value Batch Light Editing

## Status

Accepted (2026-07-24)

## Context

The light inspector (ADR 0172) is deliberately single-node: its v1 preferred
an honest limitation over false mixed-state behavior. The material inspector
has since proven the correct multi-node pattern (truthful native mixed-value
fields, all clones staged before any assignment, selected sharing groups
reusing one staged clone, a single canonical transaction with exact
rollback), and level-scale lighting work — Ashfall missions carry dozens of
practicals — makes per-node editing the bottleneck.

## Decision

The Light component group accepts multi-node selections of light-bearing
nodes with the material inspector's exact semantics:

- Every field (kind, color, intensity, position/direction, falloff,
  dimensions, radius, range, cone angles, and every other retained ADR 0172
  field) presents a native mixed state when selected nodes disagree; kind
  itself may be mixed.
- **Apply resolves only concrete fields**: mixed fields preserve each node's
  current value. Changing kind on a mixed-kind selection is allowed only by
  choosing a concrete kind, which rebuilds each light preserving
  kind-compatible fields and defaulting the rest (stated in the UI).
- Replacement lights are constructed independently per node **before** any
  assignment (the ADR 0172 clone rule), so shared imported lights are never
  edited in place and unselected users of a shared light are untouched.
  Nodes selected together that share one light object receive one staged
  replacement each — the material inspector's sharing-group rule applied to
  the simpler per-node light ownership.
- Add Light to a multi-node selection adds only to nodes lacking a light;
  Remove Light removes from all selected light-bearing nodes. Each accepted
  action — add, apply, remove — is one canonical VSCN history transaction
  with exact rollback and no-op awareness.
- Hierarchy badges and viewport light markers continue to reflect every
  node individually.

## Consequences

- Retuning a mission's practical lights becomes one selection and one undo
  step instead of dozens.
- The "multi-node light editing remains explicitly disabled" limitation is
  removed from code and docs; the honesty note moves to describing the
  mixed-kind rebuild rule.
- `scene_light_authoring_probe.zia` grows batch cases: mixed-field
  preservation, shared-light clone safety, mixed-kind concrete rebuild,
  add/remove selection semantics, single-transaction undo.

## Alternatives Considered

- **Apply-to-all overwrite (no mixed states).** Rejected: it silently
  destroys per-node variation — the exact false behavior ADR 0172 refused.
- **Batch via component schemas only.** Rejected: schemas add missing typed
  metadata; they cannot express the typed `Light3D` component or its
  kind-dependent field set.
