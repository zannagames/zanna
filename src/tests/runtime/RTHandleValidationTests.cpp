//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTHandleValidationTests.cpp
// Purpose: Verify opaque runtime handles reject forged, too-small objects before
//          implementation-specific payload casts.
// Key invariants:
//   - Every tested receiver traps before reading a wrong-class, undersized, or
//     uninitialized native payload.
//   - Returning trap hooks receive safe sentinel results without fallthrough.
// Ownership/Lifetime:
//   - Forged managed objects are released after each probe; real resources are
//     explicitly finalized before process exit.
// Links: src/runtime/oop/rt_object.c, src/runtime/core/rt_string_ops.c,
//        src/runtime/core/rt_string_advanced.c,
//        src/runtime/core/rt_string_specialized.c, src/runtime/network/rt_network.c,
//        src/runtime/network/rt_network_udp.c,
//        src/runtime/network/rt_connpool.c,
//        src/runtime/collections/rt_map.c
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <setjmp.h>

extern "C" {
#include "rt_bag.h"
#include "rt_bimap.h"
#include "rt_binbuf.h"
#include "rt_binfile.h"
#include "rt_bitset.h"
#include "rt_bloomfilter.h"
#include "rt_bytes.h"
#include "rt_cancellation.h"
#include "rt_channel.h"
#include "rt_collection_ids.h"
#include "rt_concmap.h"
#include "rt_concqueue.h"
#include "rt_connpool.h"
#include "rt_countmap.h"
#include "rt_debounce.h"
#include "rt_defaultmap.h"
#include "rt_deque.h"
#include "rt_frozenmap.h"
#include "rt_frozenset.h"
#include "rt_future.h"
#include "rt_gc.h"
#include "rt_http_router.h"
#include "rt_internal.h"
#include "rt_intmap.h"
#include "rt_io_class_ids.h"
#include "rt_iter.h"
#include "rt_json.h"
#include "rt_linereader.h"
#include "rt_linewriter.h"
#include "rt_list.h"
#include "rt_lrucache.h"
#include "rt_map.h"
#include "rt_memstream.h"
#include "rt_msgbus.h"
#include "rt_multimap.h"
#include "rt_musicgen.h"
#include "rt_network.h"
#include "rt_object.h"
#include "rt_orderedmap.h"
#include "rt_playlist.h"
#include "rt_pqueue.h"
#include "rt_queue.h"
#include "rt_random.h"
#include "rt_ratelimit.h"
#include "rt_retry.h"
#include "rt_ring.h"
#include "rt_scheduler.h"
#include "rt_seq.h"
#include "rt_set.h"
#include "rt_sortedset.h"
#include "rt_soundbank.h"
#include "rt_sparsearray.h"
#include "rt_stack.h"
#include "rt_stream.h"
#include "rt_string.h"
#include "rt_threadpool.h"
#include "rt_threads.h"
#include "rt_treemap.h"
#include "rt_trie.h"
#include "rt_unionfind.h"
#include "rt_watcher.h"
#include "rt_weakmap.h"
#include "rt_xml.h"
#include "rt_yaml.h"

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

// When set, vm_trap RECORDS and RETURNS (like an embedder hook that resumes)
// instead of aborting — this exercises the post-trap fall-through guards
// (VDOC-200). No trap recovery (setjmp) is installed in that mode, so control
// really does return through rt_trap into the runtime function under test.
bool g_trap_returns = false;
const char *g_last_returning_trap = nullptr;

void vm_trap(const char *msg) {
    if (g_trap_returns) {
        g_last_returning_trap = msg;
        return;
    }
    rt_abort(msg);
}
}

using HandleCall = void (*)(void *);
using HandleI64Call = int64_t (*)(void *);
using HandleI8Call = int8_t (*)(void *);
using TypeOfCall = rt_string (*)(void *);

static void release_fake(void *fake) {
    if (fake && rt_obj_release_check0(fake))
        rt_obj_free(fake);
}

static void *make_fake(int64_t class_id, int64_t byte_size, uint32_t magic = 0) {
    void *fake = rt_obj_new_i64(class_id, byte_size);
    assert(fake != nullptr);
    if (magic != 0 && byte_size >= static_cast<int64_t>(sizeof(magic)))
        std::memcpy(fake, &magic, sizeof(magic));
    return fake;
}

static void expect_invalid_handle(HandleCall call,
                                  int64_t class_id,
                                  int64_t byte_size,
                                  const char *message_substring,
                                  uint32_t magic = 0) {
    void *fake = make_fake(class_id, byte_size, magic);

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) == 0) {
        call(fake);
        rt_trap_clear_recovery();
        release_fake(fake);
        assert(false && "expected invalid handle trap");
    }

    const char *err = rt_trap_get_error();
    assert(err != nullptr);
    assert(std::strstr(err, message_substring) != nullptr);
    rt_trap_clear_recovery();
    release_fake(fake);
}

static void expect_i64_handle_result(HandleI64Call call,
                                     int64_t class_id,
                                     int64_t byte_size,
                                     int64_t expected) {
    void *fake = make_fake(class_id, byte_size);
    assert(call(fake) == expected);
    release_fake(fake);
}

static void expect_i8_handle_result(HandleI8Call call,
                                    int64_t class_id,
                                    int64_t byte_size,
                                    int8_t expected) {
    void *fake = make_fake(class_id, byte_size);
    assert(call(fake) == expected);
    release_fake(fake);
}

