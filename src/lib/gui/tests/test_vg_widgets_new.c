//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/lib/gui/tests/test_vg_widgets_new.c
// Purpose: Unit and software-framebuffer tests for newer GUI widget families.
// Key invariants:
//   - Tests use the public lower-toolkit APIs and retained widget vtables.
//   - Framebuffer assertions exercise the deterministic ZannaGFX software/mock
//     semantic reference rather than platform-native drawing.
// Ownership/Lifetime:
//   - Every test destroys its widgets and any ZannaGFX window it creates.
// Links: src/lib/gui/include/vg_widgets.h,
//        src/lib/gui/include/vg_theme.h,
//        src/lib/graphics/include/vgfx.h
//
//===----------------------------------------------------------------------===//

#include "vg_event.h"
#include "vg_ide_widgets.h"
#include "vg_theme.h"
#include "vg_widget.h"
#include "vg_widgets.h"
#include "vgfx.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Test Harness
//=============================================================================

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name)                                                                                  \
    do {                                                                                           \
        printf("  %-55s", #name "...");                                                            \
        fflush(stdout);                                                                            \
        test_##name();                                                                             \
        if (g_failed == 0)                                                                         \
            printf("OK\n");                                                                        \
        g_passed++;                                                                                \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL\n  (%s:%d: %s)\n", __FILE__, __LINE__, #cond);                            \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEQ(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

//=============================================================================
// Group E1 — vg_slider (vtable implementation)
//=============================================================================

TEST(slider_create_vtable_set) {
    vg_slider_t *s = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(s);
    ASSERT_NEQ(s->base.vtable, NULL); // vtable must be assigned by create
    vg_widget_destroy(&s->base);
}

TEST(slider_default_orientation) {
    vg_slider_t *h = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(h->orientation, VG_SLIDER_HORIZONTAL);
    vg_widget_destroy(&h->base);

    vg_slider_t *v = vg_slider_create(NULL, VG_SLIDER_VERTICAL);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(v->orientation, VG_SLIDER_VERTICAL);
    vg_widget_destroy(&v->base);
}

TEST(slider_set_get_value) {
    vg_slider_t *s = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(s);
    vg_slider_set_range(s, 0.0f, 100.0f);
    vg_slider_set_value(s, 50.0f);
    ASSERT_EQ(vg_slider_get_value(s), 50.0f);
    vg_widget_destroy(&s->base);
}

TEST(slider_clamp_below_min) {
    vg_slider_t *s = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(s);
    vg_slider_set_range(s, 10.0f, 90.0f);
    vg_slider_set_value(s, -5.0f);
    ASSERT_EQ(vg_slider_get_value(s), 10.0f);
    vg_widget_destroy(&s->base);
}

TEST(slider_clamp_above_max) {
    vg_slider_t *s = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(s);
    vg_slider_set_range(s, 10.0f, 90.0f);
    vg_slider_set_value(s, 200.0f);
    ASSERT_EQ(vg_slider_get_value(s), 90.0f);
    vg_widget_destroy(&s->base);
}

TEST(slider_step_snapping) {
    vg_slider_t *s = vg_slider_create(NULL, VG_SLIDER_HORIZONTAL);
    ASSERT_NOT_NULL(s);
    vg_slider_set_range(s, 0.0f, 10.0f);
    vg_slider_set_step(s, 1.0f);
    vg_slider_set_value(s, 3.7f);
    float v = vg_slider_get_value(s);
    // Value must snap to a multiple of step
    ASSERT(v == 3.0f || v == 4.0f);
    vg_widget_destroy(&s->base);
}

//=============================================================================
// Group E2 — vg_progressbar (vtable implementation)
//=============================================================================

TEST(progressbar_create_vtable_set) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    ASSERT_NEQ(pb->base.vtable, NULL);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_default_zero) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    ASSERT_EQ(vg_progressbar_get_value(pb), 0.0f);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_set_value) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    vg_progressbar_set_value(pb, 0.75f);
    ASSERT_EQ(vg_progressbar_get_value(pb), 0.75f);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_clamp_below_zero) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    vg_progressbar_set_value(pb, -0.5f);
    ASSERT_EQ(vg_progressbar_get_value(pb), 0.0f);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_clamp_above_one) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    vg_progressbar_set_value(pb, 1.5f);
    ASSERT_EQ(vg_progressbar_get_value(pb), 1.0f);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_style_change) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    vg_progressbar_set_style(pb, VG_PROGRESS_INDETERMINATE);
    ASSERT_EQ(pb->style, VG_PROGRESS_INDETERMINATE);
    vg_progressbar_set_style(pb, VG_PROGRESS_BAR);
    ASSERT_EQ(pb->style, VG_PROGRESS_BAR);
    vg_widget_destroy(&pb->base);
}

TEST(progressbar_tick_advances_indeterminate_phase) {
    vg_progressbar_t *pb = vg_progressbar_create(NULL);
    ASSERT_NOT_NULL(pb);
    vg_progressbar_set_style(pb, VG_PROGRESS_INDETERMINATE);
    vg_progressbar_tick(pb, 0.5f);
    ASSERT(pb->animation_phase > 0.34f && pb->animation_phase < 0.36f);
    ASSERT(pb->base.needs_paint);
    vg_widget_destroy(&pb->base);
}

//=============================================================================
// Group E2b — Spinner
//=============================================================================

