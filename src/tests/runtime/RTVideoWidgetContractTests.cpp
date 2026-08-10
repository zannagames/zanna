//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTVideoWidgetContractTests.cpp
// Purpose: Isolated contract tests for VideoWidget construction, app scheduling, reusable frame
//          upload, transport/fullscreen state, event edges, and lifetime invalidation.
// Key invariants:
//   - Test doubles expose update/upload call counts so same-generation idempotence is observable.
//   - Every event edge is consumed independently and never changes the public revision.
//   - Reusable RGBA bytes are inspected directly without requiring a window or codec fixture.
// Ownership/Lifetime:
//   - Stub runtime objects use calloc/free; each VideoWidget test explicitly destroys or forgets
//     its controller when lifetime behavior is under test.
// Links: src/runtime/graphics/gui/rt_videowidget.c,
//        src/runtime/graphics/gui/rt_videowidget.h
//
//===----------------------------------------------------------------------===//

#include "rt_string.h"
#include "rt_videowidget.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

struct StubString {
    char *data;
};

struct StubPixels {
    int64_t width;
    int64_t height;
    uint32_t *data;
};

struct StubWidget {
    int child_count;
    int visible;
    double flex;
    double preferred_w;
    double preferred_h;
    double max_w;
    double max_h;
    int64_t x;
    int64_t y;
    int64_t margin;
    int enabled;
    int64_t width;
    int64_t height;
    double slider_value;
    double slider_min;
    double slider_max;
    int clicked;
    const void *last_pixels;
    int64_t last_pixels_w;
    int64_t last_pixels_h;
    int upload_count;
};

struct StubPlayer {
    int64_t width;
    int64_t height;
    double duration;
    double position;
    int64_t playing;
    double volume;
    StubPixels *frame;
    int play_calls;
    int pause_calls;
    int stop_calls;
    int update_calls;
    int seek_calls;
    double last_seek;
};

struct rt_videowidget_view {
    uint64_t magic;
    void *vptr;
    void *player;
    void *root_widget;
    void *image_widget;
    void *controls_widget;
    void *play_button;
    void *pause_button;
    void *stop_button;
    void *position_slider;
    void *owner_app;
    uint8_t *rgba_scratch;
    size_t rgba_scratch_capacity;
    const void *last_frame;
    const char *error;
    int8_t show_controls;
    int8_t looping;
    int8_t auto_update;
    int8_t controls_auto_hide;
    int8_t controls_hidden_by_auto;
    int8_t buffering;
    int8_t at_end;
    int8_t failure_latched;
    int8_t manual_update_pending;
    double volume;
    double slider_last_value;
    double controls_idle_seconds;
    double last_uploaded_position;
    int32_t video_width;
    int32_t video_height;
    uint64_t revision;
    uint64_t loaded_edges;
    uint64_t failed_edges;
    uint64_t buffering_changed_edges;
    uint64_t ended_edges;
    uint64_t seeked_edges;
    uint64_t last_auto_generation;
};

bool g_open_should_fail = false;
bool g_parent_valid = true;
int64_t g_open_width = 64;
int64_t g_open_height = 32;
constexpr uint64_t kVideoWidgetMagic = 0x5254564944454F57ull;
int g_release_count = 0;
int g_widget_destroy_count = 0;
int g_open_count = 0;
int g_owner_tokens[64] = {};
size_t g_owner_index = 0;
void *g_current_owner = &g_owner_tokens[0];
uint64_t g_frame_generation = 0;
int64_t g_fullscreen = 0;
bool g_upload_should_fail = false;

rt_string stub_video_path() {
    static char path[] = "video.ogv";
    static StubString str{path};
    return reinterpret_cast<rt_string>(&str);
}

/// @brief Release one heap-backed string produced by this test's rt_string_from_bytes double.
/// @param string Stub runtime string to release; null is ignored.
void release_stub_string(rt_string string) {
    if (!string)
        return;
    auto *stub = reinterpret_cast<StubString *>(string);
    std::free(stub->data);
    std::free(stub);
}

