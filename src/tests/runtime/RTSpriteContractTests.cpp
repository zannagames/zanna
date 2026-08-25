//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSpriteContractTests.cpp
// Purpose: Isolated Sprite and SpriteAnimator correctness contracts with fake
//   runtime objects, Pixels transforms, image decoding, clocks, and blitting.
//
// Key invariants:
//   - Stub Pixels preserve the production implementation prefix, including
//     mutation generation used by transformed-frame cache keys.
//   - Tests perform no native-window work and keep all clocks deterministic.
//
// Ownership/Lifetime:
//   - Runtime-object fixtures are process-local. Heap-created transformed
//     Pixels are deleted by the fake heap-release adapter.
//
// Links: src/runtime/graphics/2d/rt_sprite.c,
//        src/runtime/graphics/2d/rt_sprite.h
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_file_path.h"
#include "rt_gif.h"
#include "rt_object.h"
#include "rt_pixels_internal.h"
#include "rt_sprite.h"
}

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef RT_PIXELS_CLASS_ID
#define RT_PIXELS_CLASS_ID INT64_C(-0x600201)
#endif

namespace {

struct ObjHeader {
    int64_t class_id;
    void (*finalizer)(void *);
};

struct StubPixels {
    rt_pixels_impl impl;
    int64_t id;

    StubPixels(int64_t width, int64_t height, int64_t identity) : impl{}, id(identity) {
        impl.width = width;
        impl.height = height;
        impl.generation = 1;
    }
};

static int64_t g_next_pixels_id = 1000;
static int64_t g_last_blit_pixels_id = 0;
static int64_t g_last_blit_x = 0;
static int64_t g_last_blit_y = 0;
static int g_tint_call_count = 0;
static int g_rotate_call_count = 0;
static int64_t g_last_tint = -2;
static bool g_clone_returns_null = false;
static bool g_gif_decode_zero_success = false;
static bool g_gif_decode_many_success = false;
static int g_gif_decode_calls = 0;
static int g_retain_call_count = 0;
static void *g_last_retained = nullptr;
static void *g_objects[128];
static int64_t g_object_class_ids[128];
static int g_object_count = 0;
static int64_t g_timer_ms = 0;
static const char *g_runtime_string = "/tmp/zanna_sprite_zero_frame.gif";
static int64_t g_runtime_string_len = 32;

static StubPixels *make_pixels(int64_t width, int64_t height, int64_t id) {
    auto *pixels = new StubPixels(width, height, id);
    assert(pixels != nullptr);
    return pixels;
}

static void reset_draw_state() {
    g_last_blit_pixels_id = 0;
    g_last_blit_x = 0;
    g_last_blit_y = 0;
    g_tint_call_count = 0;
    g_rotate_call_count = 0;
    g_last_tint = -2;
}

static ObjHeader *header_from_payload(void *obj) {
    return reinterpret_cast<ObjHeader *>(obj) - 1;
}

static void test_flip_x_uses_returned_pixels_buffer() {
    StubPixels source(8, 6, 10);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);
    assert(rt_obj_class_id(sprite) == RT_SPRITE_CLASS_ID);

    rt_sprite_set_flip_x(sprite, 1);
    rt_sprite_draw(sprite, reinterpret_cast<void *>(1));

    assert(g_last_blit_pixels_id == 110);
    assert(g_last_blit_x == 0);
    assert(g_last_blit_y == 0);
}

static void test_scale_setters_clamp_to_positive_values() {
    StubPixels source(10, 10, 20);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    rt_sprite_set_scale_x(sprite, -50);
    rt_sprite_set_scale_y(sprite, 0);

    assert(rt_sprite_get_scale_x(sprite) == 1);
    assert(rt_sprite_get_scale_y(sprite) == 1);
}

static void test_tiny_positive_scale_still_has_nonzero_collision_bounds() {
    StubPixels a_src(10, 10, 30);
    StubPixels b_src(10, 10, 40);
    void *a = rt_sprite_new(&a_src);
    void *b = rt_sprite_new(&b_src);
    assert(a != nullptr);
    assert(b != nullptr);

    rt_sprite_set_scale_x(a, 1);
    rt_sprite_set_scale_y(a, 1);
    rt_sprite_set_scale_x(b, 1);
    rt_sprite_set_scale_y(b, 1);

    rt_sprite_set_x(a, 5);
    rt_sprite_set_y(a, 7);
    rt_sprite_set_x(b, 5);
    rt_sprite_set_y(b, 7);

    assert(rt_sprite_contains(a, 5, 7) == 1);
    assert(rt_sprite_overlaps(a, b) == 1);
}

