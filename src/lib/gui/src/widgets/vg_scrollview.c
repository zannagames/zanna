//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_scrollview.c
// Purpose: ScrollView widget implementation — scrollable container with
//          optional horizontal/vertical scrollbars, mouse-wheel handling,
//          explicit content-size override, and scroll-to-widget support.
// Key invariants:
//   - scroll_x and scroll_y are always clamped by clamp_scroll to [0, content-viewport].
//   - Scrollbar visibility (show_h/v_scrollbar) is updated on every layout pass.
//   - Descendant reveal requests survive zero-sized intermediate layouts and
//     are guarded by the target's immutable live-widget ID.
// Ownership/Lifetime:
//   - Children are owned by the base widget's child list; destroyed with the parent.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h,
//        docs/adr/0165-scrollview-descendant-reveal.md
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements scroll-container measurement, clipped subtree painting,
///        scrollbar interaction, smooth scrolling, and descendant reveal.
/// @details Child geometry remains content-relative while paint temporarily
///          promotes coordinates to screen space. Pending reveal requests carry
///          immutable widget IDs so deferred layout cannot act on reused handles.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void scrollview_destroy(vg_widget_t *widget);
static void scrollview_measure(vg_widget_t *widget, float available_width, float available_height);
static void scrollview_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void scrollview_paint(vg_widget_t *widget, void *canvas);
static bool scrollview_handle_event(vg_widget_t *widget, vg_event_t *event);
static void scrollview_render_normal_subtree(vg_widget_t *widget,
                                             void *canvas,
                                             float parent_abs_x,
                                             float parent_abs_y);
static void scrollview_render_overlay_subtree(vg_widget_t *widget,
                                              void *canvas,
                                              float parent_abs_x,
                                              float parent_abs_y);
static void scrollview_recompute_content_size(vg_scrollview_t *scroll,
                                              float available_width,
                                              float available_height);
static void scrollview_get_viewport_size(const vg_scrollview_t *scroll,
                                         float *out_width,
                                         float *out_height);
static void scrollview_arrange_children(vg_scrollview_t *scroll, float content_area_width);
static bool scrollview_is_live_descendant(const vg_scrollview_t *scroll,
                                          const vg_widget_t *child,
                                          uint64_t child_id);
static bool scrollview_apply_descendant_reveal(vg_scrollview_t *scroll, vg_widget_t *child);

//=============================================================================
// ScrollView VTable
//=============================================================================

