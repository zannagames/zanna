//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_gc.h
// Purpose: Cycle-detecting garbage collector supplementing atomic reference counting, using a
// trial-deletion mark-sweep algorithm to find and break unreachable reference cycles among tracked
// objects.
//
// Key invariants:
//   - Heap objects and RT_ELEM_OBJ/RT_ELEM_BOX arrays may be registered via rt_gc_track.
//   - The collector does not move objects; heap addresses remain stable.
//   - rt_gc_collect is synchronous; reentrant calls during an active pass return 0.
//   - Collection traverses only while the exclusive managed-graph barrier is held.
//   - Weak references registered via rt_weakref_new are zeroed when their target is freed.
//
// Ownership/Lifetime:
//   - Tracked objects are owned by their reference counts; the GC only breaks cycles.
//   - rt_gc_track records a borrowed address; it does not increment the object's refcount.
//   - rt_obj_free untracks automatically; callers may still untrack explicitly
//     when removing an object from cycle detection before it becomes unreachable.
//
// Links: src/runtime/core/rt_gc.c (implementation), src/runtime/oop/rt_object.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares cycle collection, weak references, and graph coordination.
/// @details Reference counting remains the primary ownership mechanism. These
///   APIs add synchronous trial-deletion for registered cyclic graphs and
///   zeroing observers that never retain their target.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Callback that enumerates every strong reference held by @p obj.
/// @details For each reference, the callback must call @p visitor(child, ctx).
/// @param child Borrowed strong child address, or NULL if the traversal chooses
///   to report empty slots.
/// @param ctx Opaque context originally passed to the traversal callback.
typedef void (*rt_gc_visitor_t)(void *child, void *ctx);
/// @brief Traversal function registered per tracked object type.
/// @details Called during a collection pass to enumerate all strong child
///          references so the collector can adjust trial reference counts.
/// @param obj Borrowed tracked payload whose current outgoing edges are stable.
/// @param visitor Collector callback to invoke once for each strong child.
/// @param ctx Opaque collector context to pass through unchanged.
typedef void (*rt_gc_traverse_fn)(void *obj, rt_gc_visitor_t visitor, void *ctx);

//=============================================================================
// Internal Mutator Coordination
//=============================================================================

/// @brief Enter a nestable shared scope that may change the managed object graph.
/// @details Runtime ownership and container code calls this before changing a tracked
///          payload's strong edges or reference counts. The outermost scope coordinates with
///          the collector's exclusive traversal barrier; nested scopes are thread-local and
///          do not acquire the native lock again.
/// @internal This coordination hook is not a language runtime-registry API.
void rt_gc_mutator_enter(void);

/// @brief Leave one managed-graph mutator scope.
/// @details The outermost exit releases the shared barrier and runs any collection pass that
///          was deferred to avoid a shared-to-exclusive lock upgrade.
/// @internal This coordination hook is not a language runtime-registry API.
void rt_gc_mutator_exit(void);

/// @brief Unwind every shared mutator scope held by the calling thread before a trap transfer.
/// @details `longjmp` bypasses lexical cleanup, so the central trap dispatcher invokes this
///          hook before jumping or calling a returning embedder trap handler. Collector-owned
///          exclusive access remains held for the collector recovery path.
/// @internal Only the trap dispatcher should call this function.
void rt_gc_mutator_abort_for_trap(void);

/// @brief Query whether a nested finalizer release is an intra-cycle edge handled by the GC.
/// @details The heap uses this only while decrementing a reference. It prevents container
///          destructors from double-decrementing another member whose count the collector is
///          already normalizing; ordinary and external-child releases return zero.
/// @param payload Managed payload targeted by the release.
/// @return 1 when the decrement must be suppressed; otherwise 0.
/// @internal Collector/heap reclaim handshake; not a language runtime-registry API.
int8_t rt_gc_should_suppress_cycle_release(void *payload);

//=============================================================================
// GC Tracking
//=============================================================================

/// @brief Register an object or object-reference array for cycle collection.
/// @details Registering an existing address updates its traversal callback.
///   NULL arguments are accepted as a no-op. Invalid payloads, unsupported heap
///   kinds, capacity overflow, and allocation failure raise a runtime trap.
/// @param obj Heap object or RT_ELEM_OBJ/RT_ELEM_BOX array with a live RT_MAGIC header.
/// @param traverse Function that enumerates @p obj's strong references
///                 by calling the visitor for each child.
/// @note The collector borrows both the payload address and callback; it does
///   not increment the payload's reference count.
void rt_gc_track(void *obj, rt_gc_traverse_fn traverse);