void reset_open_state() {
    g_open_should_fail = false;
    g_parent_valid = true;
    g_open_width = 64;
    g_open_height = 32;
    g_release_count = 0;
    g_widget_destroy_count = 0;
    g_open_count = 0;
    assert(g_owner_index + 1u < sizeof(g_owner_tokens) / sizeof(g_owner_tokens[0]));
    g_current_owner = &g_owner_tokens[++g_owner_index];
    g_frame_generation = 0;
    g_fullscreen = 0;
    g_upload_should_fail = false;
}

} // namespace

extern "C" void *rt_obj_new_i64(int64_t, int64_t byte_size) {
    return std::calloc(1, static_cast<size_t>(byte_size));
}

extern "C" void rt_obj_set_finalizer(void *, void (*)(void *)) {}

extern "C" int rt_obj_release_check0(void *) {
    return 1;
}

extern "C" void rt_obj_free(void *obj) {
    g_release_count++;
    std::free(obj);
}

extern "C" void *rt_gui_widget_parent_container_checked(void *handle) {
    return g_parent_valid ? handle : nullptr;
}

extern "C" void *rt_gui_widget_owner_app(void *) {
    return g_current_owner;
}

extern "C" uint64_t rt_gui_app_frame_generation_for_owner(void *app) {
    return app == g_current_owner ? g_frame_generation : 0u;
}

extern "C" const char *rt_string_cstr(rt_string str) {
    return str ? reinterpret_cast<StubString *>(str)->data : nullptr;
}

extern "C" int64_t rt_pixels_width(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->width : 0;
}

extern "C" int64_t rt_pixels_height(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->height : 0;
}

extern "C" const uint32_t *rt_pixels_raw_buffer(void *pixels) {
    return pixels ? static_cast<StubPixels *>(pixels)->data : nullptr;
}

extern "C" void *rt_image_new(void *) {
    return std::calloc(1, sizeof(StubWidget));
}

extern "C" void rt_image_set_pixels(void *image, void *pixels, int64_t w, int64_t h) {
    auto *widget = static_cast<StubWidget *>(image);
    widget->last_pixels = pixels;
    widget->last_pixels_w = w;
    widget->last_pixels_h = h;
}

extern "C" int rt_gui_image_try_set_rgba_bytes(void *image,
                                               const uint8_t *rgba,
                                               int64_t w,
                                               int64_t h) {
    if (g_upload_should_fail)
        return 0;
    auto *widget = static_cast<StubWidget *>(image);
    widget->last_pixels = rgba;
    widget->last_pixels_w = w;
    widget->last_pixels_h = h;
    widget->upload_count++;
    return 1;
}

extern "C" void rt_image_set_scale_mode(void *, int64_t) {}

extern "C" void rt_widget_set_size(void *widget, int64_t w, int64_t h) {
    auto *stub = static_cast<StubWidget *>(widget);
    stub->width = w;
    stub->height = h;
}

extern "C" void rt_widget_set_flex(void *widget, double flex) {
    static_cast<StubWidget *>(widget)->flex = flex;
}

extern "C" void rt_widget_set_visible(void *widget, int64_t visible) {
    static_cast<StubWidget *>(widget)->visible = visible ? 1 : 0;
}

extern "C" void rt_widget_set_enabled(void *widget, int64_t enabled) {
    static_cast<StubWidget *>(widget)->enabled = enabled ? 1 : 0;
}

extern "C" void rt_widget_set_preferred_size(void *widget, double w, double h) {
    auto *stub = static_cast<StubWidget *>(widget);
    stub->preferred_w = w;
    stub->preferred_h = h;
}

extern "C" void rt_widget_set_max_size(void *widget, double w, double h) {
    auto *stub = static_cast<StubWidget *>(widget);
    stub->max_w = w;
    stub->max_h = h;
}

extern "C" void rt_widget_set_margin(void *widget, int64_t margin) {
    static_cast<StubWidget *>(widget)->margin = margin;
}

