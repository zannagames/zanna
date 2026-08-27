---
status: active
audience: contributors
last-verified: 2026-08-27
---

# ADR 0297: Maintain Game3D Name Indices Incrementally

## Status

Accepted (2026-08-27)

## Context

`World3D` already incrementally inserts spawned entity names, but renaming or
despawning an entity invalidates the entire open-addressed name table. The next
lookup, or the end of every tree despawn, scans and reinserts the complete entity
registry. This makes ordinary lifecycle operations scale with the world rather
than with the changed name.

The entity setter is a separate runtime translation unit from the world-owned
index implementation. Adding its native maintenance helper changes the internal
runtime C ABI and therefore requires an ADR under ADR 0006.

## Decision

Add the native helper
`game3d_world_name_index_remove_entity(rt_game3d_world *, rt_game3d_entity *,
const char *old_name)` and use it before registry removal or a spawned-entity
rename.

Removing the indexed winner closes the affected linear-probing cluster by
reinserting only its displaced entries. If duplicate names exist, the current
registry is scanned only until the earliest surviving duplicate is found and
that entity is promoted. Removing a later duplicate is a no-op. Rename then uses
the established incremental insertion helper for the new name. Corrupt or
already-invalid tables retain the existing fail-closed rebuild-on-lookup path.

## Consequences

- Normal rename and despawn keep the existing name-table allocation and avoid a
  full registry rebuild.
- Duplicate-name lookup continues to promote the earliest surviving registrant.
- Removing the winning member of a duplicate-name group may scan the entity
  registry until its replacement is found; unrelated unique-name removals do
  not scan it.
- No scripting registry, IL, grammar, verifier, serialized format, or external
  dependency changes.

## Alternatives Considered

- **Use tombstones.** Rejected because tombstones accumulate and require a
  later full rehash policy; closing the short affected cluster keeps absence
  probes simple.
- **Store duplicate chains in every slot.** Rejected because it increases
  steady-state memory and mutation complexity for an uncommon case.
- **Continue lazy full rebuilds.** Rejected because a single rename can make the
  next lookup unexpectedly linear in the complete world size.
