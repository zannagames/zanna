//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_image.c
// Purpose: Image and FloatingPanel GUI widgets for the Zanna runtime. Split out
//          of rt_gui_widgets_complex.c; shares GUI types via rt_gui_internal.h.
//
// Key invariants:
//   - Mirrors rt_gui_widgets_complex.c's ZANNA_ENABLE_GRAPHICS guard: real
//     widgets when graphics is enabled, no-op stubs otherwise.
//   - Image handles are validated via rt_image_checked before use.
//   - FloatingPanel public geometry crosses effective UI scale exactly once.
//
// Ownership/Lifetime:
//   - Widgets are owned by the GUI widget tree; this layer borrows them.
//
// Links: src/runtime/graphics/gui/rt_gui_widgets_complex.c (other complex widgets),
//        src/runtime/graphics/gui/rt_gui_internal.h (shared GUI types + API)
//
//===----------------------------------------------------------------------===//

#include "rt_gui_internal.h"
#include "rt_pixels.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Resolve a parent-container handle to its widget (file-local copy).
/// @details Three-state contract: a NULL handle returns NULL (legitimate top-level
///          placement); a valid handle returns its container widget; a non-NULL
///          handle that fails to resolve also returns NULL.
static vg_widget_t *rt_widget_parent_or_null_if_invalid(void *parent) {
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    return parent_widget;
}

/// @brief Safe-cast a handle to a live Image widget, or NULL.
static vg_image_t *rt_image_checked(void *handle) {
    return (vg_image_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_IMAGE);
}

/// @brief Atomically upload caller-converted straight RGBA bytes into an Image widget.
/// @details This internal media fast path performs no conversion allocation. The lower Image
///          copies into reusable owned storage and preserves its old pixels on validation or OOM.
int rt_gui_image_try_set_rgba_bytes(void *image,
                                    const uint8_t *rgba,
                                    int64_t width,
                                    int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (!img || !rgba || width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX)
        return 0;
    return vg_image_try_set_pixels(img, rgba, (int)width, (int)height) ? 1 : 0;
}

//=============================================================================
// Image Widget
//=============================================================================

/// @brief Create an image widget — displays a Pixels object as a static image.
void *rt_image_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_image_create(parent_widget);
}

/// @brief Convert a Zanna Pixels object to byte-order RGBA and upload to an image widget.
/// @details Zanna stores pixels as packed 0xRRGGBBAA 32-bit integers; the lower image expects
///          interleaved [R, G, B, A] bytes. This function allocates a temporary conversion buffer,
///          shuffles channels, performs an atomic lower upload, and then frees the buffer.
///          Width/height zero defaults to the Pixels object dimensions.
/// @param image  Image widget to update (may be NULL — no-op).
/// @param pixels Zanna Pixels object providing source pixel data (NULL clears the image).
/// @param width  Crop width (0 = full Pixels width).
/// @param height Crop height (0 = full Pixels height).
/// @return 1 on success, 0 on failure (NULL image, zero dimensions, or OOM).
static int rt_image_set_from_pixels_object(vg_image_t *image,
                                           void *pixels,
                                           int64_t width,
                                           int64_t height) {
    if (!image)
        return 0;
    if (!pixels) {
        vg_image_clear(image);
        return 1;
    }

    int64_t source_w = rt_pixels_width(pixels);
    int64_t source_h = rt_pixels_height(pixels);
    const uint32_t *src = rt_pixels_raw_buffer(pixels);
    if (source_w <= 0 || source_h <= 0 || !src)
        return 0;
    if ((uintmax_t)source_w > (uintmax_t)SIZE_MAX || (uintmax_t)source_h > (uintmax_t)SIZE_MAX)
        return 0;
    if ((size_t)source_w > SIZE_MAX / (size_t)source_h)
        return 0;

    if (width <= 0)
        width = source_w;
    if (height <= 0)
        height = source_h;
    if (width > source_w)
        width = source_w;
    if (height > source_h)
        height = source_h;
    if (width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX)
        return 0;

    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / 4)
        return 0;

    size_t pixel_count = w * h;
    uint8_t *rgba = (uint8_t *)malloc(pixel_count * 4);
    if (!rgba)
        return 0;

    size_t stride = (size_t)source_w;
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            uint32_t px = src[y * stride + x];
            size_t out = (y * w + x) * 4;
            rgba[out + 0] = (uint8_t)((px >> 24) & 0xFF);
            rgba[out + 1] = (uint8_t)((px >> 16) & 0xFF);
            rgba[out + 2] = (uint8_t)((px >> 8) & 0xFF);
            rgba[out + 3] = (uint8_t)(px & 0xFF);
        }
    }

    const bool uploaded = vg_image_try_set_pixels(image, rgba, (int)width, (int)height);
    free(rgba);
    return uploaded ? 1 : 0;
}

