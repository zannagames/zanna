//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_msgbus.h
// Purpose: Pub/sub message bus for decoupled event communication, providing topic-based
// publish/subscribe with integer subscription IDs for selective unsubscription.
//
// Key invariants:
//   - Topics are string keys; each topic can have multiple subscribers.
//   - Subscribers are identified by unique integer IDs returned from rt_msgbus_subscribe.
//   - rt_msgbus_publish delivers to all current subscribers synchronously before returning.
//   - Unsubscribing from within a callback is safe for future publishes; the
//     current publish uses a stable snapshot.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned managed references; ordinary runtime
//     ownership releases them, and cycle collection breaks unreachable cycles.
//   - The bus retains subscriber callback objects while subscribed.
//   - Managed published data is retained for the synchronous dispatch window.
//     Raw foreign pointers remain borrowed and must outlive the publish call.
//
// Links: src/runtime/core/rt_msgbus.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares synchronous exact-topic publish/subscribe messaging.
/// @details Bus operations are safe against concurrent mutation. Publish uses
///   a retained subscriber snapshot, invokes callbacks without the bus lock,
///   and preserves insertion order within one topic.

#pragma once

#include <stdint.h>

#include "rt_string.h"

/// @brief Runtime class identifier assigned to MessageBus instances.
#define RT_MSGBUS_CLASS_ID INT64_C(-0x430501)
/// @brief Runtime class identifier assigned to native callback wrappers.
#define RT_MSGBUS_CALLBACK_CLASS_ID INT64_C(-0x430502)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Native subscriber callback invoked synchronously during publish.
/// @param data Opaque payload supplied to @ref rt_msgbus_publish.
typedef void (*rt_msgbus_callback_fn)(void *data);

/// @brief Create a new message bus.
/// @details Initializes 32 topic buckets and starts positive subscription IDs at one.
/// @return Caller-owned managed MessageBus object, or NULL after a returning
///   allocation or cycle-tracking trap.
void *rt_msgbus_new(void);

/// @brief Wrap a native callback pointer in a runtime-managed callback object.
/// @param callback Function pointer of shape `void (*)(void *data)`, or NULL.
/// @return Caller-owned managed callback wrapper, or NULL for a NULL function
///   or allocation failure after a returning trap.
void *rt_msgbus_callback_new(rt_msgbus_callback_fn callback);

/// @brief Subscribe to a topic. Returns subscription ID for later unsubscribe.
/// @details Retains the exact-byte topic string and callback wrapper. Callbacks
///   on one topic are appended and later invoked in subscription order.
/// @param obj Borrowed MessageBus pointer; NULL returns -1.
/// @param topic Borrowed non-NULL runtime string; embedded NUL bytes are part of the key.
/// @param callback Borrowed callback object returned by @ref rt_msgbus_callback_new.
/// @return Unique positive subscription ID, or -1 for missing arguments or
///   after a returning validation/allocation/counter trap.
int64_t rt_msgbus_subscribe(void *obj, rt_string topic, void *callback);

/// @brief Unsubscribe by subscription ID.
/// @details Removes at most one registration and releases its retained topic
///   and callback. An empty topic node is removed as well.
/// @param obj Borrowed MessageBus pointer; NULL returns zero.
/// @param sub_id Subscription ID returned by subscribe.
/// @return 1 if found and removed, otherwise 0.
int8_t rt_msgbus_unsubscribe(void *obj, int64_t sub_id);

/// @brief Publish a message to a topic.
/// @details Snapshots and retains current callback wrappers, then invokes them
///   synchronously without the bus lock in subscription order. Mutations during
///   callbacks affect only future publishes. A callback trap stops delivery,
///   cleans up all temporary retains, and is re-raised.
/// @param obj Borrowed MessageBus pointer; NULL returns zero.
/// @param topic Borrowed non-NULL exact-byte topic string.
/// @param data Message data. Runtime strings/objects are retained during dispatch;
///        raw foreign pointers are borrowed for the duration of the call.
/// @return Number of snapshotted callbacks processed, or zero when none or
///   after a returning failure trap.
int64_t rt_msgbus_publish(void *obj, rt_string topic, void *data);

/// @brief Get number of subscribers for a topic.
/// @param obj Borrowed MessageBus pointer; NULL returns zero.
/// @param topic Borrowed topic string; NULL returns zero and invalid handles trap.
/// @return Locked snapshot of the topic's subscriber count, or zero if absent.
int64_t rt_msgbus_subscriber_count(void *obj, rt_string topic);

/// @brief Get total number of subscriptions across all topics.
/// @param obj Borrowed MessageBus pointer; NULL returns zero.
/// @return Locked snapshot of total active registrations.
int64_t rt_msgbus_total_subscriptions(void *obj);

/// @brief Get all active topic names.
/// @details Copies exact topic bytes under the bus lock. Result ordering follows
///   hash-bucket traversal and is unspecified.
/// @param obj Borrowed MessageBus pointer; NULL produces an empty sequence.
/// @return Caller-owned managed `Seq[str]`, or NULL after a returning trap.
void *rt_msgbus_topics(void *obj);

/// @brief Remove all subscriptions for a topic.
/// @details Detaches the topic under lock, then releases callbacks, retained
///   names, and nodes outside the lock. Missing inputs/topics are no-ops.
/// @param obj Borrowed MessageBus pointer.
/// @param topic Borrowed exact-byte topic string.
void rt_msgbus_clear_topic(void *obj, rt_string topic);

/// @brief Remove all subscriptions.
/// @details Detaches all topic chains under lock, preserves the reusable bucket
///   array and next subscription ID, then performs cleanup outside the lock.
/// @param obj Borrowed MessageBus pointer; NULL is a no-op.
void rt_msgbus_clear(void *obj);

#ifdef __cplusplus
}
#endif