static vg_widget_vtable_t g_scrollview_vtable = {.destroy = scrollview_destroy,
                                                 .measure = scrollview_measure,
                                                 .arrange = scrollview_arrange,
                                                 .paint = scrollview_paint,
                                                 .handle_event = scrollview_handle_event,
                                                 .can_focus = NULL,
                                                 .on_focus = NULL};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Returns true if @p widget renders its own children (ScrollView or custom widget with
/// paint_overlay), suppressing recursive subtree painting.
/// @param widget Widget whose paint ownership policy is inspected.
/// @return `true` when recursive traversal must stop after painting the widget.
static bool scrollview_widget_paints_children_internally(const vg_widget_t *widget) {
    if (!widget)
        return false;
    if (widget->type == VG_WIDGET_SCROLLVIEW)
        return true;
    return widget->type == VG_WIDGET_CUSTOM && widget->vtable && widget->vtable->paint_overlay;
}

/// @brief Calls vg_widget_measure on every visible child with the given available dimensions.
/// @param scroll Scroll view whose direct children are measured.
/// @param available_width Width offered to every child.
/// @param available_height Height offered to every child.
static void scrollview_measure_children(vg_scrollview_t *scroll,
                                        float available_width,
                                        float available_height) {
    vg_widget_t *base = &scroll->base;
    VG_FOREACH_VISIBLE_CHILD(base, child) {
        vg_widget_measure(child, available_width, available_height);
    }
}

/// @brief Measures all children and computes the total content extent, respecting explicit
/// overrides for each axis.
/// @param scroll Scroll view whose content dimensions are updated.
/// @param available_width Width offered for child measurement.
/// @param available_height Height offered for child measurement.
static void calculate_content_size(vg_scrollview_t *scroll,
                                   float available_width,
                                   float available_height) {
    vg_widget_t *base = &scroll->base;
    float auto_width = 0.0f;
    float auto_height = 0.0f;
    float flow_y = 0.0f;

    scrollview_measure_children(scroll, available_width, available_height);

    VG_FOREACH_VISIBLE_CHILD(base, child) {
        float right =
            child->layout.margin_left + child->measured_width + child->layout.margin_right;
        float top = flow_y + child->layout.margin_top;
        float bottom = top + child->measured_height + child->layout.margin_bottom;

        if (right > auto_width)
            auto_width = right;
        if (bottom > auto_height)
            auto_height = bottom;

        flow_y = bottom;
    }

    scroll->content_width =
        scroll->has_explicit_content_width ? scroll->explicit_content_width : auto_width;
    scroll->content_height =
        scroll->has_explicit_content_height ? scroll->explicit_content_height : auto_height;
}

/// @brief Returns the scrollbar thumb length proportional to visible/total content, clamped to the
/// theme minimum.
/// @param track_size Full scrollbar track length.
/// @param content_size Total scrollable content extent.
/// @param viewport_size Visible content extent.
/// @return Thumb length constrained to the track and theme minimum.
static float scrollbar_thumb_size(float track_size, float content_size, float viewport_size) {
    if (track_size <= 0.0f || content_size <= 0.0f || viewport_size <= 0.0f)
        return track_size;

    vg_theme_t *theme = vg_theme_get_current();
    float visible_ratio = viewport_size / content_size;
    if (visible_ratio > 1.0f)
        visible_ratio = 1.0f;
    float thumb_size = track_size * visible_ratio;
    if (thumb_size < theme->scrollbar.min_thumb_size)
        thumb_size = theme->scrollbar.min_thumb_size;
    if (thumb_size > track_size)
        thumb_size = track_size;
    return thumb_size;
}

/// @brief Converts a scroll position to the thumb's pixel offset within its track.
/// @param scroll_pos Current content scroll offset.
/// @param scroll_range Maximum content scroll offset.
/// @param track_size Full track length.
/// @param thumb_size Thumb length.
/// @return Pixel offset within the thumb's travel range.
static float scrollbar_thumb_offset(float scroll_pos,
                                    float scroll_range,
                                    float track_size,
                                    float thumb_size) {
    float thumb_travel = track_size - thumb_size;
    if (scroll_range <= 0.0f || thumb_travel <= 0.0f)
        return 0.0f;
    return (scroll_pos / scroll_range) * thumb_travel;
}

/// @brief Converts a thumb drag position back to a scroll offset, clamping thumb_offset to the
/// valid travel range.
/// @param thumb_offset Requested pixel offset within the track.
/// @param track_size Full track length.
/// @param thumb_size Thumb length.
/// @param scroll_range Maximum content scroll offset.
/// @return Corresponding clamped content scroll offset.
static float scrollbar_scroll_from_thumb(float thumb_offset,
                                         float track_size,
                                         float thumb_size,
                                         float scroll_range) {
    float thumb_travel = track_size - thumb_size;
    if (scroll_range <= 0.0f || thumb_travel <= 0.0f)
        return 0.0f;
    if (thumb_offset < 0.0f)
        thumb_offset = 0.0f;
    if (thumb_offset > thumb_travel)
        thumb_offset = thumb_travel;
    return (thumb_offset / thumb_travel) * scroll_range;
}

/// @brief Clamps scroll_x and scroll_y to [0, content_size - viewport_size], zeroing axes not
/// enabled by direction flags.
/// @param scroll Scroll view whose offsets are normalized.
static void clamp_scroll(vg_scrollview_t *scroll) {
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    scrollview_get_viewport_size(scroll, &viewport_width, &viewport_height);

    if (!(scroll->direction & VG_SCROLL_HORIZONTAL))
        scroll->scroll_x = 0.0f;
    if (!(scroll->direction & VG_SCROLL_VERTICAL))
        scroll->scroll_y = 0.0f;

    float max_scroll_x = scroll->content_width - viewport_width;
    float max_scroll_y = scroll->content_height - viewport_height;

    if (max_scroll_x < 0)
        max_scroll_x = 0;
    if (max_scroll_y < 0)
        max_scroll_y = 0;

    if (scroll->scroll_x < 0)
        scroll->scroll_x = 0;
    if (scroll->scroll_y < 0)
        scroll->scroll_y = 0;
    if (scroll->scroll_x > max_scroll_x)
        scroll->scroll_x = max_scroll_x;
    if (scroll->scroll_y > max_scroll_y)
        scroll->scroll_y = max_scroll_y;
}

/// @brief Writes the usable viewport dimensions (widget size minus visible scrollbar gutters) into
/// @p out_width and @p out_height.
/// @param scroll Scroll view to inspect.
/// @param out_width Optional destination for non-negative viewport width.
/// @param out_height Optional destination for non-negative viewport height.
static void scrollview_get_viewport_size(const vg_scrollview_t *scroll,
                                         float *out_width,
                                         float *out_height) {
    if (!scroll) {
        if (out_width)
            *out_width = 0.0f;
        if (out_height)
            *out_height = 0.0f;
        return;
    }

    float width = scroll->base.width;
    float height = scroll->base.height;
    if (scroll->show_v_scrollbar)
        width -= scroll->scrollbar_width;
    if (scroll->show_h_scrollbar)
        height -= scroll->scrollbar_width;
    if (width < 0.0f)
        width = 0.0f;
    if (height < 0.0f)
        height = 0.0f;

    if (out_width)
        *out_width = width;
    if (out_height)
        *out_height = height;
}

/// @brief Delegates to calculate_content_size to update content_width/height from child extents or
/// explicit overrides.
/// @param scroll Scroll view to recompute.
/// @param available_width Width offered for child measurement.
/// @param available_height Height offered for child measurement.
static void scrollview_recompute_content_size(vg_scrollview_t *scroll,
                                              float available_width,
                                              float available_height) {
    if (!scroll)
        return;
    calculate_content_size(scroll, available_width, available_height);
}

/// @brief Arrange direct content children using the current scroll offsets.
/// @param scroll Scroll view whose visible children are arranged.
/// @param content_area_width Usable width excluding a vertical scrollbar.
static void scrollview_arrange_children(vg_scrollview_t *scroll, float content_area_width) {
    if (!scroll)
        return;

    vg_widget_t *widget = &scroll->base;
    float flow_y = 0.0f;
    VG_FOREACH_VISIBLE_CHILD(widget, child) {
        float child_x = child->layout.margin_left - scroll->scroll_x;
        float child_y = flow_y + child->layout.margin_top - scroll->scroll_y;
        float child_w = child->measured_width;
        float child_h = child->measured_height;
        // Stretch content to AT LEAST the viewport width so vertical-scroll
        // content fills across (settings panels, lists); content that is
        // intrinsically wider keeps its width so it can scroll horizontally.
        // This MUST match the width used when the content was measured in
        // calculate_content_size() (= content_area_width), otherwise
        // word-wrapped children wrap to a different line count between measure
        // and arrange, producing overlapping text and a wrong content height.
        float fill_w = content_area_width - child->layout.margin_left - child->layout.margin_right;
        if (fill_w < 0.0f)
            fill_w = 0.0f;
        if (child_w < fill_w)
            child_w = fill_w;
        vg_widget_arrange(child, child_x, child_y, child_w, child_h);
        flow_y += child->layout.margin_top + child_h + child->layout.margin_bottom;
    }
}

/// @brief Validate a pending non-owning descendant pointer against liveness and ID reuse.
/// @param scroll Scroll view expected to contain @p child.
/// @param child Borrowed target handle.
/// @param child_id Immutable live-widget ID captured with the handle.
/// @return `true` when the complete parent chain remains live and reaches the
///         scroll view.
static bool scrollview_is_live_descendant(const vg_scrollview_t *scroll,
                                          const vg_widget_t *child,
                                          uint64_t child_id) {
    if (!scroll || !child || child_id == 0 || !vg_widget_is_live(child) || child->id != child_id)
        return false;

    const vg_widget_t *base = &scroll->base;
    const vg_widget_t *parent = child->parent;
    while (parent && parent != base) {
        if (!vg_widget_is_live(parent))
            return false;
        parent = parent->parent;
    }
    return parent == base;
}

/// @brief Resolve one validated descendant against the current content viewport.
/// @param scroll Scroll view whose offsets may change.
/// @param child Live descendant to reveal.
/// @return `true` when either scroll offset changed.
static bool scrollview_apply_descendant_reveal(vg_scrollview_t *scroll, vg_widget_t *child) {
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    scrollview_get_viewport_size(scroll, &viewport_width, &viewport_height);

    float child_x = 0.0f;
    float child_y = 0.0f;
    for (vg_widget_t *widget = child; widget && widget != &scroll->base; widget = widget->parent) {
        child_x += widget->x;
        child_y += widget->y;
    }
    child_x += scroll->scroll_x;
    child_y += scroll->scroll_y;

    float old_x = scroll->scroll_x;
    float old_y = scroll->scroll_y;
    if (child_x < scroll->scroll_x) {
        scroll->scroll_x = child_x;
    } else if (child_x + child->width > scroll->scroll_x + viewport_width) {
        scroll->scroll_x = child_x + child->width - viewport_width;
    }
    if (child_y < scroll->scroll_y) {
        scroll->scroll_y = child_y;
    } else if (child_y + child->height > scroll->scroll_y + viewport_height) {
        scroll->scroll_y = child_y + child->height - viewport_height;
    }

    clamp_scroll(scroll);
    return scroll->scroll_x != old_x || scroll->scroll_y != old_y;
}

//=============================================================================
// ScrollView Implementation
//=============================================================================

/// @brief Create a scroll view widget.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_scrollview_t, or NULL on allocation failure.
vg_scrollview_t *vg_scrollview_create(vg_widget_t *parent) {
    vg_scrollview_t *scroll = calloc(1, sizeof(vg_scrollview_t));
    if (!scroll)
        return NULL;

    // Initialize base widget
    vg_widget_init(&scroll->base, VG_WIDGET_SCROLLVIEW, &g_scrollview_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize scrollview-specific fields
    scroll->scroll_x = 0;
    scroll->scroll_y = 0;
    scroll->content_width = 0;  // Auto
    scroll->content_height = 0; // Auto
    scroll->explicit_content_width = 0.0f;
    scroll->explicit_content_height = 0.0f;
    scroll->has_explicit_content_width = false;
    scroll->has_explicit_content_height = false;
    scroll->direction = VG_SCROLL_BOTH;

    // Scrollbars
    scroll->show_h_scrollbar = true;
    scroll->show_v_scrollbar = true;
    scroll->auto_hide_scrollbars = true;
    scroll->scrollbar_width = theme->scrollbar.width;

    // Scrollbar appearance
    scroll->track_color =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.5f);
    scroll->thumb_color =
        vg_color_blend(theme->colors.bg_tertiary, theme->colors.border_primary, 0.35f);
    scroll->thumb_hover_color =
        vg_color_blend(theme->colors.bg_hover, theme->colors.accent_primary, 0.2f);

    // State
    scroll->h_scrollbar_hovered = false;
    scroll->v_scrollbar_hovered = false;
    scroll->h_scrollbar_dragging = false;
    scroll->v_scrollbar_dragging = false;
    scroll->drag_offset = 0;
    scroll->scroll_to_widget = NULL;
    scroll->scroll_to_widget_id = 0;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &scroll->base);
    }

    return scroll;
}