extern "C" void rt_widget_set_position(void *widget, int64_t x, int64_t y) {
    auto *stub = static_cast<StubWidget *>(widget);
    stub->x = x;
    stub->y = y;
}

extern "C" void rt_app_set_fullscreen(void *app, int64_t fullscreen) {
    if (app == g_current_owner)
        g_fullscreen = fullscreen ? 1 : 0;
}

extern "C" int64_t rt_app_is_fullscreen(void *app) {
    return app == g_current_owner ? g_fullscreen : 0;
}

extern "C" void rt_widget_destroy(void *widget) {
    g_widget_destroy_count++;
    std::free(widget);
}

extern "C" int64_t rt_widget_was_clicked(void *widget) {
    auto *stub = static_cast<StubWidget *>(widget);
    int clicked = stub->clicked;
    stub->clicked = 0;
    return clicked;
}

extern "C" void *rt_vbox_new(void) {
    return std::calloc(1, sizeof(StubWidget));
}

extern "C" void *rt_hbox_new(void) {
    return std::calloc(1, sizeof(StubWidget));
}

extern "C" void rt_container_set_spacing(void *, double) {}

extern "C" void rt_widget_add_child(void *parent, void *) {
    static_cast<StubWidget *>(parent)->child_count++;
}

extern "C" void *rt_button_new(void *parent, void *) {
    auto *button = static_cast<StubWidget *>(std::calloc(1, sizeof(StubWidget)));
    if (parent)
        static_cast<StubWidget *>(parent)->child_count++;
    return button;
}

extern "C" void *rt_slider_new(void *parent, int64_t) {
    auto *slider = static_cast<StubWidget *>(std::calloc(1, sizeof(StubWidget)));
    if (parent)
        static_cast<StubWidget *>(parent)->child_count++;
    return slider;
}

extern "C" void rt_slider_set_value(void *slider, double value) {
    static_cast<StubWidget *>(slider)->slider_value = value;
}

extern "C" double rt_slider_get_value(void *slider) {
    return static_cast<StubWidget *>(slider)->slider_value;
}

extern "C" void rt_slider_set_range(void *slider, double min, double max) {
    auto *stub = static_cast<StubWidget *>(slider);
    stub->slider_min = min;
    stub->slider_max = max;
}

extern "C" rt_string rt_string_from_bytes(const char *data, size_t len) {
    auto *str = static_cast<StubString *>(std::calloc(1, sizeof(StubString)));
    str->data = static_cast<char *>(std::calloc(len + 1, 1));
    if (data && len > 0)
        std::memcpy(str->data, data, len);
    str->data[len] = '\0';
    return reinterpret_cast<rt_string>(str);
}

/// @brief Isolated borrowed-string double used only by VideoWidget's static control labels.
/// @details The button-construction double never inspects or retains this value, so no runtime
///          string allocation is needed and the production no-allocation contract is preserved.
extern "C" rt_string rt_const_cstr(const char *text) {
    return reinterpret_cast<rt_string>(const_cast<char *>(text));
}

extern "C" void *rt_videoplayer_open(rt_string) {
    g_open_count++;
    if (g_open_should_fail)
        return nullptr;
    auto *player = static_cast<StubPlayer *>(std::calloc(1, sizeof(StubPlayer)));
    player->width = g_open_width;
    player->height = g_open_height;
    player->duration = 8.0;
    player->volume = 1.0;
    auto *frame = static_cast<StubPixels *>(std::calloc(1, sizeof(StubPixels)));
    frame->width = g_open_width;
    frame->height = g_open_height;
    if (g_open_width > 0 && g_open_height > 0 &&
        g_open_width <= std::numeric_limits<int32_t>::max() &&
        g_open_height <= std::numeric_limits<int32_t>::max()) {
        const auto pixel_count =
            static_cast<size_t>(g_open_width) * static_cast<size_t>(g_open_height);
        frame->data = static_cast<uint32_t *>(std::calloc(pixel_count, sizeof(uint32_t)));
        if (frame->data)
            frame->data[0] = 0x11223344u;
    }
    player->frame = frame;
    return player;
}

