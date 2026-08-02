//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_embed_channel.c
// Purpose: Exercise shared-memory embed channel lifecycle, roles, frames, and input.
// Key invariants:
//   - Every channel name is unique to this test process.
//   - Tests close both mapped sides after each successful lifecycle.
// Ownership/Lifetime:
//   - Each test owns all channel handles it creates or attaches.
//   - Named OS objects are released when the host handle closes.
// Links: src/lib/graphics/src/vgfx_embed_channel.c,
//        docs/adr/0225-play-loop-and-embedded-play.md
//
//===----------------------------------------------------------------------===//

#include "test_harness.h"
#include "vgfx_embed_channel.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

static unsigned int channel_counter;

/// @brief Build a collision-resistant portable name for one test channel.
static void next_channel_name(char name[97]) {
#if defined(_WIN32)
    unsigned long process_id = (unsigned long)_getpid();
#else
    unsigned long process_id = (unsigned long)getpid();
#endif
    channel_counter++;
    snprintf(name,
             97,
             "zanna_embed_test_%lu_%llu_%u",
             process_id,
             (unsigned long long)time(NULL),
             channel_counter);
}

static void test_embed_rejects_invalid_arguments(void) {
    TEST_BEGIN("Embed: invalid arguments and missing channels");

    vgfx_embed_channel_t *channel = (vgfx_embed_channel_t *)(uintptr_t)1u;
    ASSERT_EQ(vgfx_embed_channel_create(NULL, 4, 4, &channel), 0);
    ASSERT_NULL(channel);
    ASSERT_EQ(vgfx_embed_channel_create("bad/name", 4, 4, &channel), 0);
    ASSERT_NULL(channel);
    ASSERT_EQ(vgfx_embed_channel_create("valid_name", 0, 4, &channel), 0);
    ASSERT_NULL(channel);
    ASSERT_EQ(vgfx_embed_channel_create("valid_name", 4, 8193, &channel), 0);
    ASSERT_NULL(channel);
    ASSERT_EQ(vgfx_embed_channel_create("valid_name", 4, 4, NULL), 0);

    char name[97];
    next_channel_name(name);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &channel), 0);
    ASSERT_NULL(channel);

    TEST_END();
}

static void test_embed_lifecycle_and_exclusive_producer(void) {
    TEST_BEGIN("Embed: unique host and exclusive producer lifecycle");

    char name[97];
    next_channel_name(name);
    vgfx_embed_channel_t *host = NULL;
    vgfx_embed_channel_t *duplicate_host = NULL;
    vgfx_embed_channel_t *producer = NULL;
    vgfx_embed_channel_t *duplicate_producer = NULL;
    ASSERT_EQ(vgfx_embed_channel_create(name, 4, 3, &host), 1);
    ASSERT_NOT_NULL(host);
    ASSERT_EQ(vgfx_embed_channel_create(name, 4, 3, &duplicate_host), 0);
    ASSERT_NULL(duplicate_host);
    ASSERT_EQ(vgfx_embed_channel_producer_attached(host), 0);
    ASSERT_EQ(vgfx_embed_channel_producer_exited(host), 0);

    ASSERT_EQ(vgfx_embed_channel_attach(name, &producer), 1);
    ASSERT_NOT_NULL(producer);
    ASSERT_EQ(vgfx_embed_channel_producer_attached(host), 1);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &duplicate_producer), 0);
    ASSERT_NULL(duplicate_producer);

    uint8_t pixel[4] = {1u, 2u, 3u, 4u};
    int32_t width = -1;
    int32_t height = -1;
    vgfx_embed_event_t event = {VGFX_EMBED_EVENT_CLOSE, 0, 0, 0};
    ASSERT_EQ(vgfx_embed_channel_publish_frame(host, pixel, 1, 1), 0);
    ASSERT_EQ(vgfx_embed_channel_acquire_frame(producer, pixel, &width, &height), 0);
    ASSERT_EQ(width, 0);
    ASSERT_EQ(height, 0);
    ASSERT_EQ(vgfx_embed_channel_push_event(producer, &event), 0);
    ASSERT_EQ(vgfx_embed_channel_poll_event(host, &event), 0);

    vgfx_embed_channel_close(producer);
    producer = NULL;
    ASSERT_EQ(vgfx_embed_channel_producer_attached(host), 0);
    ASSERT_EQ(vgfx_embed_channel_producer_exited(host), 1);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &producer), 1);
    ASSERT_EQ(vgfx_embed_channel_producer_attached(host), 1);
    ASSERT_EQ(vgfx_embed_channel_producer_exited(host), 0);
    vgfx_embed_channel_mark_exited(producer);
    ASSERT_EQ(vgfx_embed_channel_producer_attached(host), 0);
    ASSERT_EQ(vgfx_embed_channel_producer_exited(host), 1);

    vgfx_embed_channel_close(producer);
    vgfx_embed_channel_close(host);
    TEST_END();
}