/// @brief VTable destroy: releases input capture if this widget currently holds it.
static void scrollview_destroy(vg_widget_t *widget) {
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
}

/// @brief VTable measure: claims all available space as measured size, then applies layout
/// constraints.
static void scrollview_measure(vg_widget_t *widget, float available_width, float available_height) {
    // ScrollView takes all available space by default
    widget->measured_width = available_width > 0 ? available_width : 200;
    widget->measured_height = available_height > 0 ? available_height : 200;

    vg_widget_apply_constraints(widget);
}

/// @brief VTable arrange: positions the widget, resolves scrollbar visibility via a 3-pass
/// convergence loop, and stacks children vertically offset by the current scroll position.
static void scrollview_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_scrollview_t *scroll = (vg_scrollview_t *)widget;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    float content_area_width = width;
    float content_area_height = height;

    scrollview_recompute_content_size(scroll, content_area_width, content_area_height);

    // Determine if scrollbars are needed
    bool needs_h = false;
    bool needs_v = false;

    if (scroll->auto_hide_scrollbars) {
        for (int pass = 0; pass < 3; pass++) {
            float pass_width = width - (needs_v ? scroll->scrollbar_width : 0.0f);
            float pass_height = height - (needs_h ? scroll->scrollbar_width : 0.0f);
            if (pass_width < 0.0f)
                pass_width = 0.0f;
            if (pass_height < 0.0f)
                pass_height = 0.0f;

            scrollview_recompute_content_size(scroll, pass_width, pass_height);

            bool new_h =
                (scroll->direction & VG_SCROLL_HORIZONTAL) && scroll->content_width > pass_width;
            bool new_v =
                (scroll->direction & VG_SCROLL_VERTICAL) && scroll->content_height > pass_height;
            if (new_h == needs_h && new_v == needs_v)
                break;
            needs_h = new_h;
            needs_v = new_v;
        }
        scroll->show_h_scrollbar = needs_h;
        scroll->show_v_scrollbar = needs_v;
    } else {
        needs_h = scroll->show_h_scrollbar;
        needs_v = scroll->show_v_scrollbar;
    }

    content_area_width = width - (scroll->show_v_scrollbar ? scroll->scrollbar_width : 0.0f);
    content_area_height = height - (scroll->show_h_scrollbar ? scroll->scrollbar_width : 0.0f);
    if (content_area_width < 0.0f)
        content_area_width = 0.0f;
    if (content_area_height < 0.0f)
        content_area_height = 0.0f;

    scrollview_recompute_content_size(scroll, content_area_width, content_area_height);

    // Clamp scroll position
    clamp_scroll(scroll);

    scrollview_arrange_children(scroll, content_area_width);

    // ScrollTo can be issued while an enclosing split pane is collapsed. The
    // immediate best effort preserves existing synchronous behavior; this
    // second resolution uses settled descendant and viewport geometry.
    vg_widget_t *reveal_target = scroll->scroll_to_widget;
    uint64_t reveal_target_id = scroll->scroll_to_widget_id;
    scroll->scroll_to_widget = NULL;
    scroll->scroll_to_widget_id = 0;
    if (scrollview_is_live_descendant(scroll, reveal_target, reveal_target_id) &&
        scrollview_apply_descendant_reveal(scroll, reveal_target)) {
        scrollview_arrange_children(scroll, content_area_width);
    }
}