static void expect_type_unknown(TypeOfCall call, int64_t class_id, int64_t byte_size) {
    void *fake = make_fake(class_id, byte_size);
    rt_string type = call(fake);
    assert(type != nullptr);
    assert(std::strcmp(rt_string_cstr(type), "unknown") == 0);
    rt_string_unref(type);
    release_fake(fake);
}

static void call_thread_join(void *obj) {
    rt_thread_join(obj);
}

static void call_safe_thread_has_error(void *obj) {
    (void)rt_thread_has_error(obj);
}

static void call_safe_i64_get(void *obj) {
    (void)rt_safe_i64_get(obj);
}

static void call_gate_get_permits(void *obj) {
    (void)rt_gate_get_permits(obj);
}

static void call_promise_is_done(void *obj) {
    (void)rt_promise_is_done(obj);
}

static void call_future_is_done(void *obj) {
    (void)rt_future_is_done(obj);
}

static void call_threadpool_get_size(void *obj) {
    (void)rt_threadpool_get_size(obj);
}

static void call_channel_get_len(void *obj) {
    (void)rt_channel_get_len(obj);
}

static void call_concqueue_len(void *obj) {
    (void)rt_concqueue_len(obj);
}

static void call_concmap_len(void *obj) {
    (void)rt_concmap_len(obj);
}

static void call_cancellation_check(void *obj) {
    (void)rt_cancellation_is_cancelled(obj);
}

static void call_debounce_get_delay(void *obj) {
    (void)rt_debounce_get_delay(obj);
}

static void call_throttle_get_interval(void *obj) {
    (void)rt_throttle_get_interval(obj);
}

static void call_scheduler_pending(void *obj) {
    (void)rt_scheduler_pending(obj);
}

static void call_weakref_get(void *obj) {
    (void)rt_weakref_get((rt_weakref *)obj);
}

static void call_weakref_alive(void *obj) {
    (void)rt_weakref_alive((rt_weakref *)obj);
}

static void call_weakref_reset(void *obj) {
    rt_weakref_reset((rt_weakref *)obj, nullptr);
}

static void call_msgbus_total_subscriptions(void *obj) {
    (void)rt_msgbus_total_subscriptions(obj);
}

static void call_random_next(void *obj) {
    (void)rt_rnd_method(obj);
}

static void call_seq_len(void *obj) {
    (void)rt_seq_len(obj);
}

static void call_list_len(void *obj) {
    (void)rt_list_len(obj);
}

static void call_set_len(void *obj) {
    (void)rt_set_len(obj);
}

static void call_stack_len(void *obj) {
    (void)rt_stack_len(obj);
}

static void call_queue_len(void *obj) {
    (void)rt_queue_len(obj);
}

static void call_ring_len(void *obj) {
    (void)rt_ring_len(obj);
}

static void call_deque_len(void *obj) {
    (void)rt_deque_len(obj);
}

static void call_bag_len(void *obj) {
    (void)rt_bag_len(obj);
}

static void call_orderedmap_len(void *obj) {
    (void)rt_orderedmap_len(obj);
}

static void call_treemap_len(void *obj) {
    (void)rt_treemap_len(obj);
}

static void call_frozenmap_len(void *obj) {
    (void)rt_frozenmap_len(obj);
}

static void call_frozenset_len(void *obj) {
    (void)rt_frozenset_len(obj);
}

static void call_sparse_len(void *obj) {
    (void)rt_sparse_len(obj);
}

static void call_intmap_len(void *obj) {
    (void)rt_intmap_len(obj);
}

static void call_defaultmap_len(void *obj) {
    (void)rt_defaultmap_len(obj);
}

static void call_multimap_len(void *obj) {
    (void)rt_multimap_len(obj);
}

static void call_lrucache_len(void *obj) {
    (void)rt_lrucache_len(obj);
}

static void call_trie_len(void *obj) {
    (void)rt_trie_len(obj);
}

static void call_bimap_len(void *obj) {
    (void)rt_bimap_len(obj);
}

static void call_bitset_len(void *obj) {
    (void)rt_bitset_len(obj);
}

static void call_bloomfilter_count(void *obj) {
    (void)rt_bloomfilter_count(obj);
}

static void call_countmap_len(void *obj) {
    (void)rt_countmap_len(obj);
}

static void call_pqueue_len(void *obj) {
    (void)rt_pqueue_len(obj);
}

static void call_iter_count(void *obj) {
    (void)rt_iter_count(obj);
}

static void call_sortedset_len(void *obj) {
    (void)rt_sortedset_len(obj);
}

static void call_unionfind_count(void *obj) {
    (void)rt_unionfind_count(obj);
}

static void call_weakmap_len(void *obj) {
    (void)rt_weakmap_len(obj);
}

static void call_map_len(void *obj) {
    (void)rt_map_len(obj);
}

static void call_binbuf_len(void *obj) {
    (void)rt_binbuf_get_len(obj);
}

static void call_bytes_len(void *obj) {
    (void)rt_bytes_len(obj);
}

static void call_binfile_pos(void *obj) {
    (void)rt_binfile_pos(obj);
}

static void call_memstream_len(void *obj) {
    (void)rt_memstream_get_len(obj);
}

static void call_stream_type(void *obj) {
    (void)rt_stream_get_type(obj);
}