TEST(spinner_create_vtable_set) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    ASSERT_NEQ(spinner->base.vtable, NULL);
    vg_widget_destroy(&spinner->base);
}

TEST(spinner_arrow_keys_adjust_value) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    vg_spinner_set_range(spinner, 0.0, 10.0);
    vg_spinner_set_step(spinner, 2.0);

    vg_event_t up = {0};
    up.type = VG_EVENT_KEY_DOWN;
    up.key.key = VG_KEY_UP;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &up));
    ASSERT_EQ(vg_spinner_get_value(spinner), 2.0);

    vg_event_t down = up;
    down.key.key = VG_KEY_DOWN;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &down));
    ASSERT_EQ(vg_spinner_get_value(spinner), 0.0);

    vg_widget_destroy(&spinner->base);
}

TEST(spinner_mouse_buttons_adjust_value) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    spinner->base.width = 80.0f;
    spinner->base.height = 28.0f;
    vg_spinner_set_range(spinner, 0.0, 10.0);
    vg_spinner_set_step(spinner, 1.0);

    vg_event_t down = {0};
    down.type = VG_EVENT_MOUSE_DOWN;
    down.mouse.x = 70.0f;
    down.mouse.y = 5.0f;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &down));
    ASSERT_EQ(vg_spinner_get_value(spinner), 1.0);

    vg_event_t up = down;
    up.type = VG_EVENT_MOUSE_UP;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &up));

    down.mouse.y = 24.0f;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &down));
    ASSERT_EQ(vg_spinner_get_value(spinner), 0.0);

    vg_widget_destroy(&spinner->base);
}

TEST(spinner_text_entry_commits_value) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    vg_spinner_set_range(spinner, -100.0, 100.0);

    vg_event_t ch4 = {0};
    ch4.type = VG_EVENT_KEY_CHAR;
    ch4.key.codepoint = '4';
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &ch4));

    vg_event_t ch2 = ch4;
    ch2.key.codepoint = '2';
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &ch2));
    ASSERT(spinner->editing);
    ASSERT_STR_EQ(spinner->text_buffer, "42");

    vg_event_t enter = {0};
    enter.type = VG_EVENT_KEY_DOWN;
    enter.key.key = VG_KEY_ENTER;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &enter));
    ASSERT_EQ(vg_spinner_get_value(spinner), 42.0);
    ASSERT(!spinner->editing);

    vg_widget_destroy(&spinner->base);
}

TEST(spinner_escape_cancels_pending_edit) {
    vg_spinner_t *spinner = vg_spinner_create(NULL);
    ASSERT_NOT_NULL(spinner);
    vg_spinner_set_value(spinner, 12.0);

    vg_event_t ch9 = {0};
    ch9.type = VG_EVENT_KEY_CHAR;
    ch9.key.codepoint = '9';
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &ch9));
    ASSERT(spinner->editing);
    ASSERT_STR_EQ(spinner->text_buffer, "9");

    vg_event_t esc = {0};
    esc.type = VG_EVENT_KEY_DOWN;
    esc.key.key = VG_KEY_ESCAPE;
    ASSERT(spinner->base.vtable->handle_event(&spinner->base, &esc));
    ASSERT_EQ(vg_spinner_get_value(spinner), 12.0);
    ASSERT_STR_EQ(spinner->text_buffer, "12");
    ASSERT(!spinner->editing);

    vg_widget_destroy(&spinner->base);
}

//=============================================================================
// Group E2c — Image
//=============================================================================

TEST(image_scale_none_preserves_original_size) {
    vgfx_window_params_t params = {
        .width = 8, .height = 8, .title = "image", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);

    const uint8_t pixel[4] = {255, 0, 0, 255};
    vg_image_set_pixels(image, pixel, 1, 1);
    vg_image_set_scale_mode(image, VG_IMAGE_SCALE_NONE);
    image->bg_color = 0x000000;
    vg_widget_arrange(&image->base, 0.0f, 0.0f, 3.0f, 3.0f);

    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&image->base, win);

    vgfx_color_t color = 0;
    ASSERT_EQ(vgfx_point(win, 0, 0, &color), 1);
    ASSERT_EQ(color, 0xFF0000);
    ASSERT_EQ(vgfx_point(win, 2, 2, &color), 1);
    ASSERT_EQ(color, 0x000000);

    vg_widget_destroy(&image->base);
    vgfx_destroy_window(win);
}

TEST(image_focusable_opt_in_focus_and_ring) {
    vgfx_window_params_t params = {
        .width = 8, .height = 8, .title = "image", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);

    const uint8_t pixel[4] = {0, 255, 0, 255};
    vg_image_set_pixels(image, pixel, 1, 1);
    vg_image_set_scale_mode(image, VG_IMAGE_SCALE_STRETCH);
    image->bg_color = 0x000000;
    vg_widget_arrange(&image->base, 0.0f, 0.0f, 8.0f, 8.0f);

    /* Presentation images decline focus by default. */
    ASSERT(image->base.vtable->can_focus != NULL);
    ASSERT(!image->base.vtable->can_focus(&image->base));

    /* The opt-in makes the image focusable like any interactive widget. */
    vg_image_set_focusable(image, true);
    ASSERT(image->base.vtable->can_focus(&image->base));
    vg_widget_set_focus(&image->base);
    ASSERT((image->base.state & VG_STATE_FOCUSED) != 0);

    /* A focused focusable image paints the theme focus ring over its border
     * while interior content stays image pixels. */
    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&image->base, win);
    const vg_theme_t *theme = vg_theme_get_current();
    ASSERT_NOT_NULL(theme);
    vgfx_color_t corner = 0;
    vgfx_color_t center = 0;
    ASSERT_EQ(vgfx_point(win, 0, 0, &corner), 1);
    ASSERT_EQ(vgfx_point(win, 4, 4, &center), 1);
    ASSERT_EQ(corner, theme->focus.glow_color & 0x00FFFFFFu);
    ASSERT_EQ(center, 0x00FF00);

    /* Clearing the opt-in restores presentation-only behavior. */
    vg_image_set_focusable(image, false);
    ASSERT(!image->base.vtable->can_focus(&image->base));

    vg_widget_destroy(&image->base);
    vgfx_destroy_window(win);
}