/// @brief VTable paint: clips children to the content viewport, recurses into normal and overlay
/// subtrees, then draws vertical and horizontal scrollbar tracks and thumbs.
static void scrollview_paint(vg_widget_t *widget, void *canvas) {
    vg_scrollview_t *scroll = (vg_scrollview_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;

    // Draw scrollbars
    float content_area_width =
        widget->width - (scroll->show_v_scrollbar ? scroll->scrollbar_width : 0);
    float content_area_height =
        widget->height - (scroll->show_h_scrollbar ? scroll->scrollbar_width : 0);

    int32_t clip_x = (int32_t)widget->x;
    int32_t clip_y = (int32_t)widget->y;
    int32_t clip_w = (int32_t)content_area_width;
    int32_t clip_h = (int32_t)content_area_height;

    if (clip_w > 0 && clip_h > 0) {
        vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);
        VG_FOREACH_VISIBLE_CHILD(widget, child) {
            scrollview_render_normal_subtree(child, canvas, widget->x, widget->y);
            vgfx_set_clip(win, clip_x, clip_y, clip_w, clip_h);
        }
        vgfx_clear_clip(win);
    }

    // Overlay children such as dropdown panels and tooltips should escape the
    // scroll viewport clip so they can float above the content area.
    VG_FOREACH_VISIBLE_CHILD(widget, child) {
        scrollview_render_overlay_subtree(child, canvas, widget->x, widget->y);
    }

    // Vertical scrollbar
    if (scroll->show_v_scrollbar && scroll->content_height > content_area_height) {
        float track_x = widget->x + widget->width - scroll->scrollbar_width;
        float track_y = widget->y;
        float track_height = content_area_height;
        float thumb_height =
            scrollbar_thumb_size(track_height, scroll->content_height, content_area_height);
        float scroll_range = scroll->content_height - content_area_height;
        float thumb_y = track_y + scrollbar_thumb_offset(
                                      scroll->scroll_y, scroll_range, track_height, thumb_height);
        float inset = scroll->scrollbar_width > 9.0f ? 2.0f : 1.0f;

        // Draw track
        vgfx_fill_rect(win,
                       (int32_t)track_x,
                       (int32_t)track_y,
                       (int32_t)scroll->scrollbar_width,
                       (int32_t)track_height,
                       scroll->track_color);
        vgfx_rect(win,
                  (int32_t)track_x,
                  (int32_t)track_y,
                  (int32_t)scroll->scrollbar_width,
                  (int32_t)track_height,
                  theme->colors.border_secondary);

        // Draw thumb
        uint32_t thumb_color = scroll->v_scrollbar_hovered || scroll->v_scrollbar_dragging
                                   ? scroll->thumb_hover_color
                                   : scroll->thumb_color;
        float thumb_w = scroll->scrollbar_width - inset * 2.0f;
        vg_draw_round_rect_fill(
            win, track_x + inset, thumb_y, thumb_w, thumb_height, thumb_w * 0.5f, thumb_color);
    }

    // Horizontal scrollbar
    if (scroll->show_h_scrollbar && scroll->content_width > content_area_width) {
        float track_x = widget->x;
        float track_y = widget->y + widget->height - scroll->scrollbar_width;
        float track_width = content_area_width;
        float thumb_width =
            scrollbar_thumb_size(track_width, scroll->content_width, content_area_width);
        float scroll_range = scroll->content_width - content_area_width;
        float thumb_x = track_x + scrollbar_thumb_offset(
                                      scroll->scroll_x, scroll_range, track_width, thumb_width);
        float inset = scroll->scrollbar_width > 9.0f ? 2.0f : 1.0f;

        vgfx_window_t win_h = (vgfx_window_t)canvas;

        // Draw track
        vgfx_fill_rect(win_h,
                       (int32_t)track_x,
                       (int32_t)track_y,
                       (int32_t)track_width,
                       (int32_t)scroll->scrollbar_width,
                       scroll->track_color);
        vgfx_rect(win_h,
                  (int32_t)track_x,
                  (int32_t)track_y,
                  (int32_t)track_width,
                  (int32_t)scroll->scrollbar_width,
                  theme->colors.border_secondary);

        // Draw thumb
        uint32_t thumb_color = scroll->h_scrollbar_hovered || scroll->h_scrollbar_dragging
                                   ? scroll->thumb_hover_color
                                   : scroll->thumb_color;
        float thumb_h = scroll->scrollbar_width - inset * 2.0f;
        vg_draw_round_rect_fill(
            win_h, thumb_x, track_y + inset, thumb_width, thumb_h, thumb_h * 0.5f, thumb_color);
    }
}

