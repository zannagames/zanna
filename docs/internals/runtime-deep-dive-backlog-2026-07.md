---
status: active
audience: contributors
last-verified: 2026-07-29
---

# Runtime Deep-Dive Backlog — July 2026

This is the durable backlog for the July 2026 review of Zanna's core runtime,
memory management, threads, collections, I/O, archives, networking,
localization, and audio paths. It supplements rather than replaces
`runtime-hardening-audit-2026-07.md`: that older ledger records 64 previously
closed findings, while the entries below come from the current source review.

The identifiers are stable. A `Fixed-WT` item has an implementation and focused
regression coverage in the shared working tree, but is not claimed to be
committed or released. `Open` means the source evidence is strong enough for
implementation. `Measure` is a performance recommendation that requires a
focused benchmark before changing policy. `ADR` means the likely fix changes a
public/runtime contract and therefore needs an ADR under `AGENTS.md`.

Priorities are: P0 memory safety or state corruption; P1 correctness,
concurrency, or unbounded-resource risk; P2 material performance/resilience;
P3 maintainability, diagnostics, or additional assurance.

## Core memory, GC, pools, traps, and strings

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-001 | Fixed-WT | P1 | `rt_heap_realloc` in `core/rt_heap.c` checked `elem_size == 0` before normalizing `cap = max(new_len, new_cap)`. | Validate zero-sized elements against the effective capacity so `new_len > new_cap` cannot create a non-empty zero-stride allocation. |
| ZRT-002 | Fixed-WT | P0 | The locked free-list branch of `pop_from_freelist` changed block state after dropping `freelist_lock`. | Claim the block and decrement `free_count` while the list lock still protects the head, preventing a shutdown/free race from observing contradictory state. |
| ZRT-003 | Fixed-WT | P0 | Locked `push_to_freelist` published the node before updating `free_count`, while lifecycle code reads both. | Publish the node and update the checked counter in one lock-protected transaction. |
| ZRT-004 | Fixed-WT | P1 | `rt_pool_stats` accepted a signed enum but rejected only values at or above `RT_POOL_COUNT`. | Reject negative size-class values as well as oversized values. |
| ZRT-005 | Fixed-WT | P0 | Heap-backed string helpers trusted `s->heap` and could dereference a forged or stale wrapper/header pair. | Resolve `s->data` through the live heap registry and require the registered header to equal the wrapper header before reading it. |
| ZRT-006 | Fixed-WT | P0 | Many functions in `rt_string_ops.c`, `rt_string_advanced.c`, and `rt_string_specialized.c` read wrapper fields before validating the string handle. | Validate every non-null string argument before dereference and return immediately if a configured trap hook returns. |
| ZRT-007 | Open | P2 | `g_heap_registry_` in `rt_heap.c` uses one process-wide spin lock for lookup, retain, release, allocation registration, and deletion. | Shard the live-payload registry by mixed pointer hash, including independent locks/counters, then benchmark high-thread-count retain/release. |
| ZRT-008 | Open | P2 | `rt_heap_registry_ensure_capacity_locked_` grows when live entries plus tombstones cross 5/8, even when a same-capacity rehash would remove enough tombstones. | Add tombstone-dominance compaction before geometric growth to reduce memory and probe length in churn-heavy workloads. |
| ZRT-009 | Open | P2 | `rt_heap_get_containing_info` scans every registry slot for interior-pointer queries. | Add a range index or page-to-allocation side table so interior validation is not O(total live allocations). |
| ZRT-010 | ADR | P0 | `rt_heap_try_get_header` returns a borrowed header after releasing the registry lock; another owner can release the allocation before a caller dereferences it. | Replace cross-module borrowed-header use with lock-bounded value snapshots or a pin/unpin API; document the new lifetime contract in an ADR. |
| ZRT-011 | Open | P2 | `rt_heap_realloc` allocates the replacement before validating that the old payload is live and uniquely owned. | Validate existence, kind, and exclusive ownership before reserving replacement storage, while preserving race safety through an explicit reservation state. |
| ZRT-012 | Open | P1 | `rt_heap_realloc` holds the global registry lock while copying the old payload into the new allocation and moving registry state. | Introduce a relocating/reserved state so potentially large copies happen outside the registry lock without allowing retain/release of the old address. |
| ZRT-013 | Measure | P2 | `rt_heap_alloc_zeroed_` zeroes all managed capacity, including capacity not yet logically observable. | Benchmark a split zeroed/uninitialized internal allocator and zero only newly observable regions; retain zeroed allocation for public safety-sensitive paths. |
| ZRT-014 | Open | P2 | Every managed graph mutation shares the single `g_gc_world_lock` read side in `rt_gc.c`; collection takes it exclusively. | Measure lock contention and prototype epoch/handshake-based quiescence so unrelated mutators do not contend on one RW lock. |
| ZRT-015 | Open | P1 | `rt_gc_collect` stops mutators before allocating the snapshot sized from `g_gc.count`. | Reserve/reuse snapshot capacity before acquiring the exclusive graph barrier and revalidate the count after quiescence. |
| ZRT-016 | Open | P2 | GC traversal grows `gc_edge_list` and snapshot work arrays with `realloc` during the stop-the-world interval. | Reuse bounded scratch arenas or pre-size from tracked edge metadata to keep allocator latency out of the pause. |
| ZRT-017 | ADR | P2 | `rt_gc_collect` performs a complete synchronous trial-deletion pass once requested. | Define an incremental or time-budgeted collection contract with explicit safe-point progress and an ADR for observable scheduling semantics. |
| ZRT-018 | Open | P1 | `gc_entry.survived` is `uint16_t`; long-lived entries can eventually wrap unless every increment saturates. | Centralize a saturating increment and add a regression that drives the counter through its maximum. |
| ZRT-019 | Open | P2 | Weak-reference buckets allocate one `weak_chain` node per registration and use linked chains. | Use slabbed/open-addressed weak registration storage and reuse nodes to reduce allocator pressure during weak-reference churn. |
| ZRT-020 | Open | P2 | Each collection builds a fresh open-addressed garbage membership set with `calloc`. | Cache and clear reusable membership storage, with an upper retention bound, to avoid per-pass allocation and zeroing. |
| ZRT-021 | Open | P1 | Reclaim-phase finalizers run while the exclusive managed-graph barrier remains held. | Split logical graph detachment from user finalization where possible, so slow/reentrant finalizers do not extend global pause time; preserve resurrection semantics with tests. |
| ZRT-022 | Measure | P2 | Pool slabs remain attached until shutdown even when every block in a slab is free. | Add an opt-in trim policy that reclaims fully free excess slabs after a cold interval; benchmark fragmentation and synchronization cost. |
| ZRT-023 | Measure | P2 | `rt_pool_alloc` clears the entire selected size class, not merely the requested bytes. | Measure clearing requested size plus metadata-safe tail poisoning in debug builds, keeping deterministic zero semantics where callers require them. |
| ZRT-024 | Open | P3 | Pool fallback allocations are excluded from per-class stats, so oversize and exhaustion behavior is invisible. | Add separate checked fallback allocation/free byte counters and expose them through diagnostics without changing existing class totals. |
| ZRT-025 | Open | P1 | Pool free accepts both a pointer and caller-supplied size even though slab metadata identifies the owning class. | Validate that the supplied size maps to the owner class and trap on mismatch; add boundary tests for every adjacent class. |