static void test_extreme_collision_bounds_do_not_wrap() {
    StubPixels a_src(2, 2, 31);
    StubPixels b_src(1, 1, 41);
    void *a = rt_sprite_new(&a_src);
    void *b = rt_sprite_new(&b_src);
    assert(a != nullptr);
    assert(b != nullptr);

    rt_sprite_set_x(a, INT64_MAX - 1);
    rt_sprite_set_y(a, 0);
    rt_sprite_set_x(b, INT64_MAX);
    rt_sprite_set_y(b, 1);

    assert(rt_sprite_contains(a, INT64_MAX, 1) == 1);
    assert(rt_sprite_overlaps(a, b) == 1);

    rt_sprite_move(a, 100, 0);
    assert(rt_sprite_get_x(a) == INT64_MAX);
}

static void test_sprite_new_retains_initial_frame_without_cloning() {
    StubPixels source(4, 4, 60);
    g_retain_call_count = 0;
    g_last_retained = nullptr;
    g_clone_returns_null = true;
    void *sprite = rt_sprite_new(&source);
    g_clone_returns_null = false;
    assert(sprite != nullptr);
    assert(g_retain_call_count == 1);
    assert(g_last_retained == &source);
    reset_draw_state();
    rt_sprite_draw(sprite, reinterpret_cast<void *>(1));
    assert(g_last_blit_pixels_id == 60);
}

static void test_sprite_from_file_rejects_zero_frame_gif_success() {
    const char *path = "/tmp/zanna_sprite_zero_frame.gif";
    std::FILE *f = std::fopen(path, "wb");
    assert(f != nullptr);
    const unsigned char header[6] = {'G', 'I', 'F', '8', '9', 'a'};
    assert(std::fwrite(header, 1, sizeof(header), f) == sizeof(header));
    std::fclose(f);

    g_gif_decode_zero_success = true;
    void *sprite = rt_sprite_from_file(reinterpret_cast<void *>(1));
    g_gif_decode_zero_success = false;
    assert(sprite == nullptr);
    std::remove(path);
}

static void test_sprite_from_file_loads_all_gif_frames_and_delays() {
    const char *path = "/tmp/zanna_sprite_zero_frame.gif";
    std::FILE *f = std::fopen(path, "wb");
    assert(f != nullptr);
    const unsigned char header[6] = {'G', 'I', 'F', '8', '9', 'a'};
    assert(std::fwrite(header, 1, sizeof(header), f) == sizeof(header));
    std::fclose(f);

    g_gif_decode_many_success = true;
    void *sprite = rt_sprite_from_file(reinterpret_cast<void *>(1));
    g_gif_decode_many_success = false;
    assert(sprite != nullptr);
    assert(rt_sprite_get_frame_count(sprite) == 70);
    assert(rt_sprite_get_frame_delay_at(sprite, 0) == 10);
    assert(rt_sprite_get_frame_delay_at(sprite, 69) == 79);
    std::remove(path);
}

static void test_animator_get_current_rejects_corrupt_clip_index() {
    rt_sprite_animator_t *anim = rt_sprite_animator_new();
    assert(anim != nullptr);
    assert(rt_sprite_animator_add_clip(anim, "walk", 0, 1, 100, 1) == 1);
    assert(rt_sprite_animator_play(anim, "walk") == 1);
    anim->current_clip = anim->clip_count;
    assert(rt_sprite_animator_get_current(anim) == nullptr);
}

static void test_transformed_tint_zero_is_black_and_negative_is_no_tint() {
    StubPixels source(6, 6, 50);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, -1, 255);
    assert(g_tint_call_count == 0);
    assert(g_last_blit_pixels_id == 50);

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, 0, 255);
    assert(g_tint_call_count == 1);
    assert(g_last_tint == 0);
    assert(g_last_blit_pixels_id != 50);
}

