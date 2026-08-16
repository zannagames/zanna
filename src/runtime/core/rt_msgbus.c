//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_msgbus.c
// Purpose: Implements the Zanna message bus (pub/sub event system) for the
//          runtime. Topics are hashed into a dynamically resized bucket array; each
//          topic maintains a singly-linked list of subscriptions identified by
//          unique integer IDs. Publishers dispatch to all matching subscribers.
//
// Key invariants:
//   - Topic names are hashed with FNV-1a (64-bit) and stored in a bucket array
//     that grows above a 0.75 load factor; collisions are resolved by chaining.
//   - Each subscription holds a retained reference to its topic string and an
//     opaque callback pointer (managed by the GC).
//   - Subscription IDs are monotonically increasing 64-bit integers; they are
//     never reused within a bus instance.
//   - Each topic caches its subscriber tail for constant-time ordered append.
//   - Publishing delivers messages in subscription-insertion order per topic.
//   - The bus finalizer releases all topic strings, callbacks, and subscription
//     nodes; it is invoked by the GC when the bus object is collected.
//
// Ownership/Lifetime:
//   - Bus and callback-wrapper instances are reference-counted managed objects;
//     language/runtime ownership releases returned references, while the cycle
//     collector handles unreachable callback/bus cycles.
//   - Topic strings are retained on subscription and released in the finalizer
//     or on explicit unsubscribe.
//   - Callback pointers are retained on subscribe and released on unsubscribe,
//     clear, or finalization.
//   - Managed publish payloads are retained while the subscriber snapshot is
//     invoked; raw foreign pointers are borrowed for the duration of the call.
//
// Links: src/runtime/core/rt_msgbus.h (public API),
//        src/runtime/core/rt_seq.c (sequence helpers used for dispatch),
//        src/runtime/core/rt_string.c (string reference counting)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements a synchronous, thread-safe, topic-keyed message bus.

#include "rt_msgbus.h"

#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <sched.h>
#endif

#include "rt_trap.h"

/// @brief Install a borrowed legacy trap recovery buffer for cleanup.
/// @param buf Live jump buffer until the matching clear operation.
void rt_trap_set_recovery(jmp_buf *buf);
/// @brief Pop the current legacy trap recovery buffer.
void rt_trap_clear_recovery(void);
/// @brief Read the current thread's captured trap diagnostic.
/// @return Runtime-owned C string valid until the next trap or clear.
const char *rt_trap_get_error(void);

// --- Subscription ---

/// @brief One retained callback registration in topic insertion order.
typedef struct mb_sub {
    int64_t id;
    rt_string topic;
    void *callback; // Stored as opaque pointer
    struct mb_sub *next;
} mb_sub;

// --- Topic bucket (hash chain) ---

/// @brief One exact-byte topic key and its ordered subscriber chain.
typedef struct mb_topic {
    rt_string name;
    char *key_bytes;
    size_t key_len;
    uint64_t key_hash;
    mb_sub *subs;
    mb_sub *tail;
    int64_t count;
    struct mb_topic *next;
} mb_topic;

/// @brief Managed MessageBus payload and synchronized hash-table state.
typedef struct {
    void *vptr;
    mb_topic **buckets;
    int64_t bucket_count;
    int64_t topic_count;
    int64_t next_id;
    int64_t total_subs;
    int lock;
} rt_msgbus_impl;

/// @brief Managed wrapper around a native callback function pointer.
typedef struct {
    void *vptr;
    rt_msgbus_callback_fn fn;
} rt_msgbus_callback_impl;

/// @brief Detached topic byte copy used while constructing a result sequence.
typedef struct {
    char *bytes;
    size_t len;
} mb_topic_copy;

// --- FNV-1a hash ---