/// @brief Register a newly allocated object- or box-reference array transactionally.
/// @details Uses the collector's generic pointer-slot traversal and reports
///          failure without trapping so the heap can unregister and reclaim a
///          not-yet-published payload. Other runtime code should use
///          @ref rt_gc_track with a type-specific traversal callback.
/// @param array Exact live `RT_ELEM_OBJ` or `RT_ELEM_BOX` array payload.
/// @return 1 when registration succeeds, including the NULL no-op case;
///   otherwise 0 so an unpublished allocation can be rolled back.
/// @internal Heap/collector construction handshake; not a language ABI.
int8_t rt_gc_track_reference_array(void *array);

/// @brief Transactionally register a newly allocated compiled class instance (ADR 0315).
/// @details Non-trapping, like @ref rt_gc_track_reference_array: called by
///          rt_obj_new_i64 before the payload escapes, for classes whose
///          registered layout (rt_class_layout) has at least one strong slot.
///          The collector traverses the instance through that layout.
/// @param payload Exact live object payload with a positive class id.
/// @return 1 when tracked, otherwise 0 (the caller rolls the allocation back).
int8_t rt_gc_track_class_instance(void *payload);

/// @brief Remove an object from cycle tracking.
/// @details NULL and untracked addresses are no-ops. Removing bookkeeping does
///   not release an object reference.
/// @param obj The object to untrack (e.g. before manual free).
void rt_gc_untrack(void *obj);

/// @brief Relocate internal GC bookkeeping when a managed payload moves during resize.
/// @details Rehashes a tracked entry and updates every weak observer from @p old_payload to
///          @p new_payload. The heap calls this while an outer mutator scope prevents collection
///          from traversing either address.
/// @param old_payload Retired payload address; it need not remain registered with the heap.
/// @param new_payload Replacement live payload address.
/// @note NULL, identical, or untracked addresses are accepted as no-ops.
/// @internal Heap-resize coordination hook; not a language runtime-registry API.
void rt_gc_relocate_payload(void *old_payload, void *new_payload);

/// @brief Check whether an object is currently tracked by the cycle collector.
/// @param obj The object to query.
/// @return 1 if @p obj is tracked, 0 otherwise.
int8_t rt_gc_is_tracked(void *obj);

//=============================================================================
// Cycle Collection
//=============================================================================

/// @brief Run one cycle-collection pass over all tracked objects.
/// @details Collection is synchronous and trap-safe. Calls from a mutator scope
///   schedule a deferred pass; reentrant calls and unavailable exclusive graph
///   access return without collecting.
/// @return Number of reclaimed cycle members, or zero when nothing is freed or
///   a returning trap path is taken.
int64_t rt_gc_collect(void);

/// @brief Get the total number of currently tracked objects.
/// @return The count of objects registered with rt_gc_track() that have
///         not yet been untracked or collected.
int64_t rt_gc_tracked_count(void);

//=============================================================================
// Zeroing Weak References
//=============================================================================

/// @brief Opaque weak reference handle returned by rt_weakref_new().
typedef struct rt_weakref rt_weakref;

/// @brief Runtime class identifier assigned to managed weak-reference objects.
#define RT_WEAKREF_CLASS_ID INT64_C(-0x57524546)

/// @brief Create a zeroing weak reference to a managed target.
/// @details The target's refcount is NOT incremented. When the target is
///          freed, the weak reference automatically becomes NULL. Targets may
///          be live runtime objects, arrays, or string handles. Invalid targets
///          and allocation failure raise a runtime trap.
/// @param target Borrowed managed target to observe, or NULL for an empty handle.
/// @return New caller-owned weak reference handle, or NULL if a returning trap
///   hook permits failure to continue; release with @ref rt_weakref_free.
rt_weakref *rt_weakref_new(void *target);

/// @brief Dereference a weak reference and retain the live target.
/// @details Promotion is synchronized with target destruction. Mortal targets
///   gain one reference; immortal targets are returned without modifying their
///   sentinel count. Invalid handles or refcount overflow raise a trap.
/// @param ref Borrowed weak reference handle; NULL behaves as empty.
/// @return The retained target, or NULL if the target has been freed.
///         Callers own the returned reference and must release it.
void *rt_weakref_get(rt_weakref *ref);

/// @brief Check if the weak reference's target is still alive.
/// @details Reads target liveness without retaining it. Invalid non-NULL
///   handles raise a runtime trap.
/// @param ref Borrowed weak reference handle; NULL is not alive.
/// @return 1 if the target is alive, 0 if it has been freed.
int8_t rt_weakref_alive(rt_weakref *ref);

/// @brief Destroy a weak reference handle (does NOT affect the target).
/// @details Detaches and clears the weak observation, then releases the
///   caller-owned handle reference. NULL is a no-op; invalid handles trap.
/// @param ref Weak reference handle to release.
void rt_weakref_free(rt_weakref *ref);

