//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTCanvasStateContractTests.cpp
// Purpose: Validate Canvas logical/window-state synchronization with a
//   deterministic in-process ZannaGFX backend.
//
// Key invariants:
//   - HiDPI scale and logical clips are applied exactly when backend state changes.
//   - Frame timing remains defined across extreme or non-monotonic clock values.
//   - Canvas-owned title bytes round-trip independently of C-string termination.
//
// Ownership/Lifetime:
//   - Fake windows and runtime objects are heap-backed test fixtures.
//   - Returned fake runtime strings borrow static test storage.
//
// Links: src/runtime/graphics/2d/rt_canvas.c,
//        src/runtime/graphics/common/rt_graphics_internal.h
//
//===----------------------------------------------------------------------===//

extern "C" {
#include "rt_graphics_internal.h"
}

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

struct FakeWindow {
    int32_t physical_width;
    int32_t physical_height;
    float scale_factor;
    float coord_scale;
    int coord_scale_calls;
    float last_coord_scale;
    int clip_set_calls;
    int clear_clip_calls;
    int32_t clip_x;
    int32_t clip_y;
    int32_t clip_w;
    int32_t clip_h;
    int update_calls;
    int close_requested;
    int fps;
    int32_t pos_x;
    int32_t pos_y;
    int32_t monitor_w;
    int32_t monitor_h;
    int32_t mouse_x;
    int32_t mouse_y;
};

struct FakeString {
    const char *data;
    size_t len;
};

static float g_initial_scale = 1.0f;
static int64_t g_clock_us = 0;
static int g_pump_events_result = 1;
static int g_poll_event_calls = 0;
static int g_mouse_pos_calls = 0;
static int g_destroyed_windows = 0;
static void *g_object_payloads[16];
static int64_t g_object_class_ids[16];
static size_t g_object_count = 0;
static FakeString g_returned_string{nullptr, 0};
static char g_returned_string_data[128];

static FakeWindow *window_from(vgfx_window_t window) {
    return reinterpret_cast<FakeWindow *>(window);
}

static rt_canvas *new_canvas() {
    return static_cast<rt_canvas *>(rt_canvas_new(nullptr, 100, 50));
}

static void test_width_resyncs_coord_scale_after_display_move() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    auto *window = window_from(canvas->gfx_win);
    assert(window != nullptr);
    assert(window->last_coord_scale == 1.0f);

    window->scale_factor = 2.0f;
    window->physical_width = 200;
    window->physical_height = 100;

    assert(rt_canvas_width(canvas) == 100);
    assert(window->last_coord_scale == 2.0f);
    assert(window->coord_scale_calls >= 2);
}

static void test_poll_reapplies_clip_after_scale_change() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    auto *window = window_from(canvas->gfx_win);
    rt_canvas_set_clip_rect(canvas, 10, 20, 30, 40);
    int initial_clip_calls = window->clip_set_calls;

    window->scale_factor = 2.0f;
    window->physical_width = 200;
    window->physical_height = 100;

    int64_t event_type = rt_canvas_poll(canvas);
    assert(event_type == 0);
    assert(window->last_coord_scale == 2.0f);
    assert(window->clip_set_calls == initial_clip_calls + 1);
    assert(window->clip_x == 10);
    assert(window->clip_y == 20);
    assert(window->clip_w == 30);
    assert(window->clip_h == 40);
}

static void test_resync_skips_unchanged_backend_state() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    auto *window = window_from(canvas->gfx_win);
    int coord_calls = window->coord_scale_calls;
    int clear_calls = window->clear_clip_calls;
    assert(rt_canvas_width(canvas) == 100);
    assert(rt_canvas_height(canvas) == 50);
    assert(window->coord_scale_calls == coord_calls);
    assert(window->clear_clip_calls == clear_calls);

    rt_canvas_set_clip_rect(canvas, 1, 2, 3, 4);
    int clip_calls = window->clip_set_calls;
    rt_canvas_set_clip_rect(canvas, 1, 2, 3, 4);
    assert(window->clip_set_calls == clip_calls);
}