TEST(image_opacity_and_stretch_affect_framebuffer) {
    vgfx_window_params_t params = {
        .width = 8, .height = 8, .title = "image", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);

    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);

    const uint8_t pixel[4] = {255, 0, 0, 255};
    vg_image_set_pixels(image, pixel, 1, 1);
    vg_image_set_scale_mode(image, VG_IMAGE_SCALE_STRETCH);
    vg_image_set_opacity(image, 0.5f);
    vg_widget_arrange(&image->base, 0.0f, 0.0f, 2.0f, 2.0f);

    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&image->base, win);

    vgfx_color_t color = 0;
    ASSERT_EQ(vgfx_point(win, 1, 1, &color), 1);
    ASSERT(color == 0x800000 || color == 0x7F0000);

    vg_widget_destroy(&image->base);
    vgfx_destroy_window(win);
}

/// @brief Verify status uploads are atomic, reuse storage, and avoid revisions for byte-identical
/// replacements.
TEST(image_atomic_upload_and_region_update) {
    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);

    const uint8_t original[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    ASSERT(vg_image_try_set_pixels(image, original, 2, 2));
    uint8_t *allocated = image->pixels;
    const size_t capacity = image->pixel_capacity;
    const uint64_t uploaded_revision = image->content_revision;
    ASSERT(image->pixels_opaque);

    ASSERT(!vg_image_try_set_pixels(image, original, -1, 2));
    ASSERT(image->pixels == allocated);
    ASSERT_EQ(memcmp(image->pixels, original, sizeof(original)), 0);
    ASSERT_EQ(image->content_revision, uploaded_revision);

    ASSERT(vg_image_try_set_pixels(image, original, 2, 2));
    ASSERT(image->pixels == allocated);
    ASSERT_EQ(image->pixel_capacity, capacity);
    ASSERT_EQ(image->content_revision, uploaded_revision);

    const uint8_t source[16] = {20, 21, 22, 255, 30, 31, 32, 128, 40, 41, 42, 255, 50, 51, 52, 255};
    ASSERT(vg_image_update_region(image, source, 2, 2, 1, 0, 1, 1, 0, 1));
    ASSERT_EQ(image->pixels[8], 30);
    ASSERT_EQ(image->pixels[9], 31);
    ASSERT_EQ(image->pixels[10], 32);
    ASSERT_EQ(image->pixels[11], 128);
    ASSERT(!image->pixels_opaque);
    const uint64_t region_revision = image->content_revision;

    uint8_t snapshot[16];
    memcpy(snapshot, image->pixels, sizeof(snapshot));
    ASSERT(!vg_image_update_region(image, source, 2, 2, 1, 1, 2, 1, 0, 0));
    ASSERT_EQ(memcmp(image->pixels, snapshot, sizeof(snapshot)), 0);
    ASSERT_EQ(image->content_revision, region_revision);

    ASSERT(vg_image_update_region(image, image->pixels, 2, 2, 0, 0, 1, 1, 1, 1));
    ASSERT_EQ(memcmp(image->pixels + 12, image->pixels, 4), 0);

    vg_widget_destroy(&image->base);
}

