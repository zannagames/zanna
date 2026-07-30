//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGCTests.cpp
// Purpose: Validate trial-deletion cycle collection, zeroing weak references,
//          finalizer recovery, statistics, and concurrent mutator quiescence.
//
// Key invariants:
//   - Externally reachable objects survive while unreachable cycles are reclaimed.
//   - Weak observers clear on every final-release path and survive resurrection.
//   - Traversal callbacks may query GC state without deadlocking.
//   - Managed-graph mutations cannot overlap a collection traversal.
//
// Ownership/Lifetime:
//   - Test helpers explicitly balance every external object and string reference.
//   - Cycle tests intentionally transfer their last references to the collector.
//
// Links: src/runtime/core/rt_gc.c, src/runtime/core/rt_heap.c,
//        docs/adr/0116-gc-mutator-quiescence-and-array-cycles.md
//
//===----------------------------------------------------------------------===//

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
#include "rt_array.h"
#include "rt_array_obj.h"
#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

/// @brief Vm_trap.
void vm_trap(const char *msg) {
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);
void rt_weakref_reset(rt_weakref *ref, void *target);
}

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

//=============================================================================
// Test helpers
//=============================================================================

/// Simple test object: holds a pointer to another test object (child).
struct test_node {
    void *child; // Strong reference to another node (or NULL).
};

struct test_pair_node {
    void *child;
    void *extra;
};

static int g_cycle_finalizer_count = 0;
static int g_external_finalizer_count = 0;
static void *g_resurrected_object = NULL;
static std::atomic<int> g_gate_entered{0};
static std::atomic<int> g_gate_mutation_attempted{0};
static std::atomic<int> g_gate_mutation_completed{0};
static std::atomic<int> g_gate_sampled{0};
static std::atomic<int> g_gate_observed_completion{0};

/// @brief Reject every runtime-shim allocation while preserving the hook ABI.
/// @details The shutdown-finalizer sweep must not call this hook because its
///          epoch walk is allocation-free. The @p bytes and @p next parameters
///          are intentionally unused: returning NULL models complete allocator
///          exhaustion without invoking the default implementation.
static void *reject_all_runtime_allocations(int64_t bytes, void *(*next)(int64_t)) {
    (void)bytes;
    (void)next;
    return nullptr;
}

/// Traverse function for test_node: visits the child pointer.
extern "C" {
static void test_node_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    struct test_node *node = (struct test_node *)obj;
    if (node->child)
        visitor(node->child, ctx);
}

static void test_pair_node_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    struct test_pair_node *node = (struct test_pair_node *)obj;
    if (node->child)
        visitor(node->child, ctx);
    if (node->extra)
        visitor(node->extra, ctx);
}

static void gc_touching_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    ASSERT(rt_gc_is_tracked(obj) == 1, "traverse can query GC tracking");
    test_node_traverse(obj, visitor, ctx);
}

/// @brief Hold the first traversal until a worker has attempted a graph mutation.
/// @details The callback samples whether `Seq.Push` completed while traversal was active. Under
///          the GC quiescence contract the worker may announce its attempt, but completion must
///          remain blocked until the collector releases its exclusive graph scope.
static void mutator_gate_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    (void)obj;
    (void)visitor;
    (void)ctx;
    int expected = 0;
    if (!g_gate_sampled.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return;
    g_gate_entered.store(1, std::memory_order_release);
    while (!g_gate_mutation_attempted.load(std::memory_order_acquire))
        std::this_thread::yield();
    g_gate_observed_completion.store(g_gate_mutation_completed.load(std::memory_order_acquire),
                                     std::memory_order_release);
}
}

static void *make_node() {
    void *obj = rt_obj_new_i64(0, (int64_t)sizeof(struct test_node));
    struct test_node *n = (struct test_node *)obj;
    n->child = NULL;
    return obj;
}

static void set_child_retained(void *obj, void *child) {
    struct test_node *node = (struct test_node *)obj;
    if (child)
        rt_obj_retain_maybe(child);
    node->child = child;
}