static void test_embed_frame_round_trip(void) {
    TEST_BEGIN("Embed: bounded latest-frame round trip");

    char name[97];
    next_channel_name(name);
    vgfx_embed_channel_t *host = NULL;
    vgfx_embed_channel_t *producer = NULL;
    ASSERT_EQ(vgfx_embed_channel_create(name, 4, 3, &host), 1);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &producer), 1);

    int32_t max_w = 0;
    int32_t max_h = 0;
    vgfx_embed_channel_capacity(producer, &max_w, &max_h);
    ASSERT_EQ(max_w, 4);
    ASSERT_EQ(max_h, 3);

    int32_t width = -1;
    int32_t height = -1;
    ASSERT_EQ(vgfx_embed_channel_frame_size(host, &width, &height), 0);
    ASSERT_EQ(width, 0);
    ASSERT_EQ(height, 0);

    const uint8_t frame[16] = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
    uint8_t copy[48] = {0};
    ASSERT_EQ(vgfx_embed_channel_publish_frame(producer, frame, 2, 2), 1);
    ASSERT_EQ(vgfx_embed_channel_frame_size(host, &width, &height), 1);
    ASSERT_EQ(width, 2);
    ASSERT_EQ(height, 2);
    ASSERT_EQ(vgfx_embed_channel_acquire_frame(host, copy, &width, &height), 1);
    ASSERT_EQ(width, 2);
    ASSERT_EQ(height, 2);
    ASSERT_TRUE(memcmp(frame, copy, sizeof(frame)) == 0);

    width = -1;
    height = -1;
    ASSERT_EQ(vgfx_embed_channel_acquire_frame(host, copy, &width, &height), 0);
    ASSERT_EQ(width, 0);
    ASSERT_EQ(height, 0);
    ASSERT_EQ(vgfx_embed_channel_publish_frame(producer, copy, 5, 1), 0);
    ASSERT_EQ(vgfx_embed_channel_publish_frame(producer, copy, 1, 4), 0);

    vgfx_embed_channel_close(producer);
    vgfx_embed_channel_close(host);
    TEST_END();
}

static void test_embed_input_ring_preserves_owned_indices(void) {
    TEST_BEGIN("Embed: full input ring rejects newest without corruption");

    char name[97];
    next_channel_name(name);
    vgfx_embed_channel_t *host = NULL;
    vgfx_embed_channel_t *producer = NULL;
    ASSERT_EQ(vgfx_embed_channel_create(name, 2, 2, &host), 1);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &producer), 1);

    vgfx_embed_event_t event = {VGFX_EMBED_EVENT_NONE, 0, 0, 0};
    ASSERT_EQ(vgfx_embed_channel_push_event(host, &event), 0);
    event.kind = VGFX_EMBED_EVENT_CLOSE + 1;
    ASSERT_EQ(vgfx_embed_channel_push_event(host, &event), 0);
    for (int32_t i = 0; i < 256; i++) {
        event.kind = VGFX_EMBED_EVENT_MOUSE_MOVE;
        event.a = i;
        event.b = -i;
        event.c = 0;
        ASSERT_EQ(vgfx_embed_channel_push_event(host, &event), 1);
    }
    event.a = 999;
    ASSERT_EQ(vgfx_embed_channel_push_event(host, &event), 0);

    for (int32_t i = 0; i < 256; i++) {
        memset(&event, 0, sizeof(event));
        ASSERT_EQ(vgfx_embed_channel_poll_event(producer, &event), 1);
        ASSERT_EQ(event.kind, VGFX_EMBED_EVENT_MOUSE_MOVE);
        ASSERT_EQ(event.a, i);
        ASSERT_EQ(event.b, -i);
    }
    event.kind = VGFX_EMBED_EVENT_CLOSE;
    event.a = 7;
    ASSERT_EQ(vgfx_embed_channel_poll_event(producer, &event), 0);
    ASSERT_EQ(event.kind, VGFX_EMBED_EVENT_NONE);
    ASSERT_EQ(event.a, 0);

    vgfx_embed_channel_close(producer);
    vgfx_embed_channel_close(host);
    TEST_END();
}

static void test_embed_resize_clamps_to_capacity(void) {
    TEST_BEGIN("Embed: resize requests clamp and preserve role direction");

    char name[97];
    next_channel_name(name);
    vgfx_embed_channel_t *host = NULL;
    vgfx_embed_channel_t *producer = NULL;
    ASSERT_EQ(vgfx_embed_channel_create(name, 4, 3, &host), 1);
    ASSERT_EQ(vgfx_embed_channel_attach(name, &producer), 1);
    ASSERT_EQ(vgfx_embed_channel_set_size(producer, 2, 2), 0);
    ASSERT_EQ(vgfx_embed_channel_set_size(host, 0, 2), 0);
    ASSERT_EQ(vgfx_embed_channel_set_size(host, 100, 100), 1);

    vgfx_embed_event_t event = {0};
    ASSERT_EQ(vgfx_embed_channel_poll_event(producer, &event), 1);
    ASSERT_EQ(event.kind, VGFX_EMBED_EVENT_RESIZE);
    ASSERT_EQ(event.a, 4);
    ASSERT_EQ(event.b, 3);

    vgfx_embed_channel_close(producer);
    vgfx_embed_channel_close(host);
    TEST_END();
}

int main(void) {
    printf("========================================\n");
    printf("ZannaGFX Embed Channel Tests\n");
    printf("========================================\n");

    test_embed_rejects_invalid_arguments();
    test_embed_lifecycle_and_exclusive_producer();
    test_embed_frame_round_trip();
    test_embed_input_ring_preserves_owned_indices();
    test_embed_resize_clamps_to_capacity();

    TEST_SUMMARY();
    return TEST_RETURN_CODE();
}