/// @brief Compute the FNV-1a 64-bit hash of an explicit byte span.
/// @details Standard FNV-1a constants: 64-bit offset basis
///          `0xcbf29ce484222325` and prime `0x100000001b3`. Good
///          distribution for short topic strings ("user.login",
///          "tick", etc.) without the cost of a CSPRNG-seeded
///          hash like SipHash. Used only to bucket topic names —
///          collisions are handled by per-bucket chaining.
/// @param s Byte span to hash; must be valid for @p len bytes.
/// @param len Number of bytes, including any embedded NUL bytes.
/// @return FNV-1a hash of the exact byte span.
static uint64_t mb_hash_bytes(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// --- Internal helpers ---

/// @brief Acquire the bus's internal spinlock with acquire ordering.
/// @details Uses `__atomic_test_and_set` and yields on contention. The yield
///          uses platform-appropriate primitives (`SwitchToThread` on Windows,
///          `sched_yield` on POSIX). No-op on a NULL
///          bus so call-sites that handle a NULL bus uniformly don't have to
///          special-case the lock.
/// @param mb Bus instance (may be NULL).
static void mb_lock(rt_msgbus_impl *mb) {
    if (!mb)
        return;
    if (__atomic_test_and_set(&mb->lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(&mb->lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release the bus's internal spinlock with release ordering.
/// @details No-op on a NULL bus so call-sites that handle a NULL bus uniformly don't have
///          to special-case the unlock.
/// @param mb Locked bus instance, or NULL.
static void mb_unlock(rt_msgbus_impl *mb) {
    if (mb)
        __atomic_clear(&mb->lock, __ATOMIC_RELEASE);
}

/// @brief Resolve a runtime string into a `(bytes, length)` byte view, validating the handle.
/// @details Used by every public bus operation that takes a topic string. NULL traps are
///          deliberately silent (the caller treats it as "no topic"), but a non-NULL value
///          that fails `rt_string_is_handle` raises a runtime trap — callers should never
///          be passing arbitrary memory as a topic. Returns 1 when @p bytes / @p len carry
///          a usable view, 0 otherwise. The bytes pointer points into the string's storage
///          and is valid only while the caller holds whatever retain it had at call time.
/// @param topic Borrowed runtime string; NULL represents no topic.
/// @param bytes Optional output receiving the borrowed byte pointer.
/// @param len Optional output receiving the explicit byte length.
/// @return 1 for a valid non-NULL string view, otherwise 0; invalid handles trap.
static int mb_topic_view(rt_string topic, const char **bytes, size_t *len) {
    if (bytes)
        *bytes = NULL;
    if (len)
        *len = 0;
    if (!topic)
        return 0;
    if (!rt_string_is_handle(topic)) {
        rt_trap("rt_msgbus: invalid topic string");
        return 0;
    }
    int64_t n = rt_str_len(topic);
    if (n < 0) {
        rt_trap("rt_msgbus: invalid topic length");
        return 0;
    }
    const char *data = rt_string_cstr(topic);
    if (!data)
        return 0;
    if (bytes)
        *bytes = data;
    if (len)
        *len = (size_t)n;
    return 1;
}

/// @brief Validate that @p obj is a live MessageBus instance, trapping on misuse.
/// @details Guards every public entry point that dereferences `rt_msgbus_impl`. NULL is
///          treated as silently absent (returns NULL) so callers can no-op gracefully.
///          Any non-NULL pointer with the wrong runtime class id traps with a message
///          including @p fn_name so the diagnostic identifies the offending API.
/// @param obj Opaque candidate bus object.
/// @param fn_name Non-NULL operation name used in diagnostics.
/// @return Borrowed validated bus, or NULL for absent or invalid input.
static rt_msgbus_impl *mb_require(void *obj, const char *fn_name) {
    if (!obj)
        return NULL;
    if (!rt_obj_is_instance(obj, RT_MSGBUS_CLASS_ID, sizeof(rt_msgbus_impl))) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: invalid MessageBus object", fn_name);
        rt_trap(buf);
        return NULL;
    }
    return (rt_msgbus_impl *)obj;
}

/// @brief Validate @p obj as a `MessageBus` and retain it for the duration of the call.
/// @details Public entry points retain the bus on entry and release on exit
///          so a concurrent finalize cannot free the bus mid-publish or
///          mid-traversal. NULL traps fall through silently as in @ref mb_require.
/// @param obj      Caller-supplied handle.
/// @param fn_name  Function name used in the trap message on type mismatch.
/// @return Retained bus pointer; the caller must pair with @ref mb_release_bus.
static rt_msgbus_impl *mb_require_retained(void *obj, const char *fn_name) {
    rt_msgbus_impl *mb = mb_require(obj, fn_name);
    if (!mb)
        return NULL;
    int32_t retained = rt_heap_try_retain_live(mb);
    if (retained != 1) {
        char buf[192];
        snprintf(buf,
                 sizeof(buf),
                 "%s: MessageBus receiver %s",
                 fn_name ? fn_name : "MessageBus",
                 retained < 0    ? "refcount overflow"
                 : retained == 2 ? "has an immortal refcount"
                                 : "is no longer live");
        rt_trap(buf);
        return NULL;
    }
    return mb;
}

/// @brief Release the bus retain acquired by @ref mb_require_retained.
/// @details Frees the bus when the retain count drops to zero. No-op on NULL.
/// @param mb Bus instance previously retained by @ref mb_require_retained.
static void mb_release_bus(rt_msgbus_impl *mb) {
    if (mb && rt_obj_release_check0(mb))
        rt_obj_free(mb);
}

/// @brief Release a single retained-callback entry in a snapshot array.
/// @details The traversal-time snapshot retains every managed callback under
///          the bus lock. This helper unwinds one entry: NULLs its slot, clears
///          the per-entry `retained` flag, and decrements the refcount. Used by
///          both the success path (after the visitor runs) and the overflow
///          rollback path (when CAS retain failed mid-snapshot).
/// @param callbacks Snapshot array of callback pointers (modified in place).
/// @param retained  Parallel array of one-byte "was retained" flags (may be NULL).
/// @param index     Slot to release; negative indices are ignored.
static void mb_release_snapshot_callback(void **callbacks, unsigned char *retained, int64_t index) {
    if (!callbacks || index < 0)
        return;
    void *callback = callbacks[index];
    if (!callback)
        return;
    callbacks[index] = NULL;
    if (retained && !retained[index])
        return;
    if (retained)
        retained[index] = 0;
    if (rt_obj_release_check0(callback))
        rt_obj_free(callback);
}

/// @brief Test whether @p callback is a live `MessageBus.Callback` heap object.
/// @details Callbacks must be wrapped through `rt_msgbus_callback_new` so the bus has a
///          stable C-function pointer it can invoke without risking a freed pointer.
///          Strings, raw function pointers, and other heap classes all return 0 — the
///          caller treats those as misuse and traps. Returns 1 only for objects whose
///          heap-kind is `RT_HEAP_OBJECT` and whose class id matches.
/// @param callback Opaque candidate callback wrapper.
/// @return 1 for a live wrapper of the expected class, otherwise 0.
static int mb_callback_is_native(void *callback) {
    if (!callback)
        return 0;
    if (rt_string_is_handle(callback))
        return 0;
    rt_heap_info_t info;
    if (rt_heap_get_info(callback, &info))
        return (rt_heap_kind_t)info.kind == RT_HEAP_OBJECT &&
               info.class_id == RT_MSGBUS_CALLBACK_CLASS_ID;
    return 0;
}

/// @brief Invoke a registered callback with @p data, trapping if the handle is no longer valid.
/// @details Re-validates the callback's class id at delivery time — a callback could in
///          principle be freed between subscribe and publish, so the bus checks again
///          rather than trust the original validation. Returns 1 when the callback ran,
///          0 when it had a NULL function pointer (defensive — should not normally happen).
/// @param callback Retained managed callback wrapper to validate and invoke.
/// @param data Opaque publish payload forwarded unchanged.
/// @return 1 after invoking a non-NULL function, otherwise 0; invalid wrappers trap.
static int mb_invoke_callback(void *callback, void *data) {
    if (!callback)
        return 0;
    rt_heap_info_t info;
    if (rt_heap_get_info(callback, &info)) {
        if ((rt_heap_kind_t)info.kind != RT_HEAP_OBJECT ||
            info.class_id != RT_MSGBUS_CALLBACK_CLASS_ID) {
            rt_trap("rt_msgbus_publish: subscriber callback is not callable");
            return 0;
        }
        rt_msgbus_callback_impl *cb = (rt_msgbus_callback_impl *)callback;
        if (!cb->fn)
            return 0;
        cb->fn(data);
        return 1;
    }
    rt_trap("rt_msgbus_publish: subscriber callback is not callable");
    return 0;
}

/// @brief Retain a publish payload if it's a runtime-managed handle. Returns 1 iff retained.
/// @details Publish payloads can be either managed runtime objects or arbitrary foreign
///          pointers; this helper distinguishes them by checking `rt_string_is_handle` and
///          `rt_heap_is_payload`. Managed payloads are retained for the duration of the
///          subscriber-snapshot delivery so a concurrent free can't pull them away mid-call;
///          foreign pointers are returned as 0 and the caller leaves them as borrowed.
/// @param data Candidate managed handle or foreign pointer.
/// @return 1 after acquiring a managed retain, otherwise 0.
static int mb_retain_managed_payload(void *data) {
    if (!data)
        return 0;
    if (rt_string_is_handle(data)) {
        rt_memory_retain(data);
        return 1;
    }
    rt_heap_info_t info;
    if (!rt_heap_get_info(data, &info))
        return 0;
    if ((rt_heap_kind_t)info.kind == RT_HEAP_STRING)
        return 0;
    if (rt_heap_is_payload(data)) {
        rt_memory_retain(data);
        return 1;
    }
    return 0;
}

/// @brief Free a single subscriber node (drops topic ref + callback ref).
/// @details Called by `mb_finalizer` (full-bus teardown) and by the
///          unsubscribe path. The callback is treated as a GC-managed
///          opaque object so functions and bound methods both work.
/// @param s Owned subscriber node to release; NULL is a no-op.
static void mb_free_sub(mb_sub *s) {
    if (!s)
        return;
    rt_string topic = s->topic;
    s->next = NULL;
    if (topic) {
        rt_string_unref(topic);
        s->topic = NULL;
    }
    void *callback = s->callback;
    if (callback) {
        int should_free = rt_obj_release_check0(callback);
        s->callback = NULL;
        if (should_free)
            rt_obj_free(callback);
    }
    free(s);
}

/// @brief Free a single topic node — releases the topic name and the allocation itself.
/// @details Used as the per-node primitive by `mb_free_topic_chain` and the unsubscribe
///          path. Does *not* walk the topic's subscriber list — callers that need full
///          teardown should use the chain helper, which handles both layers.
/// @param t Owned, already-detached topic node; NULL is a no-op.
static void mb_free_topic_node(mb_topic *t) {
    if (!t)
        return;
    rt_string name = t->name;
    char *key_bytes = t->key_bytes;
    t->key_bytes = NULL;
    t->key_len = 0;
    t->key_hash = 0;
    t->subs = NULL;
    t->tail = NULL;
    t->next = NULL;
    free(key_bytes);
    if (name) {
        rt_string_unref(name);
        t->name = NULL;
    }
    free(t);
}

/// @brief Free an entire singly-linked subscriber chain.
/// @details Installs a trap-recovery point so that if freeing one subscriber's
///          user data traps, the remaining nodes are still released (no leak)
///          and the original error is preserved and re-raised afterward.
/// @param s Owned head of a detached subscriber chain, or NULL.
static void mb_free_sub_chain(mb_sub *s) {
    mb_sub *volatile cursor = s;
    mb_sub *volatile active_sub = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus: trap while freeing subscribers");
        rt_trap_clear_recovery();
        if (active_sub) {
            mb_sub *current = active_sub;
            active_sub = NULL;
            mb_free_sub(current);
        }
        while (cursor) {
            mb_sub *current = cursor;
            cursor = current->next;
            current->next = NULL;
            mb_free_sub(current);
        }
        rt_trap(saved_error);
        return;
    }

    while (cursor) {
        mb_sub *current = cursor;
        cursor = current->next;
        current->next = NULL;
        active_sub = current;
        mb_free_sub(current);
        active_sub = NULL;
    }
    rt_trap_clear_recovery();
}

/// @brief Free an entire bucket chain of topics, including each topic's subscriber list.
/// @details Two-level walk used by `mb_finalizer`: for each topic node, drain the
///          subscriber list via `mb_free_sub`, then `mb_free_topic_node` to release the
///          name and free the node. Iterative (no recursion) so deep buckets don't
///          consume stack.
/// @param t Owned head of a detached topic chain, or NULL.
static void mb_free_topic_chain(mb_topic *t) {
    mb_topic *volatile cursor = t;
    mb_topic *volatile active_topic = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus: trap while freeing topics");
        rt_trap_clear_recovery();
        if (active_topic) {
            mb_topic *topic = active_topic;
            active_topic = NULL;
            mb_free_topic_node(topic);
        }
        while (cursor) {
            mb_topic *current = cursor;
            cursor = current->next;
            current->next = NULL;
            mb_sub *s = current->subs;
            current->subs = NULL;
            current->tail = NULL;
            mb_free_sub_chain(s);
            mb_free_topic_node(current);
        }
        rt_trap(saved_error);
        return;
    }

    while (cursor) {
        mb_topic *current = cursor;
        cursor = current->next;
        current->next = NULL;
        mb_sub *s = current->subs;
        current->subs = NULL;
        current->tail = NULL;
        active_topic = current;
        mb_free_sub_chain(s);
        active_topic = NULL;
        mb_free_topic_node(current);
    }
    rt_trap_clear_recovery();
}

/// @brief GC finalizer — walk every bucket and free every topic + subscriber.
/// @details Two-level walk: each bucket holds a linked list of
///          `mb_topic` nodes; each topic holds its own linked list
///          of subscribers. Releases topic-name refs and frees
///          everything before nulling the bucket array.
/// @param obj Borrowed MessageBus payload being finalized.
static void mb_finalizer(void *obj) {
    rt_msgbus_impl *mb = (rt_msgbus_impl *)obj;
    rt_gc_mutator_enter();
    mb_lock(mb);
    mb_topic **buckets = mb->buckets;
    int64_t bucket_count = mb->bucket_count;
    mb->buckets = NULL;
    mb->bucket_count = 0;
    mb->topic_count = 0;
    mb->total_subs = 0;
    mb_unlock(mb);
    rt_gc_mutator_exit();

    if (buckets) {
        mb_topic *topics_to_free = NULL;
        for (int64_t i = 0; i < bucket_count; i++) {
            mb_topic *t = buckets[i];
            if (t) {
                mb_topic *tail = t;
                while (tail->next)
                    tail = tail->next;
                tail->next = topics_to_free;
                topics_to_free = t;
                buckets[i] = NULL;
            }
        }
        free(buckets);
        mb_free_topic_chain(topics_to_free);
    }
}

/// @brief GC traversal callback for MessageBus instances.
/// @details Walks every bucket and visits each subscriber's retained callback so the GC
///          marker can colour reachable callbacks live. Topic strings are reference-
///          counted independently and don't participate in cycle detection, so they
///          aren't visited here. The collector invokes traversal while holding the
///          exclusive managed-graph barrier, so subscription mutation is quiescent and
///          this walk needs neither the bus lock nor an allocation-heavy snapshot.
/// @param obj Borrowed MessageBus payload with stable outgoing edges.
/// @param visitor Collector callback invoked for each retained subscriber wrapper.
/// @param ctx Opaque collector context forwarded to @p visitor.
static void mb_traverse(void *obj, rt_gc_visitor_t visitor, void *ctx) {
    rt_msgbus_impl *mb = (rt_msgbus_impl *)obj;
    if (!mb || !visitor || !mb->buckets)
        return;

    for (int64_t i = 0; i < mb->bucket_count; i++) {
        mb_topic *t = mb->buckets[i];
        while (t) {
            for (mb_sub *s = t->subs; s; s = s->next)
                if (s->callback)
                    visitor(s->callback, ctx);
            t = t->next;
        }
    }
}

/// @brief Look up the topic node for `topic_cstr` (returns NULL if not present).
/// @details Hash → bucket → linear probe down the per-bucket chain.
///          The table grows above a 0.75 load factor, keeping expected chain
///          length bounded while preserving stable topic/subscriber nodes.
/// @param mb Locked bus instance with a live bucket array.
/// @param topic_bytes Exact topic bytes, including embedded NUL bytes.
/// @param topic_len Number of bytes in @p topic_bytes.
/// @return Borrowed matching topic node, or NULL.
static mb_topic *mb_find_topic_view_locked(rt_msgbus_impl *mb,
                                           const char *topic_bytes,
                                           size_t topic_len) {
    if (!mb || !mb->buckets || mb->bucket_count <= 0 || !topic_bytes)
        return NULL;

    uint64_t h = mb_hash_bytes(topic_bytes, topic_len);
    int64_t idx = (int64_t)(h % (uint64_t)mb->bucket_count);
    mb_topic *t = mb->buckets[idx];
    while (t) {
        if (t->key_hash == h && t->key_len == topic_len && t->key_bytes &&
            memcmp(t->key_bytes, topic_bytes, topic_len) == 0)
            return t;
        t = t->next;
    }
    return NULL;
}

/// @brief Prepare a detached topic node for possible locked installation.
/// @details Retains the topic string, copies its exact key bytes, computes the
///   hash, and leaves subscriber/link fields zeroed. This helper performs no
///   lookup or insertion; @ref mb_ensure_topic_locked consumes the spare only
///   when no matching topic already exists.
/// @param topic Borrowed runtime string whose reference is retained.
/// @param topic_bytes Borrowed exact byte view of @p topic.
/// @param topic_len Explicit byte length.
/// @return Caller-owned detached topic node, or NULL on allocation or size failure.
static mb_topic *mb_prepare_topic(rt_string topic, const char *topic_bytes, size_t topic_len) {
    rt_string retained_topic = rt_string_ref(topic);
    mb_topic *t = (mb_topic *)calloc(1, sizeof(mb_topic));
    if (!t) {
        rt_string_unref(retained_topic);
        return NULL;
    }
    if (topic_len > (size_t)INT64_MAX) {
        rt_string_unref(retained_topic);
        free(t);
        return NULL;
    }
    char *key = (char *)malloc(topic_len ? topic_len : 1);
    if (!key) {
        rt_string_unref(retained_topic);
        free(t);
        return NULL;
    }
    if (topic_len)
        memcpy(key, topic_bytes, topic_len);
    t->name = retained_topic;
    t->key_bytes = key;
    t->key_len = topic_len;
    t->key_hash = mb_hash_bytes(topic_bytes, topic_len);
    return t;
}

/// @brief Look up @p topic; if absent, install the pre-allocated `*spare` and return it.
/// @details Caller-supplied spare pattern: the caller pre-allocates a fresh `mb_topic`
///          (via `mb_prepare_topic`) before entering the lock, so the locked region never
///          itself calls `malloc`. If the topic already exists, returns it and the spare
///          is left untouched (caller frees it). On install, the function consumes the
///          spare (sets `*spare = NULL`) and links the already-retained topic node at the
///          head of its bucket. Returns NULL when the bus is invalid or when the spare
///          pointer is missing.
/// @param mb Locked bus instance.
/// @param topic_bytes Exact topic key bytes.
/// @param topic_len Key length in bytes.
/// @param spare In/out owned detached topic node; cleared when consumed.
/// @return Borrowed existing or newly installed topic, or NULL after failure.
static mb_topic *mb_ensure_topic_locked(rt_msgbus_impl *mb,
                                        const char *topic_bytes,
                                        size_t topic_len,
                                        mb_topic **spare) {
    if (!mb || !mb->buckets || mb->bucket_count <= 0)
        return NULL;
    if (!topic_bytes)
        return NULL;
    mb_topic *t = mb_find_topic_view_locked(mb, topic_bytes, topic_len);
    if (t)
        return t;

    if (!spare || !*spare) {
        rt_trap("rt_msgbus: memory allocation failed");
        return NULL;
    }
    if (mb->topic_count == INT64_MAX) {
        rt_trap("rt_msgbus: topic count overflow");
        return NULL;
    }
    t = *spare;
    *spare = NULL;
    t->subs = NULL;
    t->tail = NULL;
    t->count = 0;

    uint64_t h = t->key_hash;
    int64_t idx = (int64_t)(h % (uint64_t)mb->bucket_count);
    t->next = mb->buckets[idx];
    mb->buckets[idx] = t;
    mb->topic_count++;
    return t;
}

/// @brief Grow and rehash the topic bucket table when chains become crowded.
/// @details Uses a 0.75 load threshold and doubles the bucket count. Allocation
///          failure is deliberately best-effort: the existing table remains
///          valid and subscription still succeeds, merely retaining longer
///          collision chains. Topic and subscriber nodes never move.
/// @param mb Locked MessageBus instance with a valid bucket table.
static void mb_maybe_grow_topics_locked(rt_msgbus_impl *mb) {
    if (!mb || !mb->buckets || mb->bucket_count <= 0 || mb->topic_count <= 0)
        return;
    if (mb->topic_count <= mb->bucket_count - mb->bucket_count / 4)
        return;
    if (mb->bucket_count > INT64_MAX / 2)
        return;

    int64_t new_count = mb->bucket_count * 2;
    if ((uint64_t)new_count > (uint64_t)SIZE_MAX / sizeof(mb_topic *))
        return;
    mb_topic **new_buckets = (mb_topic **)calloc((size_t)new_count, sizeof(*new_buckets));
    if (!new_buckets)
        return;

    for (int64_t index = 0; index < mb->bucket_count; ++index) {
        mb_topic *topic = mb->buckets[index];
        while (topic) {
            mb_topic *next = topic->next;
            int64_t new_index = (int64_t)(topic->key_hash % (uint64_t)new_count);
            topic->next = new_buckets[new_index];
            new_buckets[new_index] = topic;
            topic = next;
        }
    }

    free(mb->buckets);
    mb->buckets = new_buckets;
    mb->bucket_count = new_count;
}

// --- Public API ---

/// @brief Create a new MessageBus with 32 initial hash buckets and ID counter starting at 1.
/// @details Subscriber IDs start at 1 so 0 can act as a "no subscription"
///          sentinel without ambiguity. The bucket table expands automatically
///          as distinct topics are installed. Allocation, finalizer setup, and
///          GC tracking are guarded by local trap recovery so partial state is
///          released before the error is re-raised.
/// @return Caller-owned managed MessageBus object, or NULL after a returning
///   allocation or tracking trap.
void *rt_msgbus_new(void) {
    mb_topic **buckets = (mb_topic **)calloc(32, sizeof(mb_topic *));
    if (!buckets) {
        rt_trap("rt_msgbus: memory allocation failed");
        return NULL;
    }

    rt_msgbus_impl *mb = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_new: allocation or GC track failed");
        rt_trap_clear_recovery();
        if (mb) {
            mb_topic **owned_buckets = mb->buckets;
            if (buckets != owned_buckets)
                free(buckets);
            buckets = NULL;
            free(owned_buckets);
            mb->buckets = NULL;
            if (rt_obj_release_check0(mb))
                rt_obj_free(mb);
        } else {
            free(buckets);
        }
        rt_trap(saved_error);
        return NULL;
    }

    mb = (rt_msgbus_impl *)rt_obj_new_i64(RT_MSGBUS_CLASS_ID, (int64_t)sizeof(rt_msgbus_impl));
    mb->bucket_count = 32;
    mb->topic_count = 0;
    mb->buckets = buckets;
    buckets = NULL;
    mb->next_id = 1;
    mb->total_subs = 0;
    rt_obj_set_finalizer(mb, mb_finalizer);
    rt_gc_track(mb, mb_traverse);
    rt_trap_clear_recovery();
    return (void *)mb;
}

/// @brief Wrap a native callback in a reference-counted managed object.
/// @param callback Non-NULL function invoked synchronously with publish data.
/// @return Caller-owned callback wrapper, or NULL for a NULL function or
///   allocation failure after a returning trap.
void *rt_msgbus_callback_new(rt_msgbus_callback_fn callback) {
    if (!callback)
        return NULL;
    rt_msgbus_callback_impl *cb = (rt_msgbus_callback_impl *)rt_obj_new_i64(
        RT_MSGBUS_CALLBACK_CLASS_ID, (int64_t)sizeof(rt_msgbus_callback_impl));
    if (!cb)
        return NULL;
    cb->fn = callback;
    return cb;
}

/// @brief Subscribe a callback to a topic on the message bus.
/// @details Adds the callback to the topic's subscriber list. When a message is
///          published to this topic, the callback will be invoked with the
///          payload. Topic identity uses the complete byte string, and the
///          callback and topic are retained until removal. Appends preserve
///          delivery order. Allocation, type, retain, and counter failures trap.
/// @param obj Borrowed MessageBus object; NULL returns -1.
/// @param topic Borrowed non-NULL runtime string topic.
/// @param callback Borrowed callback wrapper returned by
///   @ref rt_msgbus_callback_new.
/// @return Unique positive subscription ID, or -1 on missing arguments or
///   after a returning error trap.
int64_t rt_msgbus_subscribe(void *obj, rt_string topic, void *callback) {
    if (!obj || !topic || !callback)
        return -1;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_subscribe");
    if (!mb)
        return -1;
    const char *topic_bytes = NULL;
    size_t topic_len = 0;
    if (!mb_topic_view(topic, &topic_bytes, &topic_len)) {
        mb_release_bus(mb);
        return -1;
    }
    if (!mb_callback_is_native(callback)) {
        mb_release_bus(mb);
        rt_trap("rt_msgbus_subscribe: callback must be a MessageBus callback object");
        return -1;
    }

    mb_topic *spare_topic = NULL;
    rt_string retained_topic = NULL;
    void *retained_callback = NULL;
    mb_sub *s = NULL;
    volatile int locked = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_subscribe: trap during subscribe");
        rt_trap_clear_recovery();
        if (locked)
            mb_unlock(mb);
        free(s);
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        rt_trap(saved_error);
        return -1;
    }

    spare_topic = mb_prepare_topic(topic, topic_bytes, topic_len);
    if (!spare_topic) {
        rt_trap("rt_msgbus: memory allocation failed");
        rt_trap_clear_recovery();
        mb_release_bus(mb);
        return -1;
    }
    retained_topic = rt_string_ref(topic);
    rt_obj_retain_maybe(callback);
    retained_callback = callback;
    if (!mb_callback_is_native(retained_callback)) {
        rt_trap("rt_msgbus_subscribe: callback must be a live MessageBus callback object");
        rt_trap_clear_recovery();
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }

    s = (mb_sub *)calloc(1, sizeof(mb_sub));
    if (!s) {
        rt_trap("rt_msgbus: memory allocation failed");
        rt_trap_clear_recovery();
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }

    rt_gc_mutator_enter();
    mb_lock(mb);
    locked = 1;
    if (mb->next_id <= 0) {
        rt_trap("rt_msgbus_subscribe: subscription id overflow");
        locked = 0;
        mb_unlock(mb);
        rt_gc_mutator_exit();
        rt_trap_clear_recovery();
        free(s);
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }
    if (mb->total_subs == INT64_MAX) {
        rt_trap("rt_msgbus_subscribe: subscription count overflow");
        locked = 0;
        mb_unlock(mb);
        rt_gc_mutator_exit();
        rt_trap_clear_recovery();
        free(s);
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }
    mb_topic *t = mb_ensure_topic_locked(mb, topic_bytes, topic_len, &spare_topic);
    if (!t) {
        locked = 0;
        mb_unlock(mb);
        rt_gc_mutator_exit();
        rt_trap_clear_recovery();
        free(s);
        mb_free_topic_node(spare_topic);
        rt_string_unref(retained_topic);
        if (rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }

    if (t->count == INT64_MAX) {
        rt_trap("rt_msgbus_subscribe: subscription count overflow");
        locked = 0;
        mb_unlock(mb);
        rt_gc_mutator_exit();
        rt_trap_clear_recovery();
        free(s);
        mb_free_topic_node(spare_topic);
        if (retained_topic)
            rt_string_unref(retained_topic);
        if (retained_callback && rt_obj_release_check0(retained_callback))
            rt_obj_free(retained_callback);
        mb_release_bus(mb);
        return -1;
    }

    s->id = mb->next_id;
    mb->next_id = mb->next_id == INT64_MAX ? 0 : mb->next_id + 1;
    s->topic = retained_topic;
    s->callback = retained_callback;
    s->next = NULL;
    if (!t->subs) {
        t->subs = s;
        t->tail = s;
    } else {
        t->tail->next = s;
        t->tail = s;
    }
    t->count++;
    mb->total_subs++;
    mb_maybe_grow_topics_locked(mb);
    int64_t id = s->id;

    s = NULL;
    retained_topic = NULL;
    retained_callback = NULL;
    locked = 0;
    mb_unlock(mb);
    rt_gc_mutator_exit();
    rt_trap_clear_recovery();
    mb_free_topic_node(spare_topic);
    mb_release_bus(mb);
    return id;
}

/// @brief Remove a subscription from the message bus by its unique ID.
/// @details Performs a linear scan across all topic buckets and their subscriber
///          chains to locate the subscription matching sub_id. This O(B*S) scan
///          is acceptable because unsubscribe is infrequent relative to publish,
///          and maintaining a separate ID→subscription index would add complexity
///          for little practical benefit. Once found, the node is unlinked from
///          the singly-linked list using the classic pointer-to-pointer technique,
///          its topic string and callback are released, and the node is freed.
/// @param obj MessageBus object pointer; returns 0 if NULL.
/// @param sub_id The subscription ID returned by rt_msgbus_subscribe.
/// @return 1 if the subscription was found and removed, 0 if not found.
int8_t rt_msgbus_unsubscribe(void *obj, int64_t sub_id) {
    if (!obj)
        return 0;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_unsubscribe");
    if (!mb)
        return 0;

    mb_sub *victim = NULL;
    mb_topic *empty_topic = NULL;
    rt_gc_mutator_enter();
    mb_lock(mb);
    for (int64_t i = 0; i < mb->bucket_count; i++) {
        mb_topic **tp = &mb->buckets[i];
        while (*tp) {
            mb_topic *t = *tp;
            mb_sub **pp = &t->subs;
            mb_sub *previous = NULL;
            while (*pp) {
                if ((*pp)->id == sub_id) {
                    victim = *pp;
                    *pp = victim->next;
                    victim->next = NULL;
                    if (t->tail == victim)
                        t->tail = previous;
                    t->count--;
                    mb->total_subs--;
                    if (t->count == 0) {
                        empty_topic = t;
                        *tp = t->next;
                        t->next = NULL;
                        t->tail = NULL;
                        mb->topic_count--;
                    }
                    mb_unlock(mb);
                    rt_gc_mutator_exit();
                    jmp_buf recovery;
                    rt_trap_set_recovery(&recovery);
                    if (setjmp(recovery) != 0) {
                        char saved_error[512];
                        const char *err = rt_trap_get_error();
                        snprintf(saved_error,
                                 sizeof(saved_error),
                                 "%s",
                                 err && err[0]
                                     ? err
                                     : "rt_msgbus_unsubscribe: trap while freeing subscription");
                        rt_trap_clear_recovery();
                        mb_free_sub(victim);
                        mb_free_topic_node(empty_topic);
                        mb_release_bus(mb);
                        rt_trap(saved_error);
                        return 0;
                    }
                    mb_free_sub(victim);
                    mb_free_topic_node(empty_topic);
                    rt_trap_clear_recovery();
                    mb_release_bus(mb);
                    return 1;
                }
                previous = *pp;
                pp = &(*pp)->next;
            }
            tp = &t->next;
        }
    }
    mb_unlock(mb);
    rt_gc_mutator_exit();
    mb_release_bus(mb);
    return 0;
}

/// @brief Publish a message to all subscribers of a topic.
/// @details Looks up the topic by name using FNV-1a hashing, snapshots the
///          current subscriber list, then invokes each callback synchronously
///          in subscription-insertion order. Unsubscribe/Clear during a publish
///          affect future publishes but do not invalidate the in-flight
///          snapshot. Managed callbacks and data are retained outside the bus
///          lock. A callback trap stops delivery, releases all temporary
///          retains, and re-raises the diagnostic.
/// @param obj Borrowed MessageBus object; NULL returns zero.
/// @param topic Borrowed non-NULL exact-byte topic name.
/// @param data Opaque payload passed unchanged. Managed handles are retained
///   during dispatch; foreign pointers remain borrowed.
/// @return Number of snapshotted callbacks processed, zero when none or after
///   a returning setup/delivery trap.
int64_t rt_msgbus_publish(void *obj, rt_string topic, void *data) {
    if (!obj || !topic)
        return 0;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_publish");
    if (!mb)
        return 0;

    const char *topic_bytes = NULL;
    size_t topic_len = 0;
    if (!mb_topic_view(topic, &topic_bytes, &topic_len)) {
        mb_release_bus(mb);
        return 0;
    }

    rt_gc_mutator_enter();
    mb_lock(mb);
    mb_topic *t = mb_find_topic_view_locked(mb, topic_bytes, topic_len);
    if (!t || t->count <= 0) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        mb_release_bus(mb);
        return 0;
    }

    if ((uint64_t)t->count > (uint64_t)SIZE_MAX / sizeof(void *)) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        mb_release_bus(mb);
        rt_trap("rt_msgbus_publish: subscriber count too large");
        return 0;
    }

    void **callbacks = (void **)calloc((size_t)t->count, sizeof(void *));
    if (!callbacks) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        mb_release_bus(mb);
        rt_trap("rt_msgbus: memory allocation failed");
        return 0;
    }
    unsigned char *callback_retained =
        (unsigned char *)calloc((size_t)t->count, sizeof(unsigned char));
    if (!callback_retained) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        free(callbacks);
        mb_release_bus(mb);
        rt_trap("rt_msgbus: memory allocation failed");
        return 0;
    }

    int64_t count = 0;
    int64_t snapshot_cap = t->count;
    int retain_error = 0;
    for (mb_sub *s = t->subs; s && count < snapshot_cap; s = s->next) {
        int retained = rt_heap_try_retain_live(s->callback);
        if (retained < 0) {
            retain_error = 1;
            break;
        }
        if (retained > 0) {
            callback_retained[count] = retained == 1;
            callbacks[count++] = s->callback;
        }
    }
    mb_unlock(mb);
    rt_gc_mutator_exit();
    if (retain_error) {
        for (int64_t i = 0; i < count; ++i)
            mb_release_snapshot_callback(callbacks, callback_retained, i);
        free(callback_retained);
        free(callbacks);
        mb_release_bus(mb);
        rt_trap("rt_msgbus_publish: callback refcount overflow");
        return 0;
    }

    int data_retained = 0;
    volatile int64_t release_from = 0;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_publish: subscriber trap");
        rt_trap_clear_recovery();
        for (int64_t i = (int64_t)release_from; i < count; ++i)
            mb_release_snapshot_callback(callbacks, callback_retained, i);
        if (data_retained)
            (void)rt_memory_release(data);
        free(callback_retained);
        free(callbacks);
        mb_release_bus(mb);
        rt_trap(saved_error);
        return 0;
    }
    data_retained = mb_retain_managed_payload(data);
    for (int64_t i = 0; i < count; ++i) {
        release_from = i;
        mb_invoke_callback(callbacks[i], data);
        mb_release_snapshot_callback(callbacks, callback_retained, i);
        release_from = i + 1;
    }

    if (data_retained)
        (void)rt_memory_release(data);
    rt_trap_clear_recovery();
    free(callback_retained);
    free(callbacks);
    mb_release_bus(mb);
    return count;
}

