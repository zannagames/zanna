//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTMsgBusTests.cpp
// Purpose: Validate message-bus subscription, publication, ownership, and GC behavior.
// Key invariants: Subscribers observe insertion order and published managed
//                 payloads remain alive for the complete callback snapshot.
// Ownership/Lifetime: Each case releases its bus, topics, callbacks, and
//                     payloads after all callback activity has completed.
// Links: src/runtime/core/rt_msgbus.c
//
//===----------------------------------------------------------------------===//

#include "rt_gc.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_msgbus.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <string>

/// @brief Erase constness so borrowed text can travel through an opaque `void *`.
/// @details The runtime payload APIs take `void *`; these tests hand them
///          read-only bytes they own elsewhere and never write through the
///          result, so the cast is safe here.
/// @param text Borrowed NUL-terminated bytes.
/// @return The same address as a mutable `void *`.
static void *borrowedPayload(const char *text) {
    return const_cast<void *>(static_cast<const void *>(text));
}


extern "C" void rt_trap_set_recovery(jmp_buf *buf);
extern "C" void rt_trap_clear_recovery(void);
extern "C" const char *rt_trap_get_error(void);

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, std::strlen(s));
}

static void destroy_obj(void *obj) {
    if (rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

static int64_t subscribe_native(void *bus, rt_string topic, rt_msgbus_callback_fn fn) {
    void *callback = rt_msgbus_callback_new(fn);
    assert(callback != NULL);
    int64_t id = rt_msgbus_subscribe(bus, topic, callback);
    destroy_obj(callback);
    return id;
}

static int g_trace_len = 0;
static int g_trace[8];
static int g_last_payload = 0;
static const char *g_last_text_payload = NULL;
static void *g_current_bus = NULL;
static int64_t g_victim_sub_id = 0;
static int g_callback_finalized = 0;
static int g_trapping_callback_finalized = 0;
static int g_second_callback_finalized = 0;
static int g_payload_finalized = 0;
static int g_bus_finalized = 0;

static void reset_trace() {
    g_trace_len = 0;
    std::memset(g_trace, 0, sizeof(g_trace));
    g_last_payload = 0;
    g_last_text_payload = NULL;
}

static void cb_first(void *data) {
    g_trace[g_trace_len++] = 1;
    g_last_payload = *(int *)data;
}

static void cb_second(void *data) {
    g_trace[g_trace_len++] = 2;
    g_last_payload = *(int *)data;
}

static void cb_third(void *data) {
    g_trace[g_trace_len++] = 3;
    g_last_payload = *(int *)data;
}

static void cb_trap(void *data) {
    (void)data;
    rt_trap("subscriber boom");
}

static void cb_release_payload(void *data) {
    g_trace[g_trace_len++] = 4;
    g_last_payload = *(int *)data;
    assert(rt_memory_release(data) == 1);
}

static void cb_capture_text_payload(void *data) {
    g_trace[g_trace_len++] = 6;
    g_last_text_payload = (const char *)data;
}

static void cb_unsubscribe_other(void *data) {
    g_trace[g_trace_len++] = 10;
    g_last_payload = *(int *)data;
    if (g_current_bus && g_victim_sub_id > 0) {
        assert(rt_msgbus_unsubscribe(g_current_bus, g_victim_sub_id) == 1);
        g_victim_sub_id = 0;
    }
}

static void callback_finalizer(void *obj) {
    (void)obj;
    g_callback_finalized++;
}

static void trapping_callback_finalizer(void *obj) {
    (void)obj;
    g_trapping_callback_finalized++;
    rt_trap("callback finalizer boom");
}

static void second_callback_finalizer(void *obj) {
    (void)obj;
    g_second_callback_finalized++;
}

static void payload_finalizer(void *obj) {
    (void)obj;
    g_payload_finalized++;
}

static void bus_finalizer(void *obj) {
    (void)obj;
    g_bus_finalized++;
}

static void cb_release_current_bus(void *data) {
    (void)data;
    g_trace[g_trace_len++] = 5;
    if (g_current_bus) {
        destroy_obj(g_current_bus);
        g_current_bus = NULL;
    }
}

struct MsgBusSubMirror {
    int64_t id;
    rt_string topic;
    void *callback;
    MsgBusSubMirror *next;
};

struct MsgBusTopicMirror {
    rt_string name;
    char *key_bytes;
    size_t key_len;
    uint64_t key_hash;
    MsgBusSubMirror *subs;
    MsgBusSubMirror *tail;
    int64_t count;
    MsgBusTopicMirror *next;
};

struct MsgBusMirror {
    void *vptr;
    MsgBusTopicMirror **buckets;
    int64_t bucket_count;
    int64_t topic_count;
    int64_t next_id;
    int64_t total_subs;
    int lock;
};

static uint64_t test_topic_hash(const char *s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int test_topic_bucket(const std::string &topic) {
    return (int)(test_topic_hash(topic.data(), topic.size()) % 32u);
}

static MsgBusTopicMirror *find_topic(void *bus, rt_string topic) {
    MsgBusMirror *state = (MsgBusMirror *)bus;
    const char *bytes = rt_string_cstr(topic);
    size_t len = (size_t)rt_str_len(topic);
    for (int64_t i = 0; i < state->bucket_count; ++i) {
        MsgBusTopicMirror *t = state->buckets[i];
        while (t) {
            if (t->key_len == len && std::memcmp(t->key_bytes, bytes, len) == 0)
                return t;
            t = t->next;
        }
    }
    return nullptr;
}

static void test_new() {
    void *bus = rt_msgbus_new();
    assert(bus != NULL);
    assert(rt_obj_class_id(bus) == RT_MSGBUS_CLASS_ID);
    assert(rt_gc_is_tracked(bus) == 1);
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    destroy_obj(bus);
}

static void test_callback_wrapper() {
    void *bus = rt_msgbus_new();
    void *callback = rt_msgbus_callback_new(cb_first);
    assert(callback != NULL);
    assert(rt_obj_class_id(callback) == RT_MSGBUS_CALLBACK_CLASS_ID);
    rt_string topic = make_str("wrapped");
    int payload = 55;

    assert(rt_msgbus_subscribe(bus, topic, callback) > 0);
    destroy_obj(callback);

    reset_trace();
    assert(rt_msgbus_publish(bus, topic, &payload) == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 1);
    assert(g_last_payload == 55);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_subscribe() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("click");

    int64_t id = subscribe_native(bus, topic, cb_first);
    assert(id > 0);
    assert(rt_msgbus_total_subscriptions(bus) == 1);
    assert(rt_msgbus_subscriber_count(bus, topic) == 1);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_multiple_subscribers() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("event");

    subscribe_native(bus, topic, cb_first);
    subscribe_native(bus, topic, cb_second);
    subscribe_native(bus, topic, cb_third);

    assert(rt_msgbus_subscriber_count(bus, topic) == 3);
    assert(rt_msgbus_total_subscriptions(bus) == 3);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_multiple_topics() {
    void *bus = rt_msgbus_new();
    rt_string t1 = make_str("topic1");
    rt_string t2 = make_str("topic2");

    subscribe_native(bus, t1, cb_first);
    subscribe_native(bus, t2, cb_second);

    assert(rt_msgbus_subscriber_count(bus, t1) == 1);
    assert(rt_msgbus_subscriber_count(bus, t2) == 1);
    assert(rt_msgbus_total_subscriptions(bus) == 2);

    rt_string_unref(t1);
    rt_string_unref(t2);
    destroy_obj(bus);
}

static void test_embedded_nul_topics_are_distinct() {
    void *bus = rt_msgbus_new();
    const char raw_a[] = {'a'};
    const char raw_ax[] = {'a', '\0', 'x'};
    rt_string a = rt_string_from_bytes(raw_a, sizeof(raw_a));
    rt_string ax = rt_string_from_bytes(raw_ax, sizeof(raw_ax));
    int payload = 9;

    subscribe_native(bus, a, cb_first);
    subscribe_native(bus, ax, cb_second);

    reset_trace();
    assert(rt_msgbus_publish(bus, a, &payload) == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 1);

    reset_trace();
    assert(rt_msgbus_publish(bus, ax, &payload) == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 2);

    rt_string_unref(a);
    rt_string_unref(ax);
    destroy_obj(bus);
}

static void test_unsubscribe() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("test");

    int64_t id = subscribe_native(bus, topic, cb_first);
    assert(rt_msgbus_unsubscribe(bus, id) == 1);
    assert(rt_msgbus_subscriber_count(bus, topic) == 0);
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    void *topics = rt_msgbus_topics(bus);
    assert(rt_seq_len(topics) == 0);
    destroy_obj(topics);

    int64_t id2 = subscribe_native(bus, topic, cb_second);
    assert(id2 > 0);
    assert(rt_msgbus_subscriber_count(bus, topic) == 1);
    assert(rt_msgbus_total_subscriptions(bus) == 1);

    assert(rt_msgbus_unsubscribe(bus, id) == 0);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_publish_invokes_callbacks_in_order() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("signal");
    int payload = 42;

    subscribe_native(bus, topic, cb_first);
    subscribe_native(bus, topic, cb_second);
    subscribe_native(bus, topic, cb_third);

    reset_trace();
    int64_t notified = rt_msgbus_publish(bus, topic, &payload);
    assert(notified == 3);
    assert(g_trace_len == 3);
    assert(g_trace[0] == 1);
    assert(g_trace[1] == 2);
    assert(g_trace[2] == 3);
    assert(g_last_payload == 42);

    rt_string missing = make_str("no_such_topic");
    assert(rt_msgbus_publish(bus, missing, &payload) == 0);

    rt_string_unref(topic);
    rt_string_unref(missing);
    destroy_obj(bus);
}

/// @brief Verify tail removal followed by append preserves subscription order.
/// @details Removes the current tail from a three-subscriber topic, appends a
///          replacement, and publishes. This exercises the O(1) tail cache's
///          unlink maintenance rather than merely its fast append path.
static void test_unsubscribe_tail_then_append_preserves_order() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("tail_append");
    int64_t first = subscribe_native(bus, topic, cb_first);
    int64_t second = subscribe_native(bus, topic, cb_second);
    int64_t tail = subscribe_native(bus, topic, cb_third);
    assert(first > 0 && second > first && tail > second);
    assert(rt_msgbus_unsubscribe(bus, tail) == 1);
    assert(subscribe_native(bus, topic, cb_third) > tail);

    reset_trace();
    int payload = 17;
    assert(rt_msgbus_publish(bus, topic, &payload) == 3);
    assert(g_trace_len == 3);
    assert(g_trace[0] == 1 && g_trace[1] == 2 && g_trace[2] == 3);

    rt_string_unref(topic);
    destroy_obj(bus);
}

/// @brief Verify the topic table grows while retaining every subscription.
/// @details Registers enough distinct topics to cross several 0.75 load
///          thresholds, confirms the internal bucket table expanded, then
///          publishes through every rehashed key to validate chain relinking.
static void test_topic_table_rehashes_under_load() {
    void *bus = rt_msgbus_new();
    constexpr int kTopics = 128;

    for (int index = 0; index < kTopics; ++index) {
        std::string name = "rehash_topic_" + std::to_string(index);
        rt_string topic = make_str(name.c_str());
        assert(subscribe_native(bus, topic, cb_first) > 0);
        rt_string_unref(topic);
    }

    MsgBusMirror *state = (MsgBusMirror *)bus;
    assert(state->topic_count == kTopics);
    assert(state->bucket_count > 32);
    assert(rt_msgbus_total_subscriptions(bus) == kTopics);

    for (int index = 0; index < kTopics; ++index) {
        std::string name = "rehash_topic_" + std::to_string(index);
        rt_string topic = make_str(name.c_str());
        int payload = index;
        reset_trace();
        assert(rt_msgbus_publish(bus, topic, &payload) == 1);
        assert(g_trace_len == 1 && g_last_payload == index);
        rt_string_unref(topic);
    }

    rt_msgbus_clear(bus);
    assert(state->topic_count == 0);
    destroy_obj(bus);
}

static void test_publish_retains_managed_payload_for_dispatch() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("payload_lifetime");
    int *payload = (int *)rt_obj_new_i64(0xDA7A, (int64_t)sizeof(int));
    assert(payload != NULL);
    *payload = 66;
    rt_obj_set_finalizer(payload, payload_finalizer);
    g_payload_finalized = 0;

    subscribe_native(bus, topic, cb_release_payload);
    subscribe_native(bus, topic, cb_second);

    reset_trace();
    assert(rt_msgbus_publish(bus, topic, payload) == 2);
    assert(g_trace_len == 2);
    assert(g_trace[0] == 4);
    assert(g_trace[1] == 2);
    assert(g_last_payload == 66);
    assert(g_payload_finalized == 1);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_publish_borrows_raw_string_payload() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("raw_string_payload");
    rt_string text = make_str("borrowed bytes");
    const char *payload = rt_string_cstr(text);

    subscribe_native(bus, topic, cb_capture_text_payload);

    reset_trace();
    assert(rt_msgbus_publish(bus, topic, borrowedPayload(payload)) == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 6);
    assert(std::strcmp(g_last_text_payload, "borrowed bytes") == 0);
    assert(std::strcmp(rt_string_cstr(text), "borrowed bytes") == 0);

    rt_string_unref(text);
    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_publish_retains_bus_during_callback_release() {
    void *bus = rt_msgbus_new();
    rt_obj_set_finalizer(bus, bus_finalizer);
    rt_string topic = make_str("bus_lifetime");
    int payload = 1;
    g_bus_finalized = 0;
    g_current_bus = bus;

    subscribe_native(bus, topic, cb_release_current_bus);

    reset_trace();
    assert(rt_msgbus_publish(bus, topic, &payload) == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 5);
    assert(g_bus_finalized == 1);
    g_current_bus = NULL;
    rt_string_unref(topic);
}

static void test_unsubscribe_during_publish_uses_snapshot() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("snapshot");
    int payload = 7;

    g_current_bus = bus;
    int64_t unsub_id = subscribe_native(bus, topic, cb_unsubscribe_other);
    g_victim_sub_id = subscribe_native(bus, topic, cb_second);

    reset_trace();
    int64_t notified = rt_msgbus_publish(bus, topic, &payload);
    assert(notified == 2);
    assert(g_trace_len == 2);
    assert(g_trace[0] == 10);
    assert(g_trace[1] == 2);
    assert(rt_msgbus_subscriber_count(bus, topic) == 1);

    reset_trace();
    notified = rt_msgbus_publish(bus, topic, &payload);
    assert(notified == 1);
    assert(g_trace_len == 1);
    assert(g_trace[0] == 10);

    assert(rt_msgbus_unsubscribe(bus, unsub_id) == 1);
    g_current_bus = NULL;
    g_victim_sub_id = 0;
    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_topics() {
    void *bus = rt_msgbus_new();
    rt_string alpha = make_str("alpha");
    rt_string beta = make_str("beta");
    subscribe_native(bus, alpha, cb_first);
    subscribe_native(bus, beta, cb_second);

    void *topics = rt_msgbus_topics(bus);
    assert(rt_seq_len(topics) == 2);
    rt_string first = (rt_string)rt_seq_get(topics, 0);
    rt_string second = (rt_string)rt_seq_get(topics, 1);
    assert((std::strcmp(rt_string_cstr(first), "alpha") == 0 ||
            std::strcmp(rt_string_cstr(second), "alpha") == 0));
    assert((std::strcmp(rt_string_cstr(first), "beta") == 0 ||
            std::strcmp(rt_string_cstr(second), "beta") == 0));

    rt_string_unref(alpha);
    rt_string_unref(beta);
    destroy_obj(bus);
    assert((std::strcmp(rt_string_cstr(first), "alpha") == 0 ||
            std::strcmp(rt_string_cstr(second), "alpha") == 0));
    assert((std::strcmp(rt_string_cstr(first), "beta") == 0 ||
            std::strcmp(rt_string_cstr(second), "beta") == 0));
    destroy_obj(topics);
}

static void test_rejects_plain_heap_callback() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("bad_callback");
    void *plain = rt_obj_new_i64(0, 8);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_subscribe(bus, topic, plain);
        rt_trap_clear_recovery();
        assert(false && "plain heap callback should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("callback must be") != std::string::npos);
    }

    destroy_obj(plain);
    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_rejects_raw_pointer_callback() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("raw_callback");

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_subscribe(bus, topic, (void *)cb_first);
        rt_trap_clear_recovery();
        assert(false && "raw callback pointers must be wrapped");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("callback must be") != std::string::npos);
    }

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_subscribe_retain_trap_releases_bus() {
    void *bus = rt_msgbus_new();
    const char long_topic[] = "topic-name-longer-than-sso-for-overflow-test";
    rt_string topic = rt_string_from_bytes(long_topic, sizeof(long_topic) - 1);
    void *callback = rt_msgbus_callback_new(cb_first);
    rt_heap_hdr_t *topic_hdr = rt_heap_hdr(topic->data);
    topic_hdr->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_subscribe(bus, topic, callback);
        rt_trap_clear_recovery();
        assert(false && "topic retain overflow should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("refcount overflow") != std::string::npos);
    }

    topic_hdr->refcnt = 1;
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    destroy_obj(callback);
    rt_string_unref(topic);
    destroy_obj(bus);
    assert(rt_heap_is_payload(bus) == 0);
}