static void test_flip_rounds_positive_submillisecond_delta_up_to_one_ms() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    g_clock_us = 1000;
    rt_canvas_flip(canvas);
    assert(rt_canvas_get_delta_time(canvas) == 0);

    g_clock_us = 1500;
    rt_canvas_flip(canvas);
    assert(rt_canvas_get_delta_time(canvas) == 1);
}

static void test_flip_timing_is_overflow_safe_and_recovers_after_clock_reset() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    g_clock_us = 1;
    rt_canvas_flip(canvas);
    g_clock_us = INT64_MAX;
    rt_canvas_flip(canvas);
    int64_t delta_us = INT64_MAX - 1;
    int64_t expected_ms = delta_us / 1000 + ((delta_us % 1000) >= 500 ? 1 : 0);
    assert(rt_canvas_get_delta_time(canvas) == expected_ms);

    g_clock_us = INT64_MIN;
    rt_canvas_flip(canvas);
    assert(rt_canvas_get_delta_time(canvas) == 0);
    g_clock_us = 1000;
    rt_canvas_flip(canvas);
    assert(rt_canvas_get_delta_time(canvas) == 0);
    g_clock_us = 1500;
    rt_canvas_flip(canvas);
    assert(rt_canvas_get_delta_time(canvas) == 1);
    g_clock_us = 0;
}

static void test_shared_graphics_integer_helpers_cover_extremes() {
    assert(rtg_mul_sat64(INT64_MAX - 1, 1) == INT64_MAX - 1);
    assert(rtg_mul_sat64(INT64_MAX, 2) == INT64_MAX);
    assert(rtg_mul_sat64(INT64_MIN, 2) == INT64_MIN);
    assert(rtg_mul_sat64(INT64_MIN, -1) == INT64_MAX);
    assert(rtg_mul_sat64(-3, -7) == 21);
    assert(rtg_cos_deg_fp(INT64_MAX) == rtg_cos_deg_fp(INT64_MAX % 360));
    assert(rtg_cos_deg_fp(INT64_MIN) == rtg_cos_deg_fp(INT64_MIN % 360));
    assert(rtg_sanitize_scale(std::numeric_limits<float>::infinity()) == 1.0f);
    assert(rtg_sanitize_scale(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
    assert(rtg_sanitize_scale(100.0f) == 16.0f);
    assert(rtg_round_scaled(std::numeric_limits<double>::quiet_NaN()) == 0);
}

static void test_poll_tears_down_window_when_event_pump_fails() {
    g_initial_scale = 1.0f;
    g_pump_events_result = 0;
    g_poll_event_calls = 0;
    g_mouse_pos_calls = 0;
    g_destroyed_windows = 0;

    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    int64_t event_type = rt_canvas_poll(canvas);
    assert(event_type == 0);
    assert(rt_canvas_should_close(canvas) == 1);
    assert(canvas->gfx_win == nullptr);
    assert(g_poll_event_calls == 0);
    assert(g_mouse_pos_calls == 0);
    assert(g_destroyed_windows == 1);

    g_pump_events_result = 1;
}

static void test_window_position_and_monitor_scalar_wrappers() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    rt_canvas_set_position(canvas, 123, -45);
    assert(rt_canvas_get_window_x(canvas) == 123);
    assert(rt_canvas_get_window_y(canvas) == -45);

    auto *window = window_from(canvas->gfx_win);
    assert(window != nullptr);
    window->monitor_w = 2560;
    window->monitor_h = 1440;
    assert(rt_canvas_get_monitor_width(canvas) == 2560);
    assert(rt_canvas_get_monitor_height(canvas) == 1440);
}

static void test_title_cache_preserves_embedded_nul_bytes() {
    g_initial_scale = 1.0f;
    rt_canvas *canvas = new_canvas();
    assert(canvas != nullptr);

    const char raw[] = {'A', 'B', '\0', 'C', 'D'};
    FakeString title{raw, sizeof(raw)};
    rt_canvas_set_title(canvas, reinterpret_cast<rt_string>(&title));

    rt_string got = rt_canvas_get_title(canvas);
    assert(rt_str_len(got) == (int64_t)sizeof(raw));
    assert(std::memcmp(rt_string_cstr(got), raw, sizeof(raw)) == 0);
}

} // namespace