/// @brief Verify explicit bilinear filtering produces a deterministic interpolated center pixel
/// and that the resized cache is reused across paints.
TEST(image_bilinear_filter_and_scaled_cache) {
    vgfx_window_params_t params = {
        .width = 4, .height = 2, .title = "image-filter", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vg_image_t *image = vg_image_create(NULL);
    ASSERT_NOT_NULL(image);

    const uint8_t pixels[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    ASSERT(vg_image_try_set_pixels(image, pixels, 2, 1));
    ASSERT_EQ(vg_image_get_filter(image), VG_IMAGE_FILTER_NEAREST);
    vg_image_set_filter(image, VG_IMAGE_FILTER_BILINEAR);
    ASSERT_EQ(vg_image_get_filter(image), VG_IMAGE_FILTER_BILINEAR);
    ASSERT_EQ(vg_image_get_filter(NULL), VG_IMAGE_FILTER_NEAREST);
    vg_image_set_scale_mode(image, VG_IMAGE_SCALE_STRETCH);
    vg_widget_arrange(&image->base, 0.0f, 0.0f, 3.0f, 1.0f);

    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&image->base, win);
    vgfx_color_t color = 0;
    ASSERT_EQ(vgfx_point(win, 0, 0, &color), 1);
    ASSERT_EQ(color, 0xFF0000);
    ASSERT_EQ(vgfx_point(win, 1, 0, &color), 1);
    ASSERT_EQ(color, 0x800080);
    ASSERT_EQ(vgfx_point(win, 2, 0, &color), 1);
    ASSERT_EQ(color, 0x0000FF);
    ASSERT_NOT_NULL(image->scaled_pixels);
    uint8_t *cache = image->scaled_pixels;
    const uint64_t cached_revision = image->scaled_revision;

    vgfx_cls(win, VGFX_BLACK);
    image->base.needs_paint = true;
    vg_widget_paint(&image->base, win);
    ASSERT(image->scaled_pixels == cache);
    ASSERT_EQ(image->scaled_revision, cached_revision);

    vg_widget_destroy(&image->base);
    vgfx_destroy_window(win);
}

/// @brief Verify Minimap observes editor source/viewport revisions and retains a bounded O(1)
/// direct-mapped line-summary cache across scroll-only transitions.
TEST(minimap_revision_and_bounded_line_cache) {
    vgfx_window_params_t params = {
        .width = 128, .height = 100, .title = "minimap-cache", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vg_codeeditor_t *editor = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(editor);
    vg_codeeditor_set_text(editor, "alpha\n  beta\ngamma\n    delta");
    vg_minimap_t *minimap = vg_minimap_create(editor);
    ASSERT_NOT_NULL(minimap);
    ASSERT(vg_minimap_set_maximum_cached_lines(minimap, 2));
    vg_widget_arrange(&minimap->base, 0.0f, 0.0f, 120.0f, 100.0f);
    vg_widget_paint(&minimap->base, win);
    ASSERT_EQ(vg_minimap_get_cached_line_count(minimap), 2u);

    vg_minimap_invalidate_lines(minimap, 2u, 2u);
    ASSERT_EQ(vg_minimap_get_cached_line_count(minimap), 1u);
    const uint64_t initial_revision = vg_minimap_get_source_revision(minimap);
    vg_codeeditor_set_text(editor, "one\ntwo\nthree\nfour");
    ASSERT(vg_minimap_sync_source(minimap));
    ASSERT(vg_minimap_get_source_revision(minimap) > initial_revision);
    ASSERT_EQ(vg_minimap_get_cached_line_count(minimap), 0u);

    vg_widget_paint(&minimap->base, win);
    const uint32_t cached_before_scroll = vg_minimap_get_cached_line_count(minimap);
    const uint64_t content_revision = vg_minimap_get_source_revision(minimap);
    editor->scroll_y += 12.0f;
    ASSERT(vg_minimap_sync_source(minimap));
    ASSERT(vg_minimap_get_source_revision(minimap) > content_revision);
    ASSERT_EQ(vg_minimap_get_cached_line_count(minimap), cached_before_scroll);

    ASSERT(vg_minimap_set_maximum_cached_lines(minimap, 0u));
    ASSERT_EQ(vg_minimap_get_cached_line_count(minimap), 0u);
    vg_minimap_destroy(minimap);
    vg_widget_destroy(&editor->base);
    vgfx_destroy_window(win);
}

//=============================================================================
// Group E2d — Public color controls
//=============================================================================

/// @brief Verify translucent swatches preserve the checkerboard instead of erasing it with an
/// opaque final fill.
TEST(colorswatch_transparency_composites_over_checkerboard) {
    vgfx_window_params_t params = {
        .width = 16, .height = 8, .title = "color", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vg_colorswatch_t *swatch = vg_colorswatch_create(NULL, 0x80FF0000u);
    ASSERT_NOT_NULL(swatch);
    vg_widget_arrange(&swatch->base, 0.0f, 0.0f, 12.0f, 8.0f);

    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&swatch->base, win);
    vgfx_color_t first_checker = 0;
    vgfx_color_t second_checker = 0;
    const int first_ok = vgfx_point(win, 1, 1, &first_checker);
    const int second_ok = vgfx_point(win, 5, 1, &second_checker);

    vg_widget_destroy(&swatch->base);
    vgfx_destroy_window(win);
    ASSERT_EQ(first_ok, 1);
    ASSERT_EQ(second_ok, 1);
    ASSERT_NEQ(first_checker, second_checker);
    ASSERT_NEQ(first_checker, 0xFF0000u);
    ASSERT_NEQ(second_checker, 0xFF0000u);
}

/// @brief Verify opaque swatch fill and selection/focus borders use the live theme rather than a
/// stale construction-time palette.
TEST(colorswatch_paint_uses_opaque_rgb_and_live_focus_theme) {
    vgfx_window_params_t params = {
        .width = 12, .height = 12, .title = "color", .fps = 0, .resizable = 0};
    vgfx_window_t win = vgfx_create_window(&params);
    ASSERT_NOT_NULL(win);
    vg_colorswatch_t *swatch = vg_colorswatch_create(NULL, 0xFF123456u);
    ASSERT_NOT_NULL(swatch);
    vg_colorswatch_set_selected(swatch, true);
    vg_widget_arrange(&swatch->base, 2.0f, 2.0f, 8.0f, 8.0f);

    vg_theme_t *old_theme = vg_theme_get_current();
    vg_theme_t live_theme = *old_theme;
    live_theme.colors.border_focus = 0xFF00FF00u;
    vg_theme_set_current(&live_theme);
    vgfx_cls(win, VGFX_BLACK);
    vg_widget_paint(&swatch->base, win);
    vgfx_color_t interior = 0;
    vgfx_color_t border = 0;
    const int interior_ok = vgfx_point(win, 5, 5, &interior);
    const int border_ok = vgfx_point(win, 2, 2, &border);
    vg_theme_set_current(old_theme);

    vg_widget_destroy(&swatch->base);
    vgfx_destroy_window(win);
    ASSERT_EQ(interior_ok, 1);
    ASSERT_EQ(border_ok, 1);
    ASSERT_EQ(interior, 0x123456u);
    ASSERT_EQ(border, 0x00FF00u);
}

//=============================================================================
// Group E3 — vg_listbox (vtable implementation)
//=============================================================================

TEST(listbox_create_vtable_set) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    ASSERT_NEQ(lb->base.vtable, NULL);
    vg_widget_destroy(&lb->base);
}

TEST(listbox_add_items_count) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_add_item(lb, "Alpha", NULL);
    vg_listbox_add_item(lb, "Beta", NULL);
    vg_listbox_add_item(lb, "Gamma", NULL);
    ASSERT_EQ(lb->item_count, 3);
    vg_widget_destroy(&lb->base);
}