static void test_publish_trap_releases_snapshot_callbacks() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("trap_cleanup");
    void *callback = rt_msgbus_callback_new(cb_trap);
    rt_obj_set_finalizer(callback, callback_finalizer);
    g_callback_finalized = 0;

    assert(rt_msgbus_subscribe(bus, topic, callback) > 0);
    destroy_obj(callback);

    int payload = 1;
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_publish(bus, topic, &payload);
        rt_trap_clear_recovery();
        assert(false && "publishing trapping callback should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("subscriber boom") != std::string::npos);
    }

    assert(g_callback_finalized == 0);
    rt_msgbus_clear(bus);
    assert(g_callback_finalized == 1);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_subscribe_total_count_overflow_traps() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("total_overflow");
    void *callback = rt_msgbus_callback_new(cb_first);
    MsgBusMirror *state = (MsgBusMirror *)bus;
    state->total_subs = INT64_MAX;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_subscribe(bus, topic, callback);
        rt_trap_clear_recovery();
        assert(false && "total subscription overflow should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("subscription count overflow") != std::string::npos);
    }

    state->total_subs = 0;
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    rt_string_unref(topic);
    destroy_obj(callback);
    destroy_obj(bus);
}

static void test_subscribe_topic_count_overflow_traps() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("topic_overflow");
    void *callback = rt_msgbus_callback_new(cb_first);

    assert(rt_msgbus_subscribe(bus, topic, callback) > 0);
    MsgBusTopicMirror *topic_state = find_topic(bus, topic);
    assert(topic_state != nullptr);
    topic_state->count = INT64_MAX;
    int64_t next_id_before = ((MsgBusMirror *)bus)->next_id;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        (void)rt_msgbus_subscribe(bus, topic, callback);
        rt_trap_clear_recovery();
        assert(false && "topic subscription overflow should trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("subscription count overflow") != std::string::npos);
    }

    topic_state->count = 1;
    assert(((MsgBusMirror *)bus)->next_id == next_id_before);
    assert(rt_msgbus_total_subscriptions(bus) == 1);
    rt_string_unref(topic);
    destroy_obj(callback);
    destroy_obj(bus);
}

