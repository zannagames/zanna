//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements horizontal and vertical toolbars with overflow menus.
/// @details The toolbar owns its lightweight item records, cloned popup menus,
///          and icon resources while treating embedded custom widgets as
///          borrowed children. Layout is expressed along a primary axis,
///          flexible spacers divide unused extent, and trailing items are
///          represented by a lazily rebuilt overflow popup when space is
///          constrained. Removed items become inert retired records so managed
///          wrappers can safely detect and later reclaim stale handles.
///
///          Popup coordinates and visual bounds are converted to screen space
///          because the transient context menus are owned by, but are not
///          ordinary widget-tree children of, the toolbar.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_toolbar.c
// Purpose: Horizontal or vertical toolbar widget with buttons, toggles,
//          dropdown menus, separators, spacers, and embedded custom widgets.
//          Items that overflow the available space collapse into an overflow
//          context menu automatically.
// Key invariants:
//   - items[] is a flat pointer array grown by doubling; it is never shrunk.
//   - overflow_start_index == -1 when all items fit; recomputed every arrange pass.
//   - overflow_popup and dropdown_popup are transient context menus owned by the
//     toolbar and destroyed on dismiss or inside toolbar_destroy.
//   - overflow_popup_dirty is set whenever items change; the popup rebuilds lazily
//     only when it is about to be shown.
//   - focused_index may equal overflow_start_index to indicate the overflow button.
//   - toolbar_sync_popup_capture releases input capture when both popups are dismissed.
// Ownership/Lifetime:
//   - The toolbar is a standard widget; items are owned by the toolbar and freed
//     in toolbar_destroy. custom_widget items are NOT freed by the toolbar.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Constants
//=============================================================================

#define INITIAL_ITEM_CAPACITY 16
#define TOOLBAR_DEFAULT_PADDING 4
#define TOOLBAR_DEFAULT_SPACING 2
#define ICON_SIZE_SMALL 16
#define ICON_SIZE_MEDIUM 24
#define ICON_SIZE_LARGE 32
#define VG_TOOLBAR_ITEM_MAGIC UINT64_C(0x565447544F4F4C49)
#define VG_TOOLBAR_ITEM_RETIRED_MAGIC UINT64_C(0x565447544F4C5254)

//=============================================================================
// Forward Declarations
//=============================================================================

static void toolbar_destroy(vg_widget_t *widget);
static void toolbar_measure(vg_widget_t *widget, float available_width, float available_height);
static void toolbar_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void toolbar_paint(vg_widget_t *widget, void *canvas);
static void toolbar_paint_overlay(vg_widget_t *widget, void *canvas);
static void toolbar_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height);
static bool toolbar_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool toolbar_can_focus(vg_widget_t *widget);
static void toolbar_on_focus(vg_widget_t *widget, bool gained);
static void toolbar_sync_hover_tooltip(vg_toolbar_t *tb);
static float get_item_width(vg_toolbar_t *tb, vg_toolbar_item_t *item);
static float get_item_height(vg_toolbar_t *tb, vg_toolbar_item_t *item);

/// @brief Resolve a toolbar's absolute screen-space origin.
/// @details Toolbars may be nested inside arbitrary layout containers, so their
///          retained x/y values cannot be used directly to anchor detached popups.
/// @param toolbar Toolbar whose origin is requested; may be NULL.
/// @param x Receives screen X, or zero for NULL.
/// @param y Receives screen Y, or zero for NULL.
static void toolbar_get_screen_origin(const vg_toolbar_t *toolbar, float *x, float *y) {
    if (x)
        *x = 0.0f;
    if (y)
        *y = 0.0f;
    if (!toolbar)
        return;
    vg_widget_get_screen_bounds(&toolbar->base, x, y, NULL, NULL);
}

/// @brief Clamp a detached toolbar popup to the root widget's screen rectangle.
/// @details Context-menu painting also clamps to the native window, but doing the
///          deterministic root clamp before damage collection keeps the reported
///          visual rectangle identical to the rectangle that will be painted.
/// @param toolbar Popup owner used to locate the root viewport.
/// @param popup Measured popup whose absolute position is updated; may be NULL.
static void toolbar_clamp_popup_to_root(vg_toolbar_t *toolbar, vg_contextmenu_t *popup) {
    if (!toolbar || !popup || !vg_widget_is_live(&popup->base))
        return;
    vg_widget_t *root = &toolbar->base;
    while (root->parent)
        root = root->parent;
    float root_x = 0.0f, root_y = 0.0f, root_w = 0.0f, root_h = 0.0f;
    vg_widget_get_screen_bounds(root, &root_x, &root_y, &root_w, &root_h);
    if (!isfinite(root_x) || !isfinite(root_y) || !isfinite(root_w) || !isfinite(root_h) ||
        root_w <= 0.0f || root_h <= 0.0f) {
        return;
    }
    float max_x = root_x + root_w - popup->base.width;
    float max_y = root_y + root_h - popup->base.height;
    if (max_x < root_x)
        max_x = root_x;
    if (max_y < root_y)
        max_y = root_y;
    if (popup->base.x < root_x)
        popup->base.x = root_x;
    if (popup->base.y < root_y)
        popup->base.y = root_y;
    if (popup->base.x > max_x)
        popup->base.x = max_x;
    if (popup->base.y > max_y)
        popup->base.y = max_y;
    popup->anchor_x = (int)popup->base.x;
    popup->anchor_y = (int)popup->base.y;
}

//=============================================================================
// Toolbar VTable
//=============================================================================

static vg_widget_vtable_t g_toolbar_vtable = {.destroy = toolbar_destroy,
                                              .measure = toolbar_measure,
                                              .arrange = toolbar_arrange,
                                              .paint = toolbar_paint,
                                              .paint_overlay = toolbar_paint_overlay,
                                              .handle_event = toolbar_handle_event,
                                              .can_focus = toolbar_can_focus,
                                              .on_focus = toolbar_on_focus,
                                              .get_visual_bounds = toolbar_get_visual_bounds};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Ensure the toolbar item array can hold a requested number of entries.
/// @details Capacity grows geometrically from `INITIAL_ITEM_CAPACITY`; overflow
///          checks are performed before either doubling or multiplying by the
///          pointer size. Existing entries and capacity remain unchanged when
///          allocation fails.
/// @param tb Toolbar whose owned item-pointer array is resized.
/// @param needed Minimum number of pointer slots required.
/// @return `true` when at least @p needed slots are available, otherwise
///         `false`.
static bool ensure_item_capacity(vg_toolbar_t *tb, size_t needed) {
    if (needed <= tb->item_capacity)
        return true;

    size_t new_capacity = tb->item_capacity == 0 ? INITIAL_ITEM_CAPACITY : tb->item_capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2)
            return false;
        new_capacity *= 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(vg_toolbar_item_t *))
        return false;

    vg_toolbar_item_t **new_items = realloc(tb->items, new_capacity * sizeof(vg_toolbar_item_t *));
    if (!new_items)
        return false;

    tb->items = new_items;
    tb->item_capacity = new_capacity;
    return true;
}

/// @brief Clamp an integer to an inclusive range.
/// @param value Value to constrain.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return @p value constrained to [`min_value`, `max_value`].
static int toolbar_clampi(int value, int min_value, int max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Alpha-composite one RGBA source pixel over a destination pixel.
/// @details The supplied coverage alpha controls source contribution and is
///          also folded into the destination alpha. Both buffers use four
///          consecutive eight-bit RGBA channels.
/// @param dst Mutable destination pixel.
/// @param src Source pixel; only read by this function.
/// @param alpha Effective source alpha in the inclusive range 0–255.
static void toolbar_blend_pixel(uint8_t *dst, const uint8_t *src, uint8_t alpha) {
    if (alpha == 0)
        return;

    if (alpha == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }

    const uint32_t inv = (uint32_t)(255 - alpha);
    dst[0] = (uint8_t)(((uint32_t)src[0] * alpha + (uint32_t)dst[0] * inv + 127u) / 255u);
    dst[1] = (uint8_t)(((uint32_t)src[1] * alpha + (uint32_t)dst[1] * inv + 127u) / 255u);
    dst[2] = (uint8_t)(((uint32_t)src[2] * alpha + (uint32_t)dst[2] * inv + 127u) / 255u);
    dst[3] = (uint8_t)(alpha + (((uint32_t)dst[3] * inv + 127u) / 255u));
}

/// @brief Draw a raster toolbar icon centered within a destination rectangle.
/// @details The image is aspect-fit with nearest-neighbor sampling, clipped to
///          both the framebuffer and active graphics clip, and blended as
///          straight-alpha RGBA. Disabled icons retain their colors but use
///          reduced opacity.
/// @param win Destination graphics window.
/// @param icon Borrowed image icon descriptor.
/// @param x Destination rectangle's left edge in framebuffer coordinates.
/// @param y Destination rectangle's top edge in framebuffer coordinates.
/// @param w Available destination width.
/// @param h Available destination height.
/// @param enabled Whether to render at full opacity.
static void toolbar_draw_image_icon(
    vgfx_window_t win, const vg_icon_t *icon, float x, float y, float w, float h, bool enabled) {
    if (!icon || icon->type != VG_ICON_IMAGE || !icon->data.image.pixels || w <= 0.0f || h <= 0.0f)
        return;

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(win, &fb))
        return;

    const int sw = (int)icon->data.image.width;
    const int sh = (int)icon->data.image.height;
    if (sw <= 0 || sh <= 0)
        return;

    float scale = w / (float)sw;
    if ((float)sh * scale > h)
        scale = h / (float)sh;
    if (scale <= 0.0f)
        return;

    float draw_w = (float)sw * scale;
    float draw_h = (float)sh * scale;
    float draw_x = x + (w - draw_w) * 0.5f;
    float draw_y = y + (h - draw_h) * 0.5f;

    int start_x = toolbar_clampi((int)draw_x, 0, fb.width);
    int start_y = toolbar_clampi((int)draw_y, 0, fb.height);
    int end_x = toolbar_clampi((int)(draw_x + draw_w), 0, fb.width);
    int end_y = toolbar_clampi((int)(draw_y + draw_h), 0, fb.height);
    uint8_t opacity = enabled ? 255u : 144u;
    int clip_x = 0;
    int clip_y = 0;
    int clip_w = fb.width;
    int clip_h = fb.height;

    (void)vgfx_get_clip(win, &clip_x, &clip_y, &clip_w, &clip_h);
    if (start_x < clip_x)
        start_x = clip_x;
    if (start_y < clip_y)
        start_y = clip_y;
    if (end_x > clip_x + clip_w)
        end_x = clip_x + clip_w;
    if (end_y > clip_y + clip_h)
        end_y = clip_y + clip_h;

    for (int fb_y = start_y; fb_y < end_y; fb_y++) {
        for (int fb_x = start_x; fb_x < end_x; fb_x++) {
            float u = draw_w > 0.0f ? ((float)fb_x - draw_x) / draw_w : 0.0f;
            float v = draw_h > 0.0f ? ((float)fb_y - draw_y) / draw_h : 0.0f;
            int sx = toolbar_clampi((int)(u * (float)sw), 0, sw - 1);
            int sy = toolbar_clampi((int)(v * (float)sh), 0, sh - 1);
            const uint8_t *src = &icon->data.image.pixels[(sy * sw + sx) * 4];
            uint8_t alpha = src[3];
            if (!enabled)
                alpha = (uint8_t)(((uint32_t)alpha * opacity + 127u) / 255u);
            if (alpha == 0)
                continue;

            uint8_t *dst = &fb.pixels[fb_y * fb.stride + fb_x * 4];
            toolbar_blend_pixel(dst, src, alpha);
        }
    }
}

/// @brief Determine whether an item participates in keyboard focus traversal.
/// @param item Item to inspect; may be `NULL`.
/// @return `true` for an enabled button, toggle, or dropdown; `false` for
///         separators, spacers, custom widgets, disabled items, and `NULL`.
static bool toolbar_item_can_focus(const vg_toolbar_item_t *item) {
    if (!item || !item->enabled || !item->visible)
        return false;
    return item->type == VG_TOOLBAR_ITEM_BUTTON || item->type == VG_TOOLBAR_ITEM_TOGGLE ||
           item->type == VG_TOOLBAR_ITEM_DROPDOWN;
}

/// @brief Find an exact item pointer in a toolbar's live item array.
/// @param tb Toolbar to search; may be `NULL`.
/// @param item Pointer identity to match; may be `NULL`.
/// @return Zero-based live item index, or `-1` when the pointer is absent.
static int toolbar_index_of_item(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item)
        return -1;
    for (size_t i = 0; i < tb->item_count; i++) {
        if (tb->items[i] == item)
            return (int)i;
    }
    return -1;
}

/// @brief Destroy an item record and all resources owned directly by it.
/// @details Strings and icon storage are released before the record itself.
///          A custom widget and dropdown menu referenced by the item are
///          borrowed and are therefore not destroyed here.
/// @param item Item to destroy; `NULL` is ignored.
static void free_item(vg_toolbar_item_t *item) {
    if (!item)
        return;
    if (item->id)
        free(item->id);
    if (item->label)
        free(item->label);
    if (item->tooltip)
        free(item->tooltip);
    vg_icon_destroy(&item->icon);
    free(item);
}

/// @brief Test whether a toolbar item handle still denotes a live item.
/// @details A live item carries the active magic value and retains a non-null
///          owner. Retired tombstones deliberately fail this check, allowing
///          foreign or managed wrappers to reject stale handles without
///          invoking item operations.
/// @param item Item handle to inspect; may be `NULL`.
/// @return `true` only while @p item belongs to a live toolbar item array.
bool vg_toolbar_item_is_live(const vg_toolbar_item_t *item) {
    return item && item->magic == VG_TOOLBAR_ITEM_MAGIC && item->owner != NULL;
}

/// @brief Tear down @p item's owned strings/icon and park it on @p tb's
///        retired list for deferred freeing (use-after-free defense when an
///        item is removed during event dispatch).
/// @details Callback, menu, widget, and owner references are cleared before the
///          record receives its retired magic value. The resulting tombstone
///          remains allocated until toolbar destruction or explicit reclamation.
/// @param tb Toolbar that owns the retired-item list.
/// @param item Live item being converted into a tombstone.
static void retire_item(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item)
        return;
    free(item->id);
    item->id = NULL;
    free(item->label);
    item->label = NULL;
    free(item->tooltip);
    item->tooltip = NULL;
    vg_icon_destroy(&item->icon);
    item->owner = NULL;
    item->dropdown_menu = NULL;
    item->custom_widget = NULL;
    item->on_click = NULL;
    item->on_toggle = NULL;
    item->user_data = NULL;
    item->was_clicked = false;
    item->magic = VG_TOOLBAR_ITEM_RETIRED_MAGIC;
    item->retired_next = tb->retired_items;
    tb->retired_items = item;
}