## Threads, synchronization, futures, schedulers, and work queues

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-026 | Fixed-WT | P0 | Scheduler name paths could continue after invalid-handle traps and then read string wrapper data. | Make `scheduler_name_valid` status-returning and stop every schedule/cancel/query path after a returning trap. |
| ZRT-027 | Fixed-WT | P1 | Scheduler insertion and replacement could leak retained names or alter pending counts after allocation/retain failure. | Stage ownership before publication and update the pending counter only with the linked-list mutation. |
| ZRT-028 | Fixed-WT | P1 | Thread-pool timeout arithmetic could overflow and POSIX wait errors were not consistently distinguished from timeout. | Use checked deadline math and convert native errors into deterministic traps while preserving timeout as a normal result. |
| ZRT-029 | Fixed-WT | P0 | Concurrent-map replacement and insertion could continue after a returning retain/allocation trap. | Make mutations transactional: prepare key/value ownership first, publish once, and release displaced state outside the lock. |
| ZRT-030 | Open | P1 | Cancellation delivery in `rt_threads_common.c` can invoke cleanup that reaches another cancellation point while cancellation is still being processed. | Add a thread-local cancellation-delivery guard and defer nested delivery until the outer cleanup completes. |
| ZRT-031 | Open | P1 | Several public thread object entry points retain the receiver through helpers that can trap, but the returning-trap contract is enforced inconsistently. | Audit all retain/validate helpers and require explicit status results plus immediate exits on trap return. |
| ZRT-032 | Measure | P2 | Each Debouncer instance owns a monitoring thread rather than sharing a timing service. | Benchmark a process/context timer wheel serving many debouncers, with cancellation generations to prevent stale callbacks. |
| ZRT-033 | Open | P3 | Monitor, Future, Channel, ConcurrentQueue, Scheduler, and ThreadPool each implement similar clock/deadline conversion code. | Consolidate checked monotonic deadlines and remaining-time conversion into one internal cross-platform helper and one conformance test matrix. |
| ZRT-034 | Open | P1 | Windows scheduler time conversion multiplies performance-counter ticks before division, allowing intermediate overflow on long uptimes/high frequencies. | Use quotient/remainder conversion or 128-bit-capable checked arithmetic through the platform capability layer. |
| ZRT-035 | Measure | P2 | `rt_scheduler_is_due`, `_cancel`, `_generation_of`, and scheduling walk a name-keyed linked list. | Replace the list with a hash index plus due-time min-heap once benchmarks show meaningful scheduler cardinality. |
| ZRT-036 | Open | P2 | Due checks first find a named entry and later cleanup scans expired entries again. | Combine lookup and expiry maintenance or perform bounded lazy expiry from the earliest-deadline structure. |
| ZRT-037 | Measure | P2 | Scheduler name comparisons repeatedly validate strings and query their lengths through registries inside list scans. | Store validated byte length and a cached hash in each entry, retaining the name solely for ownership/publication. |
| ZRT-038 | Open | P1 | POSIX scheduler macros wrap mutex operations without checking every `pthread_mutex_*` return. | Replace macros with status-checking helpers that trap or abort consistently according to recoverability. |
| ZRT-039 | Measure | P2 | `rt_concmap.c` serializes all keys through one object mutex and performs resizing under that mutex. | Prototype lock striping with generation-stamped table replacement; benchmark read-heavy and disjoint-key writers. |
| ZRT-040 | Open | P1 | Concurrent-map snapshot/key extraction allocates managed/native storage while holding the map lock. | Count and reserve outside the lock, retry on generation change, and keep managed allocation/traps out of the critical section. |
| ZRT-041 | Open | P2 | A failed concurrent-map resize leaves the old high-load table in service, so chains can continue degrading indefinitely. | Record resize debt and retry with bounded backoff on later mutations; expose the failure in diagnostics. |
| ZRT-042 | Open | P1 | Channel send/receive paths retain or release managed values while holding the channel monitor, allowing finalizers/traps to prolong or reenter the critical section. | Stage retains before locking and detach transferred/released values under lock for destruction afterward. |
| ZRT-043 | Measure | P2 | Channel close and several state transitions wake every waiter with broadcast even when only one sender/receiver can progress. | Separate sender/receiver conditions or add waiter counts so normal progress signals one side and close alone broadcasts. |
| ZRT-044 | Measure | P2 | Every ConcurrentQueue enqueue performs a standalone `malloc` for `cq_node` and every dequeue frees it. | Add a bounded node cache or pool class and benchmark producer/consumer throughput plus retained-memory cost. |
| ZRT-045 | ADR | P1 | Pointer-returning ConcurrentQueue dequeue APIs use null for closed, timeout, empty, and a queued null value; only Option variants disambiguate some paths. | Define a status-plus-output API for all blocking modes and specify compatibility in an ADR. |
| ZRT-046 | Open | P1 | ConcurrentQueue and Future can fall back from monotonic initialization to realtime, while callers assume timeout immunity to wall-clock changes. | Carry the actual selected clock through deadline construction/wait and add clock-jump tests for fallback paths. |
| ZRT-047 | Measure | P2 | ThreadPool uses one strict FIFO protected by one monitor; all workers contend on the same queue. | Benchmark per-worker deques with work stealing while preserving FIFO only where the public contract promises it. |
| ZRT-048 | ADR | P1 | ThreadPool construction rejects sizes above the hard-coded 1024-worker ceiling without exposing a platform-derived limit. | Specify a documented capability/error contract and derive safe limits from platform resources in an ADR. |
| ZRT-049 | Open | P3 | ThreadPool submission returns one false result for shutdown, queue/allocation failure, invalid callback, and native synchronization errors. | Add internal structured failure reasons and surface them through diagnostics while preserving the Boolean compatibility wrapper. |
| ZRT-050 | Open | P1 | Some ThreadPool queue/state invariants remain protected only by assertions, which disappear in release builds. | Convert externally triggerable count/state violations into checked aborts or traps and fuzz shutdown/submit interleavings. |