extern "C" void rt_videoplayer_play(void *vp) {
    auto *player = static_cast<StubPlayer *>(vp);
    player->playing = 1;
    player->play_calls++;
}

extern "C" void rt_videoplayer_pause(void *vp) {
    auto *player = static_cast<StubPlayer *>(vp);
    player->playing = 0;
    player->pause_calls++;
}

extern "C" void rt_videoplayer_stop(void *vp) {
    auto *player = static_cast<StubPlayer *>(vp);
    player->playing = 0;
    player->position = 0.0;
    player->stop_calls++;
}

extern "C" void rt_videoplayer_update(void *vp, double dt) {
    auto *player = static_cast<StubPlayer *>(vp);
    player->position += dt;
    player->update_calls++;
}

extern "C" void rt_videoplayer_seek(void *vp, double seconds) {
    auto *player = static_cast<StubPlayer *>(vp);
    player->position = seconds;
    player->last_seek = seconds;
    player->seek_calls++;
}

extern "C" void rt_videoplayer_set_volume(void *vp, double vol) {
    static_cast<StubPlayer *>(vp)->volume = vol;
}

extern "C" int64_t rt_videoplayer_get_width(void *vp) {
    return static_cast<StubPlayer *>(vp)->width;
}

extern "C" int64_t rt_videoplayer_get_height(void *vp) {
    return static_cast<StubPlayer *>(vp)->height;
}

extern "C" double rt_videoplayer_get_duration(void *vp) {
    return static_cast<StubPlayer *>(vp)->duration;
}

extern "C" double rt_videoplayer_get_position(void *vp) {
    return static_cast<StubPlayer *>(vp)->position;
}

extern "C" int64_t rt_videoplayer_get_is_playing(void *vp) {
    return static_cast<StubPlayer *>(vp)->playing;
}

extern "C" void *rt_videoplayer_get_frame(void *vp) {
    return static_cast<StubPlayer *>(vp)->frame;
}

static void test_constructor_validates_inputs() {
    StubWidget parent{};
    reset_open_state();

    assert(rt_videowidget_new(nullptr, stub_video_path()) == nullptr);
    assert(rt_videowidget_new(&parent, nullptr) == nullptr);
    assert(g_open_count == 0);

    g_parent_valid = false;
    assert(rt_videowidget_new(&parent, stub_video_path()) == nullptr);
    assert(g_open_count == 0);
    g_parent_valid = true;

    g_open_should_fail = true;
    assert(rt_videowidget_new(&parent, stub_video_path()) == nullptr);

    g_open_should_fail = false;
    g_open_width = static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1;
    g_open_height = 32;
    assert(rt_videowidget_new(&parent, stub_video_path()) == nullptr);

    g_open_width = 0;
    assert(rt_videowidget_new(&parent, stub_video_path()) == nullptr);
    assert(g_release_count > 0);
}

static void test_successful_construction_sets_up_widgets() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    assert(widget != nullptr);
    assert(widget->magic == kVideoWidgetMagic);
    assert(parent.child_count == 1);
    assert(widget->root_widget != nullptr);
    assert(widget->image_widget != nullptr);
    assert(widget->controls_widget != nullptr);
    assert(widget->video_width == g_open_width);
    assert(widget->video_height == g_open_height);
    assert(widget->owner_app == g_current_owner);
    assert(rt_videowidget_is_auto_update(widget) == 1);
    auto *slider = static_cast<StubWidget *>(widget->position_slider);
    assert(slider->slider_min == 0.0);
    assert(slider->slider_max == 1.0);

    auto *player = static_cast<StubPlayer *>(widget->player);
    auto *image = static_cast<StubWidget *>(widget->image_widget);
    assert(image->last_pixels == widget->rgba_scratch);
    assert(image->last_pixels_w == g_open_width);
    assert(image->last_pixels_h == g_open_height);
    auto *rgba = static_cast<const uint8_t *>(image->last_pixels);
    assert(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 0x44);
    assert(image->upload_count == 1);
    const int64_t revision = rt_videowidget_get_revision(widget);
    assert(revision > 0);
    assert(rt_videowidget_was_loaded(widget) == 1);
    assert(rt_videowidget_was_loaded(widget) == 0);
    assert(rt_videowidget_get_revision(widget) == revision);
}

