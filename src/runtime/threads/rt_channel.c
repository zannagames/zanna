//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_channel.c
// Purpose: Implements a bounded, thread-safe channel for the Zanna.Threads.Channel
//          class, modelled after Go channels. Uses a ring buffer protected by a
//          monitor; senders block when full, receivers block when empty.
//
// Key invariants:
//   - Capacity is fixed at construction; Send blocks when count == capacity.
//   - Recv blocks when count == 0; it returns NULL immediately if closed+empty.
//   - Closing a channel unblocks all waiting senders and receivers.
//   - Sending to a closed channel traps immediately.
//   - The ring buffer is indexed by head/tail modulo capacity.
//   - All public operations acquire the monitor before touching shared state.
//
// Ownership/Lifetime:
//   - The channel retains references to all objects stored in the ring buffer.
//   - The finalizer releases retained items and frees the buffer on GC collection.
//   - Sending retains a separate reference without consuming the caller's
//     reference; receiving transfers the channel's retained reference.
//
// Links: src/runtime/threads/rt_channel.h (public API),
//        src/runtime/threads/rt_monitor.h (monitor used for synchronization),
//        src/runtime/threads/rt_threads.h (thread primitives)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Bounded FIFO and synchronous rendezvous channels for runtime objects.
/// @details A monitor serializes channel state, while short GC mutator barriers
///          publish or remove strong item edges. Timed operations use one
///          absolute deadline across monitor acquisition and condition waits.

#include "rt_channel.h"

#include "rt_gc.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_threads.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

//=============================================================================
// Internal Structures
//=============================================================================

/// @brief Channel implementation.
typedef struct channel_impl {
    void *monitor;             ///< Monitor for synchronization.
    void **buffer;             ///< Ring buffer for items.
    int64_t capacity;          ///< Buffer capacity.
    int64_t count;             ///< Number of items in buffer.
    int64_t head;              ///< Index of next read.
    int64_t tail;              ///< Index of next write.
    int64_t waiting_senders;   ///< Number of blocked senders.
    int64_t waiting_receivers; ///< Number of blocked receivers.
    int64_t sync_epoch;        ///< Monotonic synchronous handoff identifier.
    int64_t sync_acked_epoch;  ///< Last synchronous handoff consumed by a receiver.
    int8_t closed;             ///< Closed flag.
} channel_impl;

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Finalize a channel and release its monitor, buffer, and retained items.
/// @param obj Channel payload to finalize; null is ignored.
static void channel_finalizer(void *obj);

#include "rt_trap.h"

/// @brief Install a jump target for recovering a runtime trap on this thread.
/// @param buf Recovery buffer to install.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Clear the current thread's installed trap recovery target.
void rt_trap_clear_recovery(void);
/// @brief Read the current thread's most recent trap diagnostic.
/// @return Borrowed null-terminated diagnostic text, or null.
const char *rt_trap_get_error(void);

#if defined(_WIN32)
typedef struct {
    ULONGLONG deadline;
} channel_deadline;

/// @brief Compute an absolute monotonic deadline @p ms in the future.
/// @details Per-platform: GetTickCount64 on Windows, clock_gettime(CLOCK_MONOTONIC)
///          on POSIX. Used by Channel.SendFor / RecvFor to convert a
///          relative timeout into a fixed point that survives spurious
///          wakeups during the wait loop. Saturates at the per-platform
///          maximum so an absurdly large @p ms can't roll over the clock.
/// @param ms Relative timeout in milliseconds; nonpositive values add no time.
/// @return Saturating absolute Windows tick deadline.
static channel_deadline channel_deadline_from_now(int64_t ms) {
    channel_deadline d;
    ULONGLONG now = GetTickCount64();
    ULONGLONG add = ms > 0 ? (ULONGLONG)ms : 0;
    d.deadline = (ULLONG_MAX - now < add) ? ULLONG_MAX : now + add;
    return d;
}

/// @brief Return the number of milliseconds remaining until @p d (0 if already expired).
/// @details Companion to channel_deadline_from_now. Returns 0 the instant
///          the deadline lapses so wait loops can detect timeout via a
///          single == 0 check rather than a delta comparison. Saturates
///          at INT64_MAX for very large remaining intervals so the result
///          fits a signed return without overflow.
/// @param d Absolute Windows tick deadline.
/// @return Whole milliseconds remaining, clamped to 0..INT64_MAX.
static int64_t channel_remaining_ms(channel_deadline d) {
    ULONGLONG now = GetTickCount64();
    if (now >= d.deadline)
        return 0;
    ULONGLONG delta = d.deadline - now;
    return delta > (ULONGLONG)INT64_MAX ? INT64_MAX : (int64_t)delta;
}
#else
typedef struct {
    struct timespec deadline;
    clockid_t clock_id;
} channel_deadline;

