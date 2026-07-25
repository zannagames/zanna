//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_animation_events.c
// Purpose: Immutable event-batch snapshots shared by AnimStateMachine and
//          AnimTimeline.
//
// Key invariants:
//   - A snapshot preserves event order and duplicates by copying signed IDs.
//   - Null ID storage or a non-positive count creates a legitimate empty batch.
//   - A copied-array allocation failure returns NULL rather than an empty batch.
//
// Ownership/Lifetime:
//   - Batch objects own their ID arrays and release them through a finalizer.
//   - The constructor borrows and copies ID input.
//   - Ids() returns a new Seq that owns newly boxed integer elements.
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Immutable runtime snapshots of fired animation event IDs.

#include "rt_animation_events.h"

#include "rt_box.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// @brief Native payload stored in an AnimationEventBatch runtime object.
typedef struct {
    int64_t *ids;
    int64_t count;
} rt_animation_event_batch_impl;

/// @brief Safe-cast a handle to an event batch.
/// @param ptr Candidate object pointer.
/// @param api Public API name for trap diagnostics.
/// @return Batch implementation, or NULL when @p ptr is NULL.
static rt_animation_event_batch_impl *checked_event_batch(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_ANIMATION_EVENT_BATCH_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_animation_event_batch_impl *)ptr;
}

/// @brief Finalizer that releases the copied ID array.
/// @param obj Event-batch payload being finalized.
static void event_batch_finalizer(void *obj) {
    rt_animation_event_batch_impl *batch = (rt_animation_event_batch_impl *)obj;
    free(batch->ids);
    batch->ids = NULL;
    batch->count = 0;
}

/// @brief Create an immutable event-batch snapshot by copying IDs.
/// @details Allocates the object before interpreting input. Null @p ids or
///          non-positive @p count returns an owned empty batch. Positive input
///          is copied transactionally; size overflow or array allocation
///          failure releases the new object and returns NULL so failure cannot
///          masquerade as a zero-event frame.
/// @param ids Borrowed ordered ID array; may be NULL.
/// @param count Number of IDs to copy.
/// @return Owned batch object, owned empty batch, or NULL on allocation/size
///         failure.
void *rt_animation_event_batch_from_ids(const int64_t *ids, int64_t count) {
    rt_animation_event_batch_impl *batch = (rt_animation_event_batch_impl *)rt_obj_new_i64(
        RT_ANIMATION_EVENT_BATCH_CLASS_ID, (int64_t)sizeof(rt_animation_event_batch_impl));
    if (!batch)
        return NULL;
    batch->ids = NULL;
    batch->count = 0;
    rt_obj_set_finalizer(batch, event_batch_finalizer);

    if (!ids || count <= 0)
        return batch; // a legitimately empty snapshot (zero events fired)

    // A memory failure while copying the fired IDs must NOT masquerade as an empty
    // batch — that is indistinguishable from a genuine zero-event frame and would
    // silently drop gameplay events under memory pressure. Fail the whole snapshot
    // transactionally by returning NULL, matching the documented "NULL on
    // allocation failure" contract, so callers can tell the two apart (VDOC-279).
    if ((uint64_t)count > SIZE_MAX / sizeof(int64_t)) {
        if (rt_obj_release_check0(batch))
            rt_obj_free(batch);
        return NULL;
    }

    batch->ids = (int64_t *)malloc((size_t)count * sizeof(int64_t));
    if (!batch->ids) {
        if (rt_obj_release_check0(batch))
            rt_obj_free(batch);
        return NULL;
    }
    memcpy(batch->ids, ids, (size_t)count * sizeof(int64_t));
    batch->count = count;
    return batch;
}

/// @brief Return the number of event IDs in a snapshot.
/// @param ptr Borrowed batch object; may be NULL.
/// @return Stored nonnegative count, or zero for null/wrong-class fallback.
int64_t rt_animation_event_batch_count(void *ptr) {
    rt_animation_event_batch_impl *batch =
        checked_event_batch(ptr, "AnimationEventBatch.Count: expected AnimationEventBatch");
    return batch ? batch->count : 0;
}

/// @brief Read an event ID by zero-based index.
/// @param ptr Borrowed batch object; may be NULL.
/// @param index Zero-based ID position.
/// @return Stored ID, or zero for invalid object/index.
int64_t rt_animation_event_batch_get_id(void *ptr, int64_t index) {
    rt_animation_event_batch_impl *batch =
        checked_event_batch(ptr, "AnimationEventBatch.GetId: expected AnimationEventBatch");
    if (!batch || index < 0 || index >= batch->count)
        return 0;
    return batch->ids[index];
}

/// @brief Test whether a snapshot contains an event ID.
/// @details Performs a linear scan and treats duplicates as a single boolean
///          match.
/// @param ptr Borrowed batch object; may be NULL.
/// @param event_id Signed event identifier to locate.
/// @return One when any stored ID matches; otherwise zero.
int8_t rt_animation_event_batch_contains(void *ptr, int64_t event_id) {
    rt_animation_event_batch_impl *batch =
        checked_event_batch(ptr, "AnimationEventBatch.Contains: expected AnimationEventBatch");
    if (!batch)
        return 0;
    for (int64_t i = 0; i < batch->count; ++i) {
        if (batch->ids[i] == event_id)
            return 1;
    }
    return 0;
}

/// @brief Materialize event IDs as a sequence of boxed integers.
/// @details Creates a new Seq, enables element ownership for a valid batch, and
///          boxes/pushes IDs in snapshot order. Null or returning wrong-class
///          fallback yields an empty Seq.
/// @param ptr Borrowed batch object; may be NULL.
/// @return Newly allocated Seq of boxed Int64 objects, or NULL if Seq creation
///         fails.
void *rt_animation_event_batch_ids(void *ptr) {
    rt_animation_event_batch_impl *batch =
        checked_event_batch(ptr, "AnimationEventBatch.Ids: expected AnimationEventBatch");
    void *seq = rt_seq_new();
    if (!seq)
        return NULL;
    if (!batch)
        return seq;
    rt_seq_set_owns_elements(seq, 1);
    for (int64_t i = 0; i < batch->count; ++i) {
        void *boxed = rt_box_i64(batch->ids[i]);
        rt_seq_push(seq, boxed);
        if (boxed && rt_obj_release_check0(boxed))
            rt_obj_free(boxed);
    }
    return seq;
}