extern "C" void *rt_obj_new_i64(int64_t class_id, int64_t byte_size) {
    assert(byte_size >= 0);
    assert(g_object_count < sizeof(g_object_payloads) / sizeof(g_object_payloads[0]));
    void *obj = std::calloc(1, static_cast<size_t>(byte_size));
    assert(obj != nullptr);
    g_object_payloads[g_object_count] = obj;
    g_object_class_ids[g_object_count] = class_id;
    g_object_count++;
    return obj;
}

extern "C" int64_t rt_obj_class_id(void *obj) {
    for (size_t i = 0; i < g_object_count; i++) {
        if (g_object_payloads[i] == obj)
            return g_object_class_ids[i];
    }
    return 0;
}

extern "C" int8_t rt_obj_is_instance(void *obj, int64_t class_id, size_t) {
    return obj && rt_obj_class_id(obj) == class_id;
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" int32_t rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *obj) {
    for (size_t i = 0; i < g_object_count; i++) {
        if (g_object_payloads[i] == obj) {
            g_object_payloads[i] = g_object_payloads[g_object_count - 1];
            g_object_class_ids[i] = g_object_class_ids[g_object_count - 1];
            g_object_count--;
            break;
        }
    }
    std::free(obj);
}

extern "C" void rt_trap(const char *) {
    std::abort();
}

extern "C" int64_t rt_str_len(rt_string s) {
    auto *fake = reinterpret_cast<FakeString *>(s);
    return fake ? (int64_t)fake->len : 0;
}

extern "C" const char *rt_string_cstr(rt_string s) {
    auto *fake = reinterpret_cast<FakeString *>(s);
    return fake && fake->data ? fake->data : "";
}

extern "C" rt_string rt_string_from_bytes(const char *bytes, size_t len) {
    assert(len < sizeof(g_returned_string_data));
    if (len > 0)
        std::memcpy(g_returned_string_data, bytes, len);
    g_returned_string_data[len] = '\0';
    g_returned_string.data = g_returned_string_data;
    g_returned_string.len = len;
    return reinterpret_cast<rt_string>(&g_returned_string);
}

extern "C" void rt_keyboard_clear_canvas_if_matches(void *) {}

extern "C" void rt_mouse_clear_canvas_if_matches(void *) {}

extern "C" void rt_keyboard_set_canvas(void *) {}

extern "C" void rt_mouse_set_canvas(void *) {}

extern "C" void rt_pad_init(void) {}

extern "C" void rt_keyboard_begin_frame(void) {}

extern "C" void rt_mouse_begin_frame(void) {}

extern "C" void rt_mouse_finalize_frame(void) {}

extern "C" void rt_pad_begin_frame(void) {}

extern "C" void rt_pad_poll(void) {}

extern "C" void rt_keyboard_on_key_down(int64_t) {}

extern "C" void rt_keyboard_on_key_up(int64_t) {}

extern "C" void rt_keyboard_on_vgfx_key_down(int64_t) {}

extern "C" void rt_keyboard_on_vgfx_key_up(int64_t) {}

extern "C" void rt_keyboard_text_input(int32_t) {}

extern "C" void rt_mouse_update_pos(int64_t, int64_t) {}

extern "C" void rt_mouse_button_down(int64_t) {}

extern "C" void rt_mouse_button_up(int64_t) {}

extern "C" void rt_mouse_update_wheel(double, double) {}

extern "C" int8_t rt_mouse_is_captured(void) {
    return 0;
}

extern "C" int8_t rt_mouse_get_relative_mode(void) {
    return 0;
}