static void test_finalizer_trap_drains_later_buckets() {
    std::string trap_name;
    std::string count_name;
    int trap_bucket = 100;
    int count_bucket = -1;
    for (int i = 0; i < 256; ++i) {
        std::string candidate = "finalizer_bucket_" + std::to_string(i);
        int bucket = test_topic_bucket(candidate);
        if (bucket < trap_bucket) {
            trap_bucket = bucket;
            trap_name = candidate;
        }
        if (bucket > count_bucket) {
            count_bucket = bucket;
            count_name = candidate;
        }
    }
    assert(!trap_name.empty());
    assert(!count_name.empty());
    assert(trap_bucket < count_bucket);

    void *bus = rt_msgbus_new();
    rt_string trap_topic = rt_string_from_bytes(trap_name.data(), trap_name.size());
    rt_string count_topic = rt_string_from_bytes(count_name.data(), count_name.size());
    void *trap_callback = rt_msgbus_callback_new(cb_first);
    void *count_callback = rt_msgbus_callback_new(cb_second);
    rt_obj_set_finalizer(trap_callback, trapping_callback_finalizer);
    rt_obj_set_finalizer(count_callback, second_callback_finalizer);
    g_trapping_callback_finalized = 0;
    g_second_callback_finalized = 0;

    assert(rt_msgbus_subscribe(bus, trap_topic, trap_callback) > 0);
    assert(rt_msgbus_subscribe(bus, count_topic, count_callback) > 0);
    destroy_obj(trap_callback);
    destroy_obj(count_callback);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        destroy_obj(bus);
        rt_trap_clear_recovery();
        assert(false && "trapping callback finalizer should re-trap");
    } else {
        std::string message = rt_trap_get_error();
        rt_trap_clear_recovery();
        assert(message.find("callback finalizer boom") != std::string::npos);
    }

    assert(g_trapping_callback_finalized == 1);
    assert(g_second_callback_finalized == 1);
    rt_string_unref(trap_topic);
    rt_string_unref(count_topic);
}