/// @brief Compute an absolute monotonic deadline @p ms in the future.
/// @details Per-platform: GetTickCount64 on Windows, clock_gettime(CLOCK_MONOTONIC)
///          on POSIX. Used by Channel.SendFor / RecvFor to convert a
///          relative timeout into a fixed point that survives spurious
///          wakeups during the wait loop. Saturates at the per-platform
///          maximum so an absurdly large @p ms can't roll over the clock.
/// @param ms Relative timeout in milliseconds; nonpositive values add no time.
/// @return Saturating absolute POSIX deadline and the clock used to create it.
static channel_deadline channel_deadline_from_now(int64_t ms) {
    channel_deadline d;
    memset(&d, 0, sizeof(d));
    d.clock_id = CLOCK_MONOTONIC;
    if (clock_gettime(d.clock_id, &d.deadline) != 0) {
        d.clock_id = CLOCK_REALTIME;
        (void)clock_gettime(CLOCK_REALTIME, &d.deadline);
    }
    if (ms > 0) {
        int64_t add_sec = ms / 1000;
        long add_nsec = (long)((ms % 1000) * 1000000L);
        int64_t sec_room = (int64_t)LONG_MAX - (int64_t)d.deadline.tv_sec;
        if (add_sec > sec_room ||
            (add_sec == sec_room && d.deadline.tv_nsec > 999999999L - add_nsec)) {
            d.deadline.tv_sec = (time_t)LONG_MAX;
            d.deadline.tv_nsec = 999999999L;
            return d;
        }
        d.deadline.tv_sec += (time_t)add_sec;
        d.deadline.tv_nsec += add_nsec;
        if (d.deadline.tv_nsec >= 1000000000L) {
            d.deadline.tv_sec++;
            d.deadline.tv_nsec -= 1000000000L;
        }
    }
    return d;
}

/// @brief Return the number of milliseconds remaining until @p d (0 if already expired).
/// @details Companion to channel_deadline_from_now. Returns 0 the instant
///          the deadline lapses so wait loops can detect timeout via a
///          single == 0 check rather than a delta comparison. Saturates
///          at INT64_MAX for very large remaining intervals so the result
///          fits a signed return without overflow.
/// @param d Absolute POSIX deadline and its associated clock.
/// @return Whole milliseconds remaining, clamped to 0..INT64_MAX.
static int64_t channel_remaining_ms(channel_deadline d) {
    struct timespec now;
    memset(&now, 0, sizeof(now));
    if (clock_gettime(d.clock_id, &now) != 0)
        return 0;
    int64_t sec = (int64_t)d.deadline.tv_sec - (int64_t)now.tv_sec;
    int64_t ns = (int64_t)d.deadline.tv_nsec - (int64_t)now.tv_nsec;
    if (ns < 0) {
        sec--;
        ns += 1000000000L;
    }
    if (sec < 0)
        return 0;
    if (sec > INT64_MAX / 1000)
        return INT64_MAX;
    return sec * 1000 + ns / 1000000L;
}
#endif

//=============================================================================
// Channel Management
//=============================================================================

/// @brief GC finalizer for a Channel — close it and release every retained item plus the monitor.
/// @details Closes the channel under the monitor (waking any blocked
///          senders/receivers so they can observe `closed`), releases each
///          retained item still in the ring buffer, frees the buffer
///          allocation, then releases the monitor object. Safe to call
///          on a partially-initialized channel — every step NULL-checks
///          its target.
/// @param obj Channel payload to finalize; null is ignored.
static void channel_finalizer(void *obj) {
    channel_impl *ch = (channel_impl *)obj;
    if (!ch)
        return;

    if (ch->monitor) {
        rt_monitor_enter(ch->monitor);
        ch->closed = 1;
        rt_monitor_pause_all(ch->monitor);
        rt_monitor_exit(ch->monitor);
    }

    /* Detach the complete edge-bearing ring before releasing any member. The
       collector's graph barrier prevents traversal from observing refcounts
       after their corresponding slots have disappeared. */
    rt_gc_mutator_enter();
    void **buffer = ch->buffer;
    int64_t count = ch->count;
    int64_t head = ch->head;
    int64_t slots = (ch->capacity == 0) ? 1 : ch->capacity;
    ch->buffer = NULL;
    ch->count = 0;
    ch->head = 0;
    ch->tail = 0;
    rt_gc_mutator_exit();

    // Release all items in the detached buffer.
    if (buffer) {
        for (int64_t i = 0; i < count; i++) {
            int64_t idx = (head + i) % slots;
            void *item = buffer[idx];
            if (item) {
                if (rt_obj_release_check0(item))
                    rt_obj_free(item);
            }
        }
        free(buffer);
    }

    // Release monitor
    if (ch->monitor) {
        if (rt_obj_release_check0(ch->monitor))
            rt_obj_free(ch->monitor);
        ch->monitor = NULL;
    }
}

/// @brief Enumerate every strong managed item currently buffered by a Channel.
/// @details Ring publication/removal is protected by the collector's mutator
///          barrier, so exclusive traversal can inspect the compact logical
///          range without acquiring the blocking Channel monitor. This avoids
///          lock inversion between a collector and a sender waiting to publish.
/// @param obj Channel payload being traversed.
/// @param visitor Collector visitor invoked once per non-null buffered item.
/// @param ctx Opaque collector context forwarded to @p visitor.
static void channel_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    channel_impl *ch = (channel_impl *)obj;
    if (!ch || !visitor || !ch->buffer || ch->count <= 0)
        return;
    const int64_t slots = ch->capacity == 0 ? 1 : ch->capacity;
    for (int64_t i = 0; i < ch->count; ++i) {
        void *item = ch->buffer[(ch->head + i) % slots];
        if (item)
            visitor(item, ctx);
    }
}