extern "C" void rt_mouse_set_relative_native(int8_t) {}

extern "C" int8_t rt_mouse_get_relative_native(void) {
    return 0;
}

extern "C" void rt_mouse_force_delta(int64_t, int64_t) {}

extern "C" void rt_mouse_force_delta_f(double, double) {}

extern "C" int32_t vgfx_set_relative_mouse(vgfx_window_t, int32_t) {
    return 0;
}

extern "C" void vgfx_get_relative_deltas(vgfx_window_t, double *dx, double *dy) {
    if (dx)
        *dx = 0.0;
    if (dy)
        *dy = 0.0;
}

extern "C" void vgfx_warp_cursor(vgfx_window_t, int32_t, int32_t) {}

extern "C" void rt_action_update(void) {}

extern "C" int64_t rt_clock_ticks_us(void) {
    return g_clock_us;
}

extern "C" void *rt_canvas_copy_rect(void *, int64_t, int64_t, int64_t, int64_t) {
    return nullptr;
}

extern "C" int64_t rt_pixels_save_bmp(void *, void *) {
    return 0;
}

extern "C" int64_t rt_pixels_save_png(void *, void *) {
    return 0;
}

extern "C" vgfx_window_params_t vgfx_window_params_default(void) {
    vgfx_window_params_t params;
    std::memset(&params, 0, sizeof(params));
    params.width = 640;
    params.height = 480;
    return params;
}

extern "C" vgfx_window_t vgfx_create_window(const vgfx_window_params_t *params) {
    auto *window = static_cast<FakeWindow *>(std::calloc(1, sizeof(FakeWindow)));
    assert(window != nullptr);
    int32_t logical_w = params ? params->width : 640;
    int32_t logical_h = params ? params->height : 480;
    window->scale_factor = g_initial_scale;
    window->coord_scale = 1.0f;
    window->physical_width = (int32_t)rtg_scale_up_i64(logical_w, window->scale_factor);
    window->physical_height = (int32_t)rtg_scale_up_i64(logical_h, window->scale_factor);
    window->fps = -1;
    window->monitor_w = 1920;
    window->monitor_h = 1080;
    return reinterpret_cast<vgfx_window_t>(window);
}

extern "C" void vgfx_destroy_window(vgfx_window_t window) {
    g_destroyed_windows++;
    std::free(window_from(window));
}

extern "C" void vgfx_set_coord_scale(vgfx_window_t window, float scale) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->coord_scale = scale;
    fake->last_coord_scale = scale;
    fake->coord_scale_calls++;
}

extern "C" float vgfx_window_get_scale(vgfx_window_t window) {
    auto *fake = window_from(window);
    return fake ? fake->scale_factor : 1.0f;
}

// Fake windows are always windowed, so the effective coordinate scale equals the
// window scale above (the fullscreen presentation branch is never taken). These
// exist to satisfy the linker for rt_canvas_effective_coord_scale.
extern "C" int vgfx_is_fullscreen(vgfx_window_t) {
    return 0;
}

extern "C" int32_t vgfx_window_get_width(vgfx_window_t) {
    return 0;
}

extern "C" int32_t vgfx_window_get_height(vgfx_window_t) {
    return 0;
}

extern "C" int32_t vgfx_get_size(vgfx_window_t window, int32_t *width, int32_t *height) {
    auto *fake = window_from(window);
    if (!fake)
        return 0;
    if (width)
        *width = (int32_t)rtg_scale_down_i64(fake->physical_width, fake->coord_scale);
    if (height)
        *height = (int32_t)rtg_scale_down_i64(fake->physical_height, fake->coord_scale);
    return 1;
}

extern "C" void vgfx_set_clip(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->clip_x = x;
    fake->clip_y = y;
    fake->clip_w = w;
    fake->clip_h = h;
    fake->clip_set_calls++;
}

extern "C" void vgfx_clear_clip(vgfx_window_t window) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->clear_clip_calls++;
}