static void test_clear_topic() {
    void *bus = rt_msgbus_new();
    rt_string t = make_str("temp");
    subscribe_native(bus, t, cb_first);
    subscribe_native(bus, t, cb_second);

    rt_msgbus_clear_topic(bus, t);
    assert(rt_msgbus_subscriber_count(bus, t) == 0);
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    void *topics = rt_msgbus_topics(bus);
    assert(rt_seq_len(topics) == 0);
    destroy_obj(topics);

    assert(subscribe_native(bus, t, cb_first) > 0);
    assert(rt_msgbus_subscriber_count(bus, t) == 1);

    rt_string_unref(t);
    destroy_obj(bus);
}

static void test_clear() {
    void *bus = rt_msgbus_new();
    rt_string a = make_str("a");
    rt_string b = make_str("b");
    rt_string c = make_str("c");
    subscribe_native(bus, a, cb_first);
    subscribe_native(bus, b, cb_second);
    subscribe_native(bus, c, cb_third);

    rt_msgbus_clear(bus);
    assert(rt_msgbus_total_subscriptions(bus) == 0);
    void *topics = rt_msgbus_topics(bus);
    assert(rt_seq_len(topics) == 0);
    destroy_obj(topics);

    assert(subscribe_native(bus, b, cb_second) > 0);
    assert(rt_msgbus_total_subscriptions(bus) == 1);
    assert(rt_msgbus_subscriber_count(bus, b) == 1);

    rt_string_unref(a);
    rt_string_unref(b);
    rt_string_unref(c);
    destroy_obj(bus);
}