/// @brief Drain and free every item on a toolbar's retired list.
/// @param tb Toolbar whose tombstones are reclaimed; `NULL` is ignored.
static void free_retired_items(vg_toolbar_t *tb) {
    if (!tb)
        return;
    vg_toolbar_item_t *item = tb->retired_items;
    while (item) {
        vg_toolbar_item_t *next = item->retired_next;
        item->retired_next = NULL;
        free_item(item);
        item = next;
    }
    tb->retired_items = NULL;
}

/// @brief Promote the hovered item's tooltip onto the toolbar widget.
/// @details Toolbar items are lightweight records rather than standalone
///          widgets, while the shared tooltip manager only reads
///          vg_widget_t::tooltip_text. This helper mirrors the TabBar hover
///          tooltip pattern: it saves the toolbar's original widget tooltip,
///          replaces it with the hovered item tooltip, and restores the
///          original text when hover leaves the item.
/// @param tb Toolbar whose hovered item changed; NULL is ignored.
static void toolbar_sync_hover_tooltip(vg_toolbar_t *tb) {
    if (!tb)
        return;

    const char *hover_tooltip =
        (tb->hovered_item && tb->hovered_item->tooltip) ? tb->hovered_item->tooltip : NULL;
    if (hover_tooltip && hover_tooltip[0] == '\0')
        hover_tooltip = NULL;

    if (hover_tooltip) {
        if (!tb->hover_tooltip_active) {
            char *saved_tooltip = tb->base.tooltip_text ? vg_strdup(tb->base.tooltip_text) : NULL;
            if (tb->base.tooltip_text && !saved_tooltip)
                return;
            free(tb->saved_tooltip_text);
            tb->saved_tooltip_text = saved_tooltip;
            tb->hover_tooltip_active = true;
        }
        if (!tb->base.tooltip_text || strcmp(tb->base.tooltip_text, hover_tooltip) != 0)
            vg_widget_set_tooltip_text(&tb->base, hover_tooltip);
        return;
    }

    if (tb->hover_tooltip_active) {
        vg_widget_set_tooltip_text(&tb->base, tb->saved_tooltip_text);
        free(tb->saved_tooltip_text);
        tb->saved_tooltip_text = NULL;
        tb->hover_tooltip_active = false;
    }
}

/// @brief Allocate a detached toolbar item of the requested type.
/// @details The identifier is duplicated, default interaction fields are
///          initialized, and the item receives the live magic value. Ownership
///          is not established until the caller assigns an owner and appends
///          the record to a toolbar.
/// @param type Kind of toolbar item to create.
/// @param id Optional identifier to duplicate.
/// @return Newly allocated item, or `NULL` on allocation failure.
static vg_toolbar_item_t *create_item(vg_toolbar_item_type_t type, const char *id) {
    vg_toolbar_item_t *item = calloc(1, sizeof(vg_toolbar_item_t));
    if (!item)
        return NULL;

    item->type = type;
    item->magic = VG_TOOLBAR_ITEM_MAGIC;
    item->owner = NULL;
    item->id = id ? vg_strdup(id) : NULL;
    if (id && !item->id) {
        free(item);
        return NULL;
    }
    item->label = NULL;
    item->tooltip = NULL;
    item->icon.type = VG_ICON_NONE;
    item->enabled = true;
    item->checked = false;
    item->show_label = false;
    item->visible = true;
    item->dropdown_menu = NULL;
    item->custom_widget = NULL;
    item->user_data = NULL;
    item->on_click = NULL;
    item->on_toggle = NULL;

    return item;
}

/// @brief Map a toolbar icon-size setting to its unscaled pixel dimension.
/// @param size Configured toolbar icon size.
/// @return Base dimension in pixels; unknown values use the medium size.
static uint32_t get_icon_pixels(vg_toolbar_icon_size_t size) {
    switch (size) {
        case VG_TOOLBAR_ICONS_SMALL:
            return ICON_SIZE_SMALL;
        case VG_TOOLBAR_ICONS_MEDIUM:
            return ICON_SIZE_MEDIUM;
        case VG_TOOLBAR_ICONS_LARGE:
            return ICON_SIZE_LARGE;
        default:
            return ICON_SIZE_MEDIUM;
    }
}

/// @brief Obtain a positive UI scale for toolbar geometry.
/// @return Current theme scale when positive, otherwise `1.0`.
static float toolbar_ui_scale(void) {
    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme ? theme->ui_scale : 1.0f;
    return scale > 0.0f ? scale : 1.0f;
}

/// @brief Resolve a toolbar's configured icon size in device-scaled pixels.
/// @param tb Toolbar supplying the icon-size setting.
/// @return Scaled square icon dimension.
static float get_scaled_icon_pixels(vg_toolbar_t *tb) {
    return (float)get_icon_pixels(tb->icon_size) * toolbar_ui_scale();
}

/// @brief Compute the overflow button's primary-axis extent.
/// @details The extent accommodates the current icon size and item padding and
///          is never smaller than 18 scaled-layout units.
/// @param tb Toolbar supplying icon and padding configuration.
/// @return Overflow button width for a horizontal toolbar or height for a
///         vertical toolbar.
static float get_overflow_button_extent(vg_toolbar_t *tb) {
    float icon_px = get_scaled_icon_pixels(tb);
    float extent = icon_px + (float)tb->item_padding * 2.0f;
    return extent > 18.0f ? extent : 18.0f;
}

/// @brief Compute an item's intrinsic primary-axis extent.
/// @param tb Toolbar establishing orientation and presentation settings.
/// @param item Item whose extent is requested.
/// @return Intrinsic width for a horizontal toolbar or height for a vertical
///         toolbar.
static float get_item_extent(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (item && !item->visible)
        return 0.0f;
    return tb->orientation == VG_TOOLBAR_HORIZONTAL ? get_item_width(tb, item)
                                                    : get_item_height(tb, item);
}

/// @brief Obtain the exclusive upper index of directly visible items.
/// @param tb Toolbar whose overflow split is queried.
/// @return `item_count` when no overflow exists, otherwise the first overflowed
///         item index.
static int toolbar_visible_limit(vg_toolbar_t *tb) {
    return tb->overflow_start_index >= 0 ? tb->overflow_start_index : (int)tb->item_count;
}

/// @brief Test whether a focus index represents the synthetic overflow button.
/// @param tb Toolbar supplying overflow state; may be `NULL`.
/// @param index Focus index to classify.
/// @return `true` when @p index equals the active overflow split.
static bool toolbar_focus_is_overflow(const vg_toolbar_t *tb, int index) {
    return tb && tb->overflow_start_index >= 0 && index == tb->overflow_start_index;
}

/// @brief Validate a toolbar focus index against current layout and item state.
/// @param tb Toolbar whose focus domain is inspected; may be `NULL`.
/// @param index Candidate index.
/// @return `true` for the overflow button or an enabled, focusable, directly
///         visible item.
static bool toolbar_focus_index_valid(vg_toolbar_t *tb, int index) {
    if (!tb || index < 0)
        return false;
    if (toolbar_focus_is_overflow(tb, index))
        return true;
    int limit = toolbar_visible_limit(tb);
    return index < limit && toolbar_item_can_focus(tb->items[index]);
}

/// @brief Locate the first keyboard focus target.
/// @param tb Toolbar to search; may be `NULL`.
/// @return First focusable visible item index, the overflow button index when
///         it is the only target, or `-1` when no target exists.
static int toolbar_first_focus_index(vg_toolbar_t *tb) {
    if (!tb)
        return -1;
    int limit = toolbar_visible_limit(tb);
    for (int i = 0; i < limit; i++) {
        if (toolbar_item_can_focus(tb->items[i]))
            return i;
    }
    return tb->overflow_start_index >= 0 ? tb->overflow_start_index : -1;
}

/// @brief Locate the last keyboard focus target.
/// @param tb Toolbar to search; may be `NULL`.
/// @return Overflow button index when present, otherwise the last focusable
///         visible item index, or `-1`.
static int toolbar_last_focus_index(vg_toolbar_t *tb) {
    if (!tb)
        return -1;
    if (tb->overflow_start_index >= 0)
        return tb->overflow_start_index;
    int limit = toolbar_visible_limit(tb);
    for (int i = limit - 1; i >= 0; i--) {
        if (toolbar_item_can_focus(tb->items[i]))
            return i;
    }
    return -1;
}

/// @brief Advance keyboard focus through the directly visible toolbar targets.
/// @details Traversal does not wrap. A zero direction validates the current
///          index and falls back to the first target when it is stale.
/// @param tb Toolbar whose targets are traversed; may be `NULL`.
/// @param current Current focus index, or a negative value for no focus.
/// @param direction Positive to move forward, negative to move backward, or
///        zero to normalize only.
/// @return Selected focus index, `current` at the respective boundary, or
///         `-1` when the toolbar has no targets.
static int toolbar_next_focus_index(vg_toolbar_t *tb, int current, int direction) {
    if (!tb)
        return -1;
    if (direction == 0)
        return toolbar_focus_index_valid(tb, current) ? current : toolbar_first_focus_index(tb);

    int limit = toolbar_visible_limit(tb);
    if (direction > 0) {
        if (current < 0)
            return toolbar_first_focus_index(tb);
        if (!toolbar_focus_is_overflow(tb, current)) {
            for (int i = current + 1; i < limit; i++) {
                if (toolbar_item_can_focus(tb->items[i]))
                    return i;
            }
            if (tb->overflow_start_index >= 0)
                return tb->overflow_start_index;
        }
        return current;
    }

    if (current < 0)
        return toolbar_last_focus_index(tb);
    if (toolbar_focus_is_overflow(tb, current)) {
        for (int i = limit - 1; i >= 0; i--) {
            if (toolbar_item_can_focus(tb->items[i]))
                return i;
        }
        return current;
    }
    for (int i = current - 1; i >= 0; i--) {
        if (toolbar_item_can_focus(tb->items[i]))
            return i;
    }
    return current;
}

/// @brief Normalize a toolbar's stored focus index after state or layout changes.
/// @param tb Toolbar whose `focused_index` is updated; `NULL` is ignored.
static void toolbar_normalize_focus_index(vg_toolbar_t *tb) {
    if (!tb)
        return;
    if (!toolbar_focus_index_valid(tb, tb->focused_index))
        tb->focused_index = toolbar_first_focus_index(tb);
}

/// @brief Read the toolbar's arranged primary-axis dimension.
/// @param tb Arranged toolbar.
/// @return Width for a horizontal toolbar or height for a vertical toolbar.
static float toolbar_primary_available(vg_toolbar_t *tb) {
    return tb->orientation == VG_TOOLBAR_HORIZONTAL ? tb->base.width : tb->base.height;
}

/// @brief Divide unused primary-axis space among visible flexible spacers.
/// @details Intrinsic item extents, inter-item spacing, and any overflow button
///          are deducted from the arranged primary dimension. Negative
///          remainder is clamped to zero.
/// @param tb Toolbar supplying items and geometry.
/// @param max_index Exclusive visible-item limit.
/// @return Primary-axis extent assigned to each spacer, or zero when none are
///         visible.
static float toolbar_spacer_extent(vg_toolbar_t *tb, int max_index) {
    int spacer_count = 0;
    int visible_items = 0;
    float used_extent = 0.0f;

    for (int i = 0; i < max_index; i++) {
        vg_toolbar_item_t *item = tb->items[i];
        if (!item)
            continue;
        if (item->type == VG_TOOLBAR_ITEM_SPACER) {
            spacer_count++;
            visible_items++;
            continue;
        }
        float extent = get_item_extent(tb, item);
        if (extent <= 0.0f)
            continue;
        used_extent += extent;
        visible_items++;
    }

    if (spacer_count == 0)
        return 0.0f;

    float spacing_total =
        visible_items > 1 ? (float)(visible_items - 1) * (float)tb->item_spacing : 0.0f;
    float available = toolbar_primary_available(tb);
    if (tb->overflow_start_index >= 0)
        available -= get_overflow_button_extent(tb);
    float extra = available - used_extent - spacing_total;
    if (extra < 0.0f)
        extra = 0.0f;
    return extra / (float)spacer_count;
}

/// @brief Resolve an item's primary-axis extent for an arranged layout pass.
/// @param tb Toolbar supplying orientation and item metrics.
/// @param item Item being laid out; may be `NULL`.
/// @param max_index Exclusive visible-item limit retained for layout-call
///        compatibility.
/// @param spacer_extent Precomputed extent shared by flexible spacers.
/// @return Zero for `NULL`, @p spacer_extent for a spacer, or the item's
///         intrinsic primary-axis extent.
static float toolbar_layout_extent(vg_toolbar_t *tb,
                                   vg_toolbar_item_t *item,
                                   int max_index,
                                   float spacer_extent) {
    (void)max_index;
    if (!item || !item->visible)
        return 0.0f;
    if (item->type == VG_TOOLBAR_ITEM_SPACER)
        return spacer_extent;
    return get_item_extent(tb, item);
}

/// @brief Locate a visible item's toolbar-local bounding rectangle.
/// @details The calculation mirrors arranged spacer distribution and stops at
///          the overflow split, so hidden overflow items are not reported.
/// @param tb Toolbar whose current layout is queried.
/// @param target Exact item pointer to locate.
/// @param out_x Optional destination for the local left edge.
/// @param out_y Optional destination for the local top edge.
/// @param out_w Optional destination for the item width.
/// @param out_h Optional destination for the item height.
/// @return `true` when @p target is directly visible and its rectangle was
///         found; otherwise `false`.
static bool toolbar_get_item_rect(vg_toolbar_t *tb,
                                  vg_toolbar_item_t *target,
                                  float *out_x,
                                  float *out_y,
                                  float *out_w,
                                  float *out_h) {
    int max_index = toolbar_visible_limit(tb);
    float spacer_extent = toolbar_spacer_extent(tb, max_index);
    float pos = 0.0f;

    for (int i = 0; i < max_index; i++) {
        vg_toolbar_item_t *item = tb->items[i];
        float item_width = 0.0f;
        float item_height = 0.0f;
        float item_x = 0.0f;
        float item_y = 0.0f;
        float extent = toolbar_layout_extent(tb, item, max_index, spacer_extent);
        if (extent <= 0.0f)
            continue;

        if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
            item_width = extent;
            item_height = tb->base.height - 4.0f;
            if (item_height < 0.0f)
                item_height = 0.0f;
            item_x = pos;
            item_y = 2.0f;
        } else {
            item_width = tb->base.width - 4.0f;
            if (item_width < 0.0f)
                item_width = 0.0f;
            item_height = extent;
            item_x = 2.0f;
            item_y = pos;
        }

        if (item == target) {
            if (out_x)
                *out_x = item_x;
            if (out_y)
                *out_y = item_y;
            if (out_w)
                *out_w = item_width;
            if (out_h)
                *out_h = item_height;
            return true;
        }

        pos += extent + (float)tb->item_spacing;
    }

    return false;
}