/// @brief Release a retained channel reference; free the impl when refcount hits zero.
/// @details Used by entry points that took a temporary ref for the duration of
///          a blocking wait. NULL @p obj is a no-op.
/// @param obj Retained channel or other runtime-object reference, or null.
static void channel_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release a retained item from the channel's ring buffer when it's no longer reachable.
/// @details Same shape as channel_release_object but named for clarity at
///          item-popping sites. NULL @p item is a no-op.
/// @param item Retained channel-item reference, or null.
static void channel_release_item(void *item) {
    if (item && rt_obj_release_check0(item))
        rt_obj_free(item);
}

/// @brief Publish one retained item into an empty synchronous handoff slot.
/// @details The caller owns the Channel monitor and has already retained the
///          item. The short graph-mutator scope makes the pointer and logical
///          count visible atomically to cycle-collector traversal.
/// @param ch Locked synchronous channel with an empty handoff slot.
/// @param item Retained item to publish, possibly null.
static void channel_publish_sync_locked(channel_impl *ch, void *item) {
    rt_gc_mutator_enter();
    ch->buffer[0] = item;
    ch->count = 1;
    rt_gc_mutator_exit();
}

/// @brief Publish one retained item at the buffered Channel's FIFO tail.
/// @details Pointer, tail, and count updates share one collector barrier so a
///          traversal observes either the old ring or the complete new edge.
/// @param ch Locked buffered channel with available capacity.
/// @param item Retained item to enqueue, possibly null.
static void channel_publish_buffered_locked(channel_impl *ch, void *item) {
    rt_gc_mutator_enter();
    ch->buffer[ch->tail] = item;
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;
    rt_gc_mutator_exit();
}

/// @brief Remove and transfer the synchronous handoff item.
/// @details No refcount is changed: the Channel's owned reference becomes the
///          receiver's reference. The graph barrier protects slot removal from
///          concurrent cycle traversal.
/// @param ch Locked synchronous channel with a published handoff.
/// @return Transferred item reference, possibly null.
static void *channel_take_sync_locked(channel_impl *ch) {
    rt_gc_mutator_enter();
    void *item = ch->buffer[0];
    ch->buffer[0] = NULL;
    ch->count = 0;
    rt_gc_mutator_exit();
    return item;
}

/// @brief Remove and transfer the oldest buffered Channel item.
/// @details Advances the FIFO head and removes the corresponding strong graph
///          edge in one short collector-mutator scope.
/// @param ch Locked nonempty buffered channel.
/// @return Transferred oldest item reference, possibly null.
static void *channel_take_buffered_locked(channel_impl *ch) {
    rt_gc_mutator_enter();
    void *item = ch->buffer[ch->head];
    ch->buffer[ch->head] = NULL;
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    rt_gc_mutator_exit();
    return item;
}

/// @brief Snapshot the current trap error message into @p buffer (or
///        @p fallback if none) so it survives lock cleanup before re-raise.
/// @param buffer Destination character array.
/// @param buffer_size Destination capacity including its terminator.
/// @param fallback Required message when no trap diagnostic is available.
static void channel_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *err = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", err && err[0] ? err : fallback);
}

/// @brief Increment a waiter counter while holding the channel monitor.
/// @details Waiter counters drive close/wakeup decisions. Letting them wrap would
///          make a live waiter invisible and can deadlock a matching operation.
/// @param ch Locked channel whose monitor is released on overflow.
/// @param channel Retained channel reference released on overflow.
/// @param counter Waiter counter to increment.
/// @param message Trap diagnostic used on overflow.
/// @return 1 after incrementing, or 0 after cleaning up and trapping.
static int8_t channel_increment_waiter_locked(channel_impl *ch,
                                              void *channel,
                                              int64_t *counter,
                                              const char *message) {
    if (*counter == INT64_MAX) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        rt_trap(message);
        return 0;
    }
    (*counter)++;
    return 1;
}

/// @brief Advance the synchronous handoff epoch while holding the channel monitor.
/// @details The epoch is compared by senders and receivers to acknowledge an
///          unbuffered handoff. Wrapping it can make a fresh send look already
///          acknowledged, so overflow is a hard runtime error.
/// @param ch Locked synchronous channel whose monitor is released on overflow.
/// @param channel Retained channel reference released on overflow.
/// @param out Receives the new epoch on success.
/// @param message Trap diagnostic used on overflow.
/// @return 1 after advancing, or 0 after cleaning up and trapping.
static int8_t channel_next_sync_epoch_locked(channel_impl *ch,
                                             void *channel,
                                             int64_t *out,
                                             const char *message) {
    if (ch->sync_epoch == INT64_MAX) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        rt_trap(message);
        return 0;
    }
    *out = ++ch->sync_epoch;
    return 1;
}

/// @brief Retain a runtime reference on @p item while the channel lock is held,
///        trap-recovering (with @p fallback message) on a retain failure.
/// @param ch Locked channel whose monitor is released on failure.
/// @param channel Retained channel reference released on failure.
/// @param item Runtime-managed item to retain, or null.
/// @param waiting_counter Waiter count to decrement during failure cleanup, or null.
/// @param fallback Diagnostic used if the captured trap has no message.
/// @return 1 on success; 0 after cleanup and re-raising a retain trap.
static int8_t channel_retain_item_locked(
    channel_impl *ch, void *channel, void *item, int64_t *waiting_counter, const char *fallback) {
    if (!item)
        return 1;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        channel_save_trap_error(saved_error, sizeof(saved_error), fallback);
        rt_trap_clear_recovery();
        if (waiting_counter)
            (*waiting_counter)--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        rt_trap(saved_error);
        return 0;
    }

    rt_obj_retain_maybe(item);
    rt_trap_clear_recovery();
    return 1;
}