static void test_controls_drive_player_state() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);

    static_cast<StubWidget *>(widget->play_button)->clicked = 1;
    rt_videowidget_update(widget, 0.0);
    assert(player->play_calls == 1);

    static_cast<StubWidget *>(widget->pause_button)->clicked = 1;
    rt_videowidget_update(widget, 0.0);
    assert(player->pause_calls == 1);

    static_cast<StubWidget *>(widget->stop_button)->clicked = 1;
    rt_videowidget_update(widget, 0.0);
    assert(player->stop_calls == 1);
}

static void test_slider_seek_and_looping() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    auto *slider = static_cast<StubWidget *>(widget->position_slider);

    slider->slider_value = 0.5;
    rt_videowidget_update(widget, 0.0);
    assert(player->seek_calls == 1);
    assert(player->last_seek == 4.0);
    assert(rt_videowidget_was_seeked(widget) == 1);
    assert(rt_videowidget_was_seeked(widget) == 0);

    widget->looping = 1;
    player->playing = 0;
    player->position = 2.0;
    rt_videowidget_update(widget, 0.0);
    assert(player->stop_calls == 0);
    assert(player->play_calls == 0);

    player->position = player->duration;
    rt_videowidget_update(widget, 0.0);
    assert(player->stop_calls == 1);
    assert(player->play_calls == 1);
    assert(rt_videowidget_was_ended(widget) == 1);
    assert(rt_videowidget_was_ended(widget) == 0);
}

static void test_root_widget_proxy_methods() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *root = static_cast<StubWidget *>(widget->root_widget);

    assert(rt_videowidget_get_root(widget) == widget->root_widget);
    rt_videowidget_set_visible(widget, 0);
    rt_videowidget_set_enabled(widget, 1);
    rt_videowidget_set_size(widget, 320, 180);
    rt_videowidget_set_preferred_size(widget, 400.0, 240.0);
    rt_videowidget_set_max_size(widget, 800.0, 600.0);
    rt_videowidget_set_flex(widget, 2.0);
    rt_videowidget_set_margin(widget, 6);
    rt_videowidget_set_position(widget, 12, 34);

    StubWidget child{};
    int before_child_count = root->child_count;
    rt_videowidget_add_child(widget, &child);

    assert(root->visible == 0);
    assert(root->enabled == 1);
    assert(root->width == 320);
    assert(root->height == 180);
    assert(root->preferred_w == 400.0);
    assert(root->preferred_h == 240.0);
    assert(root->max_w == 800.0);
    assert(root->max_h == 600.0);
    assert(root->flex == 2.0);
    assert(root->margin == 6);
    assert(root->x == 12);
    assert(root->y == 34);
    assert(root->child_count == before_child_count + 1);
}

static void test_frame_upload_accepts_dimension_changes() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    auto *frame = player->frame;
    auto *image = static_cast<StubWidget *>(widget->image_widget);

    std::free(frame->data);
    frame->data = static_cast<uint32_t *>(std::calloc(80u * 40u, sizeof(uint32_t)));
    assert(frame->data != nullptr);
    frame->data[0] = 0xAABBCCDDu;
    frame->width = 80;
    frame->height = 40;
    rt_videowidget_update(widget, 0.0);

    assert(image->last_pixels == widget->rgba_scratch);
    assert(image->last_pixels_w == 80);
    assert(image->last_pixels_h == 40);
    auto *rgba = static_cast<const uint8_t *>(image->last_pixels);
    assert(rgba[0] == 0xAA && rgba[1] == 0xBB && rgba[2] == 0xCC && rgba[3] == 0xDD);
    assert(widget->video_width == 80);
    assert(widget->video_height == 40);
    assert(image->width == 80);
    assert(image->height == 40);
}