TEST(listbox_no_initial_selection) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_add_item(lb, "Item", NULL);
    ASSERT_NULL(vg_listbox_get_selected(lb));
    vg_widget_destroy(&lb->base);
}

TEST(listbox_select_item) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_item_t *item = vg_listbox_add_item(lb, "Item", NULL);
    ASSERT_NOT_NULL(item);
    vg_listbox_select(lb, item);
    ASSERT_EQ(vg_listbox_get_selected(lb), item);
    vg_widget_destroy(&lb->base);
}

TEST(listbox_remove_item) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_item_t *b = vg_listbox_add_item(lb, "B", NULL);
    ASSERT_EQ(lb->item_count, 2);
    vg_listbox_remove_item(lb, b);
    ASSERT_EQ(lb->item_count, 1);
    vg_widget_destroy(&lb->base);
}

TEST(listbox_remove_clears_selection) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_item_t *item = vg_listbox_add_item(lb, "X", NULL);
    vg_listbox_select(lb, item);
    ASSERT_EQ(vg_listbox_get_selected(lb), item);
    vg_listbox_remove_item(lb, item);
    ASSERT_NULL(vg_listbox_get_selected(lb));
    vg_widget_destroy(&lb->base);
}

TEST(listbox_clear_empties_list) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);
    vg_listbox_add_item(lb, "A", NULL);
    vg_listbox_add_item(lb, "B", NULL);
    vg_listbox_add_item(lb, "C", NULL);
    ASSERT_EQ(lb->item_count, 3);
    vg_listbox_clear(lb);
    ASSERT_EQ(lb->item_count, 0);
    ASSERT_NULL(lb->first_item);
    ASSERT_NULL(lb->last_item);
    vg_widget_destroy(&lb->base);
}

TEST(listbox_mutations_invalidate_layout_and_paint) {
    vg_listbox_t *lb = vg_listbox_create(NULL);
    ASSERT_NOT_NULL(lb);

    lb->base.needs_layout = false;
    lb->base.needs_paint = false;
    vg_listbox_item_t *item = vg_listbox_add_item(lb, "Item", NULL);
    ASSERT_NOT_NULL(item);
    ASSERT(lb->base.needs_layout);
    ASSERT(lb->base.needs_paint);

    lb->base.needs_layout = false;
    lb->base.needs_paint = false;
    vg_listbox_select(lb, item);
    ASSERT(!lb->base.needs_layout);
    ASSERT(lb->base.needs_paint);

    lb->base.needs_layout = false;
    lb->base.needs_paint = false;
    vg_listbox_clear(lb);
    ASSERT(lb->base.needs_layout);
    ASSERT(lb->base.needs_paint);

    vg_widget_destroy(&lb->base);
}

//=============================================================================
// Group D-other — vg_breadcrumb max_items (new feature)
//=============================================================================

TEST(breadcrumb_push_pop_basic) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    vg_breadcrumb_push(bc, "Root", NULL);
    vg_breadcrumb_push(bc, "Folder", NULL);
    ASSERT_EQ((int)bc->item_count, 2);
    vg_breadcrumb_pop(bc);
    ASSERT_EQ((int)bc->item_count, 1);
    vg_breadcrumb_destroy(bc);
}

TEST(breadcrumb_clear_resets) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    vg_breadcrumb_push(bc, "A", NULL);
    vg_breadcrumb_push(bc, "B", NULL);
    vg_breadcrumb_push(bc, "C", NULL);
    vg_breadcrumb_clear(bc);
    ASSERT_EQ((int)bc->item_count, 0);
    vg_breadcrumb_destroy(bc);
}

TEST(breadcrumb_max_items_sliding_window) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    vg_breadcrumb_set_max_items(bc, 3);
    vg_breadcrumb_push(bc, "A", NULL);
    vg_breadcrumb_push(bc, "B", NULL);
    vg_breadcrumb_push(bc, "C", NULL);
    ASSERT_EQ((int)bc->item_count, 3);
    // Push 4th — oldest (A) must be evicted
    vg_breadcrumb_push(bc, "D", NULL);
    ASSERT_EQ((int)bc->item_count, 3);
    ASSERT_STR_EQ(bc->items[0].label, "B");
    ASSERT_STR_EQ(bc->items[2].label, "D");
    vg_breadcrumb_destroy(bc);
}