/// @brief Validate-and-cast an opaque channel pointer to its impl, with optional NULL-trap.
/// @details Every public Channel entry point dispatches through this guard.
///          NULL @p channel triggers a trap with @p null_msg iff
///          @p trap_on_null is set; otherwise returns NULL. Wrong-class
///          handles always trap with "Channel: invalid object".
/// @param channel Candidate opaque Channel handle.
/// @param null_msg Diagnostic for a trapped null handle, or null for the default.
/// @param trap_on_null Nonzero to trap rather than quietly reject null.
/// @return Validated channel payload, or null for an accepted null/trap path.
static channel_impl *channel_require(void *channel, const char *null_msg, int8_t trap_on_null) {
    if (!channel) {
        if (trap_on_null)
            rt_trap(null_msg ? null_msg : "Channel: nil channel");
        return NULL;
    }
    if (!rt_obj_is_instance(channel, RT_CHANNEL_CLASS_ID, sizeof(channel_impl))) {
        rt_trap("Channel: invalid object");
        return NULL;
    }
    return (channel_impl *)channel;
}

/// @brief Create a new channel for thread-safe message passing between threads.
/// @details Capacity 0 creates a synchronous (unbuffered) channel where send blocks
///          until a receiver is ready. Capacity > 0 creates a buffered channel that
///          holds up to that many items before blocking senders. Negative
///          capacities become zero and values above 1,000,000 are clamped.
/// @param capacity Requested item capacity.
/// @return New runtime-managed Channel, or null on allocation/tracking failure.
void *rt_channel_new(int64_t capacity) {
    // Minimum capacity of 1 for buffered channels
    // Capacity of 0 means synchronous (unbuffered) channel
    if (capacity < 0)
        capacity = 0;
    if (capacity > 1000000) // Reasonable limit
        capacity = 1000000;

    // For synchronous channels, use capacity of 1 internally
    int64_t buffer_size = (capacity == 0) ? 1 : capacity;

    channel_impl *volatile ch = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        channel_save_trap_error(saved_error, sizeof(saved_error), "Channel: construction failed");
        rt_trap_clear_recovery();
        channel_release_object((void *)ch);
        rt_trap(saved_error);
        return NULL;
    }

    ch = (channel_impl *)rt_obj_new_i64(RT_CHANNEL_CLASS_ID, sizeof(channel_impl));
    if (!ch)
        rt_trap_clear_recovery();
    if (!ch)
        return NULL;

    rt_obj_set_finalizer((void *)ch, channel_finalizer);

    ((channel_impl *)ch)->monitor = rt_obj_new_i64(0, 1); // Create a monitor object
    if (!((channel_impl *)ch)->monitor) {
        rt_trap_clear_recovery();
        channel_release_object((void *)ch);
        return NULL;
    }

    ((channel_impl *)ch)->buffer = (void **)calloc((size_t)buffer_size, sizeof(void *));
    if (!((channel_impl *)ch)->buffer) {
        rt_trap_clear_recovery();
        channel_release_object((void *)ch);
        return NULL;
    }

    ((channel_impl *)ch)->capacity = capacity;
    ((channel_impl *)ch)->count = 0;
    ((channel_impl *)ch)->head = 0;
    ((channel_impl *)ch)->tail = 0;
    ((channel_impl *)ch)->waiting_senders = 0;
    ((channel_impl *)ch)->waiting_receivers = 0;
    ((channel_impl *)ch)->sync_epoch = 0;
    ((channel_impl *)ch)->sync_acked_epoch = 0;
    ((channel_impl *)ch)->closed = 0;

    rt_gc_track((void *)ch, channel_traverse);
    if (!rt_gc_is_tracked((void *)ch)) {
        rt_trap("Channel: GC tracking failed");
        return NULL;
    }

    rt_trap_clear_recovery();
    return (void *)ch;
}

//=============================================================================
// Public API - Send Operations
//=============================================================================