/// @brief Destroy a cloned context-menu hierarchy.
/// @details Submenus are recursively released before their parent because the
///          cloned toolbar popup owns the entire tree.
/// @param menu Root menu to destroy; `NULL` is ignored.
static void toolbar_destroy_contextmenu_tree(vg_contextmenu_t *menu) {
    if (!menu)
        return;

    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item && item->submenu)
            toolbar_destroy_contextmenu_tree((vg_contextmenu_t *)item->submenu);
    }
    vg_contextmenu_destroy(menu);
}

/// @brief Forward selection from a cloned dropdown entry to its source menu item.
/// @details The clone stores the original `vg_menu_item_t` in `action_data`.
///          Selection sets the source click latch, invokes its action when
///          present, and requests a toolbar repaint.
/// @param menu Cloned context menu reporting the selection; unused.
/// @param menu_item Selected cloned entry; may be `NULL`.
/// @param user_data Owning toolbar supplied when callbacks were attached.
static void toolbar_dropdown_menu_on_select(vg_contextmenu_t *menu,
                                            vg_menu_item_t *menu_item,
                                            void *user_data) {
    (void)menu;
    vg_toolbar_t *tb = (vg_toolbar_t *)user_data;
    vg_menu_item_t *source = menu_item ? (vg_menu_item_t *)menu_item->action_data : NULL;
    if (source) {
        source->was_clicked = true;
        if (source->action)
            source->action(source->action_data);
    }
    if (tb)
        tb->base.needs_paint = true;
}

/// @brief Attach toolbar selection forwarding throughout a cloned menu tree.
/// @param menu Menu or submenu whose callback is configured; `NULL` is ignored.
/// @param tb Toolbar passed back as callback user data.
static void toolbar_attach_dropdown_callbacks(vg_contextmenu_t *menu, vg_toolbar_t *tb) {
    if (!menu)
        return;
    vg_contextmenu_set_on_select(menu, toolbar_dropdown_menu_on_select, tb);
    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item && item->submenu)
            toolbar_attach_dropdown_callbacks((vg_contextmenu_t *)item->submenu, tb);
    }
}

/// @brief Clone a menu model into a toolbar-owned context-menu hierarchy.
/// @details Labels, shortcuts, enabled/checked state, and icons are mirrored.
///          Leaf clones retain a borrowed pointer to the original menu item so
///          selection can invoke the source action. Failed individual entries
///          are skipped while a failure to create the root returns `NULL`.
/// @param menu Source menu hierarchy; may be `NULL`.
/// @param tb Toolbar providing font configuration and callback context.
/// @return Newly allocated context-menu tree owned by the caller, or `NULL`.
static vg_contextmenu_t *toolbar_clone_menu_tree(vg_menu_t *menu, vg_toolbar_t *tb) {
    if (!menu)
        return NULL;

    vg_contextmenu_t *popup = vg_contextmenu_create();
    if (!popup)
        return NULL;

    vg_contextmenu_set_font(popup, tb->font, tb->font_size);
    for (vg_menu_item_t *item = menu->first_item; item; item = item->next) {
        if (item->separator) {
            vg_contextmenu_add_separator(popup);
            continue;
        }

        if (item->submenu) {
            vg_contextmenu_t *submenu = toolbar_clone_menu_tree(item->submenu, tb);
            if (!submenu)
                continue;
            vg_menu_item_t *clone = vg_contextmenu_add_submenu(popup, item->text, submenu);
            if (!clone) {
                toolbar_destroy_contextmenu_tree(submenu);
                continue;
            }
            vg_contextmenu_item_set_enabled(clone, item->enabled);
            vg_contextmenu_item_set_checked(clone, item->checked);
            if (item->icon.type != VG_ICON_NONE)
                vg_contextmenu_item_set_icon(clone, vg_icon_clone(&item->icon));
            continue;
        }

        vg_menu_item_t *clone =
            vg_contextmenu_add_item(popup, item->text, item->shortcut, NULL, item);
        if (!clone)
            continue;
        clone->action_data = item;
        vg_contextmenu_item_set_enabled(clone, item->enabled);
        vg_contextmenu_item_set_checked(clone, item->checked);
        if (item->icon.type != VG_ICON_NONE)
            vg_contextmenu_item_set_icon(clone, vg_icon_clone(&item->icon));
    }

    toolbar_attach_dropdown_callbacks(popup, tb);
    return popup;
}

/// @brief Measure an embedded custom-widget item.
/// @param tb Owning toolbar; currently retained for symmetry with other item
///        measurement helpers.
/// @param item Candidate toolbar item.
/// @param available_width Width constraint passed to the embedded widget.
/// @param available_height Height constraint passed to the embedded widget.
static void measure_toolbar_item_widget(vg_toolbar_t *tb,
                                        vg_toolbar_item_t *item,
                                        float available_width,
                                        float available_height) {
    if (!item || item->type != VG_TOOLBAR_ITEM_WIDGET || !item->custom_widget)
        return;
    vg_widget_measure(item->custom_widget, available_width, available_height);
    (void)tb;
}

/// @brief Recompute the split between directly visible and overflowed items.
/// @details Overflow is disabled when the toolbar has no overflow menu. When
///          total intrinsic extent exceeds the available primary dimension, an
///          overflow-button reservation is deducted and the first item that no
///          longer fits becomes `overflow_start_index`.
/// @param tb Toolbar whose overflow state is updated.
/// @param available_primary Available width for a horizontal toolbar or height
///        for a vertical toolbar.
static void toolbar_compute_overflow(vg_toolbar_t *tb, float available_primary) {
    tb->overflow_start_index = -1;
    if (!tb->overflow_menu)
        return;

    float total_extent = 0.0f;
    for (size_t i = 0; i < tb->item_count; i++) {
        float item_extent = get_item_extent(tb, tb->items[i]);
        if (item_extent <= 0.0f)
            continue;
        total_extent += item_extent;
        if (i + 1 < tb->item_count)
            total_extent += (float)tb->item_spacing;
    }

    if (total_extent <= available_primary)
        return;

    float limit = available_primary - get_overflow_button_extent(tb);
    if (limit < 0.0f)
        limit = 0.0f;

    float pos = 0.0f;
    for (size_t i = 0; i < tb->item_count; i++) {
        float item_extent = get_item_extent(tb, tb->items[i]);
        if (item_extent <= 0.0f)
            continue;
        if (pos + item_extent > limit) {
            tb->overflow_start_index = (int)i;
            return;
        }
        pos += item_extent + (float)tb->item_spacing;
    }
}

/// @brief Compute the synthetic overflow button's toolbar-local rectangle.
/// @param tb Arranged toolbar.
/// @param out_x Optional destination for the local left edge.
/// @param out_y Optional destination for the local top edge.
/// @param out_w Optional destination for the button width.
/// @param out_h Optional destination for the button height.
static void toolbar_get_overflow_button_rect(
    vg_toolbar_t *tb, float *out_x, float *out_y, float *out_w, float *out_h) {
    float extent = get_overflow_button_extent(tb);
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
        w = extent;
        h = tb->base.height - 4.0f;
        if (h < 0.0f)
            h = 0.0f;
        x = tb->base.width - w;
        y = 2.0f;
    } else {
        w = tb->base.width - 4.0f;
        if (w < 0.0f)
            w = 0.0f;
        h = extent;
        x = 2.0f;
        y = tb->base.height - h;
    }

    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
}

/// @brief Test a point against a half-open rectangle.
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @param x Rectangle left edge.
/// @param y Rectangle top edge.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @return `true` when the point lies in `[x,x+w)` and `[y,y+h)`.
static bool point_in_rect(float px, float py, float x, float y, float w, float h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/// @brief Hit-test a toolbar-local point against the overflow button.
/// @param tb Toolbar to test; may be `NULL`.
/// @param px Local X coordinate.
/// @param py Local Y coordinate.
/// @return `true` only when overflow is active and the point is inside its
///         synthetic button.
static bool toolbar_overflow_button_hit(vg_toolbar_t *tb, float px, float py) {
    if (!tb || tb->overflow_start_index < 0)
        return false;

    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    toolbar_get_overflow_button_rect(tb, &x, &y, &w, &h);
    return point_in_rect(px, py, x, y, w, h);
}

/// @brief Choose a display label for an overflow-menu item.
/// @details Selection falls back from non-empty label, to tooltip, to
///          identifier, and finally to a generic type name.
/// @param item Item to describe; may be `NULL`.
/// @return Borrowed, null-terminated label valid for at least the item's
///         lifetime, or a static fallback string.
static const char *toolbar_item_menu_label(vg_toolbar_item_t *item) {
    if (!item)
        return "Item";
    if (item->label && item->label[0] != '\0')
        return item->label;
    if (item->tooltip && item->tooltip[0] != '\0')
        return item->tooltip;
    if (item->id && item->id[0] != '\0')
        return item->id;
    switch (item->type) {
        case VG_TOOLBAR_ITEM_BUTTON:
            return "Button";
        case VG_TOOLBAR_ITEM_TOGGLE:
            return "Toggle";
        case VG_TOOLBAR_ITEM_DROPDOWN:
            return "Menu";
        default:
            return "Item";
    }
}

/// @brief Dismiss and destroy the active per-item dropdown popup.
/// @details Both the cloned popup tree and its association with the source
///          toolbar item are cleared.
/// @param tb Toolbar owning the popup; `NULL` or a toolbar without a popup is
///        ignored.
static void toolbar_dismiss_dropdown_popup(vg_toolbar_t *tb) {
    if (!tb || !tb->dropdown_popup)
        return;
    if (tb->dropdown_popup->is_visible)
        vg_contextmenu_dismiss(tb->dropdown_popup);
    toolbar_destroy_contextmenu_tree(tb->dropdown_popup);
    tb->dropdown_popup = NULL;
    tb->dropdown_item = NULL;
}

/// @brief Synchronize toolbar input capture with transient popup visibility.
/// @details A hidden dropdown clone is destroyed. Capture is released only
///          when neither popup remains visible and the toolbar currently owns
///          capture.
/// @param tb Toolbar whose popup/capture state is reconciled; `NULL` is ignored.
static void toolbar_sync_popup_capture(vg_toolbar_t *tb) {
    if (!tb)
        return;
    bool overflow_visible = tb->overflow_popup && tb->overflow_popup->is_visible;
    bool dropdown_visible = tb->dropdown_popup && tb->dropdown_popup->is_visible;
    if (tb->dropdown_popup && !dropdown_visible)
        toolbar_dismiss_dropdown_popup(tb);
    if ((overflow_visible || dropdown_visible) || vg_widget_get_input_capture() != &tb->base)
        return;
    vg_widget_release_input_capture();
}

/// @brief Toggle an item's dropdown menu as a transient context popup.
/// @details The source menu is cloned for each showing, measured by the context
///          menu, anchored in screen space below a horizontal item or beside a
///          vertical item, clamped to the root viewport, and given toolbar
///          input capture. Activating the already-open item dismisses it.
/// @param tb Toolbar that owns the transient popup.
/// @param item Live dropdown item whose borrowed menu model is cloned.
static void toolbar_show_dropdown_popup(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item || item->type != VG_TOOLBAR_ITEM_DROPDOWN || !item->dropdown_menu)
        return;

    if (tb->dropdown_item == item && tb->dropdown_popup && tb->dropdown_popup->is_visible) {
        toolbar_dismiss_dropdown_popup(tb);
        toolbar_sync_popup_capture(tb);
        tb->base.needs_paint = true;
        return;
    }

    toolbar_dismiss_dropdown_popup(tb);
    tb->dropdown_popup = toolbar_clone_menu_tree(item->dropdown_menu, tb);
    tb->dropdown_item = item;
    if (!tb->dropdown_popup || tb->dropdown_popup->item_count == 0) {
        toolbar_dismiss_dropdown_popup(tb);
        return;
    }

    float item_x = 0.0f;
    float item_y = 0.0f;
    float item_w = 0.0f;
    float item_h = 0.0f;
    if (!toolbar_get_item_rect(tb, item, &item_x, &item_y, &item_w, &item_h)) {
        toolbar_get_overflow_button_rect(tb, &item_x, &item_y, &item_w, &item_h);
    }

    float toolbar_x = 0.0f;
    float toolbar_y = 0.0f;
    toolbar_get_screen_origin(tb, &toolbar_x, &toolbar_y);
    int popup_x = (int)(toolbar_x + item_x);
    int popup_y = (int)(toolbar_y + item_y + item_h);
    if (tb->orientation == VG_TOOLBAR_VERTICAL) {
        popup_x = (int)(toolbar_x + item_x + item_w);
        popup_y = (int)(toolbar_y + item_y);
    }

    vg_contextmenu_show_at(tb->dropdown_popup, popup_x, popup_y);
    toolbar_clamp_popup_to_root(tb, tb->dropdown_popup);
    vg_widget_set_input_capture(&tb->base);
    tb->base.needs_paint = true;
}

/// @brief Execute an enabled item's primary toolbar action.
/// @details Buttons invoke their click callback, toggles invert state before
///          notifying their callback, and dropdowns open their menu or fall
///          back to a click callback. The activation latch and repaint flag are
///          set for all accepted item types.
/// @param tb Toolbar expected to own @p item.
/// @param item Item to activate; stale, foreign, disabled, or `NULL` items are
///        ignored.
static void toolbar_activate_item(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !vg_toolbar_item_is_live(item) || item->owner != tb || !item->enabled)
        return;

    item->was_clicked = true;
    switch (item->type) {
        case VG_TOOLBAR_ITEM_BUTTON:
            if (item->on_click)
                item->on_click(item, item->user_data);
            break;
        case VG_TOOLBAR_ITEM_TOGGLE:
            item->checked = !item->checked;
            if (item->on_toggle)
                item->on_toggle(item, item->checked, item->user_data);
            break;
        case VG_TOOLBAR_ITEM_DROPDOWN:
            if (item->dropdown_menu) {
                toolbar_show_dropdown_popup(tb, item);
            } else if (item->on_click) {
                item->on_click(item, item->user_data);
            }
            break;
        default:
            break;
    }

    tb->base.needs_paint = true;
}