static void test_visibility_and_volume_are_clamped() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *controls = static_cast<StubWidget *>(widget->controls_widget);
    auto *player = static_cast<StubPlayer *>(widget->player);

    rt_videowidget_set_show_controls(widget, 0);
    assert(controls->visible == 0);
    assert(rt_videowidget_get_show_controls(widget) == 0);
    rt_videowidget_set_show_controls(widget, 1);
    assert(controls->visible == 1);
    assert(rt_videowidget_get_show_controls(widget) == 1);
    assert(rt_videowidget_get_loop(widget) == 0);
    rt_videowidget_set_loop(widget, 1);
    assert(rt_videowidget_get_loop(widget) == 1);
    rt_videowidget_set_loop(widget, 0);
    assert(rt_videowidget_get_loop(widget) == 0);

    rt_videowidget_set_volume(widget, -1.0);
    assert(player->volume == 0.0);
    rt_videowidget_set_volume(widget, 2.0);
    assert(player->volume == 1.0);
    rt_videowidget_set_volume(widget, std::numeric_limits<double>::quiet_NaN());
    assert(player->volume == 0.0);
    rt_videowidget_set_volume(widget, std::numeric_limits<double>::infinity());
    assert(player->volume == 0.0);

    player->position = std::numeric_limits<double>::quiet_NaN();
    player->duration = std::numeric_limits<double>::infinity();
    assert(rt_videowidget_get_position(widget) == 0.0);
    assert(rt_videowidget_get_duration(widget) == 0.0);
    player->position = -1.0;
    player->duration = -1.0;
    assert(rt_videowidget_get_position(widget) == 0.0);
    assert(rt_videowidget_get_duration(widget) == 0.0);
    player->position = 2.0;
    player->duration = 8.0;
    assert(rt_videowidget_get_position(widget) == 2.0);
    assert(rt_videowidget_get_duration(widget) == 8.0);
}

static void test_nonfinite_update_delta_is_ignored() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    int before = player->update_calls;

    rt_videowidget_update(widget, std::numeric_limits<double>::quiet_NaN());
    rt_videowidget_update(widget, std::numeric_limits<double>::infinity());
    assert(player->update_calls == before);

    rt_videowidget_update(widget, 0.25);
    assert(player->update_calls == before + 1);
}

/// @brief Verify automatic scheduling and manual updates collapse to one step per generation.
static void test_auto_scheduler_is_generation_idempotent() {
    StubWidget parent{};
    reset_open_state();
    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    player->playing = 1;
    const int before = player->update_calls;

    g_frame_generation = 1;
    rt_videowidget_update_app(widget->owner_app, 0.25, g_frame_generation);
    assert(player->update_calls == before + 1);
    rt_videowidget_update(widget, 0.25);
    rt_videowidget_update_app(widget->owner_app, 0.25, g_frame_generation);
    assert(player->update_calls == before + 1);
    assert(rt_videowidget_next_deadline_ms(widget->owner_app) == 16);

    g_frame_generation = 2;
    rt_videowidget_update(widget, 0.25);
    assert(player->update_calls == before + 2);
    rt_videowidget_update_app(widget->owner_app, 0.25, g_frame_generation);
    assert(player->update_calls == before + 2);

    rt_videowidget_set_auto_update(widget, 0);
    assert(rt_videowidget_is_auto_update(widget) == 0);
    assert(rt_videowidget_next_deadline_ms(widget->owner_app) == -1);
    rt_videowidget_update(widget, 0.25);
    assert(player->update_calls == before + 3);
}