static void call_linereader_read_char(void *obj) {
    (void)rt_linereader_read_char(obj);
}

static void call_linewriter_flush(void *obj) {
    rt_linewriter_flush(obj);
}

static void call_watcher_is_watching(void *obj) {
    (void)rt_watcher_get_is_watching(obj);
}

static void call_http_router_count(void *obj) {
    (void)rt_http_router_count(obj);
}

static void call_route_match_index(void *obj) {
    (void)rt_route_match_index(obj);
}

static void call_retry_can_retry(void *obj) {
    (void)rt_retry_can_retry(obj);
}

static void call_ratelimit_available(void *obj) {
    (void)rt_ratelimit_available(obj);
}

/// @brief Invoke the ConnectionPool size getter through the generic handle-test signature.
/// @param obj Candidate ConnectionPool handle supplied by the test harness.
static void call_connpool_size(void *obj) {
    (void)rt_connpool_size(obj);
}

/// @brief Invoke the TCP remote-port getter through the generic handle-test signature.
/// @param obj Candidate TCP handle supplied by the test harness.
static void call_tcp_port(void *obj) {
    (void)rt_tcp_port(obj);
}

/// @brief Invoke the TcpServer port getter through the generic handle-test signature.
/// @param obj Candidate TcpServer handle supplied by the test harness.
static void call_tcp_server_port(void *obj) {
    (void)rt_tcp_server_port(obj);
}

/// @brief Invoke the UDP bound-port getter through the generic handle-test signature.
/// @param obj Candidate UDP handle supplied by the test harness.
static void call_udp_port(void *obj) {
    (void)rt_udp_port(obj);
}

static int8_t call_bytes_is_bytes(void *obj) {
    return rt_bytes_is_bytes(obj);
}

static int8_t call_xml_is_node(void *obj) {
    return rt_xml_is_node(obj);
}

static int64_t call_soundbank_count_result(void *obj) {
    return rt_soundbank_count(obj);
}

static int64_t call_playlist_len_result(void *obj) {
    return rt_playlist_len(obj);
}

static int64_t call_musicgen_bpm_result(void *obj) {
    return rt_musicgen_get_bpm(obj);
}