static void release_obj(void *obj) {
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void assert_weakref_get(rt_weakref *ref, void *expected, const char *message) {
    void *got = rt_weakref_get(ref);
    ASSERT(got == expected, message);
    release_obj(got);
}

static void assert_weak_load(void **slot, void *expected, const char *message) {
    void *got = rt_weak_load(slot);
    ASSERT(got == expected, message);
    release_obj(got);
}

static void assert_weak_load_string(void **slot, rt_string expected, const char *message) {
    void *got = rt_weak_load(slot);
    ASSERT(got == expected, message);
    rt_str_release_maybe((rt_string)got);
}

static void count_cycle_finalizer(void *obj) {
    (void)obj;
    g_cycle_finalizer_count++;
}

static void count_external_finalizer(void *obj) {
    (void)obj;
    g_external_finalizer_count++;
}

static void resurrecting_finalizer(void *obj) {
    g_resurrected_object = obj;
    rt_obj_resurrect(obj);
}

static void trapping_finalizer(void *obj) {
    (void)obj;
    rt_trap("gc finalizer boom");
}

//=============================================================================
// GC Tracking Tests
//=============================================================================

static void test_track_untrack() {
    void *obj = make_node();

    ASSERT(rt_gc_is_tracked(obj) == 0, "not tracked initially");

    rt_gc_track(obj, test_node_traverse);
    ASSERT(rt_gc_is_tracked(obj) == 1, "tracked after track()");

    rt_gc_untrack(obj);
    ASSERT(rt_gc_is_tracked(obj) == 0, "untracked after untrack()");

    // Clean up
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void test_track_null_safety() {
    void *obj = make_node();
    rt_gc_track(NULL, test_node_traverse);
    rt_gc_track(obj, NULL);
    rt_gc_untrack(NULL);
    ASSERT(rt_gc_is_tracked(NULL) == 0, "null is not tracked");
    release_obj(obj);
}

static void test_track_rejects_non_object_payload() {
    int32_t *arr = rt_arr_i32_new(0);
    ASSERT(arr != NULL, "numeric array allocated");

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_gc_track(arr, test_node_traverse);
        rt_trap_clear_recovery();
        ASSERT(0, "tracking arrays for object GC should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("not a heap object") != std::string::npos,
               "non-object GC track trap mentions heap object");
    }

    rt_arr_i32_release(arr);
}

static void test_tracked_count() {
    int64_t base = rt_gc_tracked_count();

    void *a = make_node();
    void *b = make_node();
    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    ASSERT(rt_gc_tracked_count() == base + 2, "count after tracking 2");

    rt_gc_untrack(a);
    ASSERT(rt_gc_tracked_count() == base + 1, "count after untracking 1");

    rt_gc_untrack(b);
    ASSERT(rt_gc_tracked_count() == base, "count back to base");

    if (rt_obj_release_check0(a))
        rt_obj_free(a);
    if (rt_obj_release_check0(b))
        rt_obj_free(b);
}

static void test_double_track() {
    void *obj = make_node();
    int64_t base = rt_gc_tracked_count();

    rt_gc_track(obj, test_node_traverse);
    rt_gc_track(obj, test_node_traverse); // should not duplicate

    ASSERT(rt_gc_tracked_count() == base + 1, "double track doesn't duplicate");

    rt_gc_untrack(obj);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

//=============================================================================
// Weak Reference Tests
//=============================================================================

static void test_weakref_basic() {
    void *obj = make_node();
    rt_weakref *ref = rt_weakref_new(obj);

    ASSERT(ref != NULL, "weakref created");
    assert_weakref_get(ref, obj, "weakref returns target");
    ASSERT(rt_weakref_alive(ref) == 1, "weakref alive");

    rt_weakref_free(ref);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void test_weakref_null_target() {
    rt_weakref *ref = rt_weakref_new(NULL);
    ASSERT(ref != NULL, "weakref with null target created");
    assert_weakref_get(ref, NULL, "weakref to null returns null");
    ASSERT(rt_weakref_alive(ref) == 0, "weakref to null not alive");
    rt_weakref_free(ref);
}

static void test_weakref_null_ref() {
    assert_weakref_get(NULL, NULL, "get(null) = null");
    ASSERT(rt_weakref_alive(NULL) == 0, "alive(null) = 0");
    /// @brief Rt_weakref_free.
    rt_weakref_free(NULL); // should not crash
    ASSERT(1, "free(null) no crash");
}

static void test_weakref_clear_on_free() {
    void *obj = make_node();
    rt_weakref *ref1 = rt_weakref_new(obj);
    rt_weakref *ref2 = rt_weakref_new(obj);

    assert_weakref_get(ref1, obj, "ref1 alive before clear");
    assert_weakref_get(ref2, obj, "ref2 alive before clear");

    // Simulate object being freed - clear weak refs
    rt_gc_clear_weak_refs(obj);

    assert_weakref_get(ref1, NULL, "ref1 cleared");
    assert_weakref_get(ref2, NULL, "ref2 cleared");
    ASSERT(rt_weakref_alive(ref1) == 0, "ref1 not alive");
    ASSERT(rt_weakref_alive(ref2) == 0, "ref2 not alive");

    rt_weakref_free(ref1);
    rt_weakref_free(ref2);
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void test_weakref_free_unregisters() {
    void *obj = make_node();
    rt_weakref *ref = rt_weakref_new(obj);

    // Free the weak ref before the object
    rt_weakref_free(ref);

    // Clearing weak refs for this target should not crash
    rt_gc_clear_weak_refs(obj);
    ASSERT(1, "clear after weakref_free no crash");

    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static void test_weakref_generic_release_unregisters() {
    void *obj = make_node();
    rt_weakref *ref = rt_weakref_new(obj);
    ASSERT(ref != NULL, "weakref created for generic release");

    ASSERT(rt_memory_release(ref) == 0, "generic release frees weakref");
    rt_gc_clear_weak_refs(obj);
    ASSERT(1, "clear after generic weakref release no crash");

    release_obj(obj);
}

static void test_weakref_double_free_traps() {
    rt_weakref *ref = rt_weakref_new(NULL);
    rt_weakref_free(ref);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_weakref_free(ref);
        rt_trap_clear_recovery();
        ASSERT(0, "double weakref free should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("weak reference") != std::string::npos,
               "double weakref free trap mentions weak reference");
    }
}

static void test_weakref_string_target_cleared_on_release() {
    rt_string text = rt_string_from_bytes("weak string target", 18);
    rt_weakref *ref = rt_weakref_new(text);
    ASSERT(ref != NULL, "weakref accepts runtime string target");
    void *got = rt_weakref_get(ref);
    ASSERT(got == text, "weakref get returns string target");
    rt_str_release_maybe((rt_string)got);

    rt_string_unref(text);
    ASSERT(rt_weakref_alive(ref) == 0, "weakref string target dead after final release");
    assert_weakref_get(ref, NULL, "weakref string target cleared after final release");
    rt_weakref_free(ref);
}

/// @brief Verify direct specialized-array release clears every weak observer.
/// @details The replacement allocation intentionally uses the same shape so the
///          pool normally reuses the released address. A stale weak target would
///          then promote the replacement object, exposing an allocator ABA bug.
static void test_weakref_array_target_cleared_on_release() {
    int32_t *target = rt_arr_i32_new(4);
    ASSERT(target != NULL, "weakref array target allocated");
    rt_weakref *ref = rt_weakref_new(target);
    ASSERT(ref != NULL, "weakref accepts runtime array target");

    void *promoted = rt_weakref_get(ref);
    ASSERT(promoted == target, "weakref promotes live array target");
    rt_arr_i32_release((int32_t *)promoted);

    void *released_address = target;
    rt_arr_i32_release(target);
    ASSERT(rt_weakref_alive(ref) == 0, "weakref array target dead after final release");
    assert_weakref_get(ref, NULL, "weakref array target cleared after final release");

    int32_t *replacement = rt_arr_i32_new(4);
    ASSERT(replacement != NULL, "replacement array allocated");
    if ((void *)replacement == released_address) {
        ASSERT(rt_weakref_alive(ref) == 0,
               "weakref does not promote replacement at recycled address");
        assert_weakref_get(ref, NULL, "weakref remains clear after address reuse");
    }

    rt_arr_i32_release(replacement);
    rt_weakref_free(ref);
}

/// @brief Verify an object-array relocation preserves GC and weak-reference identity.
/// @details A large resize is likely to move the backing heap allocation. Whether or not the
///          allocator can grow it in place, the resulting payload must remain tracked, every
///          weak observer must promote the replacement address, and final release must zero the
///          observer exactly once.
static void test_object_array_resize_relocates_gc_bookkeeping() {
    void **array = rt_arr_obj_new(1);
    ASSERT(array != nullptr, "relocating object array allocated");
    rt_weakref *weak = rt_weakref_new(array);
    ASSERT(weak != nullptr, "relocating object array weakref allocated");
    void *old_address = array;

    array = rt_arr_obj_resize(array, 4096);
    ASSERT(array != nullptr, "object array resize succeeds");
    ASSERT(rt_gc_is_tracked(array) == 1, "resized object array remains tracked");
    void *promoted = rt_weakref_get(weak);
    ASSERT(promoted == array, "weakref follows resized object array address");
    if (promoted)
        rt_arr_obj_release((void **)promoted);
    if (array != old_address)
        ASSERT(rt_heap_is_payload(old_address) == 0, "retired object array address is invalid");

    rt_arr_obj_release(array);
    ASSERT(rt_weakref_alive(weak) == 0, "resized object array weakref clears on release");
    rt_weakref_free(weak);
}

static void test_weak_store_string_target_zeroes() {
    rt_string text = rt_string_from_bytes("weak field string target", 24);
    void *slot = NULL;
    rt_weak_store(&slot, text);
    ASSERT(rt_weakref_is_handle(slot) == 1, "weak store wraps string target");
    assert_weak_load_string(&slot, text, "weak load returns live string target");

    rt_string_unref(text);
    assert_weak_load_string(&slot, NULL, "weak string field clears after release");
    rt_weak_store(&slot, NULL);
    ASSERT(slot == NULL, "weak store null frees string weak handle");
}

static void test_weakref_reset_after_clear() {
    void *old_target = make_node();
    void *new_target = make_node();
    rt_weakref *ref1 = rt_weakref_new(old_target);
    rt_weakref *ref2 = rt_weakref_new(old_target);

    rt_gc_clear_weak_refs(old_target);
    assert_weakref_get(ref1, NULL, "ref1 cleared before reset");
    assert_weakref_get(ref2, NULL, "ref2 cleared before reset");

    rt_weakref_reset(ref1, new_target);
    rt_weakref_reset(ref2, new_target);
    assert_weakref_get(ref1, new_target, "ref1 reset to new target");
    assert_weakref_get(ref2, new_target, "ref2 reset to new target");

    rt_gc_clear_weak_refs(new_target);
    assert_weakref_get(ref1, NULL, "ref1 cleared after reset target freed");
    assert_weakref_get(ref2, NULL, "ref2 cleared after reset target freed");

    rt_weakref_free(ref1);
    rt_weakref_free(ref2);
    release_obj(old_target);
    release_obj(new_target);
}

static void test_weakref_survives_finalizer_resurrection() {
    g_resurrected_object = NULL;
    void *obj = make_node();
    rt_weakref *ref = rt_weakref_new(obj);
    rt_obj_set_finalizer(obj, resurrecting_finalizer);

    release_obj(obj);
    ASSERT(g_resurrected_object == obj, "finalizer resurrected object");
    ASSERT(rt_heap_is_payload(obj) == 1, "resurrected object remains live");
    assert_weakref_get(ref, obj, "weak ref still points at resurrected object");

    release_obj(obj);
    ASSERT(rt_heap_is_payload(obj) == 0, "second release frees resurrected object");
    assert_weakref_get(ref, NULL, "weak ref cleared after real free");
    rt_weakref_free(ref);
}

static void test_weakref_rejects_raw_target() {
    int local = 42;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_weakref_new(&local);
        rt_trap_clear_recovery();
        ASSERT(0, "weakref raw target should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("weak reference target") != std::string::npos,
               "weakref raw target trap mentions target");
    }
}

static void test_collect_snapshot_retain_overflow_recovers() {
    void *obj = make_node();
    rt_gc_track(obj, test_node_traverse);
    rt_heap_hdr_t *hdr = rt_heap_hdr(obj);
    hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_gc_collect();
        rt_trap_clear_recovery();
        ASSERT(0, "gc snapshot retain overflow should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("snapshot retain") != std::string::npos,
               "gc snapshot retain overflow trap mentions snapshot retain");
    }

    hdr->refcnt = 1;
    rt_gc_untrack(obj);
    release_obj(obj);
    ASSERT(rt_gc_collect() >= 0, "gc collecting flag recovers after snapshot retain trap");
}

//=============================================================================
// Cycle Collection Tests
//=============================================================================

static void test_collect_empty() {
    int64_t base_count = rt_gc_tracked_count();
    // Make sure no tracked objects exist beyond the base
    int64_t freed = rt_gc_collect();
    // freed might be 0 if nothing is cyclic
    ASSERT(freed >= 0, "collect on empty returns >= 0");
    ASSERT(rt_gc_pass_count() > 0, "pass count incremented");
}

static void test_collect_no_cycle() {
    // Linear chain: a -> b -> c (no cycle)
    void *a = make_node();
    void *b = make_node();
    void *c = make_node();

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = c;

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);
    rt_gc_track(c, test_node_traverse);

    int64_t freed = rt_gc_collect();
    // These objects are all tracked with trial_rc starting at 1.
    // a->b means b gets trial_rc decremented to 0
    // b->c means c gets trial_rc decremented to 0
    // a keeps trial_rc 1 (nothing points to it within tracked set)
    // So a is reachable, and it reaches b and c -> all reachable
    // freed should be 0
    ASSERT(freed == 0, "no cycle -> nothing freed");

    rt_gc_untrack(a);
    rt_gc_untrack(b);
    rt_gc_untrack(c);

    if (rt_obj_release_check0(c))
        rt_obj_free(c);
    if (rt_obj_release_check0(b))
        rt_obj_free(b);
    if (rt_obj_release_check0(a))
        rt_obj_free(a);
}

static void test_collect_simple_cycle() {
    // a -> b -> a (cycle, no external references)
    void *a = make_node();
    void *b = make_node();

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    // Both start at trial_rc=1.
    // a->b: b's trial_rc -> 0
    // b->a: a's trial_rc -> 0
    // Neither has trial_rc > 0 -> both are white -> both freed
    int64_t freed = rt_gc_collect();
    ASSERT(freed == 2, "2-node cycle freed");
    ASSERT(rt_gc_is_tracked(a) == 0, "a untracked after collection");
    ASSERT(rt_gc_is_tracked(b) == 0, "b untracked after collection");
}

static void test_collect_self_cycle() {
    // a -> a (self-referencing)
    void *a = make_node();
    struct test_node *na = (struct test_node *)a;
    na->child = a;

    rt_gc_track(a, test_node_traverse);

    // trial_rc starts at 1, a->a decrements to 0 -> freed
    int64_t freed = rt_gc_collect();
    ASSERT(freed == 1, "self-cycle freed");
}

/// @brief Verify an object-reference array can collect a self-cycle.
/// @details Storing the array in its own slot leaves one internal reference after the caller
///          releases its handle. Automatic array traversal must identify that sole edge and
///          clear weak observers when reclaiming the payload.
static void test_collect_object_array_self_cycle() {
    void **arr = rt_arr_obj_new(1);
    ASSERT(arr != nullptr, "self-cyclic object array allocated");
    ASSERT(rt_gc_is_tracked(arr) == 1, "object array is automatically GC-tracked");
    rt_weakref *weak = rt_weakref_new(arr);
    rt_arr_obj_put(arr, 0, arr);

    rt_arr_obj_release(arr);
    ASSERT(rt_gc_collect() == 1, "object-array self-cycle reclaimed");
    ASSERT(rt_heap_is_payload(arr) == 0, "self-cyclic object-array storage freed");
    ASSERT(rt_weakref_alive(weak) == 0, "object-array weak observer cleared");
    rt_weakref_free(weak);
}

/// @brief Verify boxed-reference arrays are tracked automatically and can collect a self-cycle.
/// @details `RT_ELEM_BOX` arrays own managed pointer slots just like object arrays. The heap must
///          register the array before publication; otherwise the retained self-edge would leak
///          forever after the external reference is dropped.
static void test_collect_box_reference_array_self_cycle() {
    auto **array =
        static_cast<void **>(rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_BOX, sizeof(void *), 1, 1));
    ASSERT(array != nullptr, "box-reference array allocated");
    if (!array)
        return;
    ASSERT(rt_gc_is_tracked(array) == 1, "box-reference array is automatically GC tracked");

    rt_gc_mutator_enter();
    rt_heap_retain(array);
    array[0] = array;
    rt_gc_mutator_exit();
    ASSERT(rt_heap_release(array) == 1, "external box-array owner dropped, leaving self edge");

    ASSERT(rt_gc_collect() == 1, "box-reference array self-cycle collected");
    ASSERT(rt_heap_is_payload(array) == 0, "collected box-reference array storage reclaimed");
}