/// @brief Recursively paints @p widget and its non-overlay descendants, temporarily promoting
/// coordinates to screen space for each paint call.
/// @param widget Subtree root to paint.
/// @param canvas Backend canvas supplied to paint hooks.
/// @param parent_abs_x Parent's absolute horizontal origin.
/// @param parent_abs_y Parent's absolute vertical origin.
static void scrollview_render_normal_subtree(vg_widget_t *widget,
                                             void *canvas,
                                             float parent_abs_x,
                                             float parent_abs_y) {
    if (!widget || !widget->visible)
        return;

    float abs_x = parent_abs_x + widget->x;
    float abs_y = parent_abs_y + widget->y;
    float rel_x = widget->x;
    float rel_y = widget->y;
    bool was_screen_space = widget->_paint_screen_space;
    widget->x = abs_x;
    widget->y = abs_y;
    widget->_paint_screen_space = true;

    if (widget->vtable && widget->vtable->paint)
        widget->vtable->paint(widget, canvas);

    widget->_paint_screen_space = was_screen_space;
    widget->x = rel_x;
    widget->y = rel_y;

    if (scrollview_widget_paints_children_internally(widget))
        return;

    VG_FOREACH_CHILD(widget, child) {
        scrollview_render_normal_subtree(child, canvas, abs_x, abs_y);
    }
}

