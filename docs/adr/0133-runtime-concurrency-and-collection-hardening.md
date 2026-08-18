---
status: active
audience: contributors
last-verified: 2026-08-17
---

# ADR 0133: Make Runtime Concurrency Ownership Typed and Collection Capacity Explicit

## Status

Accepted

## Context

The runtime audit found several places where an ownership or capacity decision
was implicit in the C ABI. Native thread and thread-pool entry points accepted
untyped data pointers and reconstructed function pointers, which is not
portable C. Channel receive used a null output pointer both as an accidental
discard operation and as an invalid argument, making ownership transfer
ambiguous. Promise, Future, Channel, ConcurrentQueue, and ConcurrentMap objects
owned managed references but did not all expose those edges to cycle
collection.

The primary object array also allocated exactly the requested logical length on
every resize. Lists consequently performed a full allocation and copy for each
append and removal. Map and IntMap grew their bucket tables but offered no way
to return excess bucket capacity after a temporary high-water mark. Their
rehash paths recomputed hashes, compared load factors with floating-point
arithmetic, and did not provide one documented transactional rule for a failed
bucket allocation.

These decisions affect the supported runtime C ABI, the generated runtime
registry, native-code portability, managed graph traversal, and the public
collection API. They therefore require one cross-layer decision before the new
surface is registered.

## Decision

- Native thread and thread-pool constructors use typed callback typedefs. Raw
  legacy entry points remain as compatibility adapters and copy representation
  bytes without converting object pointers directly to function pointers.
- A channel receive with a null output pointer is a non-consuming readiness
  probe. Callers that intentionally consume and release an item use the
  explicitly named `rt_channel_recv_for_discard` C entry point. The helper is
  not registered as a language method because ordinary language callers
  receive managed values and can discard them through normal expression
  semantics.
- Every concurrent container that owns managed references registers a stable
  GC traversal callback. Structural mutation and traversal follow the managed
  graph barrier established by ADR 0116.
- Resizable object arrays use a minimum capacity of sixteen slots and geometric
  doubling on growth. Shrinking releases removed references and changes the
  logical length without reducing capacity; resize-to-zero retains its existing
  behavior of releasing the array. Exact construction through
  `rt_arr_obj_new` remains exact-capacity.
- Object-array `Set`/`Resize` runtime registry entries are marked with the same
  ownership semantics implemented by the C helpers: storing retains a managed
  reference, replacement/shrink releases the prior reference, and returning
  getters transfer or borrow only as their registered masks declare. Generated
  registry assertions guard those masks against drift.
- Map, IntMap, and StringSet cache each entry's hash. String-keyed entries store
  their node and copied key in one allocation. Load-factor comparisons use
  overflow-safe integer arithmetic and never depend on floating-point
  precision.
- A growth rehash allocates the complete replacement bucket array before
  relinking any entry. On failure, the old buckets, capacity, count, entries,
  and values remain unchanged, and the pending insertion is not published.
- `Zanna.Collections.Map.Trim()` and
  `Zanna.Collections.IntMap.Trim()` are added as explicit Boolean methods. They
  reduce capacity to the smallest power-of-two table, no smaller than sixteen,
  that can hold the current count below the normal 75-percent growth threshold.
  They return true if capacity is already minimal or the replacement succeeds;
  allocation failure traps and returns false to a returning trap hook while
  preserving the old table.
- `Clear()` continues to retain bucket capacity for fast reuse. There is no
  implicit shrink on removal, so existing workloads do not acquire hidden
  allocation points; callers choose when to pay for `Trim()`.

## Consequences

- Native callback invocation is portable across platforms where data and
  function pointers have different representations, while existing raw ABI
  callers remain source-compatible.
- Channel ownership transfer is explicit, and probing cannot silently consume
  an item or leak a transferred reference.
- Cycles through concurrent containers and completion objects are collectible.
- Repeated List append is amortized O(1), and repeated removal does not churn
  the allocator. Removing the final element still returns the List to its
  historical null-backing state.
- Hash-table rehashing moves cached metadata instead of reading every key and
  hashing it again. One allocation per string entry reduces allocator traffic
  and fragmentation.
- Map users can release high-water bucket storage without changing `Clear()` or
  `Remove()` behavior. Trim introduces a documented allocation/trap point and
  is safe to retry.
- The generated runtime reference and API inventories gain two methods. This is
  an additive ABI change and does not alter any existing signature.

## Alternatives Considered

- Shrink maps automatically after removals: rejected because `Remove()` would
  gain a surprising allocation failure and repeated grow/shrink workloads could
  oscillate despite hysteresis.
- Make `Clear()` return storage immediately: rejected because it would remove
  its documented capacity-reuse behavior.
- Keep exact object-array allocation and add capacity only to List: rejected
  because the array is already the storage abstraction and all resizable users
  need the same ownership-safe policy.
- Treat null channel outputs as discard: rejected because a missing output is
  otherwise an argument error and ownership transfer must not be inferred from
  an omitted destination.
- Preserve raw `void *` callbacks as the primary ABI: rejected because C does
  not guarantee a portable object-pointer/function-pointer conversion.
