---
status: accepted
audience: contributors
last-verified: 2026-09-04
---

# ADR 0321: Make 3D Render Cache Identities Generation-Safe

## Context

ADR 0173 made retained mesh revisions immutable but kept a heap object's raw
address as the backend geometry-cache identity. Mesh and MorphTarget3D payload
generations restart at one for each allocation. If the runtime allocator
reuses an address while a native backend still retains the old cache entry, a
new object with matching generation and element counts can be mistaken for the
dead object and render stale GPU data.

The draw command is the renderer/backend boundary shared by the software,
Metal, Direct3D 11, and OpenGL implementations. Adding allocation identity to
that structure changes a cross-layer C contract and therefore requires this
decision record under ADR 0006.

## Decision

`vgfx3d_draw_cmd_t` carries two nonzero 64-bit allocation generations:

- `geometry_identity` identifies the Mesh3D allocation that owns
  `geometry_key`. It is zero whenever geometry is transient and
  `geometry_key` is null.
- `morph_identity` identifies the MorphTarget3D allocation that owns
  `morph_key`. It is zero whenever no retained morph payload is bound.

Mesh3D, Material3D, SceneNode3D, and MorphTarget3D allocation generations use
one process-wide monotonically increasing 64-bit sequence which skips zero.
The existing address key remains part of cache matching because one Mesh3D can
publish distinct raw and generated-tangent variants. Native cache hits require
both the address key and its allocation generation to match, in addition to
the content revision, element counts, and encoding flags already checked.

OpenGL, Direct3D 11, and Metal geometry, morph, and extra-influence cache
entries retain the corresponding allocation generation. A command with a
non-null retained key but a zero allocation generation is treated as
uncacheable and follows the dynamic upload path; it may never match a retained
entry.

This supersedes only ADR 0173's statement that the raw heap mesh handle alone
is the backend geometry identity. Immutable retained geometry and all public
registry behavior remain unchanged.

## Consequences

- Reusing a freed runtime address cannot expose stale mesh, tangent, morph, or
  extra-influence GPU data from the prior allocation.
- All renderer and backend consumers must be rebuilt against the appended draw
  command fields.
- Cache entries grow by one 64-bit generation per retained identity.
- No IL opcode, grammar, verifier rule, registry row, serialized format, or
  external dependency changes.

## Alternatives Considered

- Evict caches from object finalizers: rejected because finalizers should not
  depend on every live backend context and queued work may still retain an old
  revision.
- Hash allocation identity into the content revision: rejected because hash
  collisions would preserve the same stale-data failure mode.
- Use integer allocation IDs as synthetic pointers: rejected because
  integer-to-pointer conversion is implementation-defined and violates the
  cross-platform policy.