static void test_transformed_tint_preserves_tagged_alpha() {
    StubPixels source(6, 6, 51);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    int64_t tagged = (INT64_C(1) << 56) | INT64_C(0x80010203);
    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, tagged, 255);
    assert(g_tint_call_count == 1);
    assert(g_last_tint == tagged);
}

static void test_set_frame_wraps_out_of_range_indices() {
    StubPixels source(4, 4, 70);
    StubPixels frame(4, 4, 71);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);
    rt_sprite_add_frame(sprite, &frame);
    assert(rt_sprite_get_frame_count(sprite) == 2);

    rt_sprite_set_frame(sprite, 5);
    assert(rt_sprite_get_frame(sprite) == 1);
    rt_sprite_set_frame(sprite, -1);
    assert(rt_sprite_get_frame(sprite) == 1);
}

static void test_animator_rejects_overlong_clip_name() {
    rt_sprite_animator_t *anim = rt_sprite_animator_new();
    assert(anim != nullptr);
    char name[65];
    std::memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    assert(rt_sprite_animator_add_clip(anim, name, 0, 1, 100, 1) == 0);
    assert(rt_sprite_animator_add_clip(anim, "", 0, 1, 100, 1) == 0);
}

static void test_transform_cache_tracks_frame_generation() {
    StubPixels source(3, 3, 80);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, 0, 255);
    assert(g_tint_call_count == 1);
    int64_t first_id = g_last_blit_pixels_id;

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, 0, 255);
    assert(g_tint_call_count == 0);
    assert(g_last_blit_pixels_id == first_id);

    source.impl.generation++;
    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 0, 0, 255);
    assert(g_tint_call_count == 1);
    assert(g_last_blit_pixels_id != first_id);
}

static void test_transform_cache_retains_alternating_working_set() {
    StubPixels source(5, 3, 801);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 90, -1, 255);
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 180, -1, 255);
    assert(g_rotate_call_count == 2);
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 90, -1, 255);
    assert(g_rotate_call_count == 2);
}

static void test_equivalent_full_rotations_use_untransformed_fast_path() {
    StubPixels source(3, 3, 81);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);

    reset_draw_state();
    rt_sprite_draw_transformed(
        sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, INT64_MAX, -1, 300);
    assert(g_rotate_call_count == 1);

    reset_draw_state();
    rt_sprite_draw_transformed(sprite, reinterpret_cast<void *>(1), 0, 0, 100, 100, 720, -1, 300);
    assert(g_rotate_call_count == 0);
    assert(g_last_blit_pixels_id == source.id);
}

static void test_off_image_rotation_origin_is_transformed_without_padding() {
    StubPixels source(4, 2, 82);
    void *sprite = rt_sprite_new(&source);
    assert(sprite != nullptr);
    rt_sprite_set_origin(sprite, -1, 5);

    reset_draw_state();
    rt_sprite_draw_transformed(
        sprite, reinterpret_cast<void *>(1), 100, 200, 100, 100, 90, -1, 255);
    assert(g_rotate_call_count == 1);
    assert(g_last_blit_x == 104);
    assert(g_last_blit_y == 201);

    reset_draw_state();
    rt_sprite_draw_transformed(
        sprite, reinterpret_cast<void *>(1), 100, 200, 100, 100, 45, -1, 255);
    assert(g_rotate_call_count == 1);
    assert(g_last_blit_x == 103);
    assert(g_last_blit_y == 198);
}

static void test_setting_current_frame_does_not_restart_animation_clock() {
    StubPixels first(1, 1, 83);
    StubPixels second(1, 1, 84);
    void *sprite = rt_sprite_new(&first);
    assert(sprite != nullptr);
    rt_sprite_add_frame(sprite, &second);
    rt_sprite_set_frame_delay(sprite, 10);

    g_timer_ms = 0;
    rt_sprite_update(sprite);
    g_timer_ms = 5;
    rt_sprite_set_frame(sprite, 0);
    g_timer_ms = 10;
    rt_sprite_update(sprite);
    assert(rt_sprite_get_frame(sprite) == 1);
}