## Collections and containers

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-051 | Fixed-WT | P0 | Bag scalar and set-operation paths could continue after an invalid receiver/key trap and dereference null or forged state. | Make receiver/key extraction status-returning and stop Add/Remove/Has/Items/Union/Intersect/Diff/Clone after failure. |
| ZRT-052 | Fixed-WT | P1 | BiMap doubled forward/inverse capacities without explicit overflow guards and did not uniformly stop after invalid key/value traps. | Check both capacity multiplications and maximum count, and validate both string sides before any table mutation. |
| ZRT-053 | Fixed-WT | P0 | `rt_binbuf_write_str` read an invalid string after a returning trap; `rt_binbuf_read_bytes` could continue after output allocation failure. | Validate the string before length/data access and return before copying into a missing Bytes result. |
| ZRT-054 | Fixed-WT | P0 | Scalar queries such as `rt_bitset_len` used `as_bitset(...)->field` directly, so a returning invalid-object trap became a null dereference. | Store the validated receiver and return a neutral scalar when a trap hook returns. |
| ZRT-055 | Fixed-WT | P0 | BloomFilter Add/MightContain used string data and length without rejecting stale/forged handles under returning traps. | Route both operations through a live-string data extractor and stop on failure. |
| ZRT-056 | Fixed-WT | P0 | Bytes string/hex/base64 constructors normalized some invalid handles to empty input and had unchecked output-size conversions. | Distinguish null from invalid handles, guard `size_t` to `int64_t` conversion, and clean partial results on every decode failure. |
| ZRT-057 | Fixed-WT | P1 | CountMap resize and load calculations could overflow, while key paths could continue after a returning trap. | Use checked geometric growth/load arithmetic, cap count, and make key extraction a required status. |
| ZRT-058 | Fixed-WT | P0 | DefaultMap constructor/set/snapshot paths could publish or leak partial ownership if a managed retain or sequence append trapped and returned. | Wrap retains/appends in local recovery, roll back staged key/value/snapshot state, then re-raise and return. |
| ZRT-059 | Fixed-WT | P0 | FrozenMap insertion/replacement retained key/value parts separately without complete rollback when the second operation trapped. | Treat slot ownership as a two-reference transaction and propagate insertion failure through constructors and merge. |
| ZRT-060 | Fixed-WT | P0 | FrozenSet construction and lookup accepted non-null invalid string elements far enough to read wrapper data. | Validate each extracted element and clean the partially built immutable table on failure. |
| ZRT-061 | Fixed-WT | P0 | Iterator constructors/operations could leak temporary managed values when adapter allocation or publication trapped. | Keep temporary ownership until iterator publication succeeds and release it in recovery paths. |
| ZRT-062 | Fixed-WT | P0 | List, Queue, Stack, Seq, SparseArray, and related scalar getters directly dereferenced the result of `as_*` validation. | Store the validated receiver and provide a neutral return after returning traps rather than dereferencing null. |
| ZRT-063 | Fixed-WT | P0 | LRU cache insertion/replacement and snapshot paths had multi-owner retain/append sequences without full returning-trap rollback. | Stage key/value/node ownership transactionally and destroy detached entries/snapshots only after leaving table mutation. |
| ZRT-064 | Fixed-WT | P1 | OrderedMap lacked consistent capacity/count overflow checks and continued after invalid key extraction in several paths. | Guard maximum count/geometric resize and require successful receiver/table/key validation for every operation. |
| ZRT-065 | Fixed-WT | P0 | MultiMap could publish a new per-key sequence before all key/value retains/appends succeeded. | Build the new sequence and key entry off-table, then publish atomically or release the entire staged entry. |
| ZRT-066 | Fixed-WT | P0 | TreeMap key/value snapshots leaked their partial sequence if sequence growth/retain trapped and returned. | Make append helpers consume the partial snapshot on failure, re-raise, and force callers to stop. |
| ZRT-067 | Fixed-WT | P0 | Trie recursive cleanup could exhaust the C stack, and clone/key collection lost partial state on returning traps. | Free subtrees iteratively without cleanup allocation; use checked grow helpers and recovery-owned clone/collection stacks. |
| ZRT-068 | Fixed-WT | P0 | UnionFind Find/Union/Reset and scalar queries could continue after invalid receiver/index traps. | Stop before parent/rank array access and return neutral values when a configured trap returns. |
| ZRT-069 | Fixed-WT | P0 | WeakMap trusted retained string keys during probing, used overflow-prone load math, and could corrupt ownership on failed rehash/snapshot append. | Revalidate keys, use checked thresholds/power-of-two invariants, rebuild transactionally, and release partial snapshots. |
| ZRT-070 | Fixed-WT | P1 | Seq sort scratch allocation multiplied logical length by pointer size without an explicit overflow guard. | Reject impossible scratch sizes before allocation and copy only the merged range back from temporary storage. |
| ZRT-071 | Fixed-WT | P1 | BinaryBuffer, BloomFilter, Bytes, Map variants, and Trie used inconsistent null-versus-invalid semantics for strings. | Preserve null as each API documents, but trap and stop for every non-null unregistered handle. |
| ZRT-072 | Open | P2 | Bag, BiMap, CountMap, DefaultMap, FrozenMap/Set, OrderedMap, TreeMap, Trie, WeakMap, and others each reimplement string validation plus data/length lookup. | Add one internal lock-bounded borrowed-string-view helper to remove duplication and repeated registry probes. |
| ZRT-073 | Open | P0 | OrderedMap, LRU, TreeMap, and remaining map variants still contain retain/release sequences whose behavior under an embedder trap hook returning is not covered uniformly. | Complete a function-by-function transaction audit and add allocation/retain fault sweeps for replacement, remove, snapshot, clone, and finalization. |
| ZRT-074 | Open | P1 | Immutable map/set snapshot builders allocate managed sequences one element at a time and recovery coverage differs by operation. | Introduce one trap-safe snapshot builder with pre-reserve, ownership transfer, and a single cleanup path. |
| ZRT-075 | Open | P0 | Frozen collection element extraction treats non-string sequence elements as boxed values according to caller convention, but validation is not centralized. | Validate the expected boxed/object representation before dereference and add forged-element tests for every constructor. |
| ZRT-076 | Measure | P2 | `rt_weakmap_len` determines liveness by walking table capacity rather than maintaining an exact live count as weak targets die. | Add a lazily reconciled live/dead counter or generation queue, then benchmark high-capacity mostly-dead maps. |
| ZRT-077 | Open | P1 | WeakMap liveness checks and weak-zero callbacks can interact with growth/compaction and cached entry state. | Specify one synchronization/liveness transition protocol and stress callback, Set, Get, Compact, and finalization concurrently. |
| ZRT-078 | Measure | P2 | WeakMap probing recomputes hashes/string metadata for retained keys during move/match operations. | Cache the key hash and validated byte length in each entry and reuse them during growth and probes. |
| ZRT-079 | Open | P1 | PriorityQueue and comparator-driven sorts can invoke user/runtime comparators after internal mutation has begun. | Stage heap/sort state so a comparator trap leaves the collection valid, and add returning-trap comparator tests. |
| ZRT-080 | Measure | P2 | Collection conversion and snapshot helpers repeatedly append without consistently reserving known source length. | Pre-reserve checked source cardinality in `rt_convert_coll.c`, snapshots, and functional Seq operations; measure allocation-count reduction. |

