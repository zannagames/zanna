---
status: active
audience: contributors
last-verified: 2026-08-26
---

# ADR 0295: Make VSCN Binary Wire Layouts Explicit and Portable

## Status

Accepted (2026-08-26)

## Context

VSCN mesh and skeletal-animation records label embedded binary payloads as
little-endian, but the v2 encoders Base64-encode native C structures. Native
structure padding and host byte order are implementation details, so those
payloads are not portable even though their tags claim otherwise. Mesh bone
maps and additional skin influences have the same issue and previously had no
layout tag. The loader also treated malformed optional rig side streams as
absent, which could silently change a rigged asset.

This changes the serialized scene-asset contract and therefore requires an ADR.

## Decision

New VSCN output uses padding-free, explicitly little-endian field layouts:

- `vgfx3d_vertex_le_v3` is exactly 92 bytes per vertex: 18 IEEE-754 binary32
  lanes in member order (`pos`, `normal`, `uv`, `uv1`, `color`, `tangent`),
  four raw `bone_indices` bytes, then four binary32 `bone_weights` lanes.
- `u32le-v1` indices are exactly four bytes each.
- `i32le-v1` bone-map entries are exactly four bytes each.
- `vgfx3d_extra_influences_le_v1` is exactly 24 bytes per vertex: four
  little-endian uint16 indices followed by four binary32 weights.
- `vgfx3d_keyframe_le_v3` is exactly 132 bytes per key: one binary64 time,
  position/rotation/scale binary32 lanes, four one-byte masks, then the six
  tangent vectors in runtime member order. It has no tail padding.

Mesh records write `indexFormat`, `boneMapFormat`, and
`extraInfluencesFormat` whenever the corresponding stream is present. Readers
accept the established untagged legacy side streams and v1/v2 vertex/keyframe
formats for compatibility, but reject unknown explicit tags. Legacy payloads
retain their historical layout interpretation; all newly emitted payloads use
the portable codecs above.

Present rig side streams are fail-closed: malformed Base64, an incorrect
decoded length, an invalid format tag, an out-of-range map/index, a non-finite
or negative extra weight, or a combined skin-weight sum above the existing
tolerance rejects the complete VSCN transaction with
`RT_ASSET_ERROR_CORRUPT`. An absent stream remains optional.

The VSCN document-version selection rules remain unchanged. The field-level
format tag is the binary-layout version, avoiding a collision with the
proposed VSCN v8 external-reference schema.

## Consequences

- New VSCN mesh, rig, and skeletal-animation payloads are byte-identical on
  macOS, Windows, Linux, and hosts of either byte order.
- Existing v1/v2 binary payloads remain readable, but older runtimes reject
  new explicit layout tags instead of misreading them.
- Corrupt optional rig data can no longer degrade silently into a different
  asset.
- The portable codecs perform member-wise conversion and require temporary
  wire buffers during in-memory serialization.

## Alternatives Considered

- **Keep native structs and add static assertions.** Rejected because size and
  offsets do not make native byte order portable and tail padding remains an
  implementation detail.
- **Promote every file to VSCN v8.** Rejected because binary field layout is
  orthogonal to the proposed external-reference schema and would consume its
  document version prematurely.
- **Drop legacy reads.** Rejected because existing saved scenes must continue
  to load.