/// @brief Recursively invokes paint_overlay on @p widget and its descendants, allowing overlays
/// (e.g. open dropdowns) to escape the scroll viewport clip.
/// @param widget Subtree root whose overlays are painted.
/// @param canvas Backend canvas supplied to overlay hooks.
/// @param parent_abs_x Parent's absolute horizontal origin.
/// @param parent_abs_y Parent's absolute vertical origin.
static void scrollview_render_overlay_subtree(vg_widget_t *widget,
                                              void *canvas,
                                              float parent_abs_x,
                                              float parent_abs_y) {
    if (!widget || !widget->visible)
        return;

    float abs_x = parent_abs_x + widget->x;
    float abs_y = parent_abs_y + widget->y;
    float rel_x = widget->x;
    float rel_y = widget->y;
    bool was_screen_space = widget->_paint_screen_space;
    widget->x = abs_x;
    widget->y = abs_y;
    widget->_paint_screen_space = true;

    if (widget->vtable && widget->vtable->paint_overlay)
        widget->vtable->paint_overlay(widget, canvas);

    widget->_paint_screen_space = was_screen_space;
    widget->x = rel_x;
    widget->y = rel_y;

    if (scrollview_widget_paints_children_internally(widget))
        return;

    VG_FOREACH_CHILD(widget, child) {
        scrollview_render_overlay_subtree(child, canvas, abs_x, abs_y);
    }
}