TEST(breadcrumb_set_max_trims_existing) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    vg_breadcrumb_push(bc, "A", NULL);
    vg_breadcrumb_push(bc, "B", NULL);
    vg_breadcrumb_push(bc, "C", NULL);
    vg_breadcrumb_push(bc, "D", NULL);
    ASSERT_EQ((int)bc->item_count, 4);
    // Restrict to 2 — oldest two (A, B) get trimmed
    vg_breadcrumb_set_max_items(bc, 2);
    ASSERT_EQ((int)bc->item_count, 2);
    ASSERT_STR_EQ(bc->items[0].label, "C");
    ASSERT_STR_EQ(bc->items[1].label, "D");
    vg_breadcrumb_destroy(bc);
}

TEST(breadcrumb_separator_change) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    ASSERT_NOT_NULL(bc->separator); // Default is ">"
    vg_breadcrumb_set_separator(bc, "/");
    ASSERT_STR_EQ(bc->separator, "/");
    vg_breadcrumb_set_separator(bc, NULL);
    ASSERT_NULL(bc->separator);
    vg_breadcrumb_destroy(bc);
}

TEST(breadcrumb_user_data_not_owned_by_default) {
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    ASSERT_NOT_NULL(bc);
    char *payload = strdup("payload");
    ASSERT_NOT_NULL(payload);
    vg_breadcrumb_push(bc, "A", payload);
    ASSERT_EQ((int)bc->item_count, 1);
    ASSERT(!bc->items[0].owns_user_data);
    vg_breadcrumb_clear(bc);
    free(payload);
    vg_breadcrumb_destroy(bc);
}

//=============================================================================
// Group D-other — vg_commandpalette clear (new feature)
//=============================================================================

TEST(commandpalette_create_basic) {
    vg_commandpalette_t *p = vg_commandpalette_create();
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((int)p->command_count, 0);
    ASSERT(!p->is_visible);
    vg_commandpalette_destroy(p);
}

TEST(commandpalette_add_and_find) {
    vg_commandpalette_t *p = vg_commandpalette_create();
    ASSERT_NOT_NULL(p);
    vg_command_t *cmd =
        vg_commandpalette_add_command(p, "file.open", "Open File", "Ctrl+O", NULL, NULL);
    ASSERT_NOT_NULL(cmd);
    ASSERT_EQ((int)p->command_count, 1);
    vg_command_t *found = vg_commandpalette_get_command(p, "file.open");
    ASSERT_EQ(found, cmd);
    vg_commandpalette_destroy(p);
}

TEST(commandpalette_remove_command) {
    vg_commandpalette_t *p = vg_commandpalette_create();
    vg_commandpalette_add_command(p, "a", "A", NULL, NULL, NULL);
    vg_commandpalette_add_command(p, "b", "B", NULL, NULL, NULL);
    ASSERT_EQ((int)p->command_count, 2);
    vg_commandpalette_remove_command(p, "a");
    ASSERT_EQ((int)p->command_count, 1);
    ASSERT_NULL(vg_commandpalette_get_command(p, "a"));
    ASSERT_NOT_NULL(vg_commandpalette_get_command(p, "b"));
    vg_commandpalette_destroy(p);
}

TEST(commandpalette_clear_all) {
    vg_commandpalette_t *p = vg_commandpalette_create();
    vg_commandpalette_add_command(p, "x", "X", NULL, NULL, NULL);
    vg_commandpalette_add_command(p, "y", "Y", NULL, NULL, NULL);
    vg_commandpalette_add_command(p, "z", "Z", NULL, NULL, NULL);
    ASSERT_EQ((int)p->command_count, 3);
    vg_commandpalette_clear(p);
    ASSERT_EQ((int)p->command_count, 0);
    ASSERT_EQ((int)p->filtered_count, 0);
    ASSERT_EQ(p->selected_index, -1);
    vg_commandpalette_destroy(p);
}

TEST(commandpalette_show_hide_toggle) {
    vg_commandpalette_t *p = vg_commandpalette_create();
    ASSERT(!p->is_visible);
    vg_commandpalette_show(p);
    ASSERT(p->is_visible);
    vg_commandpalette_hide(p);
    ASSERT(!p->is_visible);
    vg_commandpalette_toggle(p);
    ASSERT(p->is_visible);
    vg_commandpalette_toggle(p);
    ASSERT(!p->is_visible);
    vg_commandpalette_destroy(p);
}