/// @brief Return the number of active subscribers for a specific topic.
/// @details Looks up the topic bucket via FNV-1a hash and returns the cached
///          subscriber count. This is O(1) after the hash lookup because each
///          mb_topic maintains a running count that is incremented on subscribe
///          and decremented on unsubscribe, avoiding a linked-list traversal.
/// @param obj MessageBus object pointer; returns 0 if NULL.
/// @param topic Topic name string to query.
/// @return Number of subscribers for the topic, 0 if topic not found or NULL.
int64_t rt_msgbus_subscriber_count(void *obj, rt_string topic) {
    if (!obj || !topic)
        return 0;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_subscriber_count");
    if (!mb)
        return 0;
    const char *topic_bytes = NULL;
    size_t topic_len = 0;
    if (!mb_topic_view(topic, &topic_bytes, &topic_len)) {
        mb_release_bus(mb);
        return 0;
    }
    rt_gc_mutator_enter();
    mb_lock(mb);
    mb_topic *t = mb_find_topic_view_locked(mb, topic_bytes, topic_len);
    int64_t count = t ? t->count : 0;
    mb_unlock(mb);
    rt_gc_mutator_exit();
    mb_release_bus(mb);
    return count;
}

/// @brief Return the total number of active subscriptions across all topics.
/// @details Returns the cached total_subs counter maintained by the bus. This
///          is a global aggregate: it is incremented on every subscribe and
///          decremented on every unsubscribe or clear operation. Useful for
///          diagnostics and monitoring bus activity without iterating topics.
/// @param obj MessageBus object pointer; returns 0 if NULL.
/// @return Total subscription count across all topics.
int64_t rt_msgbus_total_subscriptions(void *obj) {
    if (!obj)
        return 0;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_total_subscriptions");
    if (!mb)
        return 0;
    rt_gc_mutator_enter();
    mb_lock(mb);
    int64_t total = mb->total_subs;
    mb_unlock(mb);
    rt_gc_mutator_exit();
    mb_release_bus(mb);
    return total;
}

