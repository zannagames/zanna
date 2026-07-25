//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_animation_events.h
// Purpose: Snapshot event batches for frame-based game animation APIs.
//
// Key invariants:
//   - Event batches copy event IDs from the producer when the batch is created.
//   - Batch contents are immutable and survive later animation updates.
//   - Event IDs are signed 64-bit application-defined values.
//   - Order and duplicate IDs are preserved.
//   - Empty input returns an object; copy allocation failure returns NULL.
//
// Ownership/Lifetime:
//   - Batches are GC-managed via rt_obj_new_i64 with a finalizer.
//   - IDs are stored in a private heap array owned by the batch.
//   - The caller owns the initial reference returned by creation helpers.
//
// Links: src/runtime/game/rt_animation_events.c (implementation),
//        src/runtime/game/rt_animstate.c, src/runtime/game/rt_animtimeline.c
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Immutable fired-animation-event snapshot API.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for AnimationEventBatch objects.
#define RT_ANIMATION_EVENT_BATCH_CLASS_ID INT64_C(-0x51021F)

/// @brief Create an immutable animation-event batch from a contiguous ID array.
/// @details Copies @p count IDs out of @p ids. A NULL @p ids pointer or
///          non-positive @p count creates an empty batch. The returned object is
///          independent from the animation state/timeline that produced it.
///          Size overflow or ID-array allocation failure releases the partially
///          constructed object and returns NULL rather than an empty snapshot.
/// @param ids Borrowed event IDs to copy, or NULL for an empty batch.
/// @param count Number of IDs available in @p ids.
/// @return A new Zanna.Game.AnimationEventBatch object, or NULL on allocation failure.
void *rt_animation_event_batch_from_ids(const int64_t *ids, int64_t count);

/// @brief Return the number of event IDs stored in a batch.
/// @param batch Zanna.Game.AnimationEventBatch object.
/// @return Event count, or 0 for NULL. A non-NULL wrong-type value traps.
int64_t rt_animation_event_batch_count(void *batch);

/// @brief Read one event ID from a batch by index.
/// @param batch Zanna.Game.AnimationEventBatch object.
/// @param index Zero-based event index.
/// @return Event ID, or 0 if @p index is outside the batch.
int64_t rt_animation_event_batch_get_id(void *batch, int64_t index);

/// @brief Test whether a batch contains an event ID.
/// @param batch Zanna.Game.AnimationEventBatch object.
/// @param event_id Event ID to search for.
/// @return 1 when the ID exists in the batch, otherwise 0.
int8_t rt_animation_event_batch_contains(void *batch, int64_t event_id);

/// @brief Copy batch event IDs into a new Zanna.Collections.Seq.
/// @details Each ID is boxed in original order and the Seq owns its elements.
///          The caller owns the returned Seq.
/// @param batch Borrowed Zanna.Game.AnimationEventBatch object.
/// @return New Seq of boxed integer event IDs, or an empty Seq for NULL input.
///         A non-NULL wrong-type value traps; Seq allocation failure returns NULL.
void *rt_animation_event_batch_ids(void *batch);

#ifdef __cplusplus
}
#endif