/// @brief Send an item into the channel, blocking if the buffer is full (or until a receiver is
/// ready for sync channels).
/// @details The channel retains @p item. Synchronous sends do not return until
///          the handoff is acknowledged. Closing before completion traps.
/// @param channel Nonnull Channel handle.
/// @param item Runtime-managed item to send, or null.
void rt_channel_send(void *channel, void *item) {
    channel_impl *ch = channel_require(channel, "Channel.Send: nil channel", 1);
    if (!ch)
        return;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);

    // Check if closed
    if (ch->closed) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        rt_trap("Channel.Send: send on closed channel");
        return;
    }

    // Handle synchronous channel (capacity 0)
    if (ch->capacity == 0) {
        if (!channel_increment_waiter_locked(
                ch, channel, &ch->waiting_senders, "Channel.Send: sender wait count overflow"))
            return;
        while ((ch->count != 0 || ch->waiting_receivers == 0) && !ch->closed) {
            rt_monitor_wait(ch->monitor);
        }

        if (ch->closed) {
            ch->waiting_senders--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            rt_trap("Channel.Send: send on closed channel");
            return;
        }

        int64_t my_epoch = 0;
        if (!channel_next_sync_epoch_locked(
                ch, channel, &my_epoch, "Channel.Send: synchronous handoff epoch overflow"))
            return;
        if (!channel_retain_item_locked(
                ch, channel, item, &ch->waiting_senders, "Channel.Send: item retain failed"))
            return;
        channel_publish_sync_locked(ch, item);

        rt_monitor_pause_all(ch->monitor);

        while (ch->sync_acked_epoch < my_epoch) {
            rt_monitor_wait(ch->monitor);
            if (ch->closed && ch->sync_acked_epoch < my_epoch)
                break;
        }
        if (ch->sync_acked_epoch < my_epoch) {
            void *discard_item = NULL;
            if (ch->sync_epoch == my_epoch && ch->count != 0) {
                discard_item = channel_take_sync_locked(ch);
                rt_monitor_pause_all(ch->monitor);
            }
            ch->waiting_senders--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            channel_release_item(discard_item);
            rt_trap("Channel.Send: send on closed channel");
            return;
        }
        ch->waiting_senders--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return;
    }

    // Wait for space in buffered channel
    if (!channel_increment_waiter_locked(
            ch, channel, &ch->waiting_senders, "Channel.Send: sender wait count overflow"))
        return;
    while (ch->count >= ch->capacity && !ch->closed) {
        rt_monitor_wait(ch->monitor);
    }

    if (ch->closed) {
        ch->waiting_senders--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        rt_trap("Channel.Send: send on closed channel");
        return;
    }

    // Retain the item and enqueue
    if (!channel_retain_item_locked(
            ch, channel, item, &ch->waiting_senders, "Channel.Send: item retain failed"))
        return;
    channel_publish_buffered_locked(ch, item);

    // Signal waiting receivers
    if (ch->waiting_receivers > 0) {
        rt_monitor_pause(ch->monitor);
    }

    ch->waiting_senders--;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
}

/// @brief Try to send an item without blocking. Returns 1 if sent, 0 if full or closed.
/// @details Strictly non-blocking on every channel kind:
///   - **Buffered channel** — succeeds when `count < capacity`, enqueueing the item and waking
///     one waiting receiver. Returns 0 if the buffer is full.
///   - **Synchronous channel (capacity 0)** — succeeds only when a receiver is already
///     parked in `Recv` / `RecvFor` and no prior handoff is queued. On success it publishes
///     one retained handoff slot and returns 1 immediately *without* waiting for the receiver
///     to acknowledge consumption — the receiver owns rendezvous completion. Returns 0 if no
///     receiver is waiting.
///   - **Closed channel** — always returns 0; the caller can detect this via a separate
///     `IsClosed()` check if it needs to distinguish "closed" from "full".
/// The retained handoff and the channel object reference are both released cleanly on every
/// failure path so a tight `TrySend` loop cannot leak.
/// @param channel Channel handle; null returns 0.
/// @param item Runtime-managed item to send, or null.
/// @return 1 after publishing the item, otherwise 0.
int8_t rt_channel_try_send(void *channel, void *item) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);

    // Check if closed or full
    if (ch->closed || (ch->capacity > 0 && ch->count >= ch->capacity)) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    // For synchronous channel, need a waiting receiver and an empty handoff slot
    if (ch->capacity == 0 && (ch->waiting_receivers == 0 || ch->count != 0)) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    // Synchronous TrySend is non-blocking: if a receiver is already waiting,
    // publish one handoff and return immediately. The receiver owns completion.
    if (ch->capacity == 0) {
        int64_t ignored_epoch = 0;
        if (!channel_next_sync_epoch_locked(
                ch, channel, &ignored_epoch, "Channel.TrySend: synchronous handoff epoch overflow"))
            return 0;
        if (!channel_retain_item_locked(
                ch, channel, item, NULL, "Channel.TrySend: item retain failed"))
            return 0;
        channel_publish_sync_locked(ch, item);
        rt_monitor_pause_all(ch->monitor);
    } else {
        if (!channel_retain_item_locked(
                ch, channel, item, NULL, "Channel.TrySend: item retain failed"))
            return 0;
        channel_publish_buffered_locked(ch, item);
        if (ch->waiting_receivers > 0)
            rt_monitor_pause(ch->monitor);
    }

    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return 1;
}