/// @brief Verify buffering/failure edges remain independent and recovery clears the diagnostic.
static void test_media_events_and_error_recovery() {
    StubWidget parent{};
    reset_open_state();
    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    StubPixels *frame = player->frame;
    player->playing = 1;
    player->frame = nullptr;

    rt_videowidget_update(widget, 0.0);
    assert(rt_videowidget_was_buffering_changed(widget) == 1);
    assert(rt_videowidget_was_failed(widget) == 0);
    rt_videowidget_update(widget, 0.0);
    assert(rt_videowidget_was_buffering_changed(widget) == 0);

    player->frame = frame;
    rt_videowidget_update(widget, 0.0);
    assert(rt_videowidget_was_buffering_changed(widget) == 1);
    assert(rt_videowidget_was_buffering_changed(widget) == 0);

    uint32_t *raw = frame->data;
    frame->data = nullptr;
    rt_videowidget_update(widget, 0.0);
    assert(rt_videowidget_was_failed(widget) == 1);
    assert(rt_videowidget_was_failed(widget) == 0);
    rt_string error = rt_videowidget_get_error(widget);
    assert(std::strcmp(rt_string_cstr(error), "Video frame pixel data is unavailable") == 0);
    release_stub_string(error);

    frame->data = raw;
    rt_videowidget_update(widget, 0.0);
    error = rt_videowidget_get_error(widget);
    assert(std::strcmp(rt_string_cstr(error), "") == 0);
    release_stub_string(error);
}

/// @brief Verify controls auto-hide deterministically and fullscreen remains app-scoped.
static void test_controls_auto_hide_and_fullscreen() {
    StubWidget parent{};
    reset_open_state();
    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    auto *player = static_cast<StubPlayer *>(widget->player);
    auto *controls = static_cast<StubWidget *>(widget->controls_widget);

    rt_videowidget_set_controls_auto_hide(widget, 1);
    player->playing = 1;
    rt_videowidget_update(widget, 3.0);
    assert(widget->controls_hidden_by_auto == 1);
    assert(controls->visible == 0);
    rt_videowidget_pause(widget);
    assert(widget->controls_hidden_by_auto == 0);
    assert(controls->visible == 1);

    assert(rt_videowidget_is_fullscreen(widget) == 0);
    rt_videowidget_set_fullscreen(widget, 1);
    assert(rt_videowidget_is_fullscreen(widget) == 1);
    rt_videowidget_set_fullscreen(widget, 0);
    assert(rt_videowidget_is_fullscreen(widget) == 0);
}

/// @brief Verify app destruction invalidates controllers without double-destroying the app tree.
static void test_app_forget_invalidates_controller() {
    StubWidget parent{};
    reset_open_state();
    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    void *root = widget->root_widget;
    assert(root != nullptr);
    rt_videowidget_forget_app(widget->owner_app);
    assert(rt_videowidget_get_root(widget) == nullptr);
    assert(rt_videowidget_get_revision(widget) == 0);
    assert(g_widget_destroy_count == 0);
    rt_widget_destroy(root);
    rt_obj_free(widget);
}

static void test_destroy_releases_player_and_widget_tree() {
    StubWidget parent{};
    reset_open_state();

    auto *widget =
        static_cast<rt_videowidget_view *>(rt_videowidget_new(&parent, stub_video_path()));
    assert(widget != nullptr);
    assert(widget->player != nullptr);
    assert(widget->root_widget != nullptr);

    rt_videowidget_destroy(widget);

    assert(widget->player == nullptr);
    assert(widget->root_widget == nullptr);
    assert(widget->image_widget == nullptr);
    assert(widget->controls_widget == nullptr);
    assert(g_release_count > 0);
    assert(g_widget_destroy_count == 1);

    rt_videowidget_destroy(widget);
    assert(g_widget_destroy_count == 1);
}

int main() {
    test_constructor_validates_inputs();
    test_successful_construction_sets_up_widgets();
    test_controls_drive_player_state();
    test_slider_seek_and_looping();
    test_root_widget_proxy_methods();
    test_frame_upload_accepts_dimension_changes();
    test_visibility_and_volume_are_clamped();
    test_nonfinite_update_delta_is_ignored();
    test_auto_scheduler_is_generation_idempotent();
    test_media_events_and_error_recovery();
    test_controls_auto_hide_and_fullscreen();
    test_app_forget_invalidates_controller();
    test_destroy_releases_player_and_widget_tree();
    std::printf("RTVideoWidgetContractTests passed.\n");
    return 0;
}