/// @brief Set the pixels of the image.
void rt_image_set_pixels(void *image, void *pixels, int64_t width, int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    rt_image_set_from_pixels_object(rt_image_checked(image), pixels, width, height);
}

/// @brief Atomically upload a Pixels object and report whether it completed.
/// @details NULL source objects are rejected so callers cannot accidentally clear a valid image;
///          the legacy SetPixels entry point retains its NULL-clears compatibility behavior.
int64_t rt_image_try_set_pixels(void *image, void *pixels, int64_t width, int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    if (!pixels)
        return 0;
    return rt_image_set_from_pixels_object(rt_image_checked(image), pixels, width, height) ? 1 : 0;
}

/// @brief Convert and atomically copy a rectangular Pixels region into an existing image.
/// @details Rectangle arithmetic is validated before allocating or touching image state. Only the
///          requested source region is converted, which bounds transient memory to width*height*4.
int64_t rt_image_update_region(void *image,
                               void *pixels,
                               int64_t source_x,
                               int64_t source_y,
                               int64_t width,
                               int64_t height,
                               int64_t dest_x,
                               int64_t dest_y) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (!img || !pixels || source_x < 0 || source_y < 0 || width <= 0 || height <= 0 ||
        dest_x < 0 || dest_y < 0 || source_x > INT32_MAX || source_y > INT32_MAX ||
        width > INT32_MAX || height > INT32_MAX || dest_x > INT32_MAX || dest_y > INT32_MAX) {
        return 0;
    }

    const int64_t source_width = rt_pixels_width(pixels);
    const int64_t source_height = rt_pixels_height(pixels);
    const uint32_t *source = rt_pixels_raw_buffer(pixels);
    if (!source || source_width <= 0 || source_height <= 0 || width > source_width ||
        height > source_height || source_x > source_width - width ||
        source_y > source_height - height || (uintmax_t)source_width > (uintmax_t)SIZE_MAX) {
        return 0;
    }

    const size_t region_width = (size_t)width;
    const size_t region_height = (size_t)height;
    if (region_width > SIZE_MAX / region_height || region_width * region_height > SIZE_MAX / 4u) {
        return 0;
    }
    uint8_t *rgba = (uint8_t *)malloc(region_width * region_height * 4u);
    if (!rgba)
        return 0;

    const size_t stride = (size_t)source_width;
    for (size_t y = 0; y < region_height; ++y) {
        for (size_t x = 0; x < region_width; ++x) {
            const uint32_t packed = source[((size_t)source_y + y) * stride + (size_t)source_x + x];
            const size_t output = (y * region_width + x) * 4u;
            rgba[output + 0u] = (uint8_t)((packed >> 24u) & 0xFFu);
            rgba[output + 1u] = (uint8_t)((packed >> 16u) & 0xFFu);
            rgba[output + 2u] = (uint8_t)((packed >> 8u) & 0xFFu);
            rgba[output + 3u] = (uint8_t)(packed & 0xFFu);
        }
    }

    const bool updated = vg_image_update_region(img,
                                                rgba,
                                                (int)width,
                                                (int)height,
                                                0,
                                                0,
                                                (int)width,
                                                (int)height,
                                                (int)dest_x,
                                                (int)dest_y);
    free(rgba);
    return updated ? 1 : 0;
}