/// @brief Verify cycle traversal crosses between a Seq and an object array.
/// @details Each container owns the other after external references are dropped. The collector
///          must treat the RT_HEAP_OBJECT and RT_ELEM_OBJ array as one unreachable component.
static void test_collect_mixed_seq_object_array_cycle() {
    void *seq = rt_seq_new_owned();
    void **arr = rt_arr_obj_new(1);
    ASSERT(seq != nullptr && arr != nullptr, "mixed-cycle containers allocated");
    rt_arr_obj_put(arr, 0, seq);
    rt_seq_push(seq, arr);

    release_obj(seq);
    rt_arr_obj_release(arr);
    ASSERT(rt_gc_collect() == 2, "mixed Seq/object-array cycle reclaimed");
    ASSERT(rt_heap_is_payload(seq) == 0, "mixed-cycle Seq storage freed");
    ASSERT(rt_heap_is_payload(arr) == 0, "mixed-cycle object-array storage freed");
}

/// @brief Verify the collector follows a List's backing-array edge exactly once.
/// @details The list owns its separately tracked object array, and the array owns an element
///          pointing back to the list. Dropping the caller's reference leaves a two-payload
///          cycle that must be reclaimed without double-decrementing the list element.
static void test_collect_list_backing_array_cycle() {
    void *list = rt_list_new();
    ASSERT(list != nullptr, "self-cyclic list allocated");
    rt_weakref *weak = rt_weakref_new(list);
    ASSERT(weak != nullptr, "self-cyclic list weakref allocated");
    rt_list_push(list, list);

    release_obj(list);
    ASSERT(rt_gc_collect() == 2, "List/backing-array cycle reclaimed");
    ASSERT(rt_heap_is_payload(list) == 0, "self-cyclic List storage freed");
    ASSERT(rt_weakref_alive(weak) == 0, "self-cyclic List weakref cleared");
    rt_weakref_free(weak);
}