/// @brief Return non-zero when @p candidate is a weak reference handle.
/// @param candidate Opaque pointer to validate against live heap metadata.
/// @return 1 for a live `RT_WEAKREF_CLASS_ID` object, otherwise 0.
int8_t rt_weakref_is_handle(void *candidate);

/// @brief Re-point a weak reference at a different target, or NULL to clear.
/// @details Allocates the new target chain before detaching the old one, so
///   allocation failure leaves the observation unchanged. Invalid handles or
///   targets and allocation failure raise a runtime trap.
/// @param ref Borrowed weak-reference handle; NULL is a no-op.
/// @param target Borrowed replacement managed target, or NULL to clear.
void rt_weakref_reset(rt_weakref *ref, void *target);

/// @brief Clear all weak references pointing to a target being freed.
/// @details Called internally when a managed object, array, or string handle is
///          being freed. Integrated into the runtime-managed release paths.
/// @param target The target being freed; all weak references to it are zeroed.
void rt_gc_clear_weak_refs(void *target);

//=============================================================================
// Statistics
//=============================================================================

/// @brief Get the total number of objects freed by the collector since startup.
/// @return Saturating cumulative count of objects reclaimed by cycle
///   collection since startup or the last @ref rt_gc_shutdown.
int64_t rt_gc_total_collected(void);

/// @brief Get the number of collection passes run since startup.
/// @return Count of recorded passes, including empty passes, since startup or
///   the last @ref rt_gc_shutdown. Rejected, deferred, and early setup-failure
///   calls are not counted.
int64_t rt_gc_pass_count(void);

//=============================================================================
// Auto-Trigger
//=============================================================================

/// @brief Set the allocation threshold for automatic cycle collection.
/// @details When @p n > 0, every @p n-th heap allocation (via rt_heap_alloc)
///          publishes coalescing collection debt. A later managed-graph
///          boundary or @ref rt_gc_safepoint runs the pass outside the allocator
///          call stack. Set to 0 to disable auto-triggering (default).
/// @param n Positive number of allocations between collection requests; zero
///   or negative disables auto-triggering. Existing debt is cleared.
void rt_gc_set_threshold(int64_t n);

/// @brief Get the current auto-trigger allocation threshold.
/// @return The threshold, or 0 if auto-triggering is disabled.
int64_t rt_gc_get_threshold(void);

/// @brief Notify the GC that a heap allocation occurred.
/// @details Increments an internal counter and publishes collection debt when
///          the counter reaches the configured threshold. It never collects or
///          invokes callbacks synchronously. Called from rt_heap_alloc(); no-op
///          when auto-triggering is disabled.
void rt_gc_notify_alloc(void);

/// @brief Service pending automatic-collection debt at an explicit safe boundary.
/// @details Claims at most one coalesced request and performs a synchronous
///          collection only when the calling thread is outside a graph-mutator
///          or collector scope. A call inside a mutator is deferred to its
///          outermost exit. VM runtime-call boundaries invoke this hook after a
///          native helper has fully initialized its result.
/// @internal This coordination hook is not a language runtime-registry API.
void rt_gc_safepoint(void);

//=============================================================================
// Shutdown
//=============================================================================

/// @brief Run finalizers on all GC-tracked objects without freeing them.
/// @details Iterates every live entry in the tracking table and invokes its
///          heap finalizer (if present).  Finalizer pointers are cleared after
///          invocation to prevent double-finalization. Live mortal objects are
///          retained while the tracking lock is held, then callbacks execute
///          outside both the lock and the exclusive graph barrier. Payloads
///          already at refcount zero are skipped because ownership has passed
///          to the ordinary deferred-destruction path that made the final
///          decrement; competing with it could double-finalize or double-free.
///
///          Must be called BEFORE rt_gc_shutdown() so the tracking table is
///          still valid during traversal.
/// @note    The sweep uses an allocation-free epoch walk, so low-memory shutdown
///          cannot skip registered finalizers merely because a snapshot could
///          not be allocated.
void rt_gc_run_all_finalizers(void);

/// @brief Release all GC internal state (tracking table, weak ref buckets).
/// @details Should only be called during program shutdown after all tracked
///          objects have been freed or are about to be reclaimed by the OS.
///          Typically called from an atexit handler. It zeroes registered weak
///          handles, resets statistics and trigger state, and leaves the
///          process-lifetime synchronization primitives reusable.
/// @warning Calling this while tracked objects are still in use causes
///          undefined behavior.
void rt_gc_shutdown(void);

#ifdef __cplusplus
}
#endif