/// @brief Clear the image widget's pixel data, showing nothing.
void rt_image_clear(void *image) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (img) {
        vg_image_clear(img);
    }
}

/// @brief Set the scale mode of the image.
void rt_image_set_scale_mode(void *image, int64_t mode) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (img) {
        vg_image_set_scale_mode(img, (vg_image_scale_t)rt_gui_clamp_i64_to_i32(mode, 0, 3));
    }
}

/// @brief Select nearest or bilinear image resizing.
void rt_image_set_filter(void *image, int64_t filter) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (img) {
        vg_image_set_filter(img,
                            filter == RT_IMAGE_FILTER_BILINEAR ? VG_IMAGE_FILTER_BILINEAR
                                                               : VG_IMAGE_FILTER_NEAREST);
    }
}

/// @brief Return the current image resize filter, defaulting to nearest for invalid handles.
int64_t rt_image_get_filter(void *image) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    return img ? (int64_t)vg_image_get_filter(img) : RT_IMAGE_FILTER_NEAREST;
}

/// @brief Set the opacity of the image.
void rt_image_set_opacity(void *image, double opacity) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (img) {
        double sanitized = rt_gui_double_is_finite(opacity) ? opacity : 1.0;
        vg_image_set_opacity(img, (float)rt_gui_clamp_f64(sanitized, 0.0, 1.0));
    }
}

/// @brief Opt the image into keyboard focus (click-to-focus plus a focus ring).
/// @details Interactive canvases (scene viewports) enable this so shortcut
///          ownership follows the clicked surface; presentation images keep
///          the default. Pair with the base widget's Focus/IsFocused.
void rt_image_set_focusable(void *image, int8_t focusable) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (img) {
        vg_image_set_focusable(img, focusable != 0);
    }
}

/// @brief Load an image file (PNG, BMP, JPEG, or GIF) into the image widget.
/// @details Auto-detects format from file magic bytes, decodes using rt_pixels,
///          converts from packed 0xRRGGBBAA to byte RGBA, and sets the widget pixels.
/// @param image Image widget.
/// @param path File path (runtime string).
/// @return 1 on success, 0 on failure.
int64_t rt_image_load_file(void *image, rt_string path) {
    RT_ASSERT_MAIN_THREAD();
    vg_image_t *img = rt_image_checked(image);
    if (!img || !path)
        return 0;

    // Try PNG first, then BMP, then JPEG, then GIF
    void *pixels = rt_pixels_load_png(path);
    if (!pixels)
        pixels = rt_pixels_load_bmp(path);
    if (!pixels)
        pixels = rt_pixels_load_jpeg(path);
    if (!pixels)
        pixels = rt_pixels_load_gif(path);
    if (!pixels)
        return 0;

    int ok = rt_image_set_from_pixels_object(img, pixels, 0, 0);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    return ok ? 1 : 0;
}

//=============================================================================
// FloatingPanel Widget
//=============================================================================

/// @brief Create a free-floating panel — a draggable container that floats above its parent.
///
/// Used for tool palettes, inspectors, and side panels that the
/// user can reposition. `root` is the top-level app handle that
/// owns the panel's draw layer.
void *rt_floatingpanel_new(void *root) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(root);
    if (root && !parent_widget)
        return NULL;
    vg_floatingpanel_t *panel = vg_floatingpanel_create(parent_widget);
    if (panel)
        rt_gui_apply_default_font(&panel->base);
    return panel;
}

/// @brief Validate a handle as a live FloatingPanel (NULL if not).
static vg_floatingpanel_t *rt_floatingpanel_checked(void *panel) {
    vg_floatingpanel_t *fp = (vg_floatingpanel_t *)panel;
    return vg_floatingpanel_is_live(fp) ? fp : NULL;
}