/// @brief Verify two tracked container finalizers do not double-release their cycle edges.
/// @details Seq finalizers normally release every owned element. During cycle reclaim those
///          intra-component decrements are owned by the collector, while each finalizer must
///          still clear its allocation. This regression catches the former zero-ref underflow.
static void test_collect_container_finalizers_suppress_internal_releases() {
    void *left = rt_seq_new_owned();
    void *right = rt_seq_new_owned();
    ASSERT(left != nullptr && right != nullptr, "container-cycle sequences allocated");
    rt_seq_push(left, right);
    rt_seq_push(right, left);

    release_obj(left);
    release_obj(right);
    ASSERT(rt_gc_collect() == 2, "two-container cycle reclaimed without double release");
    ASSERT(rt_heap_is_payload(left) == 0, "left container storage freed");
    ASSERT(rt_heap_is_payload(right) == 0, "right container storage freed");
}

static void test_collect_preserves_reachable() {
    // a -> b -> c -> b (b-c cycle, but a has external ref via trial_rc=1)
    void *a = make_node();
    void *b = make_node();
    void *c = make_node();

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    struct test_node *nc = (struct test_node *)c;
    na->child = b;
    nb->child = c;
    nc->child = b; // cycle between b and c

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);
    rt_gc_track(c, test_node_traverse);

    // trial_rc: a=1, b=1, c=1
    // After decrements: a->b: b=0; b->c: c=0; c->b: b=-1
    // a has trial_rc=1 -> black -> mark reachable children
    // a reaches b -> b becomes black -> b reaches c -> c becomes black
    // All reachable -> freed = 0
    int64_t freed = rt_gc_collect();
    ASSERT(freed == 0, "cycle reachable from external -> not freed");

    rt_gc_untrack(a);
    rt_gc_untrack(b);
    rt_gc_untrack(c);

    if (rt_obj_release_check0(c))
        rt_obj_free(c);
    if (rt_obj_release_check0(b))
        rt_obj_free(b);
    if (rt_obj_release_check0(a))
        rt_obj_free(a);
}