/// @brief Send within a total timeout budget.
/// @details The deadline covers monitor acquisition, capacity/rendezvous
///          waiting, and synchronous acknowledgement. Nonpositive timeouts use
///          @ref rt_channel_try_send. Closed channels return failure without a
///          closed-send trap.
/// @param channel Channel handle; null returns 0.
/// @param item Runtime-managed item to send, or null.
/// @param ms Timeout in milliseconds.
/// @return 1 after buffered publication or acknowledged handoff, otherwise 0.
int8_t rt_channel_send_for(void *channel, void *item, int64_t ms) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;

    if (ms <= 0)
        return rt_channel_try_send(channel, item);
    channel_deadline deadline = channel_deadline_from_now(ms);
    rt_obj_retain_maybe(channel);

    int64_t monitor_budget = channel_remaining_ms(deadline);
    if (monitor_budget <= 0 || !rt_monitor_try_enter_for(ch->monitor, monitor_budget)) {
        channel_release_object(channel);
        return 0;
    }

    if (ch->closed) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    // Handle synchronous channel
    if (ch->capacity == 0) {
        if (!channel_increment_waiter_locked(
                ch, channel, &ch->waiting_senders, "Channel.SendFor: sender wait count overflow"))
            return 0;
        while ((ch->count != 0 || ch->waiting_receivers == 0) && !ch->closed) {
            int64_t remaining = channel_remaining_ms(deadline);
            if (remaining <= 0 || !rt_monitor_wait_for(ch->monitor, remaining)) {
                if (ch->count == 0 && ch->waiting_receivers == 0 && !ch->closed) {
                    ch->waiting_senders--;
                    rt_monitor_exit(ch->monitor);
                    channel_release_object(channel);
                    return 0;
                }
                if (ch->count != 0 || ch->waiting_receivers == 0) {
                    ch->waiting_senders--;
                    rt_monitor_exit(ch->monitor);
                    channel_release_object(channel);
                    return 0;
                }
            }
        }

        if (ch->closed) {
            ch->waiting_senders--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return 0;
        }

        int64_t my_epoch = 0;
        if (!channel_next_sync_epoch_locked(
                ch, channel, &my_epoch, "Channel.SendFor: synchronous handoff epoch overflow"))
            return 0;
        if (!channel_retain_item_locked(
                ch, channel, item, &ch->waiting_senders, "Channel.SendFor: item retain failed"))
            return 0;
        channel_publish_sync_locked(ch, item);
        rt_monitor_pause_all(ch->monitor);

        while (ch->sync_acked_epoch < my_epoch && !ch->closed) {
            int64_t remaining = channel_remaining_ms(deadline);
            if (remaining <= 0 || !rt_monitor_wait_for(ch->monitor, remaining)) {
                if (ch->sync_acked_epoch >= my_epoch)
                    break;
                void *discard_item = NULL;
                if (ch->sync_epoch == my_epoch && ch->count != 0) {
                    discard_item = channel_take_sync_locked(ch);
                    rt_monitor_pause_all(ch->monitor);
                }
                ch->waiting_senders--;
                rt_monitor_exit(ch->monitor);
                channel_release_object(channel);
                channel_release_item(discard_item);
                return 0;
            }
        }
        if (ch->sync_acked_epoch < my_epoch) {
            void *discard_item = NULL;
            if (ch->sync_epoch == my_epoch && ch->count != 0) {
                discard_item = channel_take_sync_locked(ch);
                rt_monitor_pause_all(ch->monitor);
            }
            ch->waiting_senders--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            channel_release_item(discard_item);
            return 0;
        }
        ch->waiting_senders--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 1;
    }

    // Buffered channel
    if (!channel_increment_waiter_locked(
            ch, channel, &ch->waiting_senders, "Channel.SendFor: sender wait count overflow"))
        return 0;
    while (ch->count >= ch->capacity && !ch->closed) {
        int64_t remaining = channel_remaining_ms(deadline);
        int8_t signaled = remaining > 0 ? rt_monitor_wait_for(ch->monitor, remaining) : 0;
        if (!signaled && ch->count >= ch->capacity && !ch->closed) {
            ch->waiting_senders--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return 0;
        }
    }

    if (ch->closed) {
        ch->waiting_senders--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    if (!channel_retain_item_locked(
            ch, channel, item, &ch->waiting_senders, "Channel.SendFor: item retain failed"))
        return 0;
    channel_publish_buffered_locked(ch, item);
    if (ch->waiting_receivers > 0)
        rt_monitor_pause(ch->monitor);

    ch->waiting_senders--;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return 1;
}

//=============================================================================
// Public API - Receive Operations
//=============================================================================

/// @brief Receive the oldest item, blocking until availability or close.
/// @details Removal transfers the Channel's retained item reference to the
///          caller. A synchronous receive acknowledges its sender. Null is
///          returned both for a transmitted null item and for closed-and-empty.
/// @param channel Nonnull Channel handle.
/// @return Transferred oldest item reference, or null for a null message or
///         closed-and-empty channel.
void *rt_channel_recv(void *channel) {
    channel_impl *ch = channel_require(channel, "Channel.Recv: nil channel", 1);
    if (!ch)
        return NULL;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);

    // Handle synchronous channel
    if (ch->capacity == 0) {
        if (!channel_increment_waiter_locked(
                ch, channel, &ch->waiting_receivers, "Channel.Recv: receiver wait count overflow"))
            return NULL;

        // Signal any waiting senders
        if (ch->waiting_senders > 0)
            rt_monitor_pause(ch->monitor);

        // Wait for item
        while (ch->count == 0 && !ch->closed) {
            rt_monitor_wait(ch->monitor);
        }

        if (ch->count == 0) {
            // Closed and empty
            ch->waiting_receivers--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return NULL;
        }

        void *item = channel_take_sync_locked(ch);
        ch->sync_acked_epoch = ch->sync_epoch;

        // Signal any waiting senders
        if (ch->waiting_senders > 0)
            rt_monitor_pause_all(ch->monitor);

        ch->waiting_receivers--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return item; // Already retained by sender
    }

    // Buffered channel - wait for item
    if (!channel_increment_waiter_locked(
            ch, channel, &ch->waiting_receivers, "Channel.Recv: receiver wait count overflow"))
        return NULL;
    while (ch->count == 0 && !ch->closed) {
        rt_monitor_wait(ch->monitor);
    }

    if (ch->count == 0) {
        // Closed and empty
        ch->waiting_receivers--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return NULL;
    }

    // Dequeue item
    void *item = channel_take_buffered_locked(ch);

    // Signal waiting senders
    if (ch->waiting_senders > 0) {
        rt_monitor_pause(ch->monitor);
    }

    ch->waiting_receivers--;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return item; // Already retained by sender
}