int main() {
    constexpr uint32_t kThreadMagic = 0x56545244u;
    constexpr uint32_t kSafeThreadMagic = 0x56545346u;
    constexpr int64_t kSoundBankClassId = INT64_C(-0x730101);
    constexpr int64_t kPlaylistClassId = INT64_C(-0x730102);
    constexpr int64_t kMusicGenClassId = INT64_C(-0x730103);

    expect_invalid_handle(call_thread_join,
                          RT_THREAD_CLASS_ID,
                          sizeof(uint32_t),
                          "Thread: invalid thread handle",
                          kThreadMagic);
    expect_invalid_handle(call_safe_thread_has_error,
                          RT_SAFE_THREAD_CLASS_ID,
                          sizeof(uint32_t),
                          "Thread.HasError: invalid thread handle",
                          kSafeThreadMagic);
    expect_invalid_handle(call_safe_i64_get, RT_SAFE_I64_CLASS_ID, 1, "SafeI64: invalid object");
    expect_invalid_handle(call_gate_get_permits, RT_GATE_CLASS_ID, 1, "Gate: invalid object");
    expect_invalid_handle(call_promise_is_done, RT_PROMISE_CLASS_ID, 1, "Promise: invalid object");
    expect_invalid_handle(call_future_is_done, RT_FUTURE_CLASS_ID, 1, "Future: invalid object");
    expect_invalid_handle(
        call_threadpool_get_size, RT_THREADPOOL_CLASS_ID, 1, "Pool: invalid object");
    expect_invalid_handle(call_channel_get_len, RT_CHANNEL_CLASS_ID, 1, "Channel: invalid object");
    expect_invalid_handle(
        call_concqueue_len, RT_CONCQUEUE_CLASS_ID, 1, "ConcurrentQueue: invalid object");
    expect_invalid_handle(
        call_concmap_len, RT_CONCMAP_CLASS_ID, 1, "ConcurrentMap: invalid object");
    expect_invalid_handle(
        call_cancellation_check, RT_CANCELLATION_CLASS_ID, 1, "CancelToken: invalid object");
    expect_invalid_handle(
        call_debounce_get_delay, RT_DEBOUNCER_CLASS_ID, 1, "Debouncer: invalid object");
    expect_invalid_handle(
        call_throttle_get_interval, RT_THROTTLER_CLASS_ID, 1, "Throttler: invalid object");
    expect_invalid_handle(
        call_scheduler_pending, RT_SCHEDULER_CLASS_ID, 1, "Scheduler: invalid object");
    expect_invalid_handle(
        call_weakref_get, RT_WEAKREF_CLASS_ID, 1, "invalid or freed weak reference");
    expect_invalid_handle(
        call_weakref_alive, RT_WEAKREF_CLASS_ID, 1, "invalid or freed weak reference");
    expect_invalid_handle(
        call_weakref_reset, RT_WEAKREF_CLASS_ID, 1, "invalid or freed weak reference");
    expect_invalid_handle(
        call_msgbus_total_subscriptions, RT_MSGBUS_CLASS_ID, 1, "invalid MessageBus object");
    expect_invalid_handle(call_random_next, RT_RANDOM_CLASS_ID, 1, "Random: invalid Random object");

    expect_invalid_handle(call_seq_len, RT_SEQ_CLASS_ID, 1, "Seq: invalid Seq object");
    expect_invalid_handle(call_list_len, RT_LIST_CLASS_ID, 1, "List: invalid List object");
    expect_invalid_handle(call_set_len, RT_SET_CLASS_ID, 1, "Set: invalid Set object");
    expect_invalid_handle(call_stack_len, RT_STACK_CLASS_ID, 1, "Stack: invalid Stack object");
    expect_invalid_handle(call_queue_len, RT_QUEUE_CLASS_ID, 1, "Queue: invalid Queue object");
    expect_invalid_handle(call_ring_len, RT_RING_CLASS_ID, 1, "Ring: invalid Ring object");
    expect_invalid_handle(call_deque_len, RT_DEQUE_CLASS_ID, 1, "Deque: invalid Deque object");
    expect_invalid_handle(call_bag_len, RT_BAG_CLASS_ID, 1, "Bag.Len: invalid Bag object");
    expect_invalid_handle(call_orderedmap_len,
                          RT_ORDEREDMAP_CLASS_ID,
                          1,
                          "OrderedMap.Len: invalid OrderedMap object");
    expect_invalid_handle(
        call_treemap_len, RT_TREEMAP_CLASS_ID, 1, "TreeMap: invalid TreeMap object");
    expect_invalid_handle(
        call_frozenmap_len, RT_FROZENMAP_CLASS_ID, 1, "FrozenMap: invalid FrozenMap object");
    expect_invalid_handle(
        call_frozenset_len, RT_FROZENSET_CLASS_ID, 1, "FrozenSet.Len: invalid FrozenSet object");
    expect_invalid_handle(
        call_sparse_len, RT_SPARSEARRAY_CLASS_ID, 1, "SparseArray.Len: invalid SparseArray object");
    expect_invalid_handle(
        call_intmap_len, RT_INTMAP_CLASS_ID, 1, "IntMap.Len: invalid IntMap object");
    expect_invalid_handle(call_defaultmap_len,
                          RT_DEFAULTMAP_CLASS_ID,
                          1,
                          "DefaultMap.Len: invalid DefaultMap object");
    expect_invalid_handle(
        call_multimap_len, RT_MULTIMAP_CLASS_ID, 1, "MultiMap.Len: invalid MultiMap object");
    expect_invalid_handle(
        call_lrucache_len, RT_LRUCACHE_CLASS_ID, 1, "LRUCache.Len: invalid LRUCache object");
    expect_invalid_handle(call_trie_len, RT_TRIE_CLASS_ID, 1, "Trie.Len: invalid Trie object");
    expect_invalid_handle(call_bimap_len, RT_BIMAP_CLASS_ID, 1, "BiMap.Len: invalid BiMap object");
    expect_invalid_handle(
        call_bitset_len, RT_BITSET_CLASS_ID, 1, "BitSet.Len: invalid BitSet object");
    expect_invalid_handle(call_bloomfilter_count,
                          RT_BLOOMFILTER_CLASS_ID,
                          1,
                          "BloomFilter.Count: invalid BloomFilter object");
    expect_invalid_handle(
        call_countmap_len, RT_COUNTMAP_CLASS_ID, 1, "CountMap.Len: invalid CountMap object");
    expect_invalid_handle(call_pqueue_len, RT_PQUEUE_CLASS_ID, 1, "Heap: invalid Heap object");
    expect_invalid_handle(
        call_iter_count, RT_ITERATOR_CLASS_ID, 1, "Iterator.Count: invalid Iterator object");
    expect_invalid_handle(
        call_sortedset_len, RT_SORTEDSET_CLASS_ID, 1, "SortedSet.Len: invalid SortedSet object");
    expect_invalid_handle(call_unionfind_count,
                          RT_UNIONFIND_CLASS_ID,
                          1,
                          "UnionFind.Count: invalid UnionFind object");
    expect_invalid_handle(
        call_weakmap_len, RT_WEAKMAP_CLASS_ID, 1, "WeakMap.Len: invalid WeakMap object");
    expect_invalid_handle(call_map_len, RT_MAP_CLASS_ID, 1, "Map: invalid Map object");
    expect_invalid_handle(call_binbuf_len, RT_BINBUF_CLASS_ID, 1, "BinaryBuffer: invalid buffer");
    expect_invalid_handle(call_bytes_len, RT_BYTES_CLASS_ID, 1, "Bytes: invalid Bytes object");
    expect_i8_handle_result(call_bytes_is_bytes, RT_BYTES_CLASS_ID, 1, 0);
    expect_invalid_handle(call_binfile_pos, RT_BINFILE_CLASS_ID, 1, "BinFile.Pos: invalid file");
    expect_invalid_handle(
        call_memstream_len, RT_MEMSTREAM_CLASS_ID, 1, "MemStream.Len: invalid stream");
    expect_invalid_handle(call_stream_type, RT_STREAM_CLASS_ID, 1, "Stream.Type: null stream");
    expect_invalid_handle(
        call_linereader_read_char, RT_LINEREADER_CLASS_ID, 1, "LineReader: invalid reader");
    expect_invalid_handle(
        call_linewriter_flush, RT_LINEWRITER_CLASS_ID, 1, "LineWriter: invalid writer");
    expect_invalid_handle(
        call_watcher_is_watching, RT_WATCHER_CLASS_ID, 1, "Watcher: invalid watcher");
    expect_invalid_handle(
        call_http_router_count, RT_HTTP_ROUTER_CLASS_ID, 1, "HttpRouter.Count: invalid router");
    expect_invalid_handle(
        call_route_match_index, RT_ROUTE_MATCH_CLASS_ID, 1, "RouteMatch.Index: invalid match");
    expect_invalid_handle(
        call_retry_can_retry, RT_RETRY_CLASS_ID, 1, "RetryPolicy.CanRetry: invalid policy");
    expect_invalid_handle(call_ratelimit_available,
                          RT_RATELIMIT_CLASS_ID,
                          1,
                          "RateLimiter.Available: invalid limiter");
    expect_invalid_handle(
        call_connpool_size, RT_CONNPOOL_CLASS_ID, 1, "invalid ConnectionPool object");
    expect_invalid_handle(call_tcp_port, RT_TCP_CLASS_ID, 1, "invalid TCP connection");
    expect_invalid_handle(call_tcp_server_port, RT_TCP_SERVER_CLASS_ID, 1, "invalid TCP server");
    expect_invalid_handle(call_udp_port, RT_UDP_CLASS_ID, 1, "invalid UDP socket");

    // A class tag plus generous storage is still not a constructed native
    // object. Initialization magic must reject these same-class forgeries.
    expect_invalid_handle(
        call_connpool_size, RT_CONNPOOL_CLASS_ID, 8192, "invalid ConnectionPool object");
    expect_invalid_handle(call_tcp_port, RT_TCP_CLASS_ID, 256, "invalid TCP connection");
    expect_invalid_handle(call_tcp_server_port, RT_TCP_SERVER_CLASS_ID, 256, "invalid TCP server");
    expect_invalid_handle(call_udp_port, RT_UDP_CLASS_ID, 256, "invalid UDP socket");

    void *real_pool = rt_connpool_new(2);
    assert(real_pool != nullptr);
    assert(rt_obj_class_id(real_pool) == RT_CONNPOOL_CLASS_ID);
    release_fake(real_pool);

    expect_i8_handle_result(call_xml_is_node, RT_XML_NODE_CLASS_ID, 1, 0);
    expect_type_unknown(rt_json_type_of, RT_SEQ_CLASS_ID, 1);
    expect_type_unknown(rt_json_type_of, RT_MAP_CLASS_ID, 1);
    expect_type_unknown(rt_yaml_type_of, RT_SEQ_CLASS_ID, 1);
    expect_type_unknown(rt_yaml_type_of, RT_MAP_CLASS_ID, 1);

    expect_i64_handle_result(call_soundbank_count_result, kSoundBankClassId, 1, 0);
    expect_i64_handle_result(call_playlist_len_result, kPlaylistClassId, 1, 0);
    expect_i64_handle_result(call_musicgen_bpm_result, kMusicGenClassId, 1, 0);

    // Constant-time collection accessors must guard the NULL returned by their
    // checked cast when an embedder records a trap and resumes execution.
    {
        void *fake = make_fake(RT_RANDOM_CLASS_ID, 1);
        g_trap_returns = true;
        g_last_returning_trap = nullptr;

        assert(rt_seq_len(fake) == 0);
        assert(rt_seq_cap(fake) == 0);
        assert(rt_seq_is_empty(fake) == 1);
        assert(rt_stack_owns_elements(fake) == 0);
        assert(rt_stack_len(fake) == 0);
        assert(rt_stack_is_empty(fake) == 1);
        assert(rt_queue_owns_elements(fake) == 0);
        assert(rt_queue_len(fake) == 0);
        assert(rt_queue_is_empty(fake) == 1);
        assert(rt_map_len(fake) == 0);
        assert(rt_multimap_len(fake) == 0);
        assert(rt_multimap_key_count(fake) == 0);
        assert(rt_orderedmap_len(fake) == 0);
        assert(rt_orderedmap_is_empty(fake) == 1);
        assert(rt_bitset_len(fake) == 0);
        assert(rt_defaultmap_len(fake) == 0);
        assert(rt_defaultmap_get(fake, nullptr) == nullptr);
        assert(rt_defaultmap_has(fake, nullptr) == 0);
        rt_defaultmap_set(fake, nullptr, nullptr);
        assert(rt_defaultmap_remove(fake, nullptr) == 0);
        assert(rt_defaultmap_keys(fake) == nullptr);
        assert(rt_defaultmap_get_default(fake) == nullptr);
        assert(rt_defaultmap_is_empty(fake) == 1);
        assert(rt_intmap_len(fake) == 0);
        assert(rt_sparse_len(fake) == 0);
        assert(rt_bag_len(fake) == 0);
        assert(rt_countmap_len(fake) == 0);
        assert(rt_frozenmap_len(fake) == 0);
        assert(rt_frozenset_len(fake) == 0);
        assert(rt_lrucache_len(fake) == 0);
        assert(rt_lrucache_cap(fake) == 0);
        assert(rt_bimap_len(fake) == 0);
        assert(rt_weakmap_len(fake) == 0);
        assert(rt_weakmap_is_empty(fake) == 1);
        rt_iter_reset(fake);
        assert(rt_iter_index(fake) == 0);
        assert(rt_iter_count(fake) == 0);
        assert(rt_unionfind_find(fake, 0) == -1);
        assert(rt_unionfind_count(fake) == 0);
        assert(rt_unionfind_set_size(fake, 0) == 0);
        rt_unionfind_reset(fake);

        g_trap_returns = false;
        assert(g_last_returning_trap != nullptr);
        release_fake(fake);
    }

    // String-keyed collections must reject forged non-null strings instead of
    // silently normalizing them to the empty key after a returning trap.
    {
        void *map = rt_map_new();
        rt_string empty = rt_const_cstr("");
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *fallback = make_fake(RT_RANDOM_CLASS_ID, 1);
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);

        rt_map_set(map, empty, stored);
        assert(rt_map_len(map) == 1);
        assert(rt_map_get(map, empty) == stored);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_map_set(map, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_map_len(map) == 1);
        assert(rt_map_get(map, empty) == stored);

        g_last_returning_trap = nullptr;
        assert(rt_map_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_map_get_or(map, forged, fallback) == fallback);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_map_has(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_map_set_if_missing(map, forged, replacement) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_map_remove(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(fallback);
        release_fake(map);
    }

    // A constructor retain trap must release the incomplete DefaultMap and
    // return NULL even when the embedder's trap hook resumes execution.
    {
        void *default_value = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_heap_hdr_t *header = rt_heap_hdr(default_value);
        assert(header != nullptr);
        header->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        void *map = rt_defaultmap_new(default_value);
        g_trap_returns = false;

        assert(map == nullptr);
        assert(g_last_returning_trap != nullptr);
        header->refcnt = 1;
        release_fake(default_value);
    }

    // DefaultMap key APIs preserve a legitimate empty-key association when a
    // forged non-null string reaches a returning trap hook.
    {
        void *map = rt_defaultmap_new(nullptr);
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);

        rt_defaultmap_set(map, empty, stored);
        assert(rt_defaultmap_len(map) == 1);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_defaultmap_set(map, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_defaultmap_len(map) == 1);
        assert(rt_defaultmap_get(map, empty) == stored);

        g_last_returning_trap = nullptr;
        assert(rt_defaultmap_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_defaultmap_has(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_defaultmap_remove(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(map);
    }

    // Bag key APIs reject a forged string without addressing the real empty
    // element or publishing a second entry.
    {
        void *bag = rt_bag_new();
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(bag != nullptr);
        assert(empty != nullptr);
        assert(rt_bag_add(bag, empty) == 1);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        assert(rt_bag_add(bag, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        assert(rt_bag_len(bag) == 1);
        assert(rt_bag_has(bag, empty) == 1);

        g_last_returning_trap = nullptr;
        assert(rt_bag_has(bag, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_bag_remove(bag, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        rt_string_unref(empty);
        release_fake(bag);
    }

    // CountMap key validation must run before hashing or touching the genuine
    // empty-key counter.
    {
        void *map = rt_countmap_new();
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);
        rt_countmap_set(map, empty, 7);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_countmap_set(map, forged, 99);
        assert(g_last_returning_trap != nullptr);
        assert(rt_countmap_len(map) == 1);
        assert(rt_countmap_get(map, empty) == 7);

        g_last_returning_trap = nullptr;
        assert(rt_countmap_get(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_countmap_inc_by(map, forged, 1) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_countmap_dec(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_countmap_remove(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        assert(rt_countmap_get(map, empty) == 7);
        rt_string_unref(empty);
        release_fake(map);
    }

    // BiMap validates both directions before conflict removal, so a forged key
    // or value cannot evict a real empty-string mapping.
    {
        void *map = rt_bimap_new();
        rt_string empty = rt_const_cstr("");
        rt_string value = rt_const_cstr("value");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);
        assert(value != nullptr);
        rt_bimap_put(map, empty, value);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_bimap_put(map, forged, value);
        assert(g_last_returning_trap != nullptr);
        assert(rt_bimap_len(map) == 1);
        assert(rt_bimap_has_key(map, empty) == 1);

        g_last_returning_trap = nullptr;
        rt_bimap_put(map, empty, forged);
        assert(g_last_returning_trap != nullptr);
        assert(rt_bimap_len(map) == 1);
        assert(rt_bimap_has_value(map, value) == 1);

        g_last_returning_trap = nullptr;
        assert(rt_bimap_get_by_key(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_bimap_get_by_value(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_bimap_remove_by_key(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_bimap_remove_by_value(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        assert(rt_bimap_len(map) == 1);
        rt_string_unref(empty);
        rt_string_unref(value);
        release_fake(map);
    }

    // OrderedMap preserves its real empty-key entry and insertion order when a
    // forged key traps through a returning hook.
    {
        void *map = rt_orderedmap_new();
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);
        rt_orderedmap_set(map, empty, stored);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_orderedmap_set(map, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_orderedmap_len(map) == 1);
        assert(rt_orderedmap_get(map, empty) == stored);

        g_last_returning_trap = nullptr;
        assert(rt_orderedmap_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_orderedmap_has(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_orderedmap_remove(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        assert(rt_orderedmap_len(map) == 1);
        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(map);
    }

    // MultiMap rejects forged keys and stops immediately after a recovered
    // snapshot-retain failure instead of continuing with a released Seq.
    {
        void *map = rt_multimap_new();
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);
        rt_multimap_put(map, empty, stored);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_multimap_put(map, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_multimap_len(map) == 1);
        assert(rt_multimap_count_for(map, empty) == 1);

        g_last_returning_trap = nullptr;
        assert(rt_multimap_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_multimap_get_first(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_multimap_has(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_multimap_remove_all(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        rt_heap_hdr_t *stored_header = rt_heap_hdr(stored);
        assert(stored_header != nullptr);
        stored_header->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
        g_last_returning_trap = nullptr;
        assert(rt_multimap_get(map, empty) == nullptr);
        assert(g_last_returning_trap != nullptr);
        stored_header->refcnt = 2;
        g_trap_returns = false;

        assert(rt_multimap_len(map) == 1);
        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(map);
    }

    // LRUCache key validation happens before lookup, promotion, eviction, or
    // replacement of the legitimate empty-key entry.
    {
        void *cache = rt_lrucache_new(2);
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(cache != nullptr);
        assert(empty != nullptr);
        rt_lrucache_put(cache, empty, stored);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_lrucache_put(cache, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_lrucache_len(cache) == 1);
        assert(rt_lrucache_peek(cache, empty) == stored);

        g_last_returning_trap = nullptr;
        assert(rt_lrucache_get(cache, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_lrucache_peek(cache, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_lrucache_has(cache, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_lrucache_remove(cache, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        assert(rt_lrucache_len(cache) == 1);
        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(cache);
    }

    // TreeMap rejects forged ordered-query keys and stops snapshot iteration
    // after releasing a failed partial values sequence.
    {
        void *map = rt_treemap_new();
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *replacement = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string empty = rt_const_cstr("");
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(empty != nullptr);
        rt_treemap_set(map, empty, stored);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_treemap_set(map, forged, replacement);
        assert(g_last_returning_trap != nullptr);
        assert(rt_treemap_len(map) == 1);
        assert(rt_treemap_get(map, empty) == stored);

        g_last_returning_trap = nullptr;
        assert(rt_treemap_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_treemap_floor(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_treemap_ceil(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_treemap_remove(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        rt_heap_hdr_t *stored_header = rt_heap_hdr(stored);
        assert(stored_header != nullptr);
        stored_header->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
        g_last_returning_trap = nullptr;
        assert(rt_treemap_values(map) == nullptr);
        assert(g_last_returning_trap != nullptr);
        stored_header->refcnt = 2;
        g_trap_returns = false;

        assert(rt_treemap_len(map) == 1);
        rt_string_unref(empty);
        release_fake(stored);
        release_fake(replacement);
        release_fake(map);
    }

    // FrozenMap/FrozenSet reject forged lookup handles, and FrozenMap
    // construction rolls back a retained key when value retention traps.
    {
        void *keys = rt_seq_new_owned();
        void *values = rt_seq_new_owned();
        rt_string empty = rt_const_cstr("");
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        void *fallback = make_fake(RT_RANDOM_CLASS_ID, 1);
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        assert(keys != nullptr);
        assert(values != nullptr);
        assert(empty != nullptr);
        rt_seq_push(keys, empty);
        rt_seq_push(values, stored);

        rt_heap_hdr_t *stored_header = rt_heap_hdr(stored);
        assert(stored_header != nullptr);
        stored_header->refcnt = RT_HEAP_MAX_MORTAL_REFCNT;
        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        assert(rt_frozenmap_from_seqs(keys, values) == nullptr);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;
        stored_header->refcnt = 2;

        void *map = rt_frozenmap_from_seqs(keys, values);
        void *set = rt_frozenset_from_seq(keys);
        assert(map != nullptr);
        assert(set != nullptr);
        assert(rt_frozenmap_get(map, empty) == stored);
        assert(rt_frozenset_has(set, empty) == 1);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        assert(rt_frozenmap_get(map, forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_frozenmap_has(map, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_frozenmap_get_or(map, forged, fallback) == fallback);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_frozenset_has(set, forged) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;

        release_fake(keys);
        release_fake(values);
        rt_string_unref(empty);
        release_fake(stored);
        release_fake(fallback);
        release_fake(map);
        release_fake(set);
    }

    // WeakMap validates keys before lookup/growth, preserves an existing
    // association when weak-reference construction fails, and consumes a
    // partial Keys snapshot when retaining a key traps.
    {
        void *map = rt_weakmap_new();
        void *stored = make_fake(RT_RANDOM_CLASS_ID, 1);
        rt_string key = rt_string_from_bytes("snapshot", 8);
        rt_string other = rt_const_cstr("other");
        auto forged_key = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        void *forged_value = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
        assert(map != nullptr);
        assert(key != nullptr);
        assert(other != nullptr);
        assert(key->heap == RT_SSO_SENTINEL);
        rt_weakmap_set(map, key, stored);
        assert(rt_weakmap_len(map) == 1);

        void *got = rt_weakmap_get(map, key);
        assert(got == stored);
        release_fake(got);

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_weakmap_set(map, forged_key, stored);
        assert(g_last_returning_trap != nullptr);
        assert(rt_weakmap_len(map) == 1);

        g_last_returning_trap = nullptr;
        rt_weakmap_set(map, other, forged_value);
        assert(g_last_returning_trap != nullptr);
        assert(rt_weakmap_len(map) == 1);

        g_last_returning_trap = nullptr;
        rt_weakmap_set(map, key, forged_value);
        assert(g_last_returning_trap != nullptr);
        assert(rt_weakmap_len(map) == 1);

        g_last_returning_trap = nullptr;
        assert(rt_weakmap_get(map, forged_key) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_weakmap_has(map, forged_key) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_weakmap_remove(map, forged_key) == 0);
        assert(g_last_returning_trap != nullptr);

        assert(key->literal_refs == 2);
        key->literal_refs = RT_HEAP_MAX_MORTAL_REFCNT;
        g_last_returning_trap = nullptr;
        assert(rt_weakmap_keys(map) == nullptr);
        assert(g_last_returning_trap != nullptr);
        key->literal_refs = 2;
        g_trap_returns = false;

        got = rt_weakmap_get(map, key);
        assert(got == stored);
        release_fake(got);

        rt_string_unref(key);
        rt_string_unref(other);
        release_fake(stored);
        release_fake(map);
    }

    // VDOC-200: under a RETURNING trap hook (no setjmp recovery), the Random
    // instance methods must NOT dereference a wrong-class/null receiver — they
    // trap and return a safe sentinel instead of crashing.
    {
        g_trap_returns = true;
        void *fake = make_fake(RT_SEQ_CLASS_ID, 1); // wrong class for Random
        g_last_returning_trap = nullptr;
        assert(rt_rnd_method(fake) == 0.0);
        assert(g_last_returning_trap != nullptr);
        assert(rt_rand_int_method(fake, 10) == 0);
        assert(rt_rand_range_method(fake, 1, 6) == 0);
        rt_randomize_i64_method(fake, 42); // must not crash
        // A null receiver is equally safe.
        assert(rt_rnd_method(nullptr) == 0.0);
        assert(rt_rand_int_method(nullptr, 10) == 0);
        assert(rt_rand_range_method(nullptr, 1, 6) == 0);
        rt_randomize_i64_method(nullptr, 42);
        release_fake(fake);
        g_trap_returns = false;
    }

    // Network opaque handles obey the same returning-hook boundary: no
    // private payload read occurs after validation reports the trap.
    {
        void *fake_pool = make_fake(RT_CONNPOOL_CLASS_ID, 1);
        void *fake_tcp = make_fake(RT_TCP_CLASS_ID, 1);
        void *fake_server = make_fake(RT_TCP_SERVER_CLASS_ID, 1);
        void *fake_udp = make_fake(RT_UDP_CLASS_ID, 1);
        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        assert(rt_connpool_size(fake_pool) == 0);
        assert(g_last_returning_trap != nullptr);
        g_last_returning_trap = nullptr;
        assert(rt_tcp_port(fake_tcp) == 0);
        assert(g_last_returning_trap != nullptr);
        g_last_returning_trap = nullptr;
        assert(rt_tcp_server_port(fake_server) == 0);
        assert(g_last_returning_trap != nullptr);
        g_last_returning_trap = nullptr;
        assert(rt_udp_port(fake_udp) == 0);
        assert(g_last_returning_trap != nullptr);
        g_trap_returns = false;
        release_fake(fake_pool);
        release_fake(fake_tcp);
        release_fake(fake_server);
        release_fake(fake_udp);
    }

    // Core string operations must stop after a returning validation trap. The
    // tombstone value is deliberately non-null so any wrapper-field read would
    // fault instead of silently behaving like a null string.
    {
        auto forged = reinterpret_cast<rt_string>(static_cast<uintptr_t>(1));
        rt_string valid = rt_const_cstr("valid");
        assert(valid != nullptr);
        size_t valid_refs = valid->literal_refs;

        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        assert(rt_string_ref(forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_string_unref_count(forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_concat(forged, valid) == nullptr);
        assert(g_last_returning_trap != nullptr);
        assert(valid->literal_refs == valid_refs);

        g_last_returning_trap = nullptr;
        assert(rt_str_mid(forged, 2) != nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_mid_len(forged, 2, 1) != nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_eq(forged, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_lt(forged, valid) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_le(forged, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_gt(valid, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_ge(forged, forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_index_of(forged, rt_str_empty()) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_replace(forged, valid, valid) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_starts_with(forged, valid) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_pad_left(forged, 12, valid) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_split(forged, valid) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_text_char_is_identifier_start(forged) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_join(forged, nullptr) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_repeat(forged, 2) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_flip(forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_cmp(forged, valid) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_capitalize(forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_remove_prefix(forged, valid) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_last_index_of(forged, valid) == 0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_levenshtein(forged, valid) == -1);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_jaro(forged, valid) == 0.0);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_camel_case(forged) == nullptr);
        assert(g_last_returning_trap != nullptr);

        g_last_returning_trap = nullptr;
        assert(rt_str_like(forged, valid) == 0);
        assert(g_last_returning_trap != nullptr);

        g_trap_returns = false;
        rt_string_unref(valid);
    }

    // A returning trap from inside a GC mutator scope must unwind lexically.
    // If rt_trap cleared the scope prematurely or Seq.Push failed to exit it,
    // the synchronous collection below would be deferred on this same thread.
    {
        void *fake = make_fake(RT_LIST_CLASS_ID, 1); // wrong class for Seq
        int64_t passes = rt_gc_pass_count();
        g_trap_returns = true;
        g_last_returning_trap = nullptr;
        rt_seq_push(fake, nullptr);
        g_trap_returns = false;
        assert(g_last_returning_trap != nullptr);
        assert(rt_gc_collect() == 0);
        assert(rt_gc_pass_count() > passes);
        release_fake(fake);
    }

    std::printf("Handle validation tests: all passed\n");
    return 0;
}