## Files, streams, SaveData, watchers, archives, and compression

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-081 | Fixed-WT | P0 | `archive_open_parent_for_file_posix` accepted null `name`/`out_leaf`, then used them while splitting a normalized entry. | Reject either null argument, clear the required output before fallible work, and return immediately after the trap. |
| ZRT-082 | Fixed-WT | P0 | `savedata_require` checked only a class identifier through a direct object read and callers often dereferenced its result after a returning trap. | Use sized `rt_obj_is_instance` validation and make every caller branch on the returned pointer. |
| ZRT-083 | Fixed-WT | P0 | SaveData key/value helpers could return sentinel text after trapping, allowing Set/Get/Remove to continue with invalid string handles. | Distinguish null/invalid strings, return status/null, and stop before any length, UTF-8, or entry-list access. |
| ZRT-084 | Fixed-WT | P0 | New SaveData string entries retained key then value; a returning second retain left ambiguous partial ownership. | Make pair retain status-returning, initialize outputs, roll back either reference, and require success before node allocation. |
| ZRT-085 | Fixed-WT | P1 | POSIX/Windows `fdopen` failure cleanup overwrote the original conversion error with the descriptor-close result. | Preserve `errno` across cleanup so callers receive the actual stream-construction failure. |
| ZRT-086 | Fixed-WT | P0 | `ensure_parent_dir` created its runtime string outside the recovery scope and unconditionally unreferenced it. | Bring allocation under local recovery, keep the reference volatile across `setjmp`, and release only when publication succeeded. |
| ZRT-087 | Fixed-WT | P1 | SaveData setters ignored allocation/retain failure from entry helpers and could report success through normal return. | Propagate helper status and raise a deterministic allocation failure without mutating the old entry. |
| ZRT-088 | Fixed-WT | P1 | SaveData Load did not require an exact `fread` result and size validation did not cover the allocation terminator arithmetic explicitly. | Reject empty/unrepresentable sizes, allocate checked `size + 1`, and fail transactionally on every short read. |
| ZRT-089 | Fixed-WT | P0 | Zstd FSE table construction did not prove normalized counts exactly filled the active table before cells were consumed. | Validate signed counts and checked aggregate equality, then clear the active cells before distribution. |
| ZRT-090 | Fixed-WT | P0 | Zstd Huffman construction accepted a derived zero-bit table and had incomplete bounds on the maximum bit width. | Require `1 <= max_bits <= ZSTD_MAX_HUF_BITS` before generating decode cells. |
| ZRT-091 | Fixed-WT | P0 | Zstd parsing repeatedly used `offset + length > capacity`, which can wrap before the comparison on hostile frames. | Replace additions with `used <= capacity && add <= capacity - used` throughout literals, sequences, blocks, headers, and checksum parsing. |
| ZRT-092 | Fixed-WT | P0 | Zstd zero-length decode paths still formed output pointers or called `memcpy`/`memset` on possibly null storage. | Guard zero-length copies/fills and avoid pointer arithmetic unless at least one byte is produced. |
| ZRT-093 | Open | P1 | SaveData Load accepts any file size below `SIZE_MAX`, then allocates that entire size plus a runtime-string copy. | Introduce configurable default/hard byte ceilings, checked before allocation, with boundary and sparse-file tests. |
| ZRT-094 | Measure | P2 | SaveData stores entries in a singly linked list; Set/Get/Has/Remove are O(number of keys). | Add a string-hash index while retaining a stable serialization list, and benchmark realistic save cardinalities. |
| ZRT-095 | Open | P2 | `rt_savedata_count` walks the complete entry list on every call. | Maintain a checked entry count updated transactionally on insert/remove/clear/load. |
| ZRT-096 | Open | P2 | SaveData serializes current linked-list order, which depends on mutation/load history and makes equivalent states byte-different. | Define deterministic bytewise key ordering for persistence and add reproducible-output tests. |
| ZRT-097 | Measure | P2 | SaveData Save builds the complete JSON document in `rt_string_builder` before the atomic writer copies it to a staged file. | Add a streaming staged-file JSON encoder with the same atomic replacement and trap rollback guarantees. |
| ZRT-098 | Measure | P2 | SaveData Load copies file bytes into a native buffer, then copies again into a runtime string before streaming tokenization. | Let the JSON stream parser consume a bounded borrowed byte span or incremental file source, subject to an ADR if its public surface changes. |
| ZRT-099 | Open | P1 | Repeated JSON object keys are silently resolved by repeated `savedata_set_*_entry` replacement. | Specify reject-versus-last-wins behavior explicitly and add duplicate-key tests; rejecting is safer for corrupted save detection. |
| ZRT-100 | Open | P1 | SaveData has no independent limits for entry count, key length, value length, or total decoded string bytes. | Add checked parser budgets so many tiny entries or one huge decoded value cannot exhaust memory within the file-byte ceiling. |
| ZRT-101 | ADR | P1 | SaveData has no synchronization or generation check; concurrent Set/Save/Load can traverse and replace the same linked list. | Define thread-safety in an ADR, then use snapshots/versioning or a per-object lock with I/O outside the lock. |
| ZRT-102 | Open | P1 | Atomic Save can complete rename but fail the final parent-directory durability step, leaving the Boolean result unable to say whether the new file is already visible. | Document the commit point, preserve a distinct internal “committed but durability uncertain” result, and add syscall fault-injection coverage. |
| ZRT-103 | Fixed-WT | P1 | `rt_file_write` returned success for `len == 0` before validating the `RtFile` descriptor, unlike non-empty writes. | Validate the handle first so a closed/invalid channel cannot be reported as a successful operation merely because the payload is empty. |
| ZRT-104 | Fixed-WT | P1 | `rt_binfile_write` made one `fwrite` call and trapped on a short result without retrying a recoverable partial write. | Loop until complete or `ferror`, with checked addressability and defensive zero-progress termination. |
| ZRT-105 | Open | P1 | `rt_binfile_size` temporarily seeks; when end-position acquisition or restoration fails, the original logical position/state may be lost and one restore failure is ignored. | Centralize save/restore cleanup, report both primary and restoration failure, and invalidate position state if restoration cannot be guaranteed. |
| ZRT-106 | ADR | P2 | `rt_binfile_pos` and `rt_binfile_size` call `binfile_prepare_seek`, which flushes pending output, so nominal queries have I/O/error side effects. | Specify this behavior or separate logical buffered position from durability-changing flush in an ADR. |
| ZRT-107 | ADR | P2 | `rt_memstream_clear` intentionally retains peak capacity and there is no Trim/Shrink operation for one-off large buffers. | Add an explicit trim API with a checked target-capacity contract and document whether cursor/length can constrain shrinking. |
| ZRT-108 | Open | P1 | MemStream permits seeking to any nonnegative `int64_t`; the next small write allocates and zero-fills the entire sparse gap. | Add a configurable stream byte ceiling and reject writes whose resulting length exceeds it before allocation/zero fill. |
| ZRT-109 | Open | P1 | MemStream float encoders assume 32-bit IEEE binary32 and 64-bit IEEE binary64 representations. | Add compile-time representation/size assertions and a documented unsupported-platform diagnostic. |
| ZRT-110 | Measure | P2 | File-backed ZPAK reads serialize seek/read through one `zpak_read_lock_t`, while each entry has an independent validated offset/range. | Use descriptor-offset reads (`pread` or the Windows overlapped equivalent in adapters) to permit parallel entry reads without shared file-position locking. |