/// @brief Try to receive an item without blocking.
/// @details With a nonnull output slot, success consumes the oldest item and
///          transfers its retained reference. A null output slot only probes
///          availability and never consumes or advertises a synchronous receiver.
/// @param channel Channel handle; null returns 0.
/// @param out Receives the transferred item, or null for a non-consuming probe.
/// @return 1 when an item is available and, if requested, consumed; otherwise 0.
int8_t rt_channel_try_recv(void *channel, void **out) {
    if (out)
        *out = NULL;
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);

    if (!out) {
        int8_t available = ch->count > 0 ? 1 : 0;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return available;
    }

    // Synchronous channels require an already-published handoff. TryRecv is
    // strictly non-blocking and never waits to rendezvous with a sender.
    if (ch->capacity == 0) {
        if (ch->count == 0) {
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return 0;
        }

        void *item = channel_take_sync_locked(ch);
        ch->sync_acked_epoch = ch->sync_epoch;
        if (ch->waiting_senders > 0)
            rt_monitor_pause_all(ch->monitor);

        *out = item;

        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 1;
    }

    if (ch->count == 0) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    void *item = channel_take_buffered_locked(ch);
    if (ch->waiting_senders > 0)
        rt_monitor_pause(ch->monitor);

    *out = item;

    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return 1;
}

/// @brief Return an immediately available item through the managed-value ABI.
/// @param channel Channel handle.
/// @return Transferred item reference, or null when none is available.
/// @note A transmitted null item is indistinguishable from no item; use
///       @ref rt_channel_try_recv_option when that distinction matters.
void *rt_channel_try_recv_val(void *channel) {
    void *out = NULL;
    if (!rt_channel_try_recv(channel, &out))
        return NULL;
    return out;
}

/// @brief Managed ABI wrapper for TryRecv returning an Option.
/// @details Uses the boolean result from @ref rt_channel_try_recv so a
///          transmitted NULL value is distinguishable from "no value
///          available". The received retained transfer is released after the
///          Option has retained it.
/// @param channel Channel object pointer.
/// @return `Some(value)` when an item is received, otherwise `None`.
void *rt_channel_try_recv_option(void *channel) {
    void *out = NULL;
    if (!rt_channel_try_recv(channel, &out))
        return rt_option_none();

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        channel_save_trap_error(
            saved_error, sizeof(saved_error), "Channel.TryRecvOption: allocation failed");
        rt_trap_clear_recovery();
        channel_release_item(out);
        rt_trap(saved_error);
        return NULL;
    }

    void *option = rt_option_some(out);
    rt_trap_clear_recovery();
    channel_release_item(out);
    return option;
}

/// @brief Receive or probe within a total timeout budget.
/// @details The deadline covers monitor acquisition and condition waiting.
///          Nonpositive timeouts delegate to @ref rt_channel_try_recv. A null
///          output slot performs a non-consuming availability wait; synchronous
///          probes deliberately do not advertise a rendezvous receiver.
/// @param channel Channel handle; null returns 0.
/// @param out Receives a transferred item, or null for a non-consuming probe.
/// @param ms Timeout in milliseconds.
/// @return 1 when an item becomes available, otherwise 0 on timeout or close.
int8_t rt_channel_recv_for(void *channel, void **out, int64_t ms) {
    if (out)
        *out = NULL;
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;

    if (ms <= 0)
        return rt_channel_try_recv(channel, out);
    channel_deadline deadline = channel_deadline_from_now(ms);
    rt_obj_retain_maybe(channel);

    int64_t monitor_budget = channel_remaining_ms(deadline);
    if (monitor_budget <= 0 || !rt_monitor_try_enter_for(ch->monitor, monitor_budget)) {
        channel_release_object(channel);
        return 0;
    }

    /* A NULL output is uniformly a non-consuming availability probe. Buffered
       probes participate in receiver wakeups, but synchronous probes must not
       advertise themselves as rendezvous receivers: doing so would publish an
       item and strand its sender waiting for an acknowledgement that a probe
       deliberately does not provide. */
    if (!out) {
        int8_t counted_waiter = 0;
        if (ch->capacity > 0 && ch->count == 0 && !ch->closed) {
            if (!channel_increment_waiter_locked(ch,
                                                 channel,
                                                 &ch->waiting_receivers,
                                                 "Channel.RecvFor: receiver wait count overflow"))
                return 0;
            counted_waiter = 1;
        }

        while (ch->count == 0 && !ch->closed) {
            int64_t remaining = channel_remaining_ms(deadline);
            int8_t signaled = remaining > 0 ? rt_monitor_wait_for(ch->monitor, remaining) : 0;
            if (!signaled && ch->count == 0 && !ch->closed)
                break;
        }

        if (counted_waiter)
            ch->waiting_receivers--;
        int8_t available = ch->count > 0 ? 1 : 0;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return available;
    }

    // Handle synchronous channel
    if (ch->capacity == 0) {
        if (!channel_increment_waiter_locked(ch,
                                             channel,
                                             &ch->waiting_receivers,
                                             "Channel.RecvFor: receiver wait count overflow"))
            return 0;
        if (ch->waiting_senders > 0)
            rt_monitor_pause(ch->monitor);

        while (ch->count == 0 && !ch->closed) {
            int64_t remaining = channel_remaining_ms(deadline);
            int8_t signaled = remaining > 0 ? rt_monitor_wait_for(ch->monitor, remaining) : 0;
            if (!signaled && ch->count == 0 && !ch->closed) {
                ch->waiting_receivers--;
                rt_monitor_exit(ch->monitor);
                channel_release_object(channel);
                return 0;
            }
        }

        if (ch->count == 0) {
            ch->waiting_receivers--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return 0;
        }

        void *item = channel_take_sync_locked(ch);
        ch->sync_acked_epoch = ch->sync_epoch;
        if (ch->waiting_senders > 0)
            rt_monitor_pause_all(ch->monitor);

        *out = item;

        ch->waiting_receivers--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 1;
    }

    // Buffered channel
    if (!channel_increment_waiter_locked(
            ch, channel, &ch->waiting_receivers, "Channel.RecvFor: receiver wait count overflow"))
        return 0;
    while (ch->count == 0 && !ch->closed) {
        int64_t remaining = channel_remaining_ms(deadline);
        int8_t signaled = remaining > 0 ? rt_monitor_wait_for(ch->monitor, remaining) : 0;
        if (!signaled && ch->count == 0 && !ch->closed) {
            ch->waiting_receivers--;
            rt_monitor_exit(ch->monitor);
            channel_release_object(channel);
            return 0;
        }
    }

    if (ch->count == 0) {
        ch->waiting_receivers--;
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return 0;
    }

    void *item = channel_take_buffered_locked(ch);
    if (ch->waiting_senders > 0)
        rt_monitor_pause(ch->monitor);

    *out = item;

    ch->waiting_receivers--;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return 1;
}