/// @brief Activate the source toolbar item selected through the overflow popup.
/// @param menu Overflow context menu reporting selection; unused.
/// @param menu_item Selected overflow entry containing the source item pointer.
/// @param user_data Owning toolbar supplied as callback context.
static void toolbar_overflow_on_select(vg_contextmenu_t *menu,
                                       vg_menu_item_t *menu_item,
                                       void *user_data) {
    (void)menu;
    vg_toolbar_t *tb = (vg_toolbar_t *)user_data;
    vg_toolbar_item_t *item = menu_item ? (vg_toolbar_item_t *)menu_item->action_data : NULL;
    toolbar_activate_item(tb, item);
}

/// @brief Lazily create the toolbar's reusable overflow context menu.
/// @param tb Toolbar that will own the popup; `NULL` is ignored.
static void toolbar_ensure_overflow_popup(vg_toolbar_t *tb) {
    if (!tb || tb->overflow_popup)
        return;

    tb->overflow_popup = vg_contextmenu_create();
    if (!tb->overflow_popup)
        return;
    vg_contextmenu_set_on_select(tb->overflow_popup, toolbar_overflow_on_select, tb);
    vg_contextmenu_set_font(tb->overflow_popup, tb->font, tb->font_size);
    tb->overflow_popup_dirty = true;
}

/// @brief Rebuild overflow-popup entries from the hidden toolbar suffix.
/// @details Separators are coalesced so no leading separator is emitted, while
///          spacers and embedded widgets are omitted. Actionable items mirror
///          enabled, checked, label, and icon state and retain a pointer to the
///          source toolbar record.
/// @param tb Toolbar whose overflow popup is created or repopulated.
static void toolbar_rebuild_overflow_popup(vg_toolbar_t *tb) {
    toolbar_ensure_overflow_popup(tb);
    if (!tb || !tb->overflow_popup)
        return;

    vg_contextmenu_clear(tb->overflow_popup);
    vg_contextmenu_set_font(tb->overflow_popup, tb->font, tb->font_size);
    if (tb->overflow_start_index < 0) {
        tb->overflow_popup_dirty = false;
        return;
    }

    bool added_item = false;
    bool pending_separator = false;
    for (size_t i = (size_t)tb->overflow_start_index; i < tb->item_count; i++) {
        vg_toolbar_item_t *item = tb->items[i];
        if (!item || !item->visible)
            continue;

        if (item->type == VG_TOOLBAR_ITEM_SEPARATOR) {
            if (added_item)
                pending_separator = true;
            continue;
        }
        if (item->type == VG_TOOLBAR_ITEM_SPACER || item->type == VG_TOOLBAR_ITEM_WIDGET)
            continue;

        if (pending_separator) {
            vg_contextmenu_add_separator(tb->overflow_popup);
            pending_separator = false;
        }

        vg_menu_item_t *menu_item = vg_contextmenu_add_item(
            tb->overflow_popup, toolbar_item_menu_label(item), NULL, NULL, item);
        if (!menu_item)
            continue;
        menu_item->action_data = item;
        vg_contextmenu_item_set_enabled(menu_item, item->enabled);
        if (item->type == VG_TOOLBAR_ITEM_TOGGLE)
            vg_contextmenu_item_set_checked(menu_item, item->checked);
        if (item->icon.type != VG_ICON_NONE)
            vg_contextmenu_item_set_icon(menu_item, vg_icon_clone(&item->icon));
        added_item = true;
    }

    tb->overflow_popup_dirty = false;
}

/// @brief Dismiss a visible overflow popup and reconcile capture state.
/// @param tb Toolbar owning the popup; `NULL` and hidden popups are ignored.
static void toolbar_dismiss_overflow_popup(vg_toolbar_t *tb) {
    if (!tb || !tb->overflow_popup || !tb->overflow_popup->is_visible)
        return;
    vg_contextmenu_dismiss(tb->overflow_popup);
    toolbar_sync_popup_capture(tb);
    tb->base.needs_paint = true;
}

/// @brief Show the overflow popup at its orientation-dependent screen anchor.
/// @details Any item dropdown is dismissed first. A missing or dirty overflow
///          model is rebuilt, then the popup is shown below a horizontal
///          overflow button or beside a vertical one, root-clamped, and given
///          toolbar capture.
/// @param tb Toolbar whose hidden suffix is displayed.
static void toolbar_show_overflow_popup(vg_toolbar_t *tb) {
    if (!tb || tb->overflow_start_index < 0)
        return;
    toolbar_dismiss_dropdown_popup(tb);
    if (tb->overflow_popup_dirty || !tb->overflow_popup)
        toolbar_rebuild_overflow_popup(tb);
    if (!tb->overflow_popup || tb->overflow_popup->item_count == 0)
        return;

    float button_x = 0.0f;
    float button_y = 0.0f;
    float button_w = 0.0f;
    float button_h = 0.0f;
    toolbar_get_overflow_button_rect(tb, &button_x, &button_y, &button_w, &button_h);

    float toolbar_x = 0.0f;
    float toolbar_y = 0.0f;
    toolbar_get_screen_origin(tb, &toolbar_x, &toolbar_y);
    int popup_x = (int)(toolbar_x + button_x);
    int popup_y = (int)(toolbar_y + button_y + button_h);
    if (tb->orientation == VG_TOOLBAR_VERTICAL) {
        popup_x = (int)(toolbar_x + button_x + button_w);
        popup_y = (int)(toolbar_y + button_y);
    }

    vg_contextmenu_show_at(tb->overflow_popup, popup_x, popup_y);
    toolbar_clamp_popup_to_root(tb, tb->overflow_popup);
    vg_widget_set_input_capture(&tb->base);
    tb->base.needs_paint = true;
}

/// @brief Forward supported input to the currently visible toolbar popup.
/// @details Item dropdowns take precedence over the overflow popup. The event
///          is copied and retargeted, leaving the caller's event unchanged.
/// @param tb Toolbar owning possible transient popups.
/// @param event Mouse or keyboard event to forward.
/// @return Result of popup dispatch, or `false` when no eligible popup/event
///         combination exists.
static bool toolbar_forward_popup_event(vg_toolbar_t *tb, vg_event_t *event) {
    vg_contextmenu_t *popup = NULL;
    if (!tb || !event)
        return false;
    if (tb->dropdown_popup && tb->dropdown_popup->is_visible)
        popup = tb->dropdown_popup;
    else if (tb->overflow_popup && tb->overflow_popup->is_visible)
        popup = tb->overflow_popup;
    if (!popup)
        return false;
    if (!(event->type == VG_EVENT_MOUSE_MOVE || event->type == VG_EVENT_MOUSE_DOWN ||
          event->type == VG_EVENT_MOUSE_UP || event->type == VG_EVENT_CLICK ||
          event->type == VG_EVENT_KEY_DOWN || event->type == VG_EVENT_KEY_UP))
        return false;

    vg_event_t translated = *event;
    translated.target = &popup->base;
    return vg_event_send(&popup->base, &translated);
}

/// @brief Compute an item's intrinsic toolbar width.
/// @details Action items include horizontal padding, icon space, optional
///          measured label text, and a dropdown indicator when applicable.
///          Embedded widgets use their measured width, separators use a narrow
///          rule extent, and flexible spacers report zero intrinsic width.
/// @param tb Toolbar supplying font and presentation settings.
/// @param item Item to measure.
/// @return Intrinsic width in layout units.
static float get_item_width(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item || !item->visible)
        return 0.0f;
    float icon_px = get_scaled_icon_pixels(tb);
    float padding = (float)tb->item_padding;
    float spacing = (float)tb->item_spacing;

    switch (item->type) {
        case VG_TOOLBAR_ITEM_SEPARATOR:
            return toolbar_ui_scale() + spacing * 2.0f;

        case VG_TOOLBAR_ITEM_SPACER:
            return 0; // Spacers expand

        case VG_TOOLBAR_ITEM_BUTTON:
        case VG_TOOLBAR_ITEM_TOGGLE:
        case VG_TOOLBAR_ITEM_DROPDOWN: {
            float width = padding * 2.0f;
            if (item->icon.type != VG_ICON_NONE) {
                width += icon_px;
            }
            if (item->show_label && item->label && tb->font) {
                vg_text_metrics_t metrics;
                vg_font_measure_text(tb->font, tb->font_size, item->label, &metrics);
                if (item->icon.type != VG_ICON_NONE) {
                    width += padding;
                }
                width += metrics.width;
            }
            if (item->type == VG_TOOLBAR_ITEM_DROPDOWN) {
                width += 12.0f * toolbar_ui_scale(); // Arrow indicator
            }
            return width;
        }

        case VG_TOOLBAR_ITEM_WIDGET:
            if (item->custom_widget) {
                return item->custom_widget->measured_width + padding * 2;
            }
            return 0;

        default:
            return 0;
    }
}

/// @brief Compute an item's intrinsic toolbar height.
/// @details Action items use the larger of icon and measured font height plus
///          vertical padding. Embedded widgets use their measured height and
///          spacers deliberately contribute no intrinsic height.
/// @param tb Toolbar supplying font and presentation settings.
/// @param item Item to measure.
/// @return Intrinsic height in layout units.
static float get_item_height(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item || !item->visible)
        return 0.0f;
    float icon_px = get_scaled_icon_pixels(tb);
    float padding = (float)tb->item_padding;

    switch (item->type) {
        case VG_TOOLBAR_ITEM_SEPARATOR:
            return icon_px + padding * 2.0f;

        case VG_TOOLBAR_ITEM_SPACER:
            return 0;

        case VG_TOOLBAR_ITEM_BUTTON:
        case VG_TOOLBAR_ITEM_TOGGLE:
        case VG_TOOLBAR_ITEM_DROPDOWN: {
            float content_height = item->icon.type != VG_ICON_NONE ? icon_px : 0.0f;
            if (item->show_label && item->label && tb->font) {
                vg_font_metrics_t metrics;
                vg_font_get_metrics(tb->font, tb->font_size, &metrics);
                float text_height = metrics.ascent - metrics.descent;
                if (text_height > content_height)
                    content_height = text_height;
            }
            if (content_height <= 0.0f)
                content_height = icon_px;
            return content_height + padding * 2.0f;
        }

        case VG_TOOLBAR_ITEM_WIDGET:
            if (item->custom_widget) {
                return item->custom_widget->measured_height + padding * 2;
            }
            return 0;

        default:
            return 0;
    }
}

//=============================================================================
// Toolbar Implementation
//=============================================================================

/// @brief Create a toolbar widget, optionally attaching it to parent.
///
/// @details Initialises items[], sets orientation, derives icon size, padding, spacing, and colours
///          from the current theme, and enables overflow-menu mode by default.
///
/// @param parent      Widget to add the toolbar to; may be NULL.
/// @param orientation VG_TOOLBAR_HORIZONTAL or VG_TOOLBAR_VERTICAL.
/// @return            Newly allocated toolbar, or NULL on allocation failure.
vg_toolbar_t *vg_toolbar_create(vg_widget_t *parent, vg_toolbar_orientation_t orientation) {
    vg_toolbar_t *tb = calloc(1, sizeof(vg_toolbar_t));
    if (!tb)
        return NULL;

    // Initialize base widget
    vg_widget_init(&tb->base, VG_WIDGET_TOOLBAR, &g_toolbar_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize arrays
    tb->items = NULL;
    tb->item_count = 0;
    tb->item_capacity = 0;

    // Configuration — scale pixel constants by ui_scale for HiDPI displays.
    float s = theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    tb->orientation = orientation;
    tb->icon_size = VG_TOOLBAR_ICONS_MEDIUM;
    tb->item_padding = (int)(TOOLBAR_DEFAULT_PADDING * s);
    tb->item_spacing = (int)(TOOLBAR_DEFAULT_SPACING * s);
    tb->show_labels = false;
    tb->overflow_menu = true;

    // Font
    tb->font = NULL;
    tb->font_size = theme->typography.size_small;

    // Colors
    tb->bg_color = theme->colors.bg_secondary;
    tb->hover_color = theme->colors.bg_hover;
    tb->active_color = theme->colors.bg_active;
    tb->text_color = theme->colors.fg_primary;
    tb->disabled_color = theme->colors.fg_disabled;

    // State
    tb->hovered_item = NULL;
    tb->pressed_item = NULL;
    tb->overflow_start_index = -1;
    tb->focused_index = -1;
    tb->saved_tooltip_text = NULL;
    tb->hover_tooltip_active = false;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &tb->base);
    }

    return tb;
}

/// @brief Release toolbar-owned state during widget destruction.
/// @details Input capture, transient popups, live item records, saved tooltip
///          storage, and retired tombstones are released. Borrowed embedded
///          widgets and dropdown source menus are not destroyed.
/// @param widget Toolbar base widget being destroyed.
static void toolbar_destroy(vg_widget_t *widget) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;

    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    toolbar_dismiss_dropdown_popup(tb);
    if (tb->overflow_popup) {
        vg_contextmenu_destroy(tb->overflow_popup);
        tb->overflow_popup = NULL;
    }

    for (size_t i = 0; i < tb->item_count; i++) {
        free_item(tb->items[i]);
    }
    free(tb->items);
    free(tb->saved_tooltip_text);
    tb->saved_tooltip_text = NULL;
    free_retired_items(tb);
}

/// @brief Measure the toolbar and any embedded custom widgets.
/// @details Along the primary axis, nonzero intrinsic item extents and spacing
///          are accumulated. The cross-axis thickness is derived from scaled
///          icon size and padding. An empty primary axis consumes the available
///          constraint.
/// @param widget Toolbar base widget receiving measured dimensions.
/// @param available_width Width constraint for this measure pass.
/// @param available_height Height constraint for this measure pass.
static void toolbar_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    for (size_t i = 0; i < tb->item_count; i++)
        measure_toolbar_item_widget(tb, tb->items[i], available_width, available_height);

    float scale = toolbar_ui_scale();
    float icon_px = get_scaled_icon_pixels(tb);
    float bar_thickness = icon_px + (float)tb->item_padding * 2.0f + 4.0f * scale;

    if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
        // Calculate total width of items
        float total_width = 0;
        for (size_t i = 0; i < tb->item_count; i++) {
            float item_width = get_item_width(tb, tb->items[i]);
            if (item_width > 0) {
                total_width += item_width + tb->item_spacing;
            }
        }
        widget->measured_width = total_width > 0 ? total_width - tb->item_spacing : available_width;
        widget->measured_height = bar_thickness;
    } else {
        // Vertical orientation
        float total_height = 0;
        for (size_t i = 0; i < tb->item_count; i++) {
            float item_height = get_item_height(tb, tb->items[i]);
            if (item_height > 0) {
                total_height += item_height + tb->item_spacing;
            }
        }
        widget->measured_width = bar_thickness;
        widget->measured_height =
            total_height > 0 ? total_height - tb->item_spacing : available_height;
    }
}