/// @brief VTable handle_event: handles mouse-wheel scrolling, scrollbar click-to-jump, thumb drag
/// initiation/tracking, and hover highlighting for both scrollbar axes.
static bool scrollview_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_scrollview_t *scroll = (vg_scrollview_t *)widget;

    if (event->type == VG_EVENT_MOUSE_WHEEL) {
        float delta_x = event->wheel.delta_x * 20.0f;
        float delta_y = event->wheel.delta_y * 20.0f;

        if (vg_smooth_scroll_effective()) {
            // Ease toward an accumulated wheel target; vg_scrollview_tick
            // advances the position every frame until it lands.
            float target_x = scroll->smooth_animating ? scroll->smooth_target_x : scroll->scroll_x;
            float target_y = scroll->smooth_animating ? scroll->smooth_target_y : scroll->scroll_y;
            if (scroll->direction & VG_SCROLL_HORIZONTAL)
                target_x -= delta_x;
            if (scroll->direction & VG_SCROLL_VERTICAL)
                target_y -= delta_y;
            float keep_x = scroll->scroll_x;
            float keep_y = scroll->scroll_y;
            scroll->scroll_x = target_x;
            scroll->scroll_y = target_y;
            clamp_scroll(scroll);
            scroll->smooth_target_x = scroll->scroll_x;
            scroll->smooth_target_y = scroll->scroll_y;
            scroll->scroll_x = keep_x;
            scroll->scroll_y = keep_y;
            if (scroll->smooth_target_x != scroll->scroll_x ||
                scroll->smooth_target_y != scroll->scroll_y) {
                scroll->smooth_animating = true;
                event->handled = true;
                return true;
            }
            scroll->smooth_animating = false;
            return false;
        }

        // Instant path (smooth scrolling off or reduced motion requested).
        float old_x = scroll->scroll_x;
        float old_y = scroll->scroll_y;
        if (scroll->direction & VG_SCROLL_HORIZONTAL) {
            scroll->scroll_x -= delta_x;
        }
        if (scroll->direction & VG_SCROLL_VERTICAL) {
            scroll->scroll_y -= delta_y;
        }

        clamp_scroll(scroll);
        if (scroll->scroll_x != old_x || scroll->scroll_y != old_y) {
            widget->needs_layout = true;
            widget->needs_paint = true;
            event->handled = true;
            return true;
        }
        return false;
    }

    if (event->type == VG_EVENT_MOUSE_DOWN) {
        // Check if clicking on scrollbar
        float content_area_width =
            widget->width - (scroll->show_v_scrollbar ? scroll->scrollbar_width : 0);
        float content_area_height =
            widget->height - (scroll->show_h_scrollbar ? scroll->scrollbar_width : 0);

        // Vertical scrollbar area
        if (scroll->show_v_scrollbar && event->mouse.x >= content_area_width &&
            event->mouse.x < widget->width && event->mouse.y >= 0.0f &&
            event->mouse.y < content_area_height) {
            float track_height = content_area_height;
            float thumb_height =
                scrollbar_thumb_size(track_height, scroll->content_height, content_area_height);
            float scroll_range = scroll->content_height - content_area_height;
            float thumb_y =
                scrollbar_thumb_offset(scroll->scroll_y, scroll_range, track_height, thumb_height);

            if (event->mouse.y >= thumb_y && event->mouse.y < thumb_y + thumb_height) {
                scroll->v_scrollbar_dragging = true;
                scroll->smooth_animating = false;
                scroll->drag_offset = event->mouse.y - thumb_y;
                vg_widget_set_input_capture(widget);
            } else if (scroll_range > 0.0f) {
                float target = event->mouse.y - thumb_height * 0.5f;
                scroll->scroll_y =
                    scrollbar_scroll_from_thumb(target, track_height, thumb_height, scroll_range);
                clamp_scroll(scroll);
                widget->needs_layout = true;
                widget->needs_paint = true;
            }
            return true;
        }

        // Horizontal scrollbar area
        if (scroll->show_h_scrollbar && event->mouse.y >= content_area_height &&
            event->mouse.y < widget->height && event->mouse.x >= 0.0f &&
            event->mouse.x < content_area_width) {
            float track_width = content_area_width;
            float thumb_width =
                scrollbar_thumb_size(track_width, scroll->content_width, content_area_width);
            float scroll_range = scroll->content_width - content_area_width;
            float thumb_x =
                scrollbar_thumb_offset(scroll->scroll_x, scroll_range, track_width, thumb_width);

            if (event->mouse.x >= thumb_x && event->mouse.x < thumb_x + thumb_width) {
                scroll->h_scrollbar_dragging = true;
                scroll->smooth_animating = false;
                scroll->drag_offset = event->mouse.x - thumb_x;
                vg_widget_set_input_capture(widget);
            } else if (scroll_range > 0.0f) {
                float target = event->mouse.x - thumb_width * 0.5f;
                scroll->scroll_x =
                    scrollbar_scroll_from_thumb(target, track_width, thumb_width, scroll_range);
                clamp_scroll(scroll);
                widget->needs_layout = true;
                widget->needs_paint = true;
            }
            return true;
        }

        (void)content_area_width;
        (void)content_area_height;
    }

    if (event->type == VG_EVENT_MOUSE_UP) {
        bool handled = scroll->v_scrollbar_dragging || scroll->h_scrollbar_dragging;
        scroll->v_scrollbar_dragging = false;
        scroll->h_scrollbar_dragging = false;
        if (vg_widget_get_input_capture() == widget)
            vg_widget_release_input_capture();
        return handled;
    }

    if (event->type == VG_EVENT_MOUSE_MOVE) {
        float content_area_height =
            widget->height - (scroll->show_h_scrollbar ? scroll->scrollbar_width : 0);
        float content_area_width =
            widget->width - (scroll->show_v_scrollbar ? scroll->scrollbar_width : 0);

        if (scroll->v_scrollbar_dragging) {
            float track_height = content_area_height;
            float thumb_height =
                scrollbar_thumb_size(track_height, scroll->content_height, content_area_height);
            float scroll_range = scroll->content_height - content_area_height;
            if (scroll_range > 0 && track_height > 0.0f) {
                float thumb_y = event->mouse.y - scroll->drag_offset;
                scroll->scroll_y =
                    scrollbar_scroll_from_thumb(thumb_y, track_height, thumb_height, scroll_range);
                clamp_scroll(scroll);
                widget->needs_layout = true;
                widget->needs_paint = true;
            }
            return true;
        }

        if (scroll->h_scrollbar_dragging) {
            float track_width = content_area_width;
            float thumb_width =
                scrollbar_thumb_size(track_width, scroll->content_width, content_area_width);
            float scroll_range = scroll->content_width - content_area_width;
            if (scroll_range > 0 && track_width > 0.0f) {
                float thumb_x = event->mouse.x - scroll->drag_offset;
                scroll->scroll_x =
                    scrollbar_scroll_from_thumb(thumb_x, track_width, thumb_width, scroll_range);
                clamp_scroll(scroll);
                widget->needs_layout = true;
                widget->needs_paint = true;
            }
            return true;
        }

        // Update hover state
        bool was_h_hover = scroll->h_scrollbar_hovered;
        bool was_v_hover = scroll->v_scrollbar_hovered;

        scroll->v_scrollbar_hovered = scroll->show_v_scrollbar &&
                                      event->mouse.x >= content_area_width &&
                                      event->mouse.x < widget->width && event->mouse.y >= 0.0f &&
                                      event->mouse.y < content_area_height;
        scroll->h_scrollbar_hovered = scroll->show_h_scrollbar &&
                                      event->mouse.y >= content_area_height &&
                                      event->mouse.y < widget->height && event->mouse.x >= 0.0f &&
                                      event->mouse.x < content_area_width;

        if (was_h_hover != scroll->h_scrollbar_hovered ||
            was_v_hover != scroll->v_scrollbar_hovered) {
            widget->needs_paint = true;
        }
    }

    return false;
}