static void test_collect_preserves_cycle_with_extra_external_ref() {
    void *a = make_node();
    void *b = make_node();

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);
    rt_obj_retain_maybe(a); // extra external retain must keep the cycle reachable

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 0, "cycle with extra external retain is preserved");
    ASSERT(rt_gc_is_tracked(a) == 1, "a remains tracked");
    ASSERT(rt_gc_is_tracked(b) == 1, "b remains tracked");

    rt_gc_untrack(a);
    rt_gc_untrack(b);
    na->child = NULL;
    nb->child = NULL;
    release_obj(a); // drop extra retain
    release_obj(a); // drop initial retain
    release_obj(b);
}

static void test_promoted_root_restores_young_child() {
    void *parent = make_node();
    rt_gc_track(parent, test_node_traverse);

    for (int i = 0; i < 10; ++i)
        ASSERT(rt_gc_collect() == 0, "promotion warmup keeps parent");
    while ((rt_gc_pass_count() % 16) == 0)
        ASSERT(rt_gc_collect() == 0, "advance to non-full promoted pass");

    g_external_finalizer_count = 0;
    void *child = make_node();
    rt_obj_set_finalizer(child, count_external_finalizer);
    set_child_retained(parent, child);
    rt_gc_track(child, test_node_traverse);
    release_obj(child);

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 0, "promoted root restores reachable young child");
    ASSERT(rt_heap_is_payload(child) == 1, "young child remains live");
    ASSERT(g_external_finalizer_count == 0, "young child finalizer did not run");

    rt_gc_untrack(child);
    rt_gc_untrack(parent);
    struct test_node *np = (struct test_node *)parent;
    np->child = NULL;
    release_obj(child);
    release_obj(parent);
}

static void test_weakref_cleared_by_collect() {
    // Create a cycle and weak ref to one of the nodes
    void *a = make_node();
    void *b = make_node();

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_weakref *ref_a = rt_weakref_new(a);
    rt_weakref *ref_b = rt_weakref_new(b);

    ASSERT(rt_weakref_alive(ref_a) == 1, "ref_a alive before collect");
    ASSERT(rt_weakref_alive(ref_b) == 1, "ref_b alive before collect");

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 2, "cycle freed");

    ASSERT(rt_weakref_alive(ref_a) == 0, "ref_a dead after collect");
    ASSERT(rt_weakref_alive(ref_b) == 0, "ref_b dead after collect");
    assert_weakref_get(ref_a, NULL, "ref_a null after collect");
    assert_weakref_get(ref_b, NULL, "ref_b null after collect");

    rt_weakref_free(ref_a);
    rt_weakref_free(ref_b);
}

static void test_weak_field_zeroed_by_collect() {
    void *a = make_node();
    void *b = make_node();
    void *slot = NULL;

    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = b;
    nb->child = a;

    rt_weak_store(&slot, a);
    assert_weak_load(&slot, a, "weak field returns live target before collect");

    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 2, "cycle with weak field collected");
    assert_weak_load(&slot, NULL, "weak field cleared after collect");

    rt_weak_store(&slot, NULL);
}

static void test_collect_reclaims_cycle_storage_and_finalizers() {
    g_cycle_finalizer_count = 0;

    void *a = make_node();
    void *b = make_node();
    rt_obj_set_finalizer(a, count_cycle_finalizer);
    rt_obj_set_finalizer(b, count_cycle_finalizer);

    set_child_retained(a, b);
    set_child_retained(b, a);

    rt_weakref *ref_a = rt_weakref_new(a);
    rt_weakref *ref_b = rt_weakref_new(b);
    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    ASSERT(rt_obj_release_check0(a) == 0, "drop external a leaves cycle ref");
    ASSERT(rt_obj_release_check0(b) == 0, "drop external b leaves cycle ref");

    int64_t initial_collected = rt_gc_total_collected();
    int64_t freed = rt_gc_collect();

    ASSERT(freed == 2, "retained cycle storage reclaimed");
    ASSERT(rt_gc_total_collected() == initial_collected + 2, "total_collected counts actual frees");
    ASSERT(g_cycle_finalizer_count == 2, "cycle finalizers run");
    ASSERT(rt_heap_is_payload(a) == 0, "a removed from heap registry");
    ASSERT(rt_heap_is_payload(b) == 0, "b removed from heap registry");
    assert_weakref_get(ref_a, NULL, "weak ref a zeroed by reclaim");
    assert_weakref_get(ref_b, NULL, "weak ref b zeroed by reclaim");

    rt_weakref_free(ref_a);
    rt_weakref_free(ref_b);
}

/// @brief Verify that the registry's internal tombstone value is never accepted as a payload.
/// @details Public validators accept arbitrary opaque addresses. Address value one is reserved as
///          the open-addressing tombstone, so treating an occupied tombstone slot as an exact key
///          would make header validation subtract from an invalid address. Both lookup entry
///          points must reject the sentinel before probing and must clear an output header.
static void test_heap_registry_tombstone_is_not_a_payload() {
    void *sentinel = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
    rt_heap_hdr_t *header = reinterpret_cast<rt_heap_hdr_t *>(static_cast<uintptr_t>(1));
    ASSERT(rt_heap_is_payload(sentinel) == 0, "heap tombstone is not a payload");
    ASSERT(rt_heap_try_get_header(sentinel, &header) == 0, "heap tombstone has no header");
    ASSERT(header == nullptr, "failed tombstone lookup clears the header output");
}

