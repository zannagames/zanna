---
status: active
audience: contributors
last-verified: 2026-07-26
---

# Runtime Hardening Audit — July 2026

This ledger records the resolution of the 64 runtime correctness, ownership,
concurrency, performance, and I/O findings identified in the July 2026 deep
review. The finding numbers are stable: tests, documentation, and future
regression reviews should refer to them rather than renumbering the list.

All changes preserve the existing language surface. New public runtime entries
are additive, and every C ABI change is governed by an ADR. Runtime code remains
dependency-free and uses the shared platform capability layer outside approved
OS adapters.

## Memory management and arrays

| # | Finding | Resolution | Primary regression coverage |
|---:|---|---|---|
| 1 | Releasing one shared string-array alias could release elements still observed by another alias. | `rt_arr_str_release` now defers element release until the shared allocation's final alias is released. | `RTArrayStrTests.cpp` shared-alias release case |
| 2 | Weak references to specialized arrays could dangle after reclamation or relocation. | GC weak processing now recognizes every managed array kind, clears reclaimed targets, and follows relocated object arrays. | `RTGCTests.cpp` specialized-array clear and relocation cases |
| 3 | Cycle collection raced managed graph mutation because it was not stop-the-world. | Managed graph mutation uses a shared quiescence barrier; collectors acquire it exclusively before traversal and reclamation. | `RTGCTests.cpp` concurrent mutator/collector stress |
| 4 | Reference arrays were omitted from automatic cycle tracking. | Object and string reference arrays are registered as managed graph nodes with explicit traversal behavior. | `RTGCTests.cpp` reference-array cycle cases |
| 5 | Channels, concurrent maps/queues, promises, and futures owned managed values without participating in cycle collection. | Each owning concurrent object is GC-tracked, exposes lock-safe traversal, and brackets edge mutation with the graph barrier. | `RTChannelTests.cpp`, `RTConcMapTests.cpp`, `RTConcQueueTests.cpp`, `RTFutureTests.cpp` cycle cases |
| 6 | Heap metadata lookup returned an unpinned registry entry that could be concurrently invalidated. | `rt_heap_get_info` returns a lock-bounded value snapshot; exact and interior range checks remain under the registry lock for the complete query. | `RTCoreOwnershipTests.cpp`, `RTGCTests.cpp` |
| 7 | Heap reallocation did not reject shared allocations. | `rt_heap_realloc` requires an exact allocation with reference count one and preserves the original allocation on rejection/failure. | `RTCoreOwnershipTests.cpp` shared-reallocation case |
| 8 | Pool shutdown could free slabs while interior blocks were still outstanding. | Slabs carry aligned private headers and are reclaimed only when their class is provably empty. Active operations are protected by a lifecycle epoch. | `RTPoolTests.cpp` outstanding-block shutdown case |
| 9 | Pool double-free corrupted the free list. | Per-block atomic allocation state detects and traps double frees before list publication. | `RTPoolTests.cpp` double-free trap case |
| 10 | Pool free scanned all slabs. | Aligned slab headers provide constant-time owner lookup from any block address. | `RTPoolTests.cpp` multi-slab ownership cases |
| 11 | Pool operations serialized on a global hot lock and zeroed allocations twice. | Class-local atomic free lists replace the hot global lock; lifecycle synchronization is limited to epoch entry/exit, and allocation performs one zeroing pass. | `RTPoolTests.cpp` concurrent allocation/shutdown stress |
| 12 | Exact-capacity object-array growth made repeated append quadratic. | Object arrays use checked geometric capacity growth while preserving logical length and ownership semantics. | `RTListTests.cpp` large append/capacity case |
| 13 | `List.RemoveAt` shrank storage on every removal. | Non-empty lists retain capacity after removal; the empty state may return to the minimum representation. | `RTListTests.cpp` remove-without-shrink case |
| 14 | Object-array assignment ownership differed between registry metadata and implementation. | The runtime registry now declares the retain/release contract used by object-array stores, with compile-time/runtime ownership assertions. | `RTCoreOwnershipTests.cpp` and runtime surface audits |
| 15 | GC garbage-set membership used repeated linear scans. | Collection builds an allocation-free open-addressed membership set, making edge classification constant-time on average. | `RTGCHashTableTests.cpp`, `RTGCTests.cpp` |
| 16 | Shutdown could skip finalizers when collection bookkeeping allocation failed. | Shutdown finalization uses an allocation-free sweep path and cannot abandon finalizers because of bookkeeping OOM. | `RTShutdownTests.cpp` allocation-failure finalizer case |
| 17 | Automatic collection ran synchronously inside allocation. | Allocation only records deferred collection pressure; explicit safe points service pending work outside allocator critical paths. | `RTGCHashTableTests.cpp`, `RTGCTests.cpp` auto-trigger cases |