static void test_zero_timestamp_is_a_valid_sprite_animation_baseline() {
    StubPixels first(1, 1, 90);
    StubPixels second(1, 1, 91);
    void *sprite = rt_sprite_new(&first);
    assert(sprite != nullptr);
    rt_sprite_add_frame(sprite, &second);
    rt_sprite_set_frame_delay(sprite, 10);

    g_timer_ms = 0;
    rt_sprite_update(sprite);
    assert(rt_sprite_get_frame(sprite) == 0);
    g_timer_ms = 10;
    rt_sprite_update(sprite);
    assert(rt_sprite_get_frame(sprite) == 1);
}

static void test_zero_timestamp_is_a_valid_animator_baseline() {
    StubPixels first(1, 1, 92);
    StubPixels second(1, 1, 93);
    void *sprite = rt_sprite_new(&first);
    assert(sprite != nullptr);
    rt_sprite_add_frame(sprite, &second);
    rt_sprite_animator_t *anim = rt_sprite_animator_new();
    assert(anim != nullptr);
    assert(rt_sprite_animator_add_clip(anim, "walk", 0, 2, 10, 1) == 1);
    assert(rt_sprite_animator_play(anim, "walk") == 1);

    g_timer_ms = 0;
    rt_sprite_animator_update(anim, sprite);
    assert(rt_sprite_get_frame(sprite) == 0);
    g_timer_ms = 10;
    rt_sprite_animator_update(anim, sprite);
    assert(rt_sprite_get_frame(sprite) == 1);
}

static void test_runtime_clip_names_reject_embedded_nul() {
    rt_sprite_animator_t *anim = rt_sprite_animator_new();
    assert(anim != nullptr);
    const char embedded[] = {'w', 'a', '\0', 'l', 'k'};
    g_runtime_string = embedded;
    g_runtime_string_len = static_cast<int64_t>(sizeof(embedded));
    assert(rt_sprite_animator_add_clip_str(anim, reinterpret_cast<rt_string>(1), 0, 1, 10, 1) == 0);
    assert(rt_sprite_animator_play_str(anim, reinterpret_cast<rt_string>(1)) == 0);
    g_runtime_string = "/tmp/zanna_sprite_zero_frame.gif";
    g_runtime_string_len = 32;
}

static void test_sprite_file_path_rejects_embedded_nul_and_partial_gif_magic() {
    const char embedded[] = {'/', 't', 'm', 'p', '/', 'x', '\0', 'y'};
    g_runtime_string = embedded;
    g_runtime_string_len = static_cast<int64_t>(sizeof(embedded));
    assert(rt_sprite_from_file(reinterpret_cast<void *>(1)) == nullptr);

    const char *path = "/tmp/zanna_sprite_partial_magic.gif";
    std::FILE *file = std::fopen(path, "wb");
    assert(file != nullptr);
    const unsigned char header[6] = {'G', 'I', 'F', 'x', 'x', 'x'};
    assert(std::fwrite(header, 1, sizeof(header), file) == sizeof(header));
    std::fclose(file);
    g_runtime_string = path;
    g_runtime_string_len = static_cast<int64_t>(std::strlen(path));
    g_gif_decode_calls = 0;
    g_gif_decode_zero_success = true;
    assert(rt_sprite_from_file(reinterpret_cast<void *>(1)) == nullptr);
    g_gif_decode_zero_success = false;
    assert(g_gif_decode_calls == 0);
    std::remove(path);

    g_runtime_string = "/tmp/zanna_sprite_zero_frame.gif";
    g_runtime_string_len = 32;
}

} // namespace

extern "C" void *rt_obj_new_i64(int64_t class_id, int64_t byte_size) {
    auto *header = static_cast<ObjHeader *>(
        std::calloc(1, sizeof(ObjHeader) + static_cast<size_t>(byte_size)));
    assert(header != nullptr);
    header->class_id = class_id;
    void *payload = header + 1;
    assert(g_object_count < (int)(sizeof(g_objects) / sizeof(g_objects[0])));
    g_objects[g_object_count] = payload;
    g_object_class_ids[g_object_count] = class_id;
    g_object_count++;
    return payload;
}