/// @brief Arrange toolbar geometry, overflow state, and embedded widgets.
/// @details The supplied rectangle becomes the toolbar bounds. Custom widgets
///          are remeasured, the overflow split and focus index are normalized,
///          flexible spacer space is distributed, and visible embedded widgets
///          are centered in their item rectangles.
/// @param widget Toolbar base widget being arranged.
/// @param x Arranged left edge in parent coordinates.
/// @param y Arranged top edge in parent coordinates.
/// @param width Arranged width.
/// @param height Arranged height.
static void toolbar_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    for (size_t i = 0; i < tb->item_count; i++)
        measure_toolbar_item_widget(tb, tb->items[i], width, height);

    toolbar_compute_overflow(tb, tb->orientation == VG_TOOLBAR_HORIZONTAL ? width : height);
    toolbar_normalize_focus_index(tb);

    // Arrange custom widgets within toolbar items
    float pos = 0.0f;
    int max_index = toolbar_visible_limit(tb);
    float spacer_extent = toolbar_spacer_extent(tb, max_index);
    for (int i = 0; i < max_index; i++) {
        vg_toolbar_item_t *item = tb->items[i];
        float extent = toolbar_layout_extent(tb, item, max_index, spacer_extent);
        if (extent <= 0.0f)
            continue;

        if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
            float item_width = extent;
            if (item->type == VG_TOOLBAR_ITEM_WIDGET && item->custom_widget) {
                float iw = item->custom_widget->measured_width;
                float ih = item->custom_widget->measured_height;
                float ix = x + pos + (item_width - iw) / 2;
                float iy = y + (height - ih) / 2;
                vg_widget_arrange(item->custom_widget, ix, iy, iw, ih);
            }

            pos += item_width + tb->item_spacing;
        } else {
            float item_height = extent;
            if (item->type == VG_TOOLBAR_ITEM_WIDGET && item->custom_widget) {
                float iw = item->custom_widget->measured_width;
                float ih = item->custom_widget->measured_height;
                float ix = x + (width - iw) / 2;
                float iy = y + pos + (item_height - ih) / 2;
                vg_widget_arrange(item->custom_widget, ix, iy, iw, ih);
            }

            pos += item_height + tb->item_spacing;
        }
    }

    if (tb->overflow_popup_dirty && tb->overflow_popup && tb->overflow_popup->is_visible)
        toolbar_rebuild_overflow_popup(tb);
}

//=============================================================================
// Vector toolbar icons
//
// Zanna Studio passes Unicode glyphs as button "icons" (e.g. U+25B6 for Run), which
// render as inconsistent, mismatched font symbols. We map the known codepoints
// to crisp, uniform vector icons drawn through the shared anti-aliased core.
//=============================================================================

/// @brief Rasterize a solid triangle into a graphics window.
/// @details Integer pixel centers are tested against all three directed edges;
///          either winding is accepted. The alpha byte is discarded because
///          `vgfx_pset` consumes RGB.
/// @param win Destination graphics window.
/// @param ax First vertex X coordinate.
/// @param ay First vertex Y coordinate.
/// @param bx Second vertex X coordinate.
/// @param by Second vertex Y coordinate.
/// @param cx Third vertex X coordinate.
/// @param cy Third vertex Y coordinate.
/// @param color Packed color whose low 24 bits supply RGB.
static void toolbar_fill_tri(
    vgfx_window_t win, float ax, float ay, float bx, float by, float cx, float cy, uint32_t color) {
    uint32_t rgb = color & 0x00FFFFFFu;
    int minx = (int)floorf(fminf(ax, fminf(bx, cx)));
    int maxx = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int miny = (int)floorf(fminf(ay, fminf(by, cy)));
    int maxy = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.0f)
        return;
    for (int py = miny; py <= maxy; ++py) {
        for (int px = minx; px <= maxx; ++px) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            float w0 = (bx - ax) * (fy - ay) - (by - ay) * (fx - ax);
            float w1 = (cx - bx) * (fy - by) - (cy - by) * (fx - bx);
            float w2 = (ax - cx) * (fy - cy) - (ay - cy) * (fx - cx);
            bool inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside)
                vgfx_pset(win, px, py, rgb);
        }
    }
}

/// @brief Pick a semantic color for a known toolbar codepoint.
/// @details Action icons benefit from quick color recognition in the IDE:
///          run/continue are green, stop is red, build is amber, debug is
///          violet, file actions are blue/cyan, and search stays magenta.
///          Disabled items keep the caller's disabled color so they remain
///          visually inactive.
/// @param cp Unicode codepoint used as the toolbar icon key.
/// @param fallback Theme-provided text color to use for unmapped or disabled icons.
/// @param enabled True when the toolbar item can currently be activated.
/// @return RGB color used to draw the vector icon.
static uint32_t toolbar_vector_icon_color(uint32_t cp, uint32_t fallback, bool enabled) {
    if (!enabled)
        return fallback;
    const vg_color_scheme_t *colors = &vg_theme_get_current()->colors;
    switch (cp) {
        case 0x2B:   // New
        case 0x25A4: // Open
        case 0x2192: // Step
            return colors->accent_info;
        case 0x25BC: // Save
        case 0x21D3: // Save all
            return colors->fg_link;
        case 0x25A3: // Build
            return colors->accent_warning;
        case 0x25B6: // Run
        case 0x25B7: // Continue
            return colors->accent_success;
        case 0x25A0: // Stop
            return colors->accent_danger;
        case 0x25C7: // Debug
            return colors->accent_primary;
        default:
            return fallback;
    }
}

/// @brief Draw a recognisable vector icon for a known toolbar codepoint.
/// @details Maps the legacy single-codepoint toolbar keys onto the shared
///          scalable vector icon library (ADR 0137); unmapped codepoints fall
///          back to the font glyph path.
/// @param win Destination window/framebuffer.
/// @param cp Unicode codepoint used as the icon key.
/// @param bx Left edge of the icon box.
/// @param by Top edge of the icon box.
/// @param sz Width/height of the square icon box.
/// @param color Theme fallback color.
/// @param enabled True when the owning toolbar item is enabled.
/// @return true if handled (caller skips the font glyph); false to fall back.
static bool toolbar_draw_vector_icon(
    vgfx_window_t win, uint32_t cp, float bx, float by, float sz, uint32_t color, bool enabled) {
    const char *name = NULL;
    switch (cp) {
        case 0x2B:
            name = "new-file";
            break;
        case 0x25A4:
            name = "folder-open";
            break;
        case 0x25BC:
            name = "save";
            break;
        case 0x21D3:
            name = "save-all";
            break;
        case 0x25A3:
            name = "build";
            break;
        case 0x25B6:
            name = "run";
            break;
        case 0x25A0:
            name = "debug-stop";
            break;
        case 0x25C7:
            name = "debug";
            break;
        case 0x25B7:
            name = "debug-continue";
            break;
        case 0x2192:
            name = "debug-step-over";
            break;
        case 0x3F:
            name = "find";
            break;
        case 0x25A6:
            name = "explorer";
            break;
        case 0x2387:
            name = "source-control";
            break;
        default:
            return false;
    }
    int32_t icon_id = vg_icon_vector_find(name);
    if (icon_id == VG_ICON_VECTOR_INVALID)
        return false;
    uint32_t icon_color = toolbar_vector_icon_color(cp, color, enabled);
    vg_icon_vector_draw(
        win, icon_id, (int32_t)(bx + 0.5f), (int32_t)(by + 0.5f), (int32_t)(sz + 0.5f), icon_color);
    return true;
}

/// @brief If @p s is exactly one UTF-8 codepoint, return it; otherwise return 0.
/// @details Zanna Studio passes single Unicode glyphs (e.g. "▶") in a button's text
///          slot; we use this to detect them and substitute a vector icon. Real
///          multi-character labels and malformed leading/continuation sequences
///          return zero and render as text. This helper validates byte shape,
///          but does not reject every non-scalar or overlong encoding.
/// @param s Null-terminated UTF-8 label; may be `NULL` or empty.
/// @return Decoded codepoint when the entire string contains exactly one
///         structurally valid sequence, otherwise zero.
static uint32_t toolbar_label_single_codepoint(const char *s) {
    if (!s || !s[0])
        return 0;
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    int len;
    if (p[0] < 0x80) {
        cp = p[0];
        len = 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1Fu;
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0Fu;
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07u;
        len = 4;
    } else {
        return 0;
    }
    for (int i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (uint32_t)(p[i] & 0x3F);
    }
    return p[len] == '\0' ? cp : 0; // 0 if more than one codepoint
}