## Networking, HTTP, TLS, SSE, WebSocket, and HTTP/2

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-111 | Fixed-WT | P0 | TLS `send_record` accepted a null session or an invalid socket far enough to enter record construction/native send. | Reject both states before allocation or field access and return typed invalid/closed results. |
| ZRT-112 | Fixed-WT | P0 | TLS `recv_record` did not initialize outputs on all errors, and graceful disposal could attempt close-notify on an invalid descriptor. | Validate every pointer/socket, zero outputs first, and gate graceful network I/O on a live socket. |
| ZRT-113 | Fixed-WT | P0 | SSE transport helpers could dereference a null receiver/data pointer or operate on `INVALID_SOCK`; several state assignments were overwritten before observation. | Validate receiver/data/length/socket at the transport boundary and remove dead stores that obscured the state machine. |
| ZRT-114 | Fixed-WT | P0 | HTTP client default-header application allocated, retained, and called request setters while holding partial snapshots; returning traps leaked or continued. | Make `apply_defaults` status-returning with transactional key/value snapshots, lock cleanup, and immediate caller exit. |
| ZRT-115 | Fixed-WT | P0 | Cookie request-header construction had fallible URL accessors, native growth, and managed header publication without one recovery owner. | Give `apply_cookie_header` a recovery state that owns URL parts, native header, mutex state, and managed values until publication. |
| ZRT-116 | Fixed-WT | P0 | Response-cookie parsing could leave staged cookie mutations/resources after accessor, header, allocation, or commit traps. | Stage the complete response cookie transaction and commit only after all fallible parsing/ownership steps succeed. |
| ZRT-117 | Fixed-WT | P0 | `do_request` assumed traps never return and continued across request clone, defaults, cookie, send, response, and redirect operations. | Enclose the redirect transaction in local recovery, check every status/result, and release request/response/URL state on one failure path. |
| ZRT-118 | Fixed-WT | P0 | HTTP client cookie snapshotting trusted allocation/count results and could publish a partial Map after a returning trap. | Validate snapshot counts, stage native copies, and consume every partial managed/native result before returning null. |
| ZRT-119 | Fixed-WT | P0 | `rt_http_server_new` could publish a partially initialized server when object, mutex, or router setup trapped and returned. | Track initialized components, destroy only those components, and release the partial server before returning. |
| ZRT-120 | Fixed-WT | P0 | `rt_server_res_json` modified response state through fallible body/header cloning and insertion without preserving the prior response on failure. | Build body and replacement headers off-object, verify each insertion, then swap the completed response transactionally. |
| ZRT-121 | Fixed-WT | P0 | Multipart Build could copy into a missing/invalid Bytes allocation after a returning trap. | Check result and writable storage before `memcpy`, with local recovery releasing the partial result. |
| ZRT-122 | Fixed-WT | P1 | Generic ConnectionPool performed `select`/`MSG_PEEK` health checks while holding its bookkeeping mutex. | Reserve and retain a matching TCP under lock, probe outside it, and safely remove an unhealthy reservation after reacquiring. |
| ZRT-123 | Fixed-WT | P1 | HTTP ConnectionPool health probes and transport closure ran under the pool mutex, creating a readiness/close convoy. | Detach the candidate under lock and do health/close work outside, with slot validation on reentry. |
| ZRT-124 | Fixed-WT | P0 | A concurrent HTTP pool Clear could be followed by release of an older checked-out lease, repopulating the cleared pool. | Increment a pool generation on Clear and only return a connection when its captured generation still matches. |
| ZRT-125 | Fixed-WT | P1 | HTTP pool release allocated an origin key and probed transport health in lock-sensitive code. | Precompute/probe outside the mutex and keep the locked section to validated slot/key publication only. |
| ZRT-126 | Fixed-WT | P0 | HTTP response/status parsing read incompletely initialized line storage and insufficiently bounded status fields; `has_header` assumed every entry name was non-null. | Zero-initialize line buffers, initialize status outputs, require minimum syntax/length, and skip malformed null-name entries. |
| ZRT-127 | Fixed-WT | P0 | URL query decoding could release its output Map and then continue inserting when string/value construction trapped and returned. | Stop and return null after releasing the partial Map on any key/value/decode publication failure. |
| ZRT-128 | Fixed-WT | P0 | HTTP/2 I/O callbacks could claim more bytes than requested or claim success without initializing the destination; frame parsing then consumed invalid bytes. | Reject oversized callback counts and use deterministic initialized frame storage, with missing/oversized/unwritten callback regressions. |
| ZRT-129 | Fixed-WT | P1 | HTTP request timeout was installed as a per-socket-operation timeout, so a peer could continuously trickle bytes and keep the full response alive indefinitely. | Public request/download entry now creates one saturating monotonic deadline carried through connection setup, TLS records, HTTP/1 and HTTP/2 I/O, redirects, and response transforms; focused body, redirect, and TLS-trickle regressions enforce it. |
| ZRT-130 | Fixed-WT | P1 | Plain HTTP `http_create_tcp_socket` applied `timeout_ms` independently to each resolved address, unlike the TLS/SSE shared-budget paths. | Resolution and every address candidate now share the request's absolute deadline, with a freshly rounded remaining budget before each connection attempt. |
| ZRT-131 | Fixed-WT | P1 | HTTP gzip decoding enforced its 256 MiB ceiling only after managed gunzip output existed, copied the payload through managed/native intermediates, and had no expansion-ratio budget. | The ADR-0229 native decoder now bounds aggregate growth by an absolute limit plus caller-selected ratio/slack, adopts the first member allocation, and lets HTTP decode directly under 256 MiB and 128:1 plus 1 MiB limits; focused native and loopback bomb regressions cover the policy. |
| ZRT-132 | Measure | P2 | HTTP pool capacity is one fixed array shared by all origins; one high-cardinality origin set can evict or occupy every slot. | Add per-origin idle quotas/fair eviction and benchmark mixed-origin workloads. |
| ZRT-133 | Fixed-WT | P1 | `http_conn_pool_evict_idle_locked` reset expired entries while holding the mutex, and reset could cascade into TLS/socket teardown; generic pool expiry and clear/release paths similarly closed or released TCP under lock. | HTTP and generic pools now move compact ownership records under the bookkeeping mutex, preserve exclusive close claims across unlock, and perform transport teardown plus managed-reference release afterward. |
| ZRT-134 | Measure | P2 | HTTP request sending serializes headers plus the complete body into one contiguous `request_buf` before transport send. | Send headers and body as checked segments, or use adapter scatter/gather where available, to avoid body-sized duplication. |
| ZRT-135 | Fixed-WT | P1 | Streaming HTTP download previously risked exposing partial destination content after an error, redirect failure, or length mismatch. | The public path-based helper exclusively creates a randomized same-directory sibling, streams and syncs it, preserves ordinary replacement permissions, atomically renames it, and removes staged content on every pre-publication failure; focused download tests cover success and trapped cleanup. |
| ZRT-136 | Measure | P2 | TLS `send_record` allocates and securely clears maximum-sized record and plaintext buffers per record; `recv_record` allocates each payload. | Move bounded record scratch buffers into the session or a secure reusable arena and benchmark throughput/latency. |
| ZRT-137 | Open | P3 | Many TLS socket/protocol failures collapse to static strings such as “send failed” or “decryption failed,” losing native/protocol context. | Store structured error category, alert, handshake state, and native code while preserving the compatibility string accessor. |
| ZRT-138 | Open | P1 | The from-scratch TLS handshake/record parser has broad state and hostile-input exposure but limited differential transcript fuzzing. | Add deterministic record/handshake fuzz targets and compare accepted/rejected transcripts against independent RFC test corpora without product dependencies. |
| ZRT-139 | Measure | P2 | Certificate parsing/path verification is repeated for new sessions to the same endpoint/trust generation. | Cache only verified immutable chain results keyed by hostname, chain fingerprint, policy, and trust-store generation, with bounded expiry. |
| ZRT-140 | Open | P2 | SSE reconnect uses server retry delay but does not add randomized jitter, allowing many clients to reconnect simultaneously after an outage. | Apply bounded jitter and exponential failure backoff while respecting server-provided minimums and the caller's overall deadline. |
| ZRT-141 | Measure | P2 | SSE raw and event line readers allocate/reallocate/free native buffers for successive lines on a long-lived stream. | Retain bounded reusable line/event buffers in the SSE object and trim pathological peaks after dispatch. |
| ZRT-142 | Open | P1 | WebSocket broadcast snapshots clients but sends to them sequentially; one slow client's blocking send delays every later target despite the source comment claiming otherwise. | Queue per-client outbound frames or impose a strict per-client broadcast deadline, then continue other clients independently. |
| ZRT-143 | ADR | P2 | HTTP/2 connection operations synchronously service one request/stream flow and do not provide a dispatcher for concurrent stream multiplexing. | Specify connection/stream concurrency and cancellation in an ADR, then add a reader dispatcher and per-stream state queues. |
| ZRT-144 | Measure | P2 | HPACK dynamic-table lookup is linear, insertion memmoves the entire entry array, and each name/value is separately allocated. | Use a bounded ring for RFC index order plus hash indexes/arena storage; benchmark realistic header reuse. |
| ZRT-145 | Open | P1 | HTTP server accept/worker paths have fixed capacity but limited explicit overload policy and observability for queue rejection, slow clients, and saturation. | Define backpressure/rejection behavior and expose accepted, active, queued, rejected, timeout, and parse-limit counters. |