extern "C" int64_t rt_obj_class_id(void *obj) {
    if (!obj)
        return 0;
    for (int i = 0; i < g_object_count; i++) {
        if (g_objects[i] == obj)
            return g_object_class_ids[i];
    }
    return RT_PIXELS_CLASS_ID;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    return obj && rt_obj_class_id(obj) == class_id;
}

extern "C" void rt_obj_set_finalizer(void *obj, void (*finalizer)(void *)) {
    if (!obj)
        return;
    header_from_payload(obj)->finalizer = finalizer;
}

extern "C" int32_t rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *obj) {
    if (!obj)
        return;
    bool found = false;
    for (int i = 0; i < g_object_count; i++) {
        if (g_objects[i] == obj) {
            g_objects[i] = g_objects[g_object_count - 1];
            g_object_class_ids[i] = g_object_class_ids[g_object_count - 1];
            g_object_count--;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    std::free(header_from_payload(obj));
}

extern "C" void rt_obj_retain_maybe(void *obj) {
    g_retain_call_count++;
    g_last_retained = obj;
}

extern "C" void rt_trap(const char *) {
    std::abort();
}

extern "C" void rt_heap_release(void *obj) {
    delete static_cast<StubPixels *>(obj);
}

extern "C" int64_t rt_timer_ms(void) {
    return g_timer_ms;
}

extern "C" const char *rt_string_cstr(rt_string) {
    return g_runtime_string;
}

extern "C" int64_t rt_str_len(rt_string) {
    return g_runtime_string_len;
}

extern "C" int8_t rt_file_path_from_vstr(const ZannaString *, const char **out_path) {
    if (out_path)
        *out_path = nullptr;
    if (!out_path || !g_runtime_string || g_runtime_string_len <= 0 ||
        std::memchr(g_runtime_string, '\0', static_cast<size_t>(g_runtime_string_len)) != nullptr)
        return 0;
    *out_path = g_runtime_string;
    return 1;
}

extern "C" rt_string rt_string_from_bytes(const char *, size_t) {
    return nullptr;
}

extern "C" rt_string rt_str_empty(void) {
    return nullptr;
}

extern "C" int gif_decode_file(
    const char *, gif_frame_t **out_frames, int *out_frame_count, int *out_width, int *out_height) {
    g_gif_decode_calls++;
    if (g_gif_decode_many_success) {
        const int frame_count = 70;
        gif_frame_t *frames = static_cast<gif_frame_t *>(std::calloc(frame_count, sizeof(*frames)));
        assert(frames != nullptr);
        for (int i = 0; i < frame_count; i++) {
            frames[i].pixels = make_pixels(1, 1, 2000 + i);
            frames[i].delay_ms = 10 + i;
            frames[i].dispose_method = 0;
        }
        if (out_frames)
            *out_frames = frames;
        if (out_frame_count)
            *out_frame_count = frame_count;
        if (out_width)
            *out_width = 1;
        if (out_height)
            *out_height = 1;
        return frame_count;
    }
    if (out_frames)
        *out_frames = nullptr;
    if (out_frame_count)
        *out_frame_count = 0;
    if (out_width)
        *out_width = 0;
    if (out_height)
        *out_height = 0;
    return g_gif_decode_zero_success ? 1 : 0;
}

extern "C" void *rt_pixels_clone(void *pixels) {
    if (g_clone_returns_null)
        return nullptr;
    auto *src = static_cast<StubPixels *>(pixels);
    return src ? make_pixels(src->impl.width, src->impl.height, src->id) : nullptr;
}

extern "C" void *rt_pixels_flip_h(void *pixels) {
    auto *src = static_cast<StubPixels *>(pixels);
    return src ? make_pixels(src->impl.width, src->impl.height, src->id + 100) : nullptr;
}

extern "C" void *rt_pixels_flip_v(void *pixels) {
    auto *src = static_cast<StubPixels *>(pixels);
    return src ? make_pixels(src->impl.width, src->impl.height, src->id + 200) : nullptr;
}

extern "C" void *rt_pixels_scale(void *pixels, int64_t width, int64_t height) {
    auto *src = static_cast<StubPixels *>(pixels);
    return src ? make_pixels(width, height, g_next_pixels_id++) : nullptr;
}

extern "C" void *rt_pixels_rotate(void *pixels, double angle) {
    g_rotate_call_count++;
    auto *src = static_cast<StubPixels *>(pixels);
    if (!src)
        return nullptr;
    if (angle == 90.0 || angle == 270.0)
        return make_pixels(src->impl.height, src->impl.width, g_next_pixels_id++);
    return make_pixels(src->impl.width, src->impl.height, g_next_pixels_id++);
}

extern "C" void *rt_pixels_tint(void *pixels, int64_t tint) {
    g_tint_call_count++;
    g_last_tint = tint;
    auto *src = static_cast<StubPixels *>(pixels);
    return src ? make_pixels(src->impl.width, src->impl.height, g_next_pixels_id++) : nullptr;
}

extern "C" void *rt_pixels_transform_region_nearest(void *pixels,
                                                    int64_t,
                                                    int64_t,
                                                    int64_t,
                                                    int64_t,
                                                    int64_t width,
                                                    int64_t height,
                                                    int8_t flip_x,
                                                    int8_t flip_y,
                                                    int64_t tint,
                                                    int64_t) {
    auto *src = static_cast<StubPixels *>(pixels);
    if (!src)
        return nullptr;
    if (tint >= 0) {
        g_tint_call_count++;
        g_last_tint = tint;
    }
    int64_t id = src->id + (flip_x ? 100 : 0) + (flip_y ? 200 : 0);
    if (width != src->impl.width || height != src->impl.height || tint >= 0)
        id = g_next_pixels_id++;
    return make_pixels(width, height, id);
}

extern "C" void *rt_pixels_new(int64_t width, int64_t height) {
    return make_pixels(width, height, g_next_pixels_id++);
}

extern "C" void rt_pixels_copy(
    void *, int64_t, int64_t, void *, int64_t, int64_t, int64_t, int64_t) {}

extern "C" int64_t rt_pixels_width(void *pixels) {
    auto *p = static_cast<StubPixels *>(pixels);
    return p ? p->impl.width : 0;
}

extern "C" int64_t rt_pixels_height(void *pixels) {
    auto *p = static_cast<StubPixels *>(pixels);
    return p ? p->impl.height : 0;
}

extern "C" void *rt_pixels_subimage(void *, int64_t, int64_t, int64_t, int64_t) {
    return nullptr;
}

extern "C" void *rt_pixels_load(void *) {
    return nullptr;
}

extern "C" void *rt_pixels_load_bmp(void *) {
    return nullptr;
}

extern "C" void *rt_pixels_load_png(void *) {
    return nullptr;
}

extern "C" void *rt_pixels_load_jpeg(void *) {
    return nullptr;
}

extern "C" void rt_canvas_blit_alpha(void *, int64_t x, int64_t y, void *pixels) {
    auto *p = static_cast<StubPixels *>(pixels);
    g_last_blit_pixels_id = p ? p->id : 0;
    g_last_blit_x = x;
    g_last_blit_y = y;
}

int main() {
    test_flip_x_uses_returned_pixels_buffer();
    test_scale_setters_clamp_to_positive_values();
    test_tiny_positive_scale_still_has_nonzero_collision_bounds();
    test_extreme_collision_bounds_do_not_wrap();
    test_sprite_new_retains_initial_frame_without_cloning();
    test_sprite_from_file_rejects_zero_frame_gif_success();
    test_sprite_from_file_loads_all_gif_frames_and_delays();
    test_transformed_tint_zero_is_black_and_negative_is_no_tint();
    test_transformed_tint_preserves_tagged_alpha();
    test_set_frame_wraps_out_of_range_indices();
    test_animator_rejects_overlong_clip_name();
    test_animator_get_current_rejects_corrupt_clip_index();
    test_transform_cache_tracks_frame_generation();
    test_transform_cache_retains_alternating_working_set();
    test_equivalent_full_rotations_use_untransformed_fast_path();
    test_off_image_rotation_origin_is_transformed_without_padding();
    test_setting_current_frame_does_not_restart_animation_clock();
    test_zero_timestamp_is_a_valid_sprite_animation_baseline();
    test_zero_timestamp_is_a_valid_animator_baseline();
    test_runtime_clip_names_reject_embedded_nul();
    test_sprite_file_path_rejects_embedded_nul_and_partial_gif_magic();
    return 0;
}