/// @brief Paint the toolbar, its directly visible items, and overflow affordance.
/// @details Rendering includes themed item states, raster/glyph/vector icons,
///          optional labels, separators, dropdown arrows, embedded widgets,
///          and keyboard focus rings. Popup menus are deferred to the overlay
///          pass.
/// @param widget Toolbar base widget to paint.
/// @param canvas Graphics canvas represented by a `vgfx_window_t`.
static void toolbar_paint(vg_widget_t *widget, void *canvas) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;
    vg_theme_t *theme = vg_theme_get_current();

    // Draw toolbar background
    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   tb->bg_color);

    float icon_px = get_scaled_icon_pixels(tb);

    float pos = 0.0f;
    int max_index = toolbar_visible_limit(tb);
    float spacer_extent = toolbar_spacer_extent(tb, max_index);

    for (int i = 0; i < max_index; i++) {
        vg_toolbar_item_t *item = tb->items[i];

        float item_width, item_height;
        float item_x, item_y;
        float extent = toolbar_layout_extent(tb, item, max_index, spacer_extent);
        if (extent <= 0.0f)
            continue;

        if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
            item_width = extent;
            item_height = widget->height - 4;
            item_x = widget->x + pos;
            item_y = widget->y + 2;
        } else {
            item_width = widget->width - 4;
            item_height = extent;
            item_x = widget->x + 2;
            item_y = widget->y + pos;
        }

        switch (item->type) {
            case VG_TOOLBAR_ITEM_SEPARATOR: {
                // Draw vertical or horizontal line
                uint32_t sep_color = theme->colors.border_primary;
                if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
                    int32_t sep_x = (int32_t)(item_x + item_width / 2);
                    int32_t sep_y1 = (int32_t)(widget->y + 4);
                    int32_t sep_y2 = (int32_t)(widget->y + widget->height - 4);
                    vgfx_fill_rect(win, sep_x, sep_y1, 1, sep_y2 - sep_y1, sep_color);
                } else {
                    int32_t sep_x1 = (int32_t)(widget->x + 4);
                    int32_t sep_x2 = (int32_t)(widget->x + widget->width - 4);
                    int32_t sep_y = (int32_t)(item_y + item_height / 2);
                    vgfx_fill_rect(win, sep_x1, sep_y, sep_x2 - sep_x1, 1, sep_color);
                }
                break;
            }

            case VG_TOOLBAR_ITEM_SPACER:
                // Don't draw anything
                break;

            case VG_TOOLBAR_ITEM_BUTTON:
            case VG_TOOLBAR_ITEM_TOGGLE:
            case VG_TOOLBAR_ITEM_DROPDOWN: {
                bool keyboard_focused =
                    (widget->state & VG_STATE_FOCUSED) && tb->focused_index == i;
                // Draw button background based on state
                uint32_t btn_bg = 0; // No background by default
                if (item == tb->pressed_item) {
                    btn_bg = tb->active_color;
                } else if (item == tb->hovered_item) {
                    btn_bg = tb->hover_color;
                } else if (item->type == VG_TOOLBAR_ITEM_TOGGLE && item->checked) {
                    btn_bg = tb->active_color;
                } else if (keyboard_focused) {
                    btn_bg = vg_color_blend(tb->bg_color, tb->hover_color, 0.7f);
                }

                if (btn_bg != 0) {
                    vg_draw_round_rect_fill(win,
                                            item_x + 1.0f,
                                            item_y + 1.0f,
                                            item_width - 2.0f,
                                            item_height - 2.0f,
                                            vg_theme_get_current()->radius.sm,
                                            btn_bg);
                }

                // Determine text color
                uint32_t txt_color = item->enabled ? tb->text_color : tb->disabled_color;

                // Draw icon
                float icon_x = item_x + (item_width - icon_px) / 2;
                float icon_y = item_y + (item_height - icon_px) / 2;

                if (item->show_label && item->label) {
                    // Icon on left, label on right
                    icon_x = item_x + tb->item_padding;
                }

                switch (item->icon.type) {
                    case VG_ICON_GLYPH:
                        // Prefer a crisp vector icon for known codepoints; fall
                        // back to the font glyph for anything unmapped.
                        if (toolbar_draw_vector_icon(win,
                                                     item->icon.data.glyph,
                                                     icon_x,
                                                     icon_y,
                                                     icon_px,
                                                     txt_color,
                                                     item->enabled)) {
                            break;
                        }
                        // Draw glyph using font
                        if (tb->font) {
                            char buf[8] = {0};
                            // UTF-8 encode the glyph
                            uint32_t cp = item->icon.data.glyph;
                            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                                break;
                            } else if (cp < 0x80) {
                                buf[0] = (char)cp;
                            } else if (cp < 0x800) {
                                buf[0] = (char)(0xC0 | (cp >> 6));
                                buf[1] = (char)(0x80 | (cp & 0x3F));
                            } else if (cp < 0x10000) {
                                buf[0] = (char)(0xE0 | (cp >> 12));
                                buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                buf[2] = (char)(0x80 | (cp & 0x3F));
                            } else {
                                buf[0] = (char)(0xF0 | (cp >> 18));
                                buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                buf[3] = (char)(0x80 | (cp & 0x3F));
                            }
                            vg_font_draw_text(canvas,
                                              tb->font,
                                              icon_px,
                                              icon_x,
                                              icon_y + icon_px * 0.8f,
                                              buf,
                                              txt_color);
                        }
                        break;

                    case VG_ICON_IMAGE:
                        toolbar_draw_image_icon(
                            win, &item->icon, icon_x, icon_y, icon_px, icon_px, item->enabled);
                        break;

                    case VG_ICON_VECTOR:
                        vg_icon_vector_draw(win,
                                            item->icon.data.vector_id,
                                            (int32_t)(icon_x + 0.5f),
                                            (int32_t)(icon_y + 0.5f),
                                            (int32_t)(icon_px + 0.5f),
                                            txt_color);
                        break;

                    default:
                        break;
                }

                // Draw label if shown. A single-glyph "label" with no real icon
                // (how Zanna Studio supplies toolbar icons) is rendered as a crisp
                // centred vector icon; everything else draws as text.
                if (item->show_label && item->label && tb->font) {
                    uint32_t label_cp = (item->icon.type == VG_ICON_NONE)
                                            ? toolbar_label_single_codepoint(item->label)
                                            : 0;
                    float centred_x = item_x + (item_width - icon_px) / 2.0f;
                    if (label_cp != 0 &&
                        toolbar_draw_vector_icon(
                            win, label_cp, centred_x, icon_y, icon_px, txt_color, item->enabled)) {
                        // Drawn as a vector icon — skip the text.
                    } else {
                        float label_x = (item->icon.type == VG_ICON_NONE)
                                            ? item_x + tb->item_padding
                                            : icon_x + icon_px + tb->item_padding;
                        vg_font_metrics_t font_metrics;
                        vg_font_get_metrics(tb->font, tb->font_size, &font_metrics);
                        float label_y =
                            item_y +
                            (item_height + font_metrics.ascent + font_metrics.descent) / 2.0f;
                        vg_font_draw_text(canvas,
                                          tb->font,
                                          tb->font_size,
                                          label_x,
                                          label_y,
                                          item->label,
                                          txt_color);
                    }
                }

                // Draw dropdown arrow
                if (item->type == VG_TOOLBAR_ITEM_DROPDOWN) {
                    // Draw small triangle at right edge
                    float arrow_x = item_x + item_width - 8;
                    float arrow_y = item_y + item_height / 2;
                    vgfx_fill_rect(win, (int32_t)arrow_x, (int32_t)(arrow_y - 1), 5, 1, txt_color);
                    vgfx_fill_rect(win, (int32_t)(arrow_x + 1), (int32_t)arrow_y, 3, 1, txt_color);
                    vgfx_fill_rect(
                        win, (int32_t)(arrow_x + 2), (int32_t)(arrow_y + 1), 1, 1, txt_color);
                }
                if (keyboard_focused) {
                    uint32_t focus = theme->colors.border_focus;
                    int32_t fx = (int32_t)item_x;
                    int32_t fy = (int32_t)item_y;
                    int32_t fw = (int32_t)item_width;
                    int32_t fh = (int32_t)item_height;
                    if (fw > 1 && fh > 1) {
                        vgfx_fill_rect(win, fx, fy, fw, 1, focus);
                        vgfx_fill_rect(win, fx, fy + fh - 1, fw, 1, focus);
                        vgfx_fill_rect(win, fx, fy, 1, fh, focus);
                        vgfx_fill_rect(win, fx + fw - 1, fy, 1, fh, focus);
                    }
                }
                break;
            }

            case VG_TOOLBAR_ITEM_WIDGET:
                // Custom widget draws itself
                if (item->custom_widget) {
                    vg_widget_paint(item->custom_widget, canvas);
                }
                break;
        }

        pos += extent + tb->item_spacing;
    }

    // Draw overflow button if needed
    if (tb->overflow_start_index >= 0) {
        float ov_x = 0.0f;
        float ov_y = 0.0f;
        float ov_w = 0.0f;
        float ov_h = 0.0f;
        toolbar_get_overflow_button_rect(tb, &ov_x, &ov_y, &ov_w, &ov_h);

        uint32_t ov_bg = 0;
        if (tb->overflow_popup && tb->overflow_popup->is_visible)
            ov_bg = tb->active_color;
        else if (tb->overflow_button_hovered)
            ov_bg = tb->hover_color;
        else if ((widget->state & VG_STATE_FOCUSED) &&
                 tb->focused_index == tb->overflow_start_index)
            ov_bg = vg_color_blend(tb->bg_color, tb->hover_color, 0.7f);
        if (ov_bg != 0) {
            vgfx_fill_rect(win,
                           (int32_t)(widget->x + ov_x),
                           (int32_t)(widget->y + ov_y),
                           (int32_t)ov_w,
                           (int32_t)ov_h,
                           ov_bg);
        }

        float dot_y = widget->y + ov_y + ov_h / 2.0f;
        if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
            float center_x = widget->x + ov_x + ov_w / 2.0f;
            vgfx_fill_rect(win, (int32_t)(center_x - 6.0f), (int32_t)dot_y, 2, 2, tb->text_color);
            vgfx_fill_rect(win, (int32_t)(center_x - 1.0f), (int32_t)dot_y, 2, 2, tb->text_color);
            vgfx_fill_rect(win, (int32_t)(center_x + 4.0f), (int32_t)dot_y, 2, 2, tb->text_color);
        } else {
            float center_x = widget->x + ov_x + ov_w / 2.0f;
            float center_y = widget->y + ov_y + ov_h / 2.0f;
            vgfx_fill_rect(
                win, (int32_t)center_x, (int32_t)(center_y - 6.0f), 2, 2, tb->text_color);
            vgfx_fill_rect(
                win, (int32_t)center_x, (int32_t)(center_y - 1.0f), 2, 2, tb->text_color);
            vgfx_fill_rect(
                win, (int32_t)center_x, (int32_t)(center_y + 4.0f), 2, 2, tb->text_color);
        }
        if ((widget->state & VG_STATE_FOCUSED) && tb->focused_index == tb->overflow_start_index) {
            uint32_t focus = theme->colors.border_focus;
            int32_t fx = (int32_t)(widget->x + ov_x);
            int32_t fy = (int32_t)(widget->y + ov_y);
            int32_t fw = (int32_t)ov_w;
            int32_t fh = (int32_t)ov_h;
            if (fw > 1 && fh > 1) {
                vgfx_fill_rect(win, fx, fy, fw, 1, focus);
                vgfx_fill_rect(win, fx, fy + fh - 1, fw, 1, focus);
                vgfx_fill_rect(win, fx, fy, 1, fh, focus);
                vgfx_fill_rect(win, fx + fw - 1, fy, 1, fh, focus);
            }
        }
    }
}

/// @brief Paint visible toolbar-owned context menus in the overlay phase.
/// @details Overflow is painted before an item dropdown, matching the latter's
///          input priority if both transient records are momentarily visible.
/// @param widget Toolbar base widget owning the popups.
/// @param canvas Graphics canvas forwarded to each popup paint callback.
static void toolbar_paint_overlay(vg_widget_t *widget, void *canvas) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    if (tb->overflow_popup && tb->overflow_popup->is_visible && tb->overflow_popup->base.vtable &&
        tb->overflow_popup->base.vtable->paint) {
        tb->overflow_popup->base.vtable->paint(&tb->overflow_popup->base, canvas);
    }
    if (tb->dropdown_popup && tb->dropdown_popup->is_visible && tb->dropdown_popup->base.vtable &&
        tb->dropdown_popup->base.vtable->paint) {
        tb->dropdown_popup->base.vtable->paint(&tb->dropdown_popup->base, canvas);
    }
}

/// @brief Union one visible toolbar context-menu popup into an absolute rectangle.
/// @param popup Popup to include; NULL, hidden, or retired popups are ignored.
/// @param any In/out flag indicating whether the rectangle has an initial value.
/// @param x0 In/out left edge.
/// @param y0 In/out top edge.
/// @param x1 In/out exclusive right edge.
/// @param y1 In/out exclusive bottom edge.
static void toolbar_include_popup_bounds(
    const vg_contextmenu_t *popup, bool *any, float *x0, float *y0, float *x1, float *y1) {
    if (!popup || !popup->is_visible || !vg_widget_is_live(&popup->base) || !any || !x0 || !y0 ||
        !x1 || !y1) {
        return;
    }
    float popup_x = 0.0f, popup_y = 0.0f, popup_w = 0.0f, popup_h = 0.0f;
    vg_widget_get_screen_bounds(&popup->base, &popup_x, &popup_y, &popup_w, &popup_h);
    if (!isfinite(popup_x) || !isfinite(popup_y) || !isfinite(popup_w) || !isfinite(popup_h) ||
        popup_w <= 0.0f || popup_h <= 0.0f) {
        return;
    }
    float popup_x1 = popup_x + popup_w;
    float popup_y1 = popup_y + popup_h;
    if (!*any) {
        *x0 = popup_x;
        *y0 = popup_y;
        *x1 = popup_x1;
        *y1 = popup_y1;
        *any = true;
        return;
    }
    if (popup_x < *x0)
        *x0 = popup_x;
    if (popup_y < *y0)
        *y0 = popup_y;
    if (popup_x1 > *x1)
        *x1 = popup_x1;
    if (popup_y1 > *y1)
        *y1 = popup_y1;
}

/// @brief Report the union of visible overflow and item-dropdown popup rectangles.
/// @details Toolbar popups are owned transient widgets rather than children, so
///          ordinary tree traversal cannot discover their pixels. Their absolute
///          rectangles are returned here and unioned with the toolbar by the widget
///          core; theme shadow overflow is then applied once around the result.
/// @param widget Toolbar widget being queried.
/// @param x Receives popup-union X, or zero when no popup is visible.
/// @param y Receives popup-union Y, or zero when no popup is visible.
/// @param width Receives popup-union width, or zero when no popup is visible.
/// @param height Receives popup-union height, or zero when no popup is visible.
static void toolbar_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height) {
    if (x)
        *x = 0.0f;
    if (y)
        *y = 0.0f;
    if (width)
        *width = 0.0f;
    if (height)
        *height = 0.0f;
    vg_toolbar_t *toolbar = (vg_toolbar_t *)widget;
    if (!toolbar)
        return;
    bool any = false;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    toolbar_include_popup_bounds(toolbar->overflow_popup, &any, &x0, &y0, &x1, &y1);
    toolbar_include_popup_bounds(toolbar->dropdown_popup, &any, &x0, &y0, &x1, &y1);
    if (!any)
        return;
    if (x)
        *x = x0;
    if (y)
        *y = y0;
    if (width)
        *width = x1 - x0;
    if (height)
        *height = y1 - y0;
}

/// @brief Find the directly visible item under a toolbar-local point.
/// @details Hit rectangles use the same primary-axis extents and flexible
///          spacer allocation as painting. Separators and spacers are excluded
///          even when the point lies within their layout rectangle.
/// @param tb Toolbar to hit-test.
/// @param px Local X coordinate.
/// @param py Local Y coordinate.
/// @return Borrowed item pointer for an actionable or embedded-widget item, or
///         `NULL` when no interactable item is hit.
static vg_toolbar_item_t *find_item_at(vg_toolbar_t *tb, float px, float py) {
    float pos = 0.0f;
    int max_index = toolbar_visible_limit(tb);
    float spacer_extent = toolbar_spacer_extent(tb, max_index);

    for (int i = 0; i < max_index; i++) {
        vg_toolbar_item_t *item = tb->items[i];

        float item_width, item_height;
        float item_x, item_y;
        float extent = toolbar_layout_extent(tb, item, max_index, spacer_extent);
        if (extent <= 0.0f)
            continue;

        if (tb->orientation == VG_TOOLBAR_HORIZONTAL) {
            item_width = extent;
            item_height = tb->base.height - 4.0f;
            if (item_height < 0.0f)
                item_height = 0.0f;
            item_x = pos;
            item_y = 2.0f;
        } else {
            item_width = tb->base.width - 4.0f;
            if (item_width < 0.0f)
                item_width = 0.0f;
            item_height = extent;
            item_x = 2.0f;
            item_y = pos;
        }

        if (px >= item_x && px < item_x + item_width && py >= item_y && py < item_y + item_height) {
            if (item->type != VG_TOOLBAR_ITEM_SEPARATOR && item->type != VG_TOOLBAR_ITEM_SPACER) {
                return item;
            }
        }

        pos += extent + tb->item_spacing;
    }

    return NULL;
}