//=============================================================================
// ScrollView API
//=============================================================================

/// @brief Programmatically set the scroll offset; values are clamped to valid range.
///
/// @param scroll The scroll view to update.
/// @param x      Horizontal scroll offset in pixels.
/// @param y      Vertical scroll offset in pixels.
void vg_scrollview_set_scroll(vg_scrollview_t *scroll, float x, float y) {
    if (!scroll)
        return;

    scroll->scroll_x = x;
    scroll->scroll_y = y;
    scroll->smooth_animating = false;
    clamp_scroll(scroll);
    scroll->base.needs_layout = true;
    scroll->base.needs_paint = true;
}

/// @brief Advance an active smooth-scroll interpolation.
/// @param scroll Scroll view with target offsets.
/// @param delta_ms Elapsed time in milliseconds.
/// @return `true` while either axis still requires future animation ticks.
bool vg_scrollview_tick(vg_scrollview_t *scroll, float delta_ms) {
    if (!scroll || !scroll->smooth_animating)
        return false;
    bool moving_x = vg_smooth_scroll_step(&scroll->scroll_x, scroll->smooth_target_x, delta_ms);
    bool moving_y = vg_smooth_scroll_step(&scroll->scroll_y, scroll->smooth_target_y, delta_ms);
    clamp_scroll(scroll);
    scroll->base.needs_layout = true;
    scroll->base.needs_paint = true;
    scroll->smooth_animating = moving_x || moving_y;
    return scroll->smooth_animating;
}

/// @brief Retrieve the current scroll offset.
///
/// @param scroll The scroll view to query.
/// @param out_x  Receives the horizontal scroll offset (may be NULL).
/// @param out_y  Receives the vertical scroll offset (may be NULL).
void vg_scrollview_get_scroll(vg_scrollview_t *scroll, float *out_x, float *out_y) {
    if (!scroll)
        return;

    if (out_x)
        *out_x = scroll->scroll_x;
    if (out_y)
        *out_y = scroll->scroll_y;
}

/// @brief Override the content area size, enabling scrolling beyond child bounds.
///
/// @details Passing <= 0 for a dimension removes the explicit override for that
///          axis, reverting to the measured child extents.
///
/// @param scroll The scroll view to configure.
/// @param width  Explicit content width in pixels (or <= 0 to use child extents).
/// @param height Explicit content height in pixels (or <= 0 to use child extents).
void vg_scrollview_set_content_size(vg_scrollview_t *scroll, float width, float height) {
    if (!scroll)
        return;

    scroll->has_explicit_content_width = width > 0.0f;
    scroll->has_explicit_content_height = height > 0.0f;
    scroll->explicit_content_width = scroll->has_explicit_content_width ? width : 0.0f;
    scroll->explicit_content_height = scroll->has_explicit_content_height ? height : 0.0f;
    if (scroll->has_explicit_content_width)
        scroll->content_width = width;
    else
        scroll->content_width = 0.0f;
    if (scroll->has_explicit_content_height)
        scroll->content_height = height;
    else
        scroll->content_height = 0.0f;
    clamp_scroll(scroll);
    scroll->base.needs_layout = true;
    scroll->base.needs_paint = true;
}

/// @brief Scroll the view so that a descendant widget is fully visible.
///
/// @param scroll The scroll view to scroll.
/// @param child  Descendant widget to bring into view; no-op if not a descendant.
void vg_scrollview_scroll_to_widget(vg_scrollview_t *scroll, vg_widget_t *child) {
    if (!scroll || !child || !vg_widget_is_live(&scroll->base) ||
        scroll->base.type != VG_WIDGET_SCROLLVIEW || !vg_widget_is_live(child))
        return;
    if (!scrollview_is_live_descendant(scroll, child, child->id))
        return;

    scroll->scroll_to_widget = child;
    scroll->scroll_to_widget_id = child->id;
    scrollview_apply_descendant_reveal(scroll, child);
    scroll->base.needs_layout = true;
    scroll->base.needs_paint = true;
}

/// @brief Set which scroll axes are enabled.
///
/// @param scroll    The scroll view to configure.
/// @param direction VG_SCROLL_BOTH, VG_SCROLL_VERTICAL, or VG_SCROLL_HORIZONTAL.
void vg_scrollview_set_direction(vg_scrollview_t *scroll, vg_scroll_direction_t direction) {
    if (!scroll)
        return;

    scroll->direction = direction;
    clamp_scroll(scroll);
    scroll->base.needs_layout = true;
    scroll->base.needs_paint = true;
}
