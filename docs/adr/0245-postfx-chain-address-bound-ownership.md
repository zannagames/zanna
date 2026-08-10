---
status: active
audience: contributors
last-verified: 2026-08-10
---

# ADR 0245: Bind PostFX Backend Chain Storage to Its Owning Address

## Status

Accepted (2026-08-10)

## Context

`vgfx3d_postfx_chain_t` is a runtime C ABI structure shared by PostFX export and
the Metal, D3D11, and OpenGL backend layers. Its original pointer, count, and
capacity fields served both as traversal state and as evidence that `realloc`
or `free` was permitted. A damaged runtime payload, stale by-value copy, or
borrowed fixture could therefore make teardown touch memory the receiving
chain did not own.

Backend code also copied this struct by value before reset, copy, and free.
That pattern cannot preserve unique allocation ownership: both values expose
the same pointer, while neither records which stable object owns it.

## Decision

Append the following fields to `vgfx3d_postfx_chain_t`, after its existing ABI
prefix:

- `vgfx3d_postfx_effect_desc_t *owned_effects`
- `int32_t owned_effect_capacity`
- `uint64_t effect_storage_cookie`

The existing `effects`, `effect_count`, and `effect_capacity` fields remain
backend-readable traversal mirrors. Allocation operations are authorized only
when the owner pointer, owner capacity, and cookie form a valid tuple. The
cookie incorporates a domain value, the allocation address, the capacity, and
the address of the containing chain. A tuple that cannot prove ownership is
detached without dereferencing, resizing, clearing, or freeing its pointer.

Binding the cookie to the containing address deliberately makes a by-value
copy a borrowed view. Backends that own storage must keep the chain at a stable
address and pass that address to `vgfx3d_postfx_chain_reset`,
`vgfx3d_postfx_chain_copy`, and `vgfx3d_postfx_chain_free`. Metal therefore
stores the chain in an explicit context ivar and mutates that ivar directly.
Read-only by-value copies remain valid for traversal while their public prefix
is internally consistent, but cannot release the allocation.

Authored and backend chains share a 4,096-entry logical ceiling. Export and
copy reject larger counts independently of allocator capacity.

## Consequences

- Corrupted mirrors and borrowed copies cannot authorize `realloc` or `free`.
- Reset and free safely detach legacy, borrowed, or invalid tuples.
- Backend chain allocations remain reusable across frames when accessed at
  their stable owner address.
- All consumers must be rebuilt against the appended structure layout. No IL
  opcode, runtime registry method, language grammar, or serialized scene format
  changes.
- An invalid authoritative cookie fails closed and may intentionally leak the
  allocation rather than risk touching an untrusted pointer.

## Alternatives Considered

- Trust pointer/count/capacity after local range checks: rejected because
  plausible values do not prove allocation ownership.
- Store only a separate owner pointer: rejected because corrupt capacity could
  still authorize an invalid resize or oversized clear.
- Preserve by-value ownership transfer: rejected because ordinary C assignment
  cannot express which copy becomes the unique owner.
- Replace the public struct with an opaque heap object: rejected because it
  would require a larger backend ABI and call-site migration than the appended,
  fail-closed metadata.