/// @brief Destroy a floating panel and its overlay children.
void rt_floatingpanel_destroy(void *panel) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    if (fp)
        rt_widget_destroy(&fp->base);
}

/// @brief Set the floating panel position from public logical coordinates.
void rt_floatingpanel_set_position(void *panel, double x, double y) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    if (fp)
        vg_floatingpanel_set_position(fp,
                                      rt_gui_logical_coordinate_to_physical(&fp->base, x),
                                      rt_gui_logical_coordinate_to_physical(&fp->base, y));
}

/// @brief Center a floating panel within its parent (root) bounds.
void rt_floatingpanel_center_in_parent(void *panel) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    if (fp)
        vg_floatingpanel_center_in_parent(fp);
}

/// @brief Set floating panel dimensions from public logical lengths.
void rt_floatingpanel_set_size(void *panel, double w, double h) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    if (fp)
        vg_floatingpanel_set_size(fp,
                                  rt_gui_logical_length_to_physical(&fp->base, w),
                                  rt_gui_logical_length_to_physical(&fp->base, h));
}

/// @brief Show or hide a floating panel overlay.
void rt_floatingpanel_set_visible(void *panel, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    if (fp)
        vg_floatingpanel_set_visible(fp, visible != 0);
}

/// @brief Add a child widget to a floating panel's content area.
void rt_floatingpanel_add_child(void *panel, void *child) {
    RT_ASSERT_MAIN_THREAD();
    vg_floatingpanel_t *fp = rt_floatingpanel_checked(panel);
    vg_widget_t *child_widget = rt_gui_widget_handle_checked(child);
    if (fp && child_widget) {
        rt_gui_app_t *old_app = rt_gui_app_from_widget(child_widget);
        rt_gui_app_t *new_app = rt_gui_app_from_widget(&fp->base);
        if (old_app && old_app != new_app)
            rt_widget_forget_runtime_refs(old_app, child_widget);
        vg_floatingpanel_add_child(fp, child_widget);
        if (new_app && old_app != new_app)
            rt_gui_apply_default_font(child_widget);
    }
}

/// @brief Create a titled "card" group box attached to @p parent.
void *rt_groupbox_new(void *parent, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_groupbox_t *gb = vg_groupbox_create(parent_widget, ctitle);
    free(ctitle);
    if (gb)
        rt_gui_apply_default_font(&gb->base);
    return gb;
}

/// @brief Destroy a group box and its children.
void rt_groupbox_destroy(void *gb) {
    RT_ASSERT_MAIN_THREAD();
    vg_groupbox_t *g = (vg_groupbox_t *)rt_gui_widget_handle_checked_type(gb, VG_WIDGET_GROUPBOX);
    if (g)
        rt_widget_destroy(&g->base);
}

/// @brief Replace the group box title text.
void rt_groupbox_set_title(void *gb, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_groupbox_t *g = (vg_groupbox_t *)rt_gui_widget_handle_checked_type(gb, VG_WIDGET_GROUPBOX);
    if (!g)
        return;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_groupbox_set_title(g, ctitle);
    free(ctitle);
}