/// @brief Verify borrowed metadata inspection returns an independent scalar snapshot.
/// @details The test mutates the destination before each rejected lookup to prove that failure
///          clears every field rather than leaving stale metadata from a previously valid handle.
///          It also validates the exact logical and allocation properties copied for a live array.
static void test_heap_info_snapshot_and_failure_clearing() {
    auto *payload =
        static_cast<int64_t *>(rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_I64, sizeof(int64_t), 2, 4));
    ASSERT(payload != nullptr, "heap metadata test array allocated");
    if (!payload)
        return;

    rt_heap_info_t info{};
    ASSERT(rt_heap_get_info(payload, &info) == 1, "live payload yields metadata snapshot");
    ASSERT(info.kind == RT_HEAP_ARRAY, "snapshot preserves heap kind");
    ASSERT(info.elem_kind == RT_ELEM_I64, "snapshot preserves element kind");
    ASSERT(info.refcnt == 1, "snapshot samples live reference count");
    ASSERT(info.len == 2 && info.cap == 4, "snapshot preserves logical array bounds");
    ASSERT(info.alloc_size >= sizeof(rt_heap_hdr_t) + 4 * sizeof(int64_t),
           "snapshot preserves total allocation size");

    std::memset(&info, 0xA5, sizeof(info));
    void *sentinel = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
    ASSERT(rt_heap_get_info(sentinel, &info) == 0, "tombstone has no metadata snapshot");
    rt_heap_info_t cleared{};
    ASSERT(std::memcmp(&info, &cleared, sizeof(info)) == 0,
           "failed metadata lookup clears destination");
    ASSERT(rt_heap_get_info(payload, nullptr) == 0, "null metadata destination is rejected");

    rt_heap_release(payload);
}

/// @brief Verify zero-sized elements are rejected after capacity is clamped to length.
/// @details A caller may request no explicit capacity while supplying a positive logical length.
///          The allocator must reject that effective positive capacity before performing the
///          overflow division, and the failed resize must leave the original allocation intact.
static void test_heap_realloc_rejects_zero_element_size_with_positive_length() {
    auto *payload = static_cast<uint8_t *>(rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_U8, 1, 1, 1));
    ASSERT(payload != nullptr, "zero-sized realloc test array allocated");
    if (!payload)
        return;
    payload[0] = 0x5A;

    void *resized = rt_heap_realloc(payload, 0, 1, 0);
    ASSERT(resized == nullptr, "zero element size with positive effective capacity is rejected");

    rt_heap_info_t info{};
    ASSERT(rt_heap_get_info(payload, &info) == 1,
           "failed zero-sized realloc preserves registry entry");
    ASSERT(info.len == 1 && info.cap == 1, "failed zero-sized realloc preserves original bounds");
    ASSERT(payload[0] == 0x5A, "failed zero-sized realloc preserves original contents");
    ASSERT(rt_heap_release(payload) == 0,
           "original allocation remains releasable after rejected zero-sized realloc");
}

/// @brief Verify in-place ownership transfer is rejected for aliased heap payloads.
/// @details `rt_heap_realloc` invalidates its input address on success, so permitting a shared
///          allocation would strand every other owner. The recovered trap must leave the original
///          registry entry, metadata, contents, and both reference-counted aliases intact.
static void test_heap_realloc_requires_unique_owner() {
    auto *payload =
        static_cast<int64_t *>(rt_heap_alloc(RT_HEAP_ARRAY, RT_ELEM_I64, sizeof(int64_t), 2, 2));
    ASSERT(payload != nullptr, "shared realloc test array allocated");
    if (!payload)
        return;
    payload[0] = 17;
    payload[1] = 29;
    rt_heap_retain(payload);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_heap_realloc(payload, sizeof(int64_t), 3, 4);
        rt_trap_clear_recovery();
        ASSERT(0, "shared heap realloc should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("exactly one owner") != std::string::npos,
               "shared realloc reports unique-ownership requirement");
    }

    rt_heap_info_t info{};
    ASSERT(rt_heap_get_info(payload, &info) == 1, "failed shared realloc preserves registry entry");
    ASSERT(info.refcnt == 2 && info.len == 2 && info.cap == 2,
           "failed shared realloc preserves original metadata");
    ASSERT(payload[0] == 17 && payload[1] == 29,
           "failed shared realloc preserves original contents");
    ASSERT(rt_heap_release(payload) == 1, "first alias remains releasable after failed realloc");
    ASSERT(rt_heap_release(payload) == 0, "second alias reclaims original allocation");
}

static void test_collect_restores_cycle_after_finalizer_resurrection() {
    g_resurrected_object = NULL;
    g_cycle_finalizer_count = 0;
    void *a = make_node();
    void *b = make_node();
    rt_obj_set_finalizer(a, resurrecting_finalizer);
    rt_obj_set_finalizer(b, count_cycle_finalizer);
    set_child_retained(a, b);
    set_child_retained(b, a);

    rt_weakref *ref_a = rt_weakref_new(a);
    rt_weakref *ref_b = rt_weakref_new(b);
    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    ASSERT(rt_obj_release_check0(a) == 0, "drop external a leaves cycle ref");
    ASSERT(rt_obj_release_check0(b) == 0, "drop external b leaves cycle ref");

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 0, "resurrected cycle collection aborts reclaim");
    ASSERT(g_resurrected_object == a, "cycle finalizer resurrected a");
    ASSERT(g_cycle_finalizer_count == 1, "cycle peer finalizer ran before resurrection restore");
    ASSERT(rt_heap_is_payload(a) == 1, "resurrected a remains live");
    ASSERT(rt_heap_is_payload(b) == 1, "cycle peer remains live");
    ASSERT(rt_gc_is_tracked(a) == 1, "resurrected a re-tracked");
    ASSERT(rt_gc_is_tracked(b) == 1, "cycle peer re-tracked");
    assert_weakref_get(ref_a, a, "weak ref a remains live after resurrection");
    assert_weakref_get(ref_b, b, "weak ref b remains live after resurrection");

    rt_gc_untrack(a);
    rt_gc_untrack(b);
    struct test_node *na = (struct test_node *)a;
    struct test_node *nb = (struct test_node *)b;
    na->child = NULL;
    nb->child = NULL;
    release_obj(b); // drop a -> b edge
    ASSERT(g_cycle_finalizer_count == 2, "cycle peer finalizer restored after aborted reclaim");
    release_obj(a); // drop b -> a edge
    release_obj(a); // drop resurrection reference
    assert_weakref_get(ref_a, NULL, "weak ref a cleared after final release");
    assert_weakref_get(ref_b, NULL, "weak ref b cleared after final release");
    rt_weakref_free(ref_a);
    rt_weakref_free(ref_b);
}