TEST(commandpalette_scale_and_viewport_bounds) {
    vg_theme_t *theme = vg_theme_get_current();
    ASSERT_NOT_NULL(theme);
    float previous_scale = theme->ui_scale;

    vg_commandpalette_t *p = vg_commandpalette_create();
    ASSERT_NOT_NULL(p);
    for (int i = 0; i < 20; i++) {
        char id[24];
        char label[32];
        snprintf(id, sizeof(id), "command.%d", i);
        snprintf(label, sizeof(label), "Command number %d", i);
        ASSERT_NOT_NULL(vg_commandpalette_add_command(p, id, label, "Ctrl+Shift+P", NULL, NULL));
    }
    vg_commandpalette_show(p);
    theme->ui_scale = 2.0f;
    vg_widget_measure(&p->base, 600.0f, 400.0f);
    vg_widget_arrange(&p->base, 0.0f, 0.0f, p->base.measured_width, p->base.measured_height);

    vg_event_t wheel_down = {0};
    wheel_down.type = VG_EVENT_MOUSE_WHEEL;
    wheel_down.wheel.delta_y = -1.0f;
    bool handled_wheel = p->base.vtable->handle_event(&p->base, &wheel_down);
    int first_visible_after_wheel = p->first_visible_index;
    int selected_after_wheel = p->selected_index;

    vg_event_t third_row = vg_event_mouse(VG_EVENT_MOUSE_MOVE, 20.0f, 210.0f, VG_MOUSE_LEFT, 0);
    bool handled_third_row = p->base.vtable->handle_event(&p->base, &third_row);
    float measured_width = p->base.measured_width;
    float measured_height = p->base.measured_height;
    int selected_index = p->selected_index;
    theme->ui_scale = previous_scale;
    vg_commandpalette_destroy(p);

    // A 500-point palette should scale up, then clamp to 16-point viewport
    // margins. Its row count must also shrink so the bottom stays visible.
    ASSERT(measured_width >= 535.0f);
    ASSERT(measured_width <= 536.0f);
    ASSERT(measured_height <= 308.0f);
    ASSERT(measured_height >= 200.0f);
    ASSERT(handled_wheel);
    ASSERT_EQ(first_visible_after_wheel, 1);
    ASSERT_EQ(selected_after_wheel, 1);
    ASSERT(handled_third_row);
    ASSERT_EQ(selected_index, 3);
}

//=============================================================================
// Group D-menu — menu management (new functions)
//=============================================================================

TEST(menu_remove_item_updates_count) {
    vg_menubar_t *bar = vg_menubar_create(NULL);
    ASSERT_NOT_NULL(bar);
    vg_menu_t *menu = vg_menubar_add_menu(bar, "File");
    ASSERT_NOT_NULL(menu);
    vg_menu_item_t *item1 = vg_menu_add_item(menu, "Open", "Ctrl+O", NULL, NULL);
    vg_menu_item_t *item2 = vg_menu_add_item(menu, "Save", "Ctrl+S", NULL, NULL);
    ASSERT_NOT_NULL(item1);
    ASSERT_NOT_NULL(item2);
    ASSERT_EQ(menu->item_count, 2);
    vg_menu_remove_item(menu, item1);
    ASSERT_EQ(menu->item_count, 1);
    ASSERT_EQ(menu->first_item, item2); // item2 is now first
    vg_widget_destroy(&bar->base);
}

TEST(menu_clear_empties_list) {
    vg_menubar_t *bar = vg_menubar_create(NULL);
    vg_menu_t *menu = vg_menubar_add_menu(bar, "Edit");
    vg_menu_add_item(menu, "Cut", NULL, NULL, NULL);
    vg_menu_add_item(menu, "Copy", NULL, NULL, NULL);
    vg_menu_add_item(menu, "Paste", NULL, NULL, NULL);
    ASSERT_EQ(menu->item_count, 3);
    vg_menu_clear(menu);
    ASSERT_EQ(menu->item_count, 0);
    ASSERT_NULL(menu->first_item);
    ASSERT_NULL(menu->last_item);
    vg_widget_destroy(&bar->base);
}

TEST(menubar_remove_menu_updates_count) {
    vg_menubar_t *bar = vg_menubar_create(NULL);
    vg_menu_t *file = vg_menubar_add_menu(bar, "File");
    vg_menubar_add_menu(bar, "Edit");
    vg_menu_t *help = vg_menubar_add_menu(bar, "Help");
    ASSERT_EQ(bar->menu_count, 3);
    vg_menubar_remove_menu(bar, file);
    ASSERT_EQ(bar->menu_count, 2);
    vg_menubar_remove_menu(bar, help);
    ASSERT_EQ(bar->menu_count, 1);
    vg_widget_destroy(&bar->base);
}

//=============================================================================
// Group D-editor — vg_codeeditor new dynamic array fields
//=============================================================================

TEST(codeeditor_highlight_spans_init_zero) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    ASSERT_EQ(ed->highlight_span_count, 0);
    ASSERT_EQ(ed->highlight_span_cap, 0);
    ASSERT_NULL(ed->highlight_spans);
    vg_widget_destroy(&ed->base);
}

TEST(codeeditor_gutter_icons_init_zero) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    ASSERT_EQ(ed->gutter_icon_count, 0);
    ASSERT_EQ(ed->gutter_icon_cap, 0);
    ASSERT_NULL(ed->gutter_icons);
    vg_widget_destroy(&ed->base);
}

TEST(codeeditor_fold_regions_init_zero) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    ASSERT_EQ(ed->fold_region_count, 0);
    ASSERT_EQ(ed->fold_region_cap, 0);
    ASSERT_NULL(ed->fold_regions);
    vg_widget_destroy(&ed->base);
}

TEST(codeeditor_extra_cursors_init_zero) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    ASSERT_EQ(ed->extra_cursor_count, 0);
    ASSERT_EQ(ed->extra_cursor_cap, 0);
    ASSERT_NULL(ed->extra_cursors);
    vg_widget_destroy(&ed->base);
}