/// @brief Add a control as a child of the group box.
void rt_groupbox_add_child(void *gb, void *child) {
    RT_ASSERT_MAIN_THREAD();
    vg_groupbox_t *g = (vg_groupbox_t *)rt_gui_widget_handle_checked_type(gb, VG_WIDGET_GROUPBOX);
    vg_widget_t *child_widget = rt_gui_widget_handle_checked(child);
    if (g && child_widget) {
        rt_gui_app_t *old_app = rt_gui_app_from_widget(child_widget);
        rt_gui_app_t *new_app = rt_gui_app_from_widget(&g->base);
        if (old_app && old_app != new_app)
            rt_widget_forget_runtime_refs(old_app, child_widget);
        vg_groupbox_add_child(g, child_widget);
        if (new_app && old_app != new_app)
            rt_gui_apply_default_font(child_widget);
    }
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics disabled — no internal RGBA upload can succeed.
int rt_gui_image_try_set_rgba_bytes(void *image,
                                    const uint8_t *rgba,
                                    int64_t width,
                                    int64_t height) {
    (void)image;
    (void)rgba;
    (void)width;
    (void)height;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no image widget is created.
void *rt_image_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Set the pixels of the image.
void rt_image_set_pixels(void *image, void *pixels, int64_t width, int64_t height) {
    (void)image;
    (void)pixels;
    (void)width;
    (void)height;
}

/// @brief Stub: graphics disabled — no image upload can succeed.
int64_t rt_image_try_set_pixels(void *image, void *pixels, int64_t width, int64_t height) {
    (void)image;
    (void)pixels;
    (void)width;
    (void)height;
    return 0;
}

/// @brief Stub: graphics disabled — no image region can be updated.
int64_t rt_image_update_region(void *image,
                               void *pixels,
                               int64_t source_x,
                               int64_t source_y,
                               int64_t width,
                               int64_t height,
                               int64_t dest_x,
                               int64_t dest_y) {
    (void)image;
    (void)pixels;
    (void)source_x;
    (void)source_y;
    (void)width;
    (void)height;
    (void)dest_x;
    (void)dest_y;
    return 0;
}

/// @brief Clear the image widget's pixel data, showing nothing.
void rt_image_clear(void *image) {
    (void)image;
}

/// @brief Set the scale mode of the image.
void rt_image_set_scale_mode(void *image, int64_t mode) {
    (void)image;
    (void)mode;
}

/// @brief Stub: graphics disabled — image filtering has no target.
void rt_image_set_filter(void *image, int64_t filter) {
    (void)image;
    (void)filter;
}

/// @brief Stub: graphics disabled — nearest is the stable default filter.
int64_t rt_image_get_filter(void *image) {
    (void)image;
    return RT_IMAGE_FILTER_NEAREST;
}

/// @brief Set the opacity of the image.
void rt_image_set_opacity(void *image, double opacity) {
    (void)image;
    (void)opacity;
}

/// @brief Stub: graphics disabled — no image can take focus.
void rt_image_set_focusable(void *image, int8_t focusable) {
    (void)image;
    (void)focusable;
}

/// @brief Load image file stub (graphics disabled).
int64_t rt_image_load_file(void *image, rt_string path) {
    (void)image;
    (void)path;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no floating panel is created.
void *rt_floatingpanel_new(void *root) {
    (void)root;
    return NULL;
}

/// @brief Destroy floating panel stub (graphics disabled).
void rt_floatingpanel_destroy(void *panel) {
    (void)panel;
}

/// @brief Set the position of the floatingpanel.
void rt_floatingpanel_set_position(void *panel, double x, double y) {
    (void)panel;
    (void)x;
    (void)y;
}

/// @brief Stub: graphics disabled — no floating panel to center.
void rt_floatingpanel_center_in_parent(void *panel) {
    (void)panel;
}

/// @brief Set the width and height of a floating panel.
void rt_floatingpanel_set_size(void *panel, double w, double h) {
    (void)panel;
    (void)w;
    (void)h;
}

/// @brief Show or hide a floating panel overlay.
void rt_floatingpanel_set_visible(void *panel, int64_t visible) {
    (void)panel;
    (void)visible;
}

/// @brief Add a child widget to a floating panel's content area.
void rt_floatingpanel_add_child(void *panel, void *child) {
    (void)panel;
    (void)child;
}

/// @brief Stub: graphics disabled — no group box is created.
void *rt_groupbox_new(void *parent, rt_string title) {
    (void)parent;
    (void)title;
    return NULL;
}

/// @brief Destroy group box stub (graphics disabled).
void rt_groupbox_destroy(void *gb) {
    (void)gb;
}

/// @brief Set group box title stub (graphics disabled).
void rt_groupbox_set_title(void *gb, rt_string title) {
    (void)gb;
    (void)title;
}

/// @brief Add child to group box stub (graphics disabled).
void rt_groupbox_add_child(void *gb, void *child) {
    (void)gb;
    (void)child;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