static void test_collect_releases_untracked_external_children() {
    g_external_finalizer_count = 0;
    g_cycle_finalizer_count = 0;

    void *a = rt_obj_new_i64(0, (int64_t)sizeof(struct test_pair_node));
    void *b = rt_obj_new_i64(0, (int64_t)sizeof(struct test_pair_node));
    void *external = make_node();
    rt_obj_set_finalizer(a, count_cycle_finalizer);
    rt_obj_set_finalizer(external, count_external_finalizer);

    struct test_pair_node *na = (struct test_pair_node *)a;
    struct test_pair_node *nb = (struct test_pair_node *)b;
    na->child = b;
    rt_obj_retain_maybe(b);
    nb->child = a;
    rt_obj_retain_maybe(a);
    na->extra = external;
    rt_obj_retain_maybe(external);

    rt_gc_track(a, test_pair_node_traverse);
    rt_gc_track(b, test_pair_node_traverse);

    ASSERT(rt_obj_release_check0(a) == 0, "drop external a leaves cycle ref");
    ASSERT(rt_obj_release_check0(b) == 0, "drop external b leaves cycle ref");
    ASSERT(rt_obj_release_check0(external) == 0, "external retained only by cycle");

    int64_t freed = rt_gc_collect();
    ASSERT(freed == 2, "cycle members freed");
    ASSERT(g_cycle_finalizer_count == 1, "cycle member finalizer runs");
    ASSERT(g_external_finalizer_count == 1, "external child released by cycle collection");
    ASSERT(rt_heap_is_payload(external) == 0, "external child freed");
}

static void test_traverse_can_touch_gc_without_deadlock() {
    void *a = make_node();
    rt_gc_track(a, gc_touching_traverse);
    int64_t freed = rt_gc_collect();
    ASSERT(freed == 0, "live node not collected");
    rt_gc_untrack(a);
    release_obj(a);
}

/// @brief Verify collection traversal excludes concurrent managed-graph mutation.
/// @details A custom tracked object pauses its first traversal until a worker has entered
///          `rt_seq_push`. The worker must remain inside the barrier acquisition until the
///          synchronous collection finishes, then complete normally once mutators resume.
static void test_collect_quiesces_concurrent_mutator() {
    g_gate_entered.store(0, std::memory_order_relaxed);
    g_gate_mutation_attempted.store(0, std::memory_order_relaxed);
    g_gate_mutation_completed.store(0, std::memory_order_relaxed);
    g_gate_sampled.store(0, std::memory_order_relaxed);
    g_gate_observed_completion.store(0, std::memory_order_relaxed);

    void *gate = make_node();
    void *seq = rt_seq_new();
    ASSERT(gate != nullptr, "mutator gate allocated");
    ASSERT(seq != nullptr, "mutated sequence allocated");
    rt_gc_track(gate, mutator_gate_traverse);

    std::thread worker([seq]() {
        while (!g_gate_entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        g_gate_mutation_attempted.store(1, std::memory_order_release);
        rt_seq_push(seq, nullptr);
        g_gate_mutation_completed.store(1, std::memory_order_release);
    });

    ASSERT(rt_gc_collect() == 0, "live gate and sequence survive coordinated collection");
    worker.join();

    ASSERT(g_gate_observed_completion.load(std::memory_order_acquire) == 0,
           "graph mutation remains blocked during traversal");
    ASSERT(g_gate_mutation_completed.load(std::memory_order_acquire) == 1,
           "graph mutation resumes after collection");
    ASSERT(rt_seq_len(seq) == 1, "resumed sequence mutation is committed");

    rt_gc_untrack(gate);
    release_obj(gate);
    release_obj(seq);
}

static void test_collecting_flag_cleared_after_finalizer_trap() {
    void *a = make_node();
    rt_obj_set_finalizer(a, trapping_finalizer);
    struct test_node *na = (struct test_node *)a;
    na->child = a;
    rt_gc_track(a, test_node_traverse);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_gc_collect();
        rt_trap_clear_recovery();
        ASSERT(0, "GC finalizer trap should be recovered");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("gc finalizer boom") != std::string::npos,
               "GC finalizer trap is propagated");
    }

    rt_gc_untrack(a);

    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        release_obj(a);
        rt_trap_clear_recovery();
        ASSERT(0, "restored finalizer should trap on later release");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("gc finalizer boom") != std::string::npos,
               "GC restores finalizer after failed collection");
    }
    ASSERT(rt_heap_is_payload(a) == 0, "release after restored finalizer frees object");

    int64_t passes = rt_gc_pass_count();
    (void)rt_gc_collect();
    ASSERT(rt_gc_pass_count() > passes, "collecting flag cleared after trap");
}

static void test_run_all_finalizers_releases_snapshot_after_trap() {
    void *a = make_node();
    rt_obj_set_finalizer(a, trapping_finalizer);
    rt_gc_track(a, test_node_traverse);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        rt_gc_run_all_finalizers();
        rt_trap_clear_recovery();
        ASSERT(0, "run_all_finalizers should propagate finalizer trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        ASSERT(message.find("gc finalizer boom") != std::string::npos,
               "run_all_finalizers trap is propagated");
    }

    rt_gc_untrack(a);
    release_obj(a);
    ASSERT(rt_heap_is_payload(a) == 0, "snapshot retain released after finalizer trap");
}