TEST(codeeditor_tick_toggles_cursor_visibility) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    ed->base.state |= VG_STATE_FOCUSED;
    ed->cursor_visible = true;
    vg_codeeditor_tick(ed, 0.6f);
    ASSERT(ed->cursor_visible == false);
    ASSERT(ed->base.needs_paint);
    vg_widget_destroy(&ed->base);
}

TEST(codeeditor_mouse_wheel_scrolls_per_notch) {
    vg_codeeditor_t *ed = vg_codeeditor_create(NULL);
    ASSERT_NOT_NULL(ed);
    vg_codeeditor_set_text(ed, "one\ntwo\nthree\nfour\nfive\nsix\n");
    ed->base.width = 320.0f;
    ed->base.height = ed->line_height * 2.0f;
    ed->base.measured_width = ed->base.width;
    ed->base.measured_height = ed->base.height;

    vg_event_t wheel = {0};
    wheel.type = VG_EVENT_MOUSE_WHEEL;
    wheel.wheel.delta_y = -1.0f;
    ASSERT(ed->base.vtable->handle_event(&ed->base, &wheel));
    // Wheel input eases toward its target (Zanna Studio plan 05); settle the
    // animation so the landed distance is observable.
    int settle_guard = 0;
    while (vg_codeeditor_smooth_tick(ed, 16.0f) && settle_guard++ < 1000) {
    }
    // CODEEDITOR_MOUSE_WHEEL_LINES = 0.3f (slowed default; see vg_codeeditor.c).
    // One wheel notch produces 0.3 line_heights of scroll, not a full line.
    // Using a small tolerance to absorb any floating-point rounding from
    // the multiplication chain delta_y * line_height * MOUSE_WHEEL_LINES.
    float expected = ed->line_height * 0.3f;
    float diff = ed->scroll_y - expected;
    if (diff < 0.0f)
        diff = -diff;
    ASSERT(diff < 0.001f);

    vg_widget_destroy(&ed->base);
}

//=============================================================================
// Entry Point
//=============================================================================

int main(void) {
    printf("=== VG Widget New Features Tests ===\n\n");

    printf("Group E1 — Slider vtable:\n");
    RUN(slider_create_vtable_set);
    RUN(slider_default_orientation);
    RUN(slider_set_get_value);
    RUN(slider_clamp_below_min);
    RUN(slider_clamp_above_max);
    RUN(slider_step_snapping);

    printf("\nGroup E2 — ProgressBar vtable:\n");
    RUN(progressbar_create_vtable_set);
    RUN(progressbar_default_zero);
    RUN(progressbar_set_value);
    RUN(progressbar_clamp_below_zero);
    RUN(progressbar_clamp_above_one);
    RUN(progressbar_style_change);
    RUN(progressbar_tick_advances_indeterminate_phase);

    printf("\nGroup E2b — Spinner:\n");
    RUN(spinner_create_vtable_set);
    RUN(spinner_arrow_keys_adjust_value);
    RUN(spinner_mouse_buttons_adjust_value);
    RUN(spinner_text_entry_commits_value);
    RUN(spinner_escape_cancels_pending_edit);

    printf("\nGroup E2c — Image:\n");
    RUN(image_scale_none_preserves_original_size);
    RUN(image_focusable_opt_in_focus_and_ring);
    RUN(image_opacity_and_stretch_affect_framebuffer);
    RUN(image_atomic_upload_and_region_update);
    RUN(image_bilinear_filter_and_scaled_cache);
    RUN(minimap_revision_and_bounded_line_cache);

    printf("\nGroup E2d — Color controls:\n");
    RUN(colorswatch_transparency_composites_over_checkerboard);
    RUN(colorswatch_paint_uses_opaque_rgb_and_live_focus_theme);

    printf("\nGroup E3 — ListBox vtable:\n");
    RUN(listbox_create_vtable_set);
    RUN(listbox_add_items_count);
    RUN(listbox_no_initial_selection);
    RUN(listbox_select_item);
    RUN(listbox_remove_item);
    RUN(listbox_remove_clears_selection);
    RUN(listbox_clear_empties_list);
    RUN(listbox_mutations_invalidate_layout_and_paint);

    printf("\nGroup D-other — Breadcrumb max_items:\n");
    RUN(breadcrumb_push_pop_basic);
    RUN(breadcrumb_clear_resets);
    RUN(breadcrumb_max_items_sliding_window);
    RUN(breadcrumb_set_max_trims_existing);
    RUN(breadcrumb_separator_change);

    printf("\nGroup D-other — CommandPalette clear:\n");
    RUN(commandpalette_create_basic);
    RUN(commandpalette_add_and_find);
    RUN(commandpalette_remove_command);
    RUN(commandpalette_clear_all);
    RUN(commandpalette_show_hide_toggle);
    RUN(commandpalette_scale_and_viewport_bounds);

    printf("\nGroup D-menu — Menu management:\n");
    RUN(menu_remove_item_updates_count);
    RUN(menu_clear_empties_list);
    RUN(menubar_remove_menu_updates_count);

    printf("\nGroup D-editor — CodeEditor new fields:\n");
    RUN(codeeditor_highlight_spans_init_zero);
    RUN(codeeditor_gutter_icons_init_zero);
    RUN(codeeditor_fold_regions_init_zero);
    RUN(codeeditor_extra_cursors_init_zero);
    RUN(codeeditor_tick_toggles_cursor_visibility);
    RUN(codeeditor_mouse_wheel_scrolls_per_notch);

    printf("\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