/// @brief Route pointer and keyboard input for the toolbar.
/// @details Visible popups receive eligible events first. Local handling then
///          maintains hover/press state, tooltip mirroring, item activation,
///          overflow toggling, orientation-aware arrow traversal, Home/End,
///          Escape dismissal, and Enter/Space activation.
/// @param widget Toolbar base widget receiving the event.
/// @param event Event expressed in toolbar-local coordinates where applicable.
/// @return `true` when the toolbar or an active popup consumed the event.
static bool toolbar_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    bool popup_was_visible = (tb->overflow_popup && tb->overflow_popup->is_visible) ||
                             (tb->dropdown_popup && tb->dropdown_popup->is_visible);

    if (popup_was_visible) {
        bool popup_handled = toolbar_forward_popup_event(tb, event);
        toolbar_sync_popup_capture(tb);
        if (popup_handled) {
            widget->needs_paint = true;
            return true;
        }
        if (event->type == VG_EVENT_MOUSE_DOWN &&
            toolbar_overflow_button_hit(tb, event->mouse.x, event->mouse.y)) {
            widget->needs_paint = true;
            return true;
        }
    }

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            vg_toolbar_item_t *item = find_item_at(tb, event->mouse.x, event->mouse.y);
            bool overflow_hovered = toolbar_overflow_button_hit(tb, event->mouse.x, event->mouse.y);
            if (item != tb->hovered_item || overflow_hovered != tb->overflow_button_hovered) {
                tb->hovered_item = item;
                tb->overflow_button_hovered = overflow_hovered;
                toolbar_sync_hover_tooltip(tb);
                widget->needs_paint = true;
            }
            return false;
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (tb->hovered_item) {
                tb->hovered_item = NULL;
                toolbar_sync_hover_tooltip(tb);
                widget->needs_paint = true;
            }
            if (tb->overflow_button_hovered) {
                tb->overflow_button_hovered = false;
                widget->needs_paint = true;
            }
            if (tb->pressed_item) {
                tb->pressed_item = NULL;
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_MOUSE_DOWN: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            if (toolbar_overflow_button_hit(tb, event->mouse.x, event->mouse.y)) {
                vg_widget_set_focus(widget);
                tb->focused_index = tb->overflow_start_index;
                toolbar_dismiss_dropdown_popup(tb);
                if (tb->overflow_popup && tb->overflow_popup->is_visible)
                    toolbar_dismiss_overflow_popup(tb);
                else
                    toolbar_show_overflow_popup(tb);
                tb->pressed_item = NULL;
                return true;
            }

            vg_toolbar_item_t *item = find_item_at(tb, event->mouse.x, event->mouse.y);
            if (item && item->enabled) {
                int item_index = toolbar_index_of_item(tb, item);
                if (item_index >= 0)
                    tb->focused_index = item_index;
                vg_widget_set_focus(widget);
                toolbar_dismiss_overflow_popup(tb);
                if (item->type != VG_TOOLBAR_ITEM_DROPDOWN || item != tb->dropdown_item)
                    toolbar_dismiss_dropdown_popup(tb);
                tb->pressed_item = item;
                widget->needs_paint = true;
                return true;
            }
            toolbar_dismiss_overflow_popup(tb);
            toolbar_dismiss_dropdown_popup(tb);
            toolbar_sync_popup_capture(tb);
            return false;
        }

        case VG_EVENT_MOUSE_UP: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            if (toolbar_overflow_button_hit(tb, event->mouse.x, event->mouse.y)) {
                tb->pressed_item = NULL;
                return true;
            }
            vg_toolbar_item_t *item = find_item_at(tb, event->mouse.x, event->mouse.y);
            if (item && item == tb->pressed_item && item->enabled) {
                toolbar_activate_item(tb, item);
            }
            tb->pressed_item = NULL;
            widget->needs_paint = true;
            return item != NULL;
        }

        case VG_EVENT_KEY_DOWN: {
            if (!toolbar_focus_index_valid(tb, tb->focused_index))
                tb->focused_index = toolbar_first_focus_index(tb);

            bool horizontal = tb->orientation == VG_TOOLBAR_HORIZONTAL;
            if ((horizontal && event->key.key == VG_KEY_LEFT) ||
                (!horizontal && event->key.key == VG_KEY_UP)) {
                int next = toolbar_next_focus_index(tb, tb->focused_index, -1);
                if (next != tb->focused_index) {
                    tb->focused_index = next;
                    tb->hovered_item = toolbar_focus_is_overflow(tb, next) ? NULL : tb->items[next];
                    toolbar_sync_hover_tooltip(tb);
                    widget->needs_paint = true;
                }
                return true;
            }
            if ((horizontal && event->key.key == VG_KEY_RIGHT) ||
                (!horizontal && event->key.key == VG_KEY_DOWN)) {
                int next = toolbar_next_focus_index(tb, tb->focused_index, 1);
                if (next != tb->focused_index) {
                    tb->focused_index = next;
                    tb->hovered_item = toolbar_focus_is_overflow(tb, next) ? NULL : tb->items[next];
                    toolbar_sync_hover_tooltip(tb);
                    widget->needs_paint = true;
                }
                return true;
            }
            if (event->key.key == VG_KEY_HOME) {
                tb->focused_index = toolbar_first_focus_index(tb);
                tb->hovered_item =
                    toolbar_focus_is_overflow(tb, tb->focused_index)
                        ? NULL
                        : (tb->focused_index >= 0 ? tb->items[tb->focused_index] : NULL);
                toolbar_sync_hover_tooltip(tb);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_END) {
                tb->focused_index = toolbar_last_focus_index(tb);
                tb->hovered_item =
                    toolbar_focus_is_overflow(tb, tb->focused_index)
                        ? NULL
                        : (tb->focused_index >= 0 ? tb->items[tb->focused_index] : NULL);
                toolbar_sync_hover_tooltip(tb);
                widget->needs_paint = true;
                return true;
            }
            if (event->key.key == VG_KEY_ESCAPE) {
                bool dismissed = false;
                if (tb->overflow_popup && tb->overflow_popup->is_visible) {
                    toolbar_dismiss_overflow_popup(tb);
                    dismissed = true;
                }
                if (tb->dropdown_popup && tb->dropdown_popup->is_visible) {
                    toolbar_dismiss_dropdown_popup(tb);
                    dismissed = true;
                }
                toolbar_sync_popup_capture(tb);
                if (dismissed)
                    widget->needs_paint = true;
                return dismissed;
            }
            if (horizontal && event->key.key == VG_KEY_DOWN &&
                toolbar_focus_is_overflow(tb, tb->focused_index)) {
                toolbar_show_overflow_popup(tb);
                return true;
            }
            if (horizontal && event->key.key == VG_KEY_DOWN &&
                toolbar_focus_index_valid(tb, tb->focused_index)) {
                vg_toolbar_item_t *item = tb->items[tb->focused_index];
                if (item && item->type == VG_TOOLBAR_ITEM_DROPDOWN) {
                    toolbar_show_dropdown_popup(tb, item);
                    return true;
                }
            }
            if (event->key.key == VG_KEY_ENTER || event->key.key == VG_KEY_SPACE) {
                if (toolbar_focus_is_overflow(tb, tb->focused_index)) {
                    if (tb->overflow_popup && tb->overflow_popup->is_visible)
                        toolbar_dismiss_overflow_popup(tb);
                    else
                        toolbar_show_overflow_popup(tb);
                    widget->needs_paint = true;
                    return true;
                }
                if (toolbar_focus_index_valid(tb, tb->focused_index)) {
                    toolbar_activate_item(tb, tb->items[tb->focused_index]);
                    widget->needs_paint = true;
                    return true;
                }
            }
            return false;
        }

        default:
            return false;
    }
}

/// @brief Determine whether the toolbar can accept keyboard focus.
/// @param widget Toolbar base widget to inspect.
/// @return `true` when the widget is enabled and visible and current layout
///         exposes at least one focus target.
static bool toolbar_can_focus(vg_widget_t *widget) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    return widget->enabled && widget->visible && toolbar_first_focus_index(tb) >= 0;
}

/// @brief Update toolbar state after a widget focus transition.
/// @param widget Toolbar base widget whose focus changed.
/// @param gained `true` when focus was acquired; `false` when it was lost.
static void toolbar_on_focus(vg_widget_t *widget, bool gained) {
    vg_toolbar_t *tb = (vg_toolbar_t *)widget;
    if (gained)
        toolbar_normalize_focus_index(tb);
    widget->needs_paint = true;
}

//=============================================================================
// Toolbar API
//=============================================================================

/// @brief Append a push-button item to the toolbar.
///
/// @details The label and identifier are copied, while ownership of @p icon is
///          transferred even when creation fails. Successful insertion marks
///          layout and the lazy overflow representation dirty.
/// @param tb        Toolbar to add to; may be NULL (returns NULL).
/// @param id        Unique string identifier; may be NULL.
/// @param label     Display text (visible when show_labels is true); may be NULL.
/// @param icon      Icon descriptor (VG_ICON_NONE for no icon).
/// @param on_click  Callback invoked when the button is activated; may be NULL.
/// @param user_data Opaque pointer forwarded to on_click.
/// @return          The new item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_button(vg_toolbar_t *tb,
                                         const char *id,
                                         const char *label,
                                         vg_icon_t icon,
                                         void (*on_click)(vg_toolbar_item_t *, void *),
                                         void *user_data) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_BUTTON, id);
    if (!item) {
        vg_icon_destroy(&icon);
        return NULL;
    }

    char *label_copy = label ? vg_strdup(label) : NULL;
    if (label && !label_copy) {
        vg_icon_destroy(&icon);
        free_item(item);
        return NULL;
    }
    item->label = label_copy;
    item->icon = icon;
    item->show_label = tb->show_labels;
    item->on_click = on_click;
    item->user_data = user_data;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Append a two-state toggle button to the toolbar.
///
/// @details The label and identifier are copied, while ownership of @p icon is
///          transferred. Activation flips `checked` before invoking
///          @p on_toggle with the new state.
/// @param tb              Toolbar to add to; may be NULL (returns NULL).
/// @param id              Unique string identifier; may be NULL.
/// @param label           Display text; may be NULL.
/// @param icon            Icon descriptor.
/// @param initial_checked Initial checked state.
/// @param on_toggle       Callback invoked with the new boolean state; may be NULL.
/// @param user_data       Opaque pointer forwarded to on_toggle.
/// @return                The new item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_toggle(vg_toolbar_t *tb,
                                         const char *id,
                                         const char *label,
                                         vg_icon_t icon,
                                         bool initial_checked,
                                         void (*on_toggle)(vg_toolbar_item_t *, bool, void *),
                                         void *user_data) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_TOGGLE, id);
    if (!item) {
        vg_icon_destroy(&icon);
        return NULL;
    }

    char *label_copy = label ? vg_strdup(label) : NULL;
    if (label && !label_copy) {
        vg_icon_destroy(&icon);
        free_item(item);
        return NULL;
    }
    item->label = label_copy;
    item->icon = icon;
    item->checked = initial_checked;
    item->show_label = tb->show_labels;
    item->on_toggle = on_toggle;
    item->user_data = user_data;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Append a dropdown button that opens menu as a context popup when activated.
///
/// @details The toolbar borrows @p menu and clones its hierarchy each time the
///          dropdown is shown. Identifier and label are copied and ownership of
///          @p icon transfers to the item.
/// @param tb    Toolbar to add to; may be NULL (returns NULL).
/// @param id    Unique string identifier; may be NULL.
/// @param label Display text; may be NULL.
/// @param icon  Icon descriptor.
/// @param menu  vg_menu_t to clone and show as a popup; may be NULL (button acts as plain click).
/// @return      The new item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_dropdown(
    vg_toolbar_t *tb, const char *id, const char *label, vg_icon_t icon, struct vg_menu *menu) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_DROPDOWN, id);
    if (!item) {
        vg_icon_destroy(&icon);
        return NULL;
    }

    char *label_copy = label ? vg_strdup(label) : NULL;
    if (label && !label_copy) {
        vg_icon_destroy(&icon);
        free_item(item);
        return NULL;
    }
    item->label = label_copy;
    item->icon = icon;
    item->show_label = tb->show_labels;
    item->dropdown_menu = menu;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Append a visual separator line to the toolbar.
///
/// @details The noninteractive separator participates in primary-axis layout
///          and may be mirrored between actionable overflow entries.
/// @param tb Toolbar to add to; may be NULL (returns NULL).
/// @return   The new separator item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_separator(vg_toolbar_t *tb) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_SEPARATOR, NULL);
    if (!item)
        return NULL;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Append a flexible spacer that absorbs surplus space evenly with other spacers.
///
/// @details Spacers have no intrinsic extent and divide the remaining
///          primary-axis space during layout. They are omitted from overflow
///          menus.
/// @param tb Toolbar to add to; may be NULL (returns NULL).
/// @return   The new spacer item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_spacer(vg_toolbar_t *tb) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_SPACER, NULL);
    if (!item)
        return NULL;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Embed an arbitrary widget as a toolbar item; the toolbar arranges it but does NOT own it.
///
/// @details The widget is measured and centered in its allocated item
///          rectangle while directly visible. It is not represented in the
///          overflow popup and remains owned by its original lifecycle.
/// @param tb     Toolbar to add to; may be NULL (returns NULL).
/// @param id     Unique string identifier; may be NULL.
/// @param widget Custom widget to embed; may be NULL.
/// @return       The new wrapper item, or NULL on allocation failure.
vg_toolbar_item_t *vg_toolbar_add_widget(vg_toolbar_t *tb, const char *id, vg_widget_t *widget) {
    if (!tb)
        return NULL;
    if (tb->item_count == SIZE_MAX || !ensure_item_capacity(tb, tb->item_count + 1))
        return NULL;

    vg_toolbar_item_t *item = create_item(VG_TOOLBAR_ITEM_WIDGET, id);
    if (!item)
        return NULL;

    item->custom_widget = widget;
    item->owner = tb;

    tb->items[tb->item_count++] = item;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    return item;
}

/// @brief Remove the item at @p index: clear any hover/press/focus/dropdown
///        state referencing it, compact the items array, and retire the item
///        for deferred freeing.
/// @details Removal invalidates layout and the overflow model, dismisses any
///          active overflow popup, and synchronizes the toolbar-level tooltip.
/// @param tb Toolbar whose live array is modified.
/// @param index Zero-based live item index.
static void toolbar_remove_item_at(vg_toolbar_t *tb, size_t index) {
    if (!tb || index >= tb->item_count)
        return;

    if (tb->hovered_item == tb->items[index])
        tb->hovered_item = NULL;
    if (tb->pressed_item == tb->items[index])
        tb->pressed_item = NULL;
    if (tb->focused_index == (int)index)
        tb->focused_index = -1;
    else if (tb->focused_index > (int)index && !toolbar_focus_is_overflow(tb, tb->focused_index))
        tb->focused_index--;
    vg_toolbar_item_t *removed = tb->items[index];
    if (tb->dropdown_item == removed)
        toolbar_dismiss_dropdown_popup(tb);
    retire_item(tb, removed);
    memmove(&tb->items[index],
            &tb->items[index + 1],
            (tb->item_count - index - 1) * sizeof(vg_toolbar_item_t *));
    tb->item_count--;
    toolbar_sync_hover_tooltip(tb);
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    toolbar_dismiss_overflow_popup(tb);
}

/// @brief Remove the first live toolbar item matching an identifier.
///
/// @details The record is retired rather than immediately freed so external
///          stable handles can detect a tombstone safely.
/// @param tb Toolbar to modify; may be NULL (no-op).
/// @param id String identifier of the item to remove; may be NULL (no-op).
void vg_toolbar_remove_item(vg_toolbar_t *tb, const char *id) {
    if (!tb || !id)
        return;

    for (size_t i = 0; i < tb->item_count; i++) {
        if (tb->items[i]->id && strcmp(tb->items[i]->id, id) == 0) {
            toolbar_remove_item_at(tb, i);
            return;
        }
    }
}

/// @brief Remove an exact live item pointer from its owning toolbar.
/// @details Foreign and retired pointers are rejected. The matching record is
///          converted to a deferred-reclamation tombstone.
/// @param tb Toolbar expected to own @p item.
/// @param item Exact live item handle to remove.
void vg_toolbar_remove_item_ptr(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !vg_toolbar_item_is_live(item) || item->owner != tb)
        return;
    for (size_t i = 0; i < tb->item_count; i++) {
        if (tb->items[i] == item) {
            toolbar_remove_item_at(tb, i);
            return;
        }
    }
}