## Audio and localization

| ID | State | Pri | Source evidence | Recommendation |
|---|---|---:|---|---|
| ZRT-146 | Fixed-WT | P1 | Every audio FX Add allocated an effect before `append_fx` rejected an invalid group, leaking the node and any delay/reverb buffers. | Validate the group before allocation and make `append_fx` consume/free the node on every failure path. |
| ZRT-147 | Fixed-WT | P1 | `g_next_fx_id++` could invoke signed overflow and wrap to an identifier still owned by a live effect. | Advance without overflowing, mark wrap, and scan for an unused positive identifier before reuse. |
| ZRT-148 | Fixed-WT | P0 | Locale constructors assumed OOM traps never return and dereferenced null from `loc_alloc`. | Check allocation in invariant, canonical-tag, parse, and from-parts constructors; add returning-trap allocation regressions. |
| ZRT-149 | Open | P1 | Audio FX processing holds `g_fx_lock` through the complete DSP block; control operations spin, while mixer contention bypasses the entire effect chain for that block. | Publish immutable/refcounted effect-chain snapshots or use an audio-epoch handoff so the callback never locks or audibly drops processing. |
| ZRT-150 | Open | P1 | `rt_audio_fx_process_group` ignores `sample_rate`; biquad, delay, and reverb state is designed at a fixed 44.1 kHz. | Store design parameters and rebuild coefficients/delay lengths when the backend rate changes; test 44.1, 48, 96, and 192 kHz. |
| ZRT-151 | Measure | P2 | `mp3_file_to_wav` reads the complete encoded file into memory before decoding, in addition to decoder PCM and final WAV storage. | Feed the decoder from a bounded file reader and avoid the encoded-file-sized allocation; retain the 256 MiB source ceiling. |
| ZRT-152 | Open | P1 | Ogg `packet_append` is overflow-checked but has no packet-byte ceiling, and the stream-state/queued-packet lists have no count/aggregate budget. | Enforce per-packet, logical-stream, queued-packet, and total buffered-byte limits before growth. |
| ZRT-153 | Measure | P2 | Ogg/MP3 decoding accumulates a complete PCM buffer, then `build_wav_from_pcm` allocates and copies a second full audio payload. | Allocate the final WAV once when size is known, or stream PCM into a growable WAV payload with one bounded backing buffer. |
| ZRT-154 | Measure | P2 | MusicGen renders a full 32-bit stereo accumulator, converts it into a second full 16-bit PCM buffer, then copies that into WAV output. | Down-convert/soft-clip directly into the final WAV allocation after accumulation, eliminating the intermediate PCM buffer. |
| ZRT-155 | Measure | P2 | LocaleManager lookup, initialization matching, register, and unregister paths linearly scan `g_mgr.entries` by canonical tag. | Add a hash index keyed by canonical tag while preserving the registry array for deterministic enumeration. |
| ZRT-156 | Measure | P2 | `tz_rule_at` linearly walks every sorted transition from the beginning for each timestamp query. | Use upper-bound binary search and cache the last transition index for nearby sequential formatting. |
| ZRT-157 | ADR | P0 | MessageBundle `as_bundle` is an unchecked cast and `bundle_alloc` performs fallible locale/map retains after allocation without complete rollback. | Give localization object kinds stable class IDs in an ADR, validate sized handles, and make constructor/fallback publication transactional. |
| ZRT-158 | ADR | P0 | ListFormat, RelativeTimeFormat, NumberFormat, DateFormat, and related classes also use unchecked `as_fmt` casts before field access. | Introduce shared class-specific validators and forged-handle/returning-trap tests for every public formatter entry point. |
| ZRT-159 | Measure | P2 | Each MessageBundle locale fallback lookup rebuilds the Locale fallback list and repeatedly allocates prefixed `tag:key` strings. | Cache the immutable fallback-tag chain per Locale and probe with stack/borrowed composite keys or a two-level map. |
| ZRT-160 | Open | P0 | `rt_locale_manager_retain_data` uses unchecked signed `fetch_add`; refcount exhaustion can wrap before release detects underflow. | Implement non-wrapping try-retain semantics, propagate failure transactionally, and test the maximum boundary. |