/// @brief Return a Seq containing the names of all topics that have at least one subscriber.
/// @details Copies exact key bytes while holding the bus lock, then constructs
///   owned strings and a sequence after releasing it. Ordering follows current
///   hash-bucket traversal and is not a subscription-order guarantee.
/// @param obj Borrowed MessageBus object; NULL produces an empty sequence.
/// @return Caller-owned managed `Seq[str]`, or NULL after a returning setup,
///   allocation, or conversion trap.
void *rt_msgbus_topics(void *obj) {
    rt_msgbus_impl *mb = NULL;
    void *seq = NULL;
    jmp_buf setup_recovery;
    rt_trap_set_recovery(&setup_recovery);
    if (setjmp(setup_recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_topics: trap during setup");
        rt_trap_clear_recovery();
        if (seq && rt_obj_release_check0(seq))
            rt_obj_free(seq);
        mb_release_bus(mb);
        rt_trap(saved_error);
        return NULL;
    }

    if (obj) {
        mb = mb_require_retained(obj, "rt_msgbus_topics");
        if (!mb) {
            rt_trap_clear_recovery();
            return NULL;
        }
    }

    seq = rt_seq_new();
    if (!seq) {
        rt_trap_clear_recovery();
        mb_release_bus(mb);
        return NULL;
    }
    rt_seq_set_owns_elements(seq, 1);
    if (!mb) {
        rt_trap_clear_recovery();
        return seq;
    }

    int64_t count = 0;
    mb_lock(mb);
    for (int64_t i = 0; i < mb->bucket_count; i++) {
        mb_topic *t = mb->buckets[i];
        while (t) {
            if (t->count > 0)
                count++;
            t = t->next;
        }
    }
    mb_unlock(mb);

    if (count <= 0) {
        rt_trap_clear_recovery();
        mb_release_bus(mb);
        return seq;
    }

    mb_topic_copy *topics = (mb_topic_copy *)calloc((size_t)count, sizeof(mb_topic_copy));
    if (!topics) {
        rt_trap_clear_recovery();
        mb_release_bus(mb);
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        rt_trap("rt_msgbus: memory allocation failed");
        return NULL;
    }

    int64_t copied = 0;
    int oom = 0;
    mb_lock(mb);
    for (int64_t i = 0; i < mb->bucket_count && copied < count; i++) {
        mb_topic *t = mb->buckets[i];
        while (t && copied < count) {
            if (t->count > 0) {
                char *bytes = (char *)malloc(t->key_len ? t->key_len : 1);
                if (!bytes) {
                    oom = 1;
                    break;
                }
                if (t->key_len)
                    memcpy(bytes, t->key_bytes, t->key_len);
                topics[copied].bytes = bytes;
                topics[copied].len = t->key_len;
                copied++;
            }
            t = t->next;
        }
        if (oom)
            break;
    }
    mb_unlock(mb);
    mb_release_bus(mb);
    mb = NULL;
    rt_trap_clear_recovery();

    if (oom) {
        for (int64_t i = 0; i < copied; ++i)
            free(topics[i].bytes);
        free(topics);
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        rt_trap("rt_msgbus: memory allocation failed");
        return NULL;
    }

    volatile rt_string active_topic_name = NULL;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_topics: trap while building topic sequence");
        rt_trap_clear_recovery();
        rt_str_release_maybe((rt_string)active_topic_name);
        active_topic_name = NULL;
        for (int64_t i = 0; i < copied; ++i) {
            free(topics[i].bytes);
        }
        free(topics);
        if (rt_obj_release_check0(seq))
            rt_obj_free(seq);
        rt_trap(saved_error);
        return NULL;
    }

    for (int64_t i = 0; i < copied; ++i) {
        rt_string topic_name = rt_string_from_bytes(topics[i].bytes, topics[i].len);
        active_topic_name = topic_name;
        rt_seq_push(seq, topic_name);
        rt_string_unref(topic_name);
        active_topic_name = NULL;
        free(topics[i].bytes);
        topics[i].bytes = NULL;
    }
    rt_trap_clear_recovery();
    free(topics);
    return seq;
}

