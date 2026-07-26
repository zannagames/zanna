//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/threads/rt_channel.h
// Purpose: Thread-safe channel for inter-thread communication providing FIFO bounded and
// synchronous send/receive with blocking semantics when empty or full.
//
// Key invariants:
//   - FIFO ordering is guaranteed across all operations.
//   - Bounded channels block senders when full until a receiver makes space.
//   - Synchronous channels (capacity 0) rendezvous sender and receiver.
//   - rt_channel_recv blocks until a message is available or the channel is closed.
//
// Ownership/Lifetime:
//   - Channel objects are runtime-managed and reference-counted.
//   - Senders and receivers share the same reference-counted channel object.
//
// Links: src/runtime/threads/rt_channel.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Thread-safe FIFO channels with buffered and synchronous semantics.
/// @details Sending retains runtime-managed items; consuming receives transfer
///          that retained reference to the caller. Capacity zero selects a
///          sender/receiver rendezvous. Closing forbids new sends but preserves
///          already published items for draining.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Zanna.Threads.Channel
//=========================================================================

/// @brief Create a new bounded channel with the specified capacity.
/// @details Negative capacities become zero; values above 1,000,000 are
///          clamped. A zero-capacity channel uses an internal single handoff
///          slot but reports public capacity zero.
/// @param capacity Requested maximum buffered items, or zero for rendezvous.
/// @return New runtime-managed Channel, or null on construction failure.
void *rt_channel_new(int64_t capacity);

/// @brief Send an item to the channel, blocking if full.
/// @details Buffered sends wait for capacity. Synchronous sends wait for a
///          receiver and then for handoff acknowledgement. A closed or null
///          channel traps.
/// @param channel Nonnull Channel handle.
/// @param item Runtime-managed item to retain, or null.
void rt_channel_send(void *channel, void *item);

/// @brief Try to send an item without blocking.
/// @details A synchronous send succeeds only when a receiver is already waiting
///          and the handoff slot is empty; it does not wait for acknowledgement.
/// @param channel Channel handle; null returns failure.
/// @param item Runtime-managed item to retain, or null.
/// @return 1 after publishing, or 0 if full, closed, invalid, or unmatched.
int8_t rt_channel_try_send(void *channel, void *item);

/// @brief Send with a timeout.
/// @details The fixed deadline covers monitor acquisition, waiting for capacity
///          or a receiver, and synchronous acknowledgement. Nonpositive
///          timeouts perform @ref rt_channel_try_send.
/// @param channel Channel handle; null returns failure.
/// @param item Runtime-managed item to retain, or null.
/// @param ms Timeout in milliseconds.
/// @return 1 after completion, or 0 if timed out, closed, or invalid.
int8_t rt_channel_send_for(void *channel, void *item, int64_t ms);

/// @brief Receive an item from the channel, blocking if empty.
/// @details Blocks until an item is available or the channel is closed.
///          The Channel's retained item reference is transferred to the caller.
/// @param channel Nonnull Channel handle.
/// @return Transferred oldest item, or null for a transmitted null or a closed,
///         empty channel.
void *rt_channel_recv(void *channel);

/// @brief Try to receive an item without blocking.
/// @details Returns immediately with the item if available. Passing NULL for
///          @p out checks availability without consuming or releasing an item.
/// @param channel Channel handle; null returns failure.
/// @param out Receives the transferred item, or null for a non-consuming probe.
/// @return 1 if an item is available, otherwise 0.
int8_t rt_channel_try_recv(void *channel, void **out);

/// @brief Managed ABI wrapper for TryRecv.
/// @details Returns the transferred item directly. It cannot distinguish a
///          transmitted null from no available item.
/// @param channel Channel handle.
/// @return Transferred item, or null if empty, closed, invalid, or null-valued.
void *rt_channel_try_recv_val(void *channel);

/// @brief Managed ABI wrapper for TryRecv returning an Option.
/// @details Returns `None` when no item is immediately available and
///          `Some(value)` when an item is received. A transmitted NULL value is
///          represented as `Some(NULL)`, unlike @ref rt_channel_try_recv_val.
/// @param channel Channel object pointer.
/// @return Opaque Zanna.Option object.
void *rt_channel_try_recv_option(void *channel);

/// @brief Receive with a timeout.
/// @details Blocks up to @p ms milliseconds for an item. The timeout covers
///          both monitor acquisition and condition waiting. Passing NULL for
///          @p out is a non-consuming availability probe, matching
///          @ref rt_channel_try_recv.
/// @param channel Channel handle; null returns failure.
/// @param out Pointer to store the transferred item, or NULL to probe without
///            consuming it.
/// @param ms Timeout in milliseconds.
/// @return 1 if an item becomes available, otherwise 0 if timed out or closed.
int8_t rt_channel_recv_for(void *channel, void **out, int64_t ms);

/// @brief Receive and discard one item with a timeout.
/// @details Preserves the explicit destructive operation formerly obtained by
///          passing a NULL output pointer to @ref rt_channel_recv_for. The
///          received ownership transfer is released only after the Channel
///          monitor has been exited, so an item finalizer may safely call back
///          into the same Channel. The timeout covers monitor acquisition and
///          condition waiting.
/// @param channel Channel handle.
/// @param ms Timeout in milliseconds.
/// @return 1 if an item was received and released, or 0 on timeout/close.
int8_t rt_channel_recv_for_discard(void *channel, int64_t ms);

/// @brief Managed ABI wrapper for RecvFor.
/// @details Returns the transferred item directly; a null item is
///          indistinguishable from timeout or closed-and-empty.
/// @param channel Channel handle.
/// @param ms Timeout in milliseconds.
/// @return Transferred item, or null if timed out, closed, invalid, or null-valued.
void *rt_channel_recv_for_val(void *channel, int64_t ms);

/// @brief Close the channel.
/// @details Prevents further sends. Receivers can still drain remaining items.
///          Wakes all blocked senders and receivers. Repeated calls are
///          harmless; null is ignored.
/// @param channel Channel handle.
void rt_channel_close(void *channel);

/// @brief Get the number of items currently in the channel.
/// @param channel Channel handle; null reports zero.
/// @return Buffered item count or synchronous handoff occupancy.
int64_t rt_channel_get_len(void *channel);

/// @brief Get the channel capacity.
/// @param channel Channel handle; null reports zero.
/// @return Capacity (0 for synchronous channels).
int64_t rt_channel_get_cap(void *channel);

/// @brief Check if the channel is closed.
/// @param channel Channel handle.
/// @return 1 if closed; null is treated as closed.
int8_t rt_channel_get_is_closed(void *channel);

/// @brief Check if the channel is empty.
/// @param channel Channel handle.
/// @return 1 if no item is published; null is treated as empty.
int8_t rt_channel_get_is_empty(void *channel);

/// @brief Check if the channel is full.
/// @details A synchronous channel is full unless an empty handoff slot and an
///          already waiting receiver make an immediate send possible.
/// @param channel Channel handle.
/// @return 1 if an immediate send would block; null reports 0.
int8_t rt_channel_get_is_full(void *channel);

#ifdef __cplusplus
}
#endif
