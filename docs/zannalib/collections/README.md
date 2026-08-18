---
status: active
audience: public
last-verified: 2026-08-17
---

# Collections
> Data structures and container types for the Zanna runtime.

**Part of [Zanna Runtime Library](../README.md)**

---

## Contents

| File | Contents |
|------|----------|
| [Sequential Collections](sequential.md) | List, Queue, Stack, Deque, Ring, Heap |
| [Maps & Sets](maps-sets.md) | Map, Set, OrderedMap, SortedSet, FrozenMap, FrozenSet, SortedMap |
| [Specialized Maps](multi-maps.md) | BiMap, MultiMap, CountMap, IntMap, DefaultMap, LruCache, WeakMap, SparseArray |
| [Functional & Lazy](functional.md) | `Zanna.Collections.Seq`, `Zanna.Collections.Iterator`, and `Zanna.Functional.LazySeq` |
| [Specialized Structures](specialized.md) | F64Buffer, I64Buffer, StringSet, BloomFilter, Trie, UnionFind, BitSet, Bytes |

## Runtime Correctness Notes

- String-keyed collection types compare the full runtime string byte length.
  Embedded NUL bytes are significant and do not truncate keys or string-set
  elements.
- Owning collections retain stored objects and release them when overwritten,
  removed, cleared, or finalized. `WeakMap` is the exception: it stores zeroing
  weak references and does not keep values alive. Individual getters can still
  return borrowed references; consult the type-specific page before retaining a
  result across a mutation.
- Enumeration APIs that expose keys, values, sparse indices, or sorted slices
  return point-in-time snapshots. String snapshots own copied strings,
  object-value snapshots retain their elements, and integer index/key snapshots
  use boxed `i64` values. Snapshot order depends on the collection: hash-backed
  types generally do not promise insertion or sorted order.
- Stack and Queue default to borrowed elements at the C runtime layer, but
  conversion helpers such as `Seq.ToStack`, `List.ToQueue`, and `Stack.ToSeq`
  create retained snapshots so returned containers remain valid after the
  source is cleared.
- Packed numeric buffers (`F64Buffer`, `I64Buffer`) own contiguous primitive
  payloads internally. `Slice` returns an independent copy, while `ToList` and
  `ToSeq` box values into ordinary object collections. `I64Buffer` arithmetic
  is currently unchecked at signed overflow; see [Specialized
  Structures](specialized.md).
- Most collection capacity calculations have explicit overflow checks, but
  overflow behavior is not a universal collection-level guarantee. The
  type-specific pages document known exceptions, including the current BitSet
  staged-growth defect. Maintainers should also consult the [documentation
  review findings](../../../misc/reviews/documentation-review-findings.md).
- Collections are generally not thread-safe. Some apparent reads mutate state
  (`LruCache.Get` changes recency and `UnionFind.Find` compresses paths), so use
  external synchronization whenever an instance is shared across threads.

## See Also

- [Input/Output](../io/README.md) - File operations for persisting collections
- [Text Processing](../text/README.md) - `StringBuilder` for efficient string building, `Csv` for data import/export
- [Threads](../threads.md) - Thread-safe access patterns using `Monitor` or `RwLock`