/// @brief Remove all subscriptions from a single topic and remove its topic node.
/// @details Walks the subscriber linked list for the given topic, freeing each
///          node (releasing its topic string reference and callback).
///          The topic node is removed from the hash table when the last
///          subscriber is cleared, so Topics() never needs to skip stale
///          zero-count nodes.
/// @param obj MessageBus object pointer; no-op if NULL.
/// @param topic Topic name string to clear; no-op if NULL or not found.
void rt_msgbus_clear_topic(void *obj, rt_string topic) {
    if (!obj || !topic)
        return;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_clear_topic");
    if (!mb)
        return;
    const char *topic_bytes = NULL;
    size_t topic_len = 0;
    if (!mb_topic_view(topic, &topic_bytes, &topic_len)) {
        mb_release_bus(mb);
        return;
    }
    rt_gc_mutator_enter();
    mb_lock(mb);
    mb_topic *t = mb_find_topic_view_locked(mb, topic_bytes, topic_len);
    if (!t) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        mb_release_bus(mb);
        return;
    }

    mb_topic *removed = NULL;
    for (int64_t i = 0; i < mb->bucket_count && !removed; ++i) {
        mb_topic **tp = &mb->buckets[i];
        while (*tp) {
            if (*tp == t) {
                removed = t;
                *tp = t->next;
                t->next = NULL;
                break;
            }
            tp = &(*tp)->next;
        }
    }
    if (!removed) {
        mb_unlock(mb);
        rt_gc_mutator_exit();
        mb_release_bus(mb);
        rt_trap("rt_msgbus_clear_topic: topic unlink failed");
        return;
    }
    mb_sub *s = removed->subs;
    removed->subs = NULL;
    removed->tail = NULL;
    mb->total_subs -= removed->count;
    removed->count = 0;
    mb->topic_count--;
    mb_unlock(mb);
    rt_gc_mutator_exit();
    mb_release_bus(mb);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[512];
        const char *err = rt_trap_get_error();
        snprintf(saved_error,
                 sizeof(saved_error),
                 "%s",
                 err && err[0] ? err : "rt_msgbus_clear_topic: trap while freeing topic");
        rt_trap_clear_recovery();
        mb_free_topic_node(removed);
        rt_trap(saved_error);
        return;
    }
    mb_free_sub_chain(s);
    mb_free_topic_node(removed);
    rt_trap_clear_recovery();
}

/// @brief Remove all subscriptions and topic nodes from the message bus.
/// @details Iterates all buckets and all topic chains, freeing every subscriber
///          node in each topic. The bucket array remains allocated so the bus is
///          reusable; topic nodes are recreated on the next subscription.
/// @param obj MessageBus object pointer; no-op if NULL.
void rt_msgbus_clear(void *obj) {
    if (!obj)
        return;
    rt_msgbus_impl *mb = mb_require_retained(obj, "rt_msgbus_clear");
    if (!mb)
        return;

    mb_topic *topics_to_free = NULL;
    rt_gc_mutator_enter();
    mb_lock(mb);
    for (int64_t i = 0; i < mb->bucket_count; i++) {
        mb_topic *t = mb->buckets[i];
        if (t) {
            mb_topic *tail = t;
            while (tail->next)
                tail = tail->next;
            tail->next = topics_to_free;
            topics_to_free = t;
            mb->buckets[i] = NULL;
        }
    }
    mb->total_subs = 0;
    mb->topic_count = 0;
    mb_unlock(mb);
    rt_gc_mutator_exit();
    mb_release_bus(mb);

    mb_free_topic_chain(topics_to_free);
}