/// @brief Verify shutdown finalizers run even when the managed allocator is exhausted.
/// @details Two tracked objects are finalized while every `rt_alloc` request is rejected. The
///          sweep must use its in-table epoch marks and temporary retains only; it may neither
///          allocate a snapshot nor skip callbacks. Final references remain caller-owned and are
///          released after restoring the allocator hook.
static void test_run_all_finalizers_is_allocation_free() {
    g_cycle_finalizer_count = 0;
    void *a = make_node();
    void *b = make_node();
    rt_obj_set_finalizer(a, count_cycle_finalizer);
    rt_obj_set_finalizer(b, count_cycle_finalizer);
    rt_gc_track(a, test_node_traverse);
    rt_gc_track(b, test_node_traverse);

    rt_set_alloc_hook(reject_all_runtime_allocations);
    rt_gc_run_all_finalizers();
    rt_set_alloc_hook(nullptr);

    ASSERT(g_cycle_finalizer_count == 2,
           "allocation-free shutdown sweep runs every tracked finalizer");
    rt_gc_untrack(a);
    rt_gc_untrack(b);
    release_obj(a);
    release_obj(b);
}

//=============================================================================
// Statistics Tests
//=============================================================================

static void test_statistics() {
    int64_t initial_collected = rt_gc_total_collected();
    int64_t initial_passes = rt_gc_pass_count();

    // Run a collect
    rt_gc_collect();

    ASSERT(rt_gc_pass_count() > initial_passes, "pass count increases");
    ASSERT(rt_gc_total_collected() >= initial_collected, "total_collected >= initial");
}

static void test_threshold_get_set_contract() {
    rt_gc_set_threshold(-100);
    ASSERT(rt_gc_get_threshold() == 0, "negative threshold disables auto GC");
    rt_gc_set_threshold(13);
    ASSERT(rt_gc_get_threshold() == 13, "positive threshold is reported");
    rt_gc_set_threshold(0);
    ASSERT(rt_gc_get_threshold() == 0, "zero threshold disables auto GC");
}

static void test_shutdown_resets_statistics() {
    (void)rt_gc_collect();
    ASSERT(rt_gc_pass_count() > 0, "pass count non-zero before shutdown");
    rt_gc_shutdown();
    ASSERT(rt_gc_tracked_count() == 0, "tracked count reset by shutdown");
    ASSERT(rt_gc_pass_count() == 0, "pass count reset by shutdown");
    ASSERT(rt_gc_total_collected() == 0, "total collected reset by shutdown");
}

/// @brief Verify shutdown zeroes detached weak handles and permits later GC reuse.
/// @details A live target may outlast a process-level GC registry reset. Its observer must not
///          retain a dangling unregistered address, and lazy initialization after shutdown must
///          accept new tracking operations without inheriting stale counters or buckets.
static void test_shutdown_zeroes_weakrefs_and_allows_reuse() {
    void *target = make_node();
    rt_weakref *weak = rt_weakref_new(target);
    rt_gc_track(target, test_node_traverse);
    ASSERT(rt_weakref_alive(weak) == 1, "shutdown weakref starts alive");

    rt_gc_shutdown();
    ASSERT(rt_weakref_alive(weak) == 0, "shutdown zeroes registered weakref target");
    assert_weakref_get(weak, nullptr, "shutdown weakref promotes null");
    ASSERT(rt_gc_is_tracked(target) == 0, "shutdown detaches live tracked target");
    rt_weakref_free(weak);
    release_obj(target);

    void *replacement = make_node();
    rt_gc_track(replacement, test_node_traverse);
    ASSERT(rt_gc_tracked_count() == 1, "GC registry lazily reinitializes after shutdown");
    rt_gc_untrack(replacement);
    release_obj(replacement);
    ASSERT(rt_gc_collect() == 0, "collector runs after registry reinitialization");
}

//=============================================================================
// Main
//=============================================================================

int main() {
    // Tracking
    test_track_untrack();
    test_track_null_safety();
    test_track_rejects_non_object_payload();
    test_tracked_count();
    test_double_track();

    // Weak references
    test_weakref_basic();
    test_weakref_null_target();
    test_weakref_null_ref();
    test_weakref_clear_on_free();
    test_weakref_free_unregisters();
    test_weakref_generic_release_unregisters();
    test_weakref_double_free_traps();
    test_weakref_string_target_cleared_on_release();
    test_weakref_array_target_cleared_on_release();
    test_object_array_resize_relocates_gc_bookkeeping();
    test_weak_store_string_target_zeroes();
    test_weakref_reset_after_clear();
    test_weakref_survives_finalizer_resurrection();
    test_weakref_rejects_raw_target();
    test_collect_snapshot_retain_overflow_recovers();

    // Cycle collection
    test_collect_empty();
    test_collect_no_cycle();
    test_collect_simple_cycle();
    test_collect_self_cycle();
    test_collect_object_array_self_cycle();
    test_collect_box_reference_array_self_cycle();
    test_collect_mixed_seq_object_array_cycle();
    test_collect_list_backing_array_cycle();
    test_collect_container_finalizers_suppress_internal_releases();
    test_collect_preserves_reachable();
    test_collect_preserves_cycle_with_extra_external_ref();
    test_promoted_root_restores_young_child();
    test_weakref_cleared_by_collect();
    test_weak_field_zeroed_by_collect();
    test_collect_reclaims_cycle_storage_and_finalizers();
    test_heap_registry_tombstone_is_not_a_payload();
    test_heap_info_snapshot_and_failure_clearing();
    test_heap_realloc_rejects_zero_element_size_with_positive_length();
    test_heap_realloc_requires_unique_owner();
    test_collect_restores_cycle_after_finalizer_resurrection();
    test_collect_releases_untracked_external_children();
    test_traverse_can_touch_gc_without_deadlock();
    test_collect_quiesces_concurrent_mutator();
    test_collecting_flag_cleared_after_finalizer_trap();
    test_run_all_finalizers_releases_snapshot_after_trap();
    test_run_all_finalizers_is_allocation_free();

    // Statistics
    test_statistics();
    test_threshold_get_set_contract();
    test_shutdown_resets_statistics();
    test_shutdown_zeroes_weakrefs_and_allows_reuse();

    printf("GC tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