extern "C" int32_t vgfx_pump_events(vgfx_window_t) {
    return g_pump_events_result;
}

extern "C" int32_t vgfx_poll_event(vgfx_window_t, vgfx_event_t *) {
    g_poll_event_calls++;
    return 0;
}

extern "C" int32_t vgfx_mouse_pos(vgfx_window_t window, int32_t *x, int32_t *y) {
    g_mouse_pos_calls++;
    auto *fake = window_from(window);
    if (!fake)
        return 0;
    if (x)
        *x = (int32_t)rtg_scale_down_i64(fake->mouse_x, fake->coord_scale);
    if (y)
        *y = (int32_t)rtg_scale_down_i64(fake->mouse_y, fake->coord_scale);
    return 1;
}

extern "C" int32_t vgfx_update(vgfx_window_t window) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->update_calls++;
    return 1;
}

extern "C" int32_t vgfx_close_requested(vgfx_window_t window) {
    auto *fake = window_from(window);
    return fake ? fake->close_requested : 0;
}

extern "C" void vgfx_cls(vgfx_window_t, vgfx_color_t) {}

extern "C" void vgfx_focus(vgfx_window_t) {}

extern "C" void vgfx_set_window_size(vgfx_window_t window, int32_t w, int32_t h) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->physical_width = (int32_t)rtg_scale_up_i64(w, fake->scale_factor);
    fake->physical_height = (int32_t)rtg_scale_up_i64(h, fake->scale_factor);
}

extern "C" void vgfx_set_fullscreen(vgfx_window_t, int32_t) {}

extern "C" void vgfx_set_title(vgfx_window_t, const char *) {}

extern "C" void vgfx_set_fps(vgfx_window_t window, int32_t fps) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->fps = fps;
}

extern "C" int32_t vgfx_get_fps(vgfx_window_t window) {
    auto *fake = window_from(window);
    return fake ? fake->fps : -1;
}

extern "C" int32_t vgfx_key_down(vgfx_window_t, vgfx_key_t) {
    return 0;
}

extern "C" void vgfx_get_position(vgfx_window_t window, int32_t *x, int32_t *y) {
    auto *fake = window_from(window);
    if (x)
        *x = fake ? fake->pos_x : 0;
    if (y)
        *y = fake ? fake->pos_y : 0;
}

extern "C" void vgfx_set_position(vgfx_window_t window, int32_t x, int32_t y) {
    auto *fake = window_from(window);
    assert(fake != nullptr);
    fake->pos_x = x;
    fake->pos_y = y;
}

extern "C" void vgfx_get_monitor_size(vgfx_window_t window, int32_t *w, int32_t *h) {
    auto *fake = window_from(window);
    if (w)
        *w = fake ? fake->monitor_w : 0;
    if (h)
        *h = fake ? fake->monitor_h : 0;
}

extern "C" int32_t vgfx_is_focused(vgfx_window_t) {
    return 1;
}

extern "C" int32_t vgfx_is_minimized(vgfx_window_t) {
    return 0;
}

extern "C" int32_t vgfx_is_maximized(vgfx_window_t) {
    return 0;
}

extern "C" void vgfx_minimize(vgfx_window_t) {}

extern "C" void vgfx_maximize(vgfx_window_t) {}

extern "C" void vgfx_restore(vgfx_window_t) {}

extern "C" void vgfx_set_prevent_close(vgfx_window_t, int32_t) {}

int main() {
    test_width_resyncs_coord_scale_after_display_move();
    test_poll_reapplies_clip_after_scale_change();
    test_resync_skips_unchanged_backend_state();
    test_flip_rounds_positive_submillisecond_delta_up_to_one_ms();
    test_flip_timing_is_overflow_safe_and_recovers_after_clock_reset();
    test_shared_graphics_integer_helpers_cover_extremes();
    test_poll_tears_down_window_when_event_pump_fails();
    test_window_position_and_monitor_scalar_wrappers();
    test_title_cache_preserves_embedded_nul_bytes();
    return 0;
}