These decisions are specified by
[ADR 0116](../adr/0116-gc-mutator-quiescence-and-array-cycles.md) and
[ADR 0133](../adr/0133-runtime-concurrency-and-collection-hardening.md).

## Contexts, threads, synchronization, and collections

| # | Finding | Resolution | Primary regression coverage |
|---:|---|---|---|
| 18 | A child could start after its inherited context was cleaned up. | Thread creation reserves a binding before publishing the native thread; the child adopts it or the parent cancels it transactionally on creation failure. | `RTThreadsThreadTests.cpp`, `RTShutdownTests.cpp` |
| 19 | Inherited context state was mutable without synchronization and partly split between TLS and shared storage. | RNG, module variables, arguments, file state, lifecycle state, and type registry state have per-context recursive locks; trap unwinding releases held context locks. | `RTConcurrencyTests.cpp`, `RTThreadSafetyTests.cpp` |
| 20 | Unbound native threads could concurrently mutate the same legacy context. | Legacy-context resolution is synchronized through the lifecycle handoff and the same subsystem locks used by bound contexts. | `RTConcurrencyTests.cpp` unbound-thread cases |
| 21 | Context binding was published before legacy-state migration completed. | First binding migrates protected state before publishing TLS ownership or a runnable child. | `RTConcurrencyTests.cpp`, `RTShutdownTests.cpp` |
| 22 | Last-unbind migration raced readers and cleanup could race a new bind. | Last-unbind migration holds subsystem locks. A `READY -> CLEANING -> UNINITIALIZED` lifecycle state machine is changed under the handoff lock, and all bind/reservation paths validate `READY` there. | `RTShutdownTests.cpp` repeated bind/cleanup race |
| 23 | POSIX detach could run twice. | Detach ownership is claimed with a compare/exchange state transition before `pthread_detach`. | `RTThreadsThreadTests.cpp` concurrent detach case |
| 24 | Windows threads bypassed CRT initialization with `CreateThread`. | The Windows adapter uses `_beginthreadex` and closes the resulting native handle exactly once. | Windows thread CTests and cross-platform smoke |
| 25 | Runtime thread callbacks used non-portable data/function-pointer casts. | Thread and pool APIs use typed callback functions; raw ABI adapters transfer bits with `memcpy` only at the compatibility boundary. | `RTThreadsThreadTests.cpp`, `RTThreadPoolTests.cpp` |
| 26 | Completing a completed promise could fall through after a returning trap. | Completion exits immediately after the duplicate-completion trap path and cannot mutate the settled state. | `RTFutureTests.cpp` duplicate completion case |
| 27 | Promise listener continuations could form uncollectable Promise/Future cycles. | Promise and Future nodes are GC-tracked and traversal includes values, errors, and listener continuations. | `RTFutureTests.cpp` continuation-cycle case |
| 28 | Promoting the cached Future weak pointer raced finalization. | Promotion validates liveness and retains under synchronized state before publishing the result. | `RTFutureTests.cpp` cached-Future race stress |
| 29 | POSIX Future waits ignored condition-variable errors. | Every lock and wait result is checked and converted to a deterministic runtime trap; timed waits use one clock domain. | `RTFutureTests.cpp` wait/error paths |
| 30 | Promise listener append was linear. | Promise state maintains a listener tail pointer, making append constant-time. | `RTFutureTests.cpp` ordered high-count listeners |
| 31 | Thread-pool submission retained task arguments while holding the monitor. | Arguments are retained before entering the monitor, with trap-safe rollback on rejection. | `RTThreadPoolTests.cpp` ownership/rejection cases |
| 32 | Thread-pool construction leaked partial state on failure. | Construction is transactional across monitor, worker-array, native-thread, and managed-object setup. | `RTThreadPoolTests.cpp` injected construction failures |
| 33 | Concurrent thread-pool shutdown callers could return before shutdown completed. | One caller wins shutdown election; all others wait for the same completed terminal state. | `RTThreadPoolTests.cpp` concurrent shutdown case |
| 34 | Last release from a worker could strand self-finalization work. | An exiting worker can claim cleanup, detach queued tasks, release the resurrection claim, skip self-join, and schedule/fallback cleanup atomically. | `RTThreadPoolTests.cpp` worker-finalization fallback case |
| 35 | Thread-pool GC traversal ignored queued task arguments. | Pool traversal visits every queued managed argument while queue mutation is quiesced. | `RTThreadPoolTests.cpp` queued-argument cycle case |
| 36 | Monitor initialization failures were unchecked. | Native monitor setup is transactional and traps after rolling back every initialized component. | `RTThreadsMonitorTests.cpp` construction/error cases |
| 37 | Monitor waits ignored native failures. | POSIX and Windows adapters validate all wait results and distinguish timeout from native failure. | `RTThreadsMonitorTests.cpp` timed-wait cases |
| 38 | Monitor lookup used a single contended global table lock. | Monitor bookkeeping is partitioned into lock stripes, limiting unrelated-object contention. | `RTThreadsMonitorTests.cpp` many-object stress |
| 39 | Channel timeouts began after monitor acquisition. | The deadline is captured before acquisition; every wait receives only the remaining total budget. | `RTChannelTests.cpp` lock-contention timeout case |
| 40 | Channel timeout calculations mixed clocks. | Deadline and waits use a single monotonic clock (or the platform's equivalent relative wait). | `RTChannelTests.cpp` timed send/receive cases |
| 41 | `try_recv_option` leaked an item if Option construction trapped. | Received values remain owned until Option publication succeeds; the trap rollback releases the transferred value. | `RTChannelTests.cpp` injected Option-allocation failure |
| 42 | Null channel outputs had inconsistent consuming behavior. | Null output now means a non-consuming readiness probe. The additive `rt_channel_recv_for_discard` API explicitly consumes and releases one value. | `RTChannelTests.cpp` probe/discard cases |
| 43 | Map and IntMap growth could corrupt state after allocation failure. | Rehash builds complete replacement tables before atomically swapping state; the original table remains usable after OOM. | `RTMapTests.cpp`, `RTIntMapTests.cpp` injected resize failures |
| 44 | Bag insertion conflated duplicate and OOM results. | Duplicate remains a normal false result; allocation failure traps without mutating the Bag. | `RTBagTests.cpp` duplicate and injected OOM cases |
| 45 | Rehash recomputed hashes, fragmented allocations, and used imprecise floating thresholds. | Nodes cache hashes, replacement buckets are allocated as one table, and overflow-safe integer load calculations are shared in `rt_hash_table_util.h`. | `RTMapTests.cpp`, `RTIntMapTests.cpp`, `RTBagTests.cpp` |
| 46 | Map and IntMap could never release excess bucket capacity. | Additive `Map.Trim` and `IntMap.Trim` rebuild to the smallest geometric capacity satisfying the load policy. | `RTMapTests.cpp`, `RTIntMapTests.cpp`, generated runtime-reference contract |

The context and synchronization contracts are specified by
[ADR 0136](../adr/0136-runtime-context-binding-lifecycle-and-state-locks.md);
the collection and concurrent-owner contracts are specified by
[ADR 0133](../adr/0133-runtime-concurrency-and-collection-hardening.md).

## I/O, archives, assets, streams, watchers, and tooling

| # | Finding | Resolution | Primary regression coverage |
|---:|---|---|---|
| 47 | ZIP duplicate-name validation was quadratic. | Archive validation builds a hash index while parsing and rejects duplicates during insertion. | `RTArchiveTests.cpp` large duplicate/name-index cases |
| 48 | A corrupt lazily loaded asset could leave its manager permanently blocked. | First-use loading has explicit `unloaded/loading/loaded` states; all trap/error exits reset state and wake waiters. | `RTAssetTests.cpp` corrupt concurrent first-use case |
| 49 | The asset-manager mutex serialized archive I/O. | Callers retain a stable archive snapshot under the lock, perform I/O outside it, and atomically publish first-use state. | `RTAssetTests.cpp` 24-thread parallel load case |
| 50 | Atomic archive overwrite replaced destination permissions. | Replacement captures existing mode bits, applies them to the staged file, syncs/closes it, and then atomically renames it. | `RTArchiveTests.cpp` permission-preservation case |
| 51 | File close left a stale descriptor available to reentrant code. | The runtime marks the descriptor invalid before invoking the OS close operation. | `ErrorsIoTests.c` close/error/reentry cases |
| 52 | Archive parsing and extraction lacked resource budgets. | Validation enforces entry-count, metadata, name, expanded-size, ratio, and cumulative-output limits before allocation/extraction. | `RTArchiveTests.cpp` budget boundary and rejection cases |
| 53 | Extraction leaked partial files or managed values after a returning trap. | Extraction owns staged files and values through trap recovery, closes/removes partial output, then rethrows the original trap. | `RTArchiveTests.cpp` injected extraction trap cases |
| 54 | ZPAK trusted entry count before proving the TOC could contain it. | Reader validates header/TOC sizes and a version-specific minimum entry footprint before allocating metadata. | `TestZpakFormat.cpp` impossible-count cases |
| 55 | ZPAK ignored version and flag compatibility. | Versions 1 and 2 are explicit; unknown versions, required-flag omissions, and unknown header/entry bits are rejected. | `TestZpakFormat.cpp` version/flag matrix |
| 56 | ZPAK payloads had no integrity checksum. | ZPAK v2 stores CRC-32 per entry; the reader verifies stored output before publication while retaining v1 read compatibility. | `TestZpakFormat.cpp` checksum corruption and v1 compatibility |
| 57 | `Stream.Read` could allocate an attacker-sized buffer before learning available data. | Reads clamp requested allocation to the stream's known remaining bytes and the runtime maximum before allocation. | `RTStreamTests.cpp` oversized-request case |
| 58 | `MemStream.Clear` wiped unused capacity and forced future regrowth. | Clear resets logical length/position while retaining capacity; extending after a seek zeroes newly observable gaps only. | `RTMemStreamTests.cpp` retained-capacity and gap-zero cases |
| 59 | Watcher backend errors were logged without a recoverable event. | A native backend failure publishes the existing overflow/rescan marker so callers know their view may be stale. | `RTWatcherTests.cpp` backend-error mapping cases |
| 60 | Watcher polling, stopping, and finalization could race native state. | Public watcher operations enforce creator-thread affinity; finalization is the documented exception and performs synchronized cleanup. | `RTWatcherTests.cpp` wrong-thread Poll/Stop cases |
| 61 | `LineReader.ReadAll` restarted from the beginning and mishandled a peeked byte. | ReadAll starts at the current logical cursor and consumes the staged PeekChar byte exactly once. | `RTLineReaderTests.cpp` seek/peek/read-all cases |
| 62 | HTTP router lock initialization failure could publish a partially initialized router. | Router construction initializes synchronization transactionally before publishing the managed handle. | `RTHighLevelNetworkTests.cpp` router construction/matching cases |
| 63 | Router implementation bypassed the platform policy with raw OS macros. | Router code uses `rt_platform.h` capabilities; raw macros remain confined to approved adapter layers. | strict platform-policy lint and platform smoke |
| 64 | `cppcheck-runtime` lacked a reproducible, enforced configuration. | The target consumes the compile database, checks runtime warning/performance/portability classes with an error exit code, uses a reviewed suppression file, and runs in a dedicated CI workflow. | `cppcheck-runtime` target and `runtime-static-analysis.yml` |

Archive and snapshot behavior is specified by
[ADR 0115](../adr/0115-retained-runtime-resource-snapshots.md),
[ADR 0117](../adr/0117-archive-validation-resource-and-concurrency-policy.md),
[ADR 0119](../adr/0119-trap-safe-managed-io-ownership.md), and
[ADR 0120](../adr/0120-http-router-stable-publication-and-trap-safe-matching.md).
ZPAK v2 is specified by [ADR 0134](../adr/0134-zpak-v2-validation-and-entry-checksums.md),
and the static-analysis gate by
[ADR 0135](../adr/0135-runtime-cppcheck-build-and-ci-gate.md).

## Additional closeout defects

The closing sanitizer and slow-test passes exposed four defects outside the
numbered audit. They were fixed rather than accepted as test-environment noise:

- POSIX stack-safety initialization used one process-global alternate signal
  stack even though `sigaltstack` state and ownership are per-thread. Native
  workers now preserve sanitizer/host-owned stacks or install distinct
  thread-local storage, and process-wide handler publication is transactional.
  `RTConcurrencyTests.cpp` holds concurrent workers alive and proves their
  alternate-stack addresses are distinct.
- A thread-safety regression harness used a `volatile` start flag, and a safe
  thread fixture inspected completion before joining. Both now use real
  synchronization, allowing TSan to distinguish runtime races from harness
  races and preventing worker execution from overlapping CLI teardown.
- The Windows installer RSA exponent validator rejected every exponent whose
  first hexadecimal nibble was zero, including the documented big-endian value
  `010001`. It now rejects only a leading zero byte and has a regression for
  the non-canonical `0003` encoding.
- The broad sanitizer driver previously launched unbounded parallel builds and
  let explicit CTest `TIMEOUT` properties override its instrumented budget.
  Build/test parallelism is bounded, slow/performance and artifact-inspection
  lanes are separated explicitly, and one validated timeout value is passed to
  CMake as the floor for explicitly timed instrumented tests.

## Validation record

The focused regression matrix passed 45 of 45 CTests, covering the suites named
above plus context shutdown, handle validation, file wrappers, and network
hardening. The canonical macOS build then passed all 1,907 ordinary tests, and
the dedicated slow lane completed all 30 selected tests (with its documented
privileged-installer environment skip).

On 2026-07-18 the broad sanitizer-compatible inventory completed without a
diagnostic: ASan selected 1,891 tests in 991.93 seconds, UBSan selected 1,891 in
922.91 seconds, and TSan selected 748 VM/runtime tests in 818.01 seconds. Each
lane had only the expected unavailable-audio skip. The full-program bytecode
parity regression also passed alone under TSan in 160.16 seconds.

The post-sanitizer canonical build passed all 1,907 ordinary tests in 338.61
seconds, the eight-test runtime-surface focus, native/cross-platform smokes,
documentation generation, strict platform policy, and gating cppcheck. The
final 30-test slow lane completed in 289.07 seconds with only its documented
privileged macOS installer environment skip. Source health, formatting, and
diff-whitespace audits were also clean.