## Validation record

No full rebuild and no full `ctest` run were performed, per the concurrent-work
constraint. Focused validation completed during this review includes:

- Clang static-analyzer passes for the modified compression, TLS, SSE, HTTP
  client/server, multipart, HTTP connection-pool, generic connection-pool, and
  HTTP/2 translation units.
- Targeted builds and direct runs of `test_rt_network`,
  `test_rt_network_timeout`, `test_rt_network_harden`,
  `test_rt_network_highlevel`, `test_rt_compress`,
  `test_rt_http_client`, `test_rt_http_server`, `test_rt_https_server`,
  `test_rt_multipart`, `test_rt_http2`, `test_rt_errors_io`, and
  `test_rt_binfile`, plus the focused `test_runtime_surface_audit`.
- Focused collection/runtime tests covering pool and GC boundaries, handle
  validation, scheduler/concurrent-map behavior, BinaryBuffer, Bytes,
  BloomFilter, Trie, SaveData, Locale, Audio FX, string/version behavior, and
  the modified collection families.
- Strict syntax/static-analysis checks on the modified collection, SaveData,
  localization, audio, compression, and networking units.

The final handoff for each implementation tranche must still run the narrowest
relevant test label/executables and `./scripts/lint_platform_policy.sh` when the
change is cross-platform-sensitive. The canonical build and broad test lanes
remain deferred until the concurrently changing tree is ready for them.

## Recommended implementation order

1. Finish P0 returning-trap and forged-handle coverage, especially ZRT-010,
   ZRT-073, ZRT-075, ZRT-157, ZRT-158, and ZRT-160.
2. Add SaveData byte/entry/string budgets before its indexing/streaming work
   (ZRT-093 and ZRT-100).
3. Continue hostile-network hardening with TLS transcript fuzzing, independent
   WebSocket broadcast progress, and server overload policy (ZRT-138, ZRT-142,
   and ZRT-145).
4. Remove audio-callback locking and sample-rate assumptions (ZRT-149 and
   ZRT-150).
5. Establish microbenchmarks before sharding registries, redesigning scheduler
   and ThreadPool queues, or changing collection/codec storage.