static void test_callback_object_cleanup_on_unsubscribe() {
    void *bus = rt_msgbus_new();
    rt_string topic = make_str("cleanup");
    void *callback_obj = rt_msgbus_callback_new(cb_first);
    rt_obj_set_finalizer(callback_obj, callback_finalizer);
    g_callback_finalized = 0;

    int64_t id = rt_msgbus_subscribe(bus, topic, callback_obj);
    destroy_obj(callback_obj); // bus now owns the remaining reference
    assert(g_callback_finalized == 0);

    assert(rt_msgbus_unsubscribe(bus, id) == 1);
    assert(g_callback_finalized == 1);

    rt_string_unref(topic);
    destroy_obj(bus);
}

static void test_null_safety() {
    void *bus = rt_msgbus_new();
    rt_string x = make_str("x");
    assert(rt_msgbus_total_subscriptions(NULL) == 0);
    assert(rt_msgbus_subscriber_count(NULL, NULL) == 0);
    assert(rt_msgbus_publish(NULL, NULL, NULL) == 0);
    assert(rt_msgbus_subscribe(NULL, NULL, NULL) == -1);
    void *callback = rt_msgbus_callback_new(cb_first);
    assert(rt_msgbus_subscribe(bus, NULL, callback) == -1);
    destroy_obj(callback);
    assert(rt_msgbus_subscribe(bus, x, NULL) == -1);
    assert(rt_msgbus_unsubscribe(NULL, 1) == 0);
    rt_string_unref(x);
    destroy_obj(bus);
}

int main() {
    test_new();
    test_callback_wrapper();
    test_subscribe();
    test_multiple_subscribers();
    test_multiple_topics();
    test_embedded_nul_topics_are_distinct();
    test_unsubscribe();
    test_publish_invokes_callbacks_in_order();
    test_unsubscribe_tail_then_append_preserves_order();
    test_topic_table_rehashes_under_load();
    test_publish_retains_managed_payload_for_dispatch();
    test_publish_borrows_raw_string_payload();
    test_publish_retains_bus_during_callback_release();
    test_unsubscribe_during_publish_uses_snapshot();
    test_topics();
    test_rejects_plain_heap_callback();
    test_rejects_raw_pointer_callback();
    test_subscribe_retain_trap_releases_bus();
    test_publish_trap_releases_snapshot_callbacks();
    test_subscribe_total_count_overflow_traps();
    test_subscribe_topic_count_overflow_traps();
    test_finalizer_trap_drains_later_buckets();
    test_clear_topic();
    test_clear();
    test_callback_object_cleanup_on_unsubscribe();
    test_null_safety();
    return 0;
}