/// @brief Unlink and free one exact retained Toolbar item record.
/// @details The managed runtime invokes this only after the last stable wrapper is absent. Foreign
///          pointers are compared but never dereferenced.
/// @param tb Toolbar whose retired list is searched.
/// @param item Tombstone pointer identity to reclaim.
/// @return `true` when the exact retired record was found and freed.
bool vg_toolbar_reclaim_retired_item(vg_toolbar_t *tb, vg_toolbar_item_t *item) {
    if (!tb || !item)
        return false;
    vg_toolbar_item_t **link = &tb->retired_items;
    while (*link) {
        vg_toolbar_item_t *candidate = *link;
        if (candidate == item) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_item(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Look up a toolbar item by its string id.
///
/// @details Identifiers are compared exactly and the first matching live-array
///          entry is returned. The result is borrowed from the toolbar.
/// @param tb Toolbar to search; may be NULL (returns NULL).
/// @param id String identifier to match; may be NULL (returns NULL).
/// @return   Matching item pointer, or NULL if not found.
vg_toolbar_item_t *vg_toolbar_get_item(vg_toolbar_t *tb, const char *id) {
    if (!tb || !id)
        return NULL;

    for (size_t i = 0; i < tb->item_count; i++) {
        if (tb->items[i]->id && strcmp(tb->items[i]->id, id) == 0) {
            return tb->items[i];
        }
    }
    return NULL;
}

/// @brief Enable or disable a toolbar item, marking the overflow popup dirty and requesting
/// repaint.
///
/// @details A no-op assignment produces no invalidation. Disabling an item
///          removes it from keyboard traversal and prevents activation without
///          changing its checked state or callbacks.
/// @param item    Item to modify; may be NULL (no-op).
/// @param enabled true to enable, false to disable (item paints with disabled colour and ignores
/// input).
/// @brief Show or hide a toolbar item without removing it (ADR 0220).
/// @details Hidden items report zero extent, so layout, painting, hit-testing,
///          focus traversal, and the overflow popup all skip them and their
///          spacing. Toggling visibility invalidates layout because the
///          toolbar's content extent changes.
/// @param item Toolbar item to show or hide; non-live handles are ignored.
/// @param visible True to show the item, false to hide it.
void vg_toolbar_item_set_visible(vg_toolbar_item_t *item, bool visible) {
    if (!vg_toolbar_item_is_live(item))
        return;
    if (item->visible == visible)
        return;
    item->visible = visible;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_layout = true;
        item->owner->base.needs_paint = true;
        if (item->owner->hovered_item == item)
            toolbar_sync_hover_tooltip(item->owner);
    }
}

/// @brief Report whether a toolbar item is currently shown.
/// @param item Candidate toolbar item; non-live handles report false.
/// @return True when the live item participates in layout and interaction.
bool vg_toolbar_item_is_visible(const vg_toolbar_item_t *item) {
    if (!vg_toolbar_item_is_live(item))
        return false;
    return item->visible;
}

/// @brief Report a directly visible item's on-screen rectangle (ADR 0220).
/// @details Combines the toolbar widget's screen bounds with the arranged
///          local rectangle so callers can anchor popups or drive pointer
///          input at the item, mirroring MenuItem geometry (ADR 0219).
/// @param item Toolbar item to locate; non-live handles report false.
/// @param out_x Optional destination for the on-screen left edge.
/// @param out_y Optional destination for the on-screen top edge.
/// @param out_w Optional destination for the item width.
/// @param out_h Optional destination for the item height.
/// @return True when the item is live, visible, and not overflowed.
bool vg_toolbar_item_screen_rect(
    vg_toolbar_item_t *item, float *out_x, float *out_y, float *out_w, float *out_h) {
    if (out_x)
        *out_x = 0.0f;
    if (out_y)
        *out_y = 0.0f;
    if (out_w)
        *out_w = 0.0f;
    if (out_h)
        *out_h = 0.0f;
    if (!vg_toolbar_item_is_live(item) || !item->owner || !item->visible)
        return false;
    float local_x = 0.0f, local_y = 0.0f, local_w = 0.0f, local_h = 0.0f;
    if (!toolbar_get_item_rect(item->owner, item, &local_x, &local_y, &local_w, &local_h))
        return false;
    float origin_x = 0.0f, origin_y = 0.0f;
    vg_widget_get_screen_bounds(&item->owner->base, &origin_x, &origin_y, NULL, NULL);
    if (out_x)
        *out_x = origin_x + local_x;
    if (out_y)
        *out_y = origin_y + local_y;
    if (out_w)
        *out_w = local_w;
    if (out_h)
        *out_h = local_h;
    return true;
}

void vg_toolbar_item_set_enabled(vg_toolbar_item_t *item, bool enabled) {
    if (!vg_toolbar_item_is_live(item))
        return;
    if (item->enabled == enabled)
        return;
    item->enabled = enabled;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_paint = true;
        if (item->owner->hovered_item == item)
            toolbar_sync_hover_tooltip(item->owner);
    }
}

/// @brief Set the checked state of a toggle item, updating the overflow popup and requesting
/// repaint.
///
/// @details The state field is updated for any live item record, though visual
///          checked treatment and toggle callbacks apply to toggle items.
/// @param item    Toggle item to modify; may be NULL (no-op).
/// @param checked New checked state.
void vg_toolbar_item_set_checked(vg_toolbar_item_t *item, bool checked) {
    if (!vg_toolbar_item_is_live(item))
        return;
    if (item->checked == checked)
        return;
    item->checked = checked;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_paint = true;
        if (item->owner->hovered_item == item)
            toolbar_sync_hover_tooltip(item->owner);
    }
}

/// @brief Set the tooltip string of a toolbar item, also used as its label in the overflow menu
/// when no label is set.
///
/// @details The new string is copied before existing storage is released, so
///          allocation failure leaves the previous value intact. If the item
///          is currently hovered, the toolbar's mirrored widget tooltip is
///          synchronized immediately.
/// @param item    Item to modify; may be NULL (no-op).
/// @param tooltip Tooltip string, duplicated internally; may be NULL to clear.
void vg_toolbar_item_set_tooltip(vg_toolbar_item_t *item, const char *tooltip) {
    if (!vg_toolbar_item_is_live(item))
        return;
    if ((!item->tooltip && (!tooltip || tooltip[0] == '\0')) ||
        (item->tooltip && tooltip && strcmp(item->tooltip, tooltip) == 0))
        return;
    char *copy = tooltip ? vg_strdup(tooltip) : NULL;
    if (tooltip && !copy)
        return;
    free(item->tooltip);
    item->tooltip = copy;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_paint = true;
        if (item->owner->hovered_item == item)
            toolbar_sync_hover_tooltip(item->owner);
    }
}

/// @brief Replace the label text of a toolbar item, invalidating layout.
///
/// @details The input is duplicated before the old label is released.
///          Allocation failure therefore preserves the previous label.
/// @param item Item to modify; may be NULL (no-op).
/// @param text New label string, duplicated internally; may be NULL to clear.
void vg_toolbar_item_set_text(vg_toolbar_item_t *item, const char *text) {
    if (!vg_toolbar_item_is_live(item))
        return;
    if ((!item->label && (!text || text[0] == '\0')) ||
        (item->label && text && strcmp(item->label, text) == 0))
        return;
    char *copy = text ? vg_strdup(text) : NULL;
    if (text && !copy)
        return;
    free(item->label);
    item->label = copy;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_layout = true;
        item->owner->base.needs_paint = true;
    }
}

/// @brief Replace the icon of a toolbar item, destroying the previous icon and invalidating layout.
///
/// @details Ownership of @p icon always transfers to this function. If @p item
///          is stale, the incoming icon is destroyed instead of being stored.
/// @param item Item to modify; may be NULL (no-op).
/// @param icon New icon descriptor (ownership transfers; use VG_ICON_NONE to clear).
void vg_toolbar_item_set_icon(vg_toolbar_item_t *item, vg_icon_t icon) {
    if (!vg_toolbar_item_is_live(item)) {
        vg_icon_destroy(&icon);
        return;
    }
    vg_icon_destroy(&item->icon);
    item->icon = icon;
    if (item->owner) {
        item->owner->overflow_popup_dirty = true;
        item->owner->base.needs_layout = true;
        item->owner->base.needs_paint = true;
    }
}

/// @brief Set the icon size for all items in the toolbar, invalidating layout.
///
/// @details The stored enum is interpreted during measurement and painting;
///          actual dimensions are additionally multiplied by theme UI scale.
/// @param tb   Toolbar to modify; may be NULL (no-op).
/// @param size VG_TOOLBAR_ICONS_SMALL (16px), _MEDIUM (24px), or _LARGE (32px) before HiDPI
/// scaling.
void vg_toolbar_set_icon_size(vg_toolbar_t *tb, vg_toolbar_icon_size_t size) {
    if (!tb)
        return;
    tb->icon_size = size;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
}

/// @brief Toggle label visibility on all current and future items, invalidating layout.
///
/// @details The toolbar default and every existing item's `show_label` flag are
///          updated together so subsequently added and current items remain
///          consistent.
/// @param tb   Toolbar to modify; may be NULL (no-op).
/// @param show true to display item labels beside their icons.
void vg_toolbar_set_show_labels(vg_toolbar_t *tb, bool show) {
    if (!tb)
        return;
    tb->show_labels = show;
    for (size_t i = 0; i < tb->item_count; i++) {
        tb->items[i]->show_label = show;
    }
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
}

/// @brief Set the font and size used for item labels and the overflow/dropdown popups.
///
/// @details The font is borrowed. A nonpositive size selects the theme's small
///          typography size, and an already-created overflow popup receives the
///          new font immediately.
/// @param tb   Toolbar to modify; may be NULL (no-op).
/// @param font Font to use; NULL falls back to no text rendering.
/// @param size Point size; ≤0 defaults to theme->typography.size_small.
void vg_toolbar_set_font(vg_toolbar_t *tb, vg_font_t *font, float size) {
    if (!tb)
        return;
    tb->font = font;
    tb->font_size = size > 0 ? size : vg_theme_get_current()->typography.size_small;
    tb->base.needs_layout = true;
    tb->overflow_popup_dirty = true;
    if (tb->overflow_popup)
        vg_contextmenu_set_font(tb->overflow_popup, tb->font, tb->font_size);
}

//=============================================================================
// Icon Helpers
//=============================================================================

/// @brief Create a VG_ICON_GLYPH icon from a Unicode codepoint, drawn with the toolbar's font.
///
/// @details The descriptor stores the scalar value directly and owns no heap
///          memory. Known toolbar action glyphs may be substituted by the
///          vector-icon mapping during painting.
/// @param codepoint Unicode scalar value.
/// @return          Initialised icon descriptor (stack value; no allocation).
vg_icon_t vg_icon_from_glyph(uint32_t codepoint) {
    vg_icon_t icon = {0};
    icon.type = VG_ICON_GLYPH;
    icon.data.glyph = codepoint;
    return icon;
}

/// @brief Create a VG_ICON_IMAGE icon by copying w×h RGBA pixels from rgba.
///
/// @details Dimensions are checked for multiplication overflow before a
///          four-byte-per-pixel allocation is made. The result is independent
///          of the caller's input buffer.
/// @param rgba Pointer to w×h×4 bytes of RGBA data to copy; may be NULL (returns VG_ICON_NONE).
/// @param w    Image width in pixels; must be > 0.
/// @param h    Image height in pixels; must be > 0.
/// @return     Icon with a heap-allocated pixel copy, or VG_ICON_NONE on allocation failure.
vg_icon_t vg_icon_from_pixels(uint8_t *rgba, uint32_t w, uint32_t h) {
    vg_icon_t icon = {0};
    if (!rgba || w == 0 || h == 0)
        return icon;

    if ((size_t)w > SIZE_MAX / (size_t)h)
        return icon;
    size_t pixel_count = (size_t)w * (size_t)h;
    if (pixel_count > SIZE_MAX / 4)
        return icon;

    icon.type = VG_ICON_IMAGE;
    size_t size = pixel_count * 4;
    icon.data.image.pixels = malloc(size);
    if (icon.data.image.pixels) {
        memcpy(icon.data.image.pixels, rgba, size);
        icon.data.image.width = w;
        icon.data.image.height = h;
    } else {
        icon.type = VG_ICON_NONE;
    }
    return icon;
}

/// @brief Create a VG_ICON_PATH icon referencing an image file by path (path is duplicated).
///
/// @details This constructor stores only the path; it does not read or decode
///          the referenced image file.
/// @param path File-system path to the image; may be NULL (returns VG_ICON_NONE).
/// @return     Icon with a heap-duplicated path string, or VG_ICON_NONE on allocation failure.
vg_icon_t vg_icon_from_file(const char *path) {
    vg_icon_t icon = {0};
    if (!path)
        return icon;

    icon.type = VG_ICON_PATH;
    icon.data.path = vg_strdup(path);
    if (!icon.data.path) {
        icon.type = VG_ICON_NONE;
    }
    return icon;
}

/// @brief Create a VG_ICON_VECTOR icon referencing a named scalable icon.
///
/// @details The integer registry identifier is stored by value and no resource
///          ownership is acquired.
/// @param vector_id Icon id from vg_icon_vector_find; negative returns VG_ICON_NONE.
/// @return          Vector icon reference (no heap allocation).
vg_icon_t vg_icon_from_vector(int32_t vector_id) {
    vg_icon_t icon = {0};
    if (vector_id < 0)
        return icon;
    icon.type = VG_ICON_VECTOR;
    icon.data.vector_id = vector_id;
    return icon;
}

/// @brief Deep-copy an icon descriptor, duplicating heap allocations for IMAGE and PATH types.
///
/// @details Glyph and vector descriptors are copied by value. Unknown or empty
///          icon types produce `VG_ICON_NONE`.
/// @param icon Source icon; may be NULL (returns VG_ICON_NONE).
/// @return     Independent copy; caller owns any allocations.
vg_icon_t vg_icon_clone(const vg_icon_t *icon) {
    vg_icon_t copy = {0};
    if (!icon)
        return copy;

    switch (icon->type) {
        case VG_ICON_GLYPH:
            return vg_icon_from_glyph(icon->data.glyph);
        case VG_ICON_IMAGE:
            return vg_icon_from_pixels(
                icon->data.image.pixels, icon->data.image.width, icon->data.image.height);
        case VG_ICON_PATH:
            return vg_icon_from_file(icon->data.path);
        case VG_ICON_VECTOR:
            return vg_icon_from_vector(icon->data.vector_id);
        default:
            return copy;
    }
}

/// @brief Free heap allocations inside icon and reset its type to VG_ICON_NONE.
///
/// @details Image pixel buffers and path strings are owned by their descriptor;
///          glyph and vector variants contain no allocation.
/// @param icon Icon to clear; may be NULL (no-op).
void vg_icon_destroy(vg_icon_t *icon) {
    if (!icon)
        return;

    switch (icon->type) {
        case VG_ICON_IMAGE:
            free(icon->data.image.pixels);
            break;
        case VG_ICON_PATH:
            free(icon->data.path);
            break;
        default:
            break;
    }
    icon->type = VG_ICON_NONE;
}