/// @brief Receive and release one item after leaving the Channel monitor.
/// @details This explicit destructive API keeps availability probes
///          non-consuming while preserving callers that intentionally want to
///          drain an item without observing its value. Ownership returned by
///          @ref rt_channel_recv_for is released outside every Channel lock.
/// @param channel Channel object pointer.
/// @param ms Total timeout budget in milliseconds.
/// @return 1 when an item was drained, otherwise 0.
int8_t rt_channel_recv_for_discard(void *channel, int64_t ms) {
    void *item = NULL;
    if (!rt_channel_recv_for(channel, &item, ms))
        return 0;
    channel_release_item(item);
    return 1;
}

/// @brief Return an item received within a timeout through the managed-value ABI.
/// @param channel Channel handle.
/// @param ms Timeout in milliseconds.
/// @return Transferred item reference, or null for timeout, close, or a null item.
void *rt_channel_recv_for_val(void *channel, int64_t ms) {
    void *out = NULL;
    if (!rt_channel_recv_for(channel, &out, ms))
        return NULL;
    return out;
}

//=============================================================================
// Public API - Close
//=============================================================================

/// @brief Close the channel and wake every blocked sender and receiver.
/// @details Closing is idempotent. Buffered items remain available for draining;
///          future sends fail and blocking sends trap when they observe close.
/// @param channel Channel handle; null is ignored.
void rt_channel_close(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);

    if (ch->closed) {
        rt_monitor_exit(ch->monitor);
        channel_release_object(channel);
        return; // Already closed
    }

    ch->closed = 1;

    // Wake all waiters
    rt_monitor_pause_all(ch->monitor);

    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
}

//=============================================================================
// Public API - Properties
//=============================================================================

/// @brief Read the number of currently published items.
/// @param channel Channel handle; null reports zero.
/// @return Buffered item count or synchronous handoff occupancy.
int64_t rt_channel_get_len(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);
    int64_t len = ch->count;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);

    return len;
}

/// @brief Read the channel's configured public capacity.
/// @param channel Channel handle; null reports zero.
/// @return Capacity, where zero identifies a synchronous channel.
int64_t rt_channel_get_cap(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;
    rt_obj_retain_maybe(channel);
    rt_monitor_enter(ch->monitor);
    int64_t cap = ch->capacity;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return cap;
}

/// @brief Check whether a channel has been closed.
/// @param channel Channel handle.
/// @return Closed flag; null is treated as closed.
int8_t rt_channel_get_is_closed(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 1;
    rt_obj_retain_maybe(channel);
    rt_monitor_enter(ch->monitor);
    int8_t closed = ch->closed;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);
    return closed;
}

/// @brief Check whether no item is currently published.
/// @param channel Channel handle.
/// @return 1 when empty; null is treated as empty.
int8_t rt_channel_get_is_empty(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 1;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);
    int8_t empty = (ch->count == 0) ? 1 : 0;
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);

    return empty;
}

/// @brief Check whether an immediate send would lack capacity or a receiver.
/// @details A synchronous channel is full unless its handoff slot is empty and
///          at least one receiver is already waiting.
/// @param channel Channel handle.
/// @return 1 when a send would block, otherwise 0; null reports 0.
int8_t rt_channel_get_is_full(void *channel) {
    channel_impl *ch = channel_require(channel, NULL, 0);
    if (!ch)
        return 0;
    rt_obj_retain_maybe(channel);

    rt_monitor_enter(ch->monitor);
    int8_t full = (ch->capacity == 0) ? ((ch->count != 0 || ch->waiting_receivers == 0) ? 1 : 0)
                                      : ((ch->count >= ch->capacity) ? 1 : 0);
    rt_monitor_exit(ch->monitor);
    channel_release_object(channel);

    return full;
}
