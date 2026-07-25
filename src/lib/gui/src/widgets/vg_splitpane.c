//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_splitpane.c
// Purpose: SplitPane widget — divides its area into two resizable panels
//          separated by a draggable divider, oriented either horizontally or
//          vertically.
// Key invariants:
//   - split_position is a normalised fraction in [0.0, 1.0] giving the first
//     panel's share of the total available space.
//   - min_first_size and min_second_size are pixel minimums enforced during
//     arrange; the divider cannot be dragged past them.
//   - The first and second child widgets are the widget hierarchy children
//     in order; vg_splitpane_get_first/second return them directly.
// Ownership/Lifetime:
//   - No extra heap allocations beyond the widget itself; no owned pointers.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements two-panel split layout, minimum-size resolution, divider
///        dragging and keyboard control, and collapsible sides.
/// @details The divider position is stored as a normalized fraction while
///          arrange resolves physical minimums. Collapse preserves one restore
///          position across direct transitions between collapsed sides.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void splitpane_destroy(vg_widget_t *widget);
static void splitpane_measure(vg_widget_t *widget, float available_width, float available_height);
static void splitpane_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void splitpane_paint(vg_widget_t *widget, void *canvas);
static bool splitpane_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool splitpane_can_focus(vg_widget_t *widget);
static bool splitpane_adjust_position_by_pixels(vg_splitpane_t *split, float delta_pixels);

//=============================================================================
// SplitPane VTable
//=============================================================================

static vg_widget_vtable_t g_splitpane_vtable = {.destroy = splitpane_destroy,
                                                .measure = splitpane_measure,
                                                .arrange = splitpane_arrange,
                                                .paint = splitpane_paint,
                                                .handle_event = splitpane_handle_event,
                                                .can_focus = splitpane_can_focus,
                                                .on_focus = NULL};

//=============================================================================
// SplitPane Implementation
//=============================================================================

/// @brief Create a split pane widget with the given split direction.
///
/// @details Default split_position is 0.5 (50/50).  Add children with
///          vg_widget_add_child; the first child is the first (left/top) panel
///          and the second child is the second (right/bottom) panel.
///
/// @param parent    Widget to attach to as a child (may be NULL).
/// @param direction VG_SPLIT_HORIZONTAL divides left/right; VG_SPLIT_VERTICAL
///                  divides top/bottom.
/// @return Newly allocated vg_splitpane_t, or NULL on allocation failure.
vg_splitpane_t *vg_splitpane_create(vg_widget_t *parent, vg_split_direction_t direction) {
    vg_splitpane_t *split = calloc(1, sizeof(vg_splitpane_t));
    if (!split)
        return NULL;

    // Initialize base widget
    vg_widget_init(&split->base, VG_WIDGET_SPLITPANE, &g_splitpane_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize splitpane-specific fields
    split->direction = direction;
    split->split_position = 0.5f; // 50% by default
    split->min_first_size = 50.0f;
    split->min_second_size = 50.0f;
    split->restore_position = split->split_position;
    split->collapsed_side = VG_SPLIT_COLLAPSED_NONE;
    {
        float scale = theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
        split->splitter_size = 6.0f * scale;
    }

    split->splitter_color = theme->colors.border_primary;
    split->splitter_hover_color = theme->colors.border_focus;

    // State
    split->splitter_hovered = false;
    split->dragging = false;
    split->drag_start = 0;
    split->drag_start_split = 0;

    // Create two child panes (containers)
    vg_widget_t *first = vg_widget_create(VG_WIDGET_CONTAINER);
    vg_widget_t *second = vg_widget_create(VG_WIDGET_CONTAINER);

    if (first)
        vg_widget_add_child(&split->base, first);
    if (second)
        vg_widget_add_child(&split->base, second);

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &split->base);
    }

    return split;
}

/// @brief VTable destroy: releases input capture if this widget currently holds it; child cleanup
/// is handled by the base widget.
/// @param widget Split-pane widget base being destroyed.
static void splitpane_destroy(vg_widget_t *widget) {
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    // Children are destroyed by base widget cleanup
    (void)widget;
}

/// @brief VTable measure: claims all available space and distributes it to both child panels
/// according to split_position and minimum sizes.
/// @param widget Split-pane widget base to measure.
/// @param available_width Width offered by the parent.
/// @param available_height Height offered by the parent.
static void splitpane_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_splitpane_t *split = (vg_splitpane_t *)widget;

    // SplitPane fills available space
    widget->measured_width = available_width > 0 ? available_width : 400;
    widget->measured_height = available_height > 0 ? available_height : 300;

    // Apply constraints
    if (widget->constraints.preferred_width > 0) {
        widget->measured_width = widget->constraints.preferred_width;
    }
    if (widget->constraints.preferred_height > 0) {
        widget->measured_height = widget->constraints.preferred_height;
    }
    if (widget->measured_width < widget->constraints.min_width) {
        widget->measured_width = widget->constraints.min_width;
    }
    if (widget->measured_height < widget->constraints.min_height) {
        widget->measured_height = widget->constraints.min_height;
    }

    vg_widget_t *first = widget->first_child;
    vg_widget_t *second = first ? first->next_sibling : NULL;
    if (!first || !second)
        return;

    if (split->direction == VG_SPLIT_HORIZONTAL) {
        float available = widget->measured_width - split->splitter_size;
        if (available < 0.0f)
            available = 0.0f;
        float first_width = split->collapsed_side == VG_SPLIT_COLLAPSED_FIRST ? 0.0f
                            : split->collapsed_side == VG_SPLIT_COLLAPSED_SECOND
                                ? available
                                : available * split->split_position;
        if (split->collapsed_side == VG_SPLIT_COLLAPSED_NONE) {
            if (first_width < split->min_first_size)
                first_width = split->min_first_size;
            if (available - first_width < split->min_second_size)
                first_width = available - split->min_second_size;
        }
        if (first_width < 0.0f)
            first_width = 0.0f;
        float second_width = available - first_width;
        if (second_width < 0.0f)
            second_width = 0.0f;
        vg_widget_measure(first, first_width, widget->measured_height);
        vg_widget_measure(second, second_width, widget->measured_height);
    } else {
        float available = widget->measured_height - split->splitter_size;
        if (available < 0.0f)
            available = 0.0f;
        float first_height = split->collapsed_side == VG_SPLIT_COLLAPSED_FIRST ? 0.0f
                             : split->collapsed_side == VG_SPLIT_COLLAPSED_SECOND
                                 ? available
                                 : available * split->split_position;
        if (split->collapsed_side == VG_SPLIT_COLLAPSED_NONE) {
            if (first_height < split->min_first_size)
                first_height = split->min_first_size;
            if (available - first_height < split->min_second_size)
                first_height = available - split->min_second_size;
        }
        if (first_height < 0.0f)
            first_height = 0.0f;
        float second_height = available - first_height;
        if (second_height < 0.0f)
            second_height = 0.0f;
        vg_widget_measure(first, widget->measured_width, first_height);
        vg_widget_measure(second, widget->measured_width, second_height);
    }
}

/// @brief Resolves the first panel's pixel size from @p requested_first, clamping to both minimums;
/// distributes proportionally when space is too small to satisfy both.
/// @param available Physical extent excluding the divider.
/// @param requested_first Requested first-panel extent.
/// @param min_first First-panel minimum.
/// @param min_second Second-panel minimum.
/// @return Resolved first-panel extent in the range zero through @p available.
static float resolve_first_size(float available,
                                float requested_first,
                                float min_first,
                                float min_second) {
    if (available <= 0.0f)
        return 0.0f;
    if (min_first + min_second > available) {
        float total_min = min_first + min_second;
        if (total_min <= 0.0f)
            return available * 0.5f;
        return available * (min_first / total_min);
    }
    float first = requested_first;
    if (first < min_first)
        first = min_first;
    if (available - first < min_second)
        first = available - min_second;
    if (first < 0.0f)
        first = 0.0f;
    if (first > available)
        first = available;
    return first;
}

/// @brief VTable arrange: positions the widget and arranges both child panels side-by-side (or
/// stacked), separated by the splitter bar.
/// @param widget Split-pane widget base to arrange.
/// @param x Assigned left coordinate.
/// @param y Assigned top coordinate.
/// @param width Assigned width.
/// @param height Assigned height.
static void splitpane_arrange(vg_widget_t *widget, float x, float y, float width, float height) {
    vg_splitpane_t *split = (vg_splitpane_t *)widget;

    widget->x = x;
    widget->y = y;
    widget->width = width;
    widget->height = height;

    // Get the two panes
    vg_widget_t *first = widget->first_child;
    vg_widget_t *second = first ? first->next_sibling : NULL;

    if (!first || !second)
        return;

    if (split->direction == VG_SPLIT_HORIZONTAL) {
        // Left/Right split
        float available = width - split->splitter_size;
        if (available < 0.0f)
            available = 0.0f;
        float first_width = split->collapsed_side == VG_SPLIT_COLLAPSED_FIRST ? 0.0f
                            : split->collapsed_side == VG_SPLIT_COLLAPSED_SECOND
                                ? available
                                : resolve_first_size(available,
                                                     available * split->split_position,
                                                     split->min_first_size,
                                                     split->min_second_size);
        float second_width = available - first_width;
        if (second_width < 0.0f)
            second_width = 0.0f;

        // Arrange first pane
        vg_widget_arrange(first, 0, 0, first_width, height);

        // Arrange second pane
        vg_widget_arrange(second, first_width + split->splitter_size, 0, second_width, height);
    } else {
        // Top/Bottom split
        float available = height - split->splitter_size;
        if (available < 0.0f)
            available = 0.0f;
        float first_height = split->collapsed_side == VG_SPLIT_COLLAPSED_FIRST ? 0.0f
                             : split->collapsed_side == VG_SPLIT_COLLAPSED_SECOND
                                 ? available
                                 : resolve_first_size(available,
                                                      available * split->split_position,
                                                      split->min_first_size,
                                                      split->min_second_size);
        float second_height = available - first_height;
        if (second_height < 0.0f)
            second_height = 0.0f;

        // Arrange first pane
        vg_widget_arrange(first, 0, 0, width, first_height);

        // Arrange second pane
        vg_widget_arrange(second, 0, first_height + split->splitter_size, width, second_height);
    }
}

/// @brief VTable can_focus: returns true when the widget is both enabled and visible.
/// @param widget Candidate split-pane widget.
/// @return `true` when keyboard focus is permitted.
static bool splitpane_can_focus(vg_widget_t *widget) {
    return widget && widget->enabled && widget->visible;
}

/// @brief Nudges the split position by @p delta_pixels, recomputing the normalised fraction;
/// returns true if the position changed.
/// @param split Split pane to adjust.
/// @param delta_pixels Signed first-panel size change in physical pixels.
/// @return `true` when the normalized position changed.
static bool splitpane_adjust_position_by_pixels(vg_splitpane_t *split, float delta_pixels) {
    if (!split)
        return false;

    float available =
        (split->direction == VG_SPLIT_HORIZONTAL ? split->base.width : split->base.height) -
        split->splitter_size;
    if (available <= 0.0f)
        return false;

    float first_size = available * split->split_position + delta_pixels;
    float resolved =
        resolve_first_size(available, first_size, split->min_first_size, split->min_second_size);
    float new_position = available > 0.0f ? resolved / available : split->split_position;
    if (new_position < 0.0f)
        new_position = 0.0f;
    if (new_position > 1.0f)
        new_position = 1.0f;
    if (new_position == split->split_position)
        return false;

    split->split_position = new_position;
    split->collapsed_side = VG_SPLIT_COLLAPSED_NONE;
    split->restore_position = new_position;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
    return true;
}

/// @brief VTable paint: draws the splitter bar background, centre line, and three grip dots in the
/// hover/drag or default colour.
/// @param widget Arranged split-pane widget base to paint.
/// @param canvas Backend canvas used for divider primitives.
static void splitpane_paint(vg_widget_t *widget, void *canvas) {
    vg_splitpane_t *split = (vg_splitpane_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    // Draw splitter
    uint32_t color = split->splitter_hovered || split->dragging ? split->splitter_hover_color
                                                                : split->splitter_color;
    uint32_t bg = vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.4f);

    vg_widget_t *first = widget->first_child;
    if (!first)
        return;

    float splitter_x, splitter_y, splitter_w, splitter_h;

    if (split->direction == VG_SPLIT_HORIZONTAL) {
        splitter_x = widget->x + first->width;
        splitter_y = widget->y;
        splitter_w = split->splitter_size;
        splitter_h = widget->height;
    } else {
        splitter_x = widget->x;
        splitter_y = widget->y + first->height;
        splitter_w = widget->width;
        splitter_h = split->splitter_size;
    }

    // Draw splitter bar
    vgfx_fill_rect((vgfx_window_t)canvas,
                   (int32_t)splitter_x,
                   (int32_t)splitter_y,
                   (int32_t)splitter_w,
                   (int32_t)splitter_h,
                   bg);
    if (split->direction == VG_SPLIT_HORIZONTAL) {
        int32_t line_x = (int32_t)(splitter_x + splitter_w * 0.5f);
        vgfx_fill_rect((vgfx_window_t)canvas,
                       line_x,
                       (int32_t)splitter_y + 2,
                       1,
                       (int32_t)splitter_h - 4,
                       color);
        int32_t cy = (int32_t)(splitter_y + splitter_h * 0.5f);
        vgfx_fill_circle((vgfx_window_t)canvas, line_x, cy - 7, 1, color);
        vgfx_fill_circle((vgfx_window_t)canvas, line_x, cy, 1, color);
        vgfx_fill_circle((vgfx_window_t)canvas, line_x, cy + 7, 1, color);
    } else {
        int32_t line_y = (int32_t)(splitter_y + splitter_h * 0.5f);
        vgfx_fill_rect((vgfx_window_t)canvas,
                       (int32_t)splitter_x + 2,
                       line_y,
                       (int32_t)splitter_w - 4,
                       1,
                       color);
        int32_t cx = (int32_t)(splitter_x + splitter_w * 0.5f);
        vgfx_fill_circle((vgfx_window_t)canvas, cx - 7, line_y, 1, color);
        vgfx_fill_circle((vgfx_window_t)canvas, cx, line_y, 1, color);
        vgfx_fill_circle((vgfx_window_t)canvas, cx + 7, line_y, 1, color);
    }
}

/// @brief VTable handle_event: tracks splitter hover, initiates and tracks drag, releases on
/// mouse-up, and handles arrow/Home/End keyboard nudges.
/// @param widget Split-pane widget receiving input.
/// @param event Pointer or keyboard event to interpret.
/// @return `true` when divider dragging or keyboard movement consumes the event.
static bool splitpane_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_splitpane_t *split = (vg_splitpane_t *)widget;

    vg_widget_t *first = widget->first_child;
    if (!first)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            float local_x = event->mouse.x;
            float local_y = event->mouse.y;

            if (split->dragging) {
                // Calculate new split position
                float pos;
                if (split->direction == VG_SPLIT_HORIZONTAL) {
                    float available = widget->width - split->splitter_size;
                    if (available <= 0.0f)
                        available = 1.0f;
                    pos = (local_x - split->drag_start + split->drag_start_split * available) /
                          available;
                    float min_pos = split->min_first_size / available;
                    float max_pos = 1.0f - (split->min_second_size / available);
                    if (min_pos > max_pos)
                        min_pos = max_pos = split->drag_start_split;
                    if (pos < min_pos)
                        pos = min_pos;
                    if (pos > max_pos)
                        pos = max_pos;
                } else {
                    float available = widget->height - split->splitter_size;
                    if (available <= 0.0f)
                        available = 1.0f;
                    pos = (local_y - split->drag_start + split->drag_start_split * available) /
                          available;
                    float min_pos = split->min_first_size / available;
                    float max_pos = 1.0f - (split->min_second_size / available);
                    if (min_pos > max_pos)
                        min_pos = max_pos = split->drag_start_split;
                    if (pos < min_pos)
                        pos = min_pos;
                    if (pos > max_pos)
                        pos = max_pos;
                }

                // Clamp position
                if (pos < 0)
                    pos = 0;
                if (pos > 1)
                    pos = 1;

                if (split->split_position != pos ||
                    split->collapsed_side != VG_SPLIT_COLLAPSED_NONE) {
                    split->split_position = pos;
                    split->collapsed_side = VG_SPLIT_COLLAPSED_NONE;
                    split->restore_position = pos;
                    vg_widget_invalidate_layout(widget);
                    vg_widget_note_change(widget);
                }
                return true;
            }

            // Check if over splitter
            bool was_hovered = split->splitter_hovered;
            if (split->direction == VG_SPLIT_HORIZONTAL) {
                split->splitter_hovered =
                    local_x >= first->width && local_x < first->width + split->splitter_size;
            } else {
                split->splitter_hovered =
                    local_y >= first->height && local_y < first->height + split->splitter_size;
            }

            if (was_hovered != split->splitter_hovered) {
                widget->needs_paint = true;
            }

            return false;
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (split->splitter_hovered) {
                split->splitter_hovered = false;
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_MOUSE_DOWN:
            if (split->splitter_hovered) {
                split->dragging = true;
                split->drag_start_split = split->split_position;
                if (split->direction == VG_SPLIT_HORIZONTAL) {
                    split->drag_start = event->mouse.x;
                } else {
                    split->drag_start = event->mouse.y;
                }
                vg_widget_set_input_capture(widget);
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_UP:
            if (split->dragging) {
                split->dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                return true;
            }
            return false;

        case VG_EVENT_KEY_DOWN: {
            float step = 16.0f * ((vg_theme_get_current()->ui_scale > 0.0f)
                                      ? vg_theme_get_current()->ui_scale
                                      : 1.0f);
            if (event->key.key == VG_KEY_HOME) {
                vg_splitpane_set_position(split, 0.0f);
                return true;
            }
            if (event->key.key == VG_KEY_END) {
                vg_splitpane_set_position(split, 1.0f);
                return true;
            }
            if (split->direction == VG_SPLIT_HORIZONTAL) {
                if (event->key.key == VG_KEY_LEFT)
                    return splitpane_adjust_position_by_pixels(split, -step);
                if (event->key.key == VG_KEY_RIGHT)
                    return splitpane_adjust_position_by_pixels(split, step);
            } else {
                if (event->key.key == VG_KEY_UP)
                    return splitpane_adjust_position_by_pixels(split, -step);
                if (event->key.key == VG_KEY_DOWN)
                    return splitpane_adjust_position_by_pixels(split, step);
            }
            break;
        }

        default:
            break;
    }

    return false;
}

//=============================================================================
// SplitPane API
//=============================================================================

/// @brief Set the split position as a normalised fraction of the available space.
///
/// @param split    The split pane to update.
/// @param position Fraction for the first panel in [0.0, 1.0]; clamped if outside range.
void vg_splitpane_set_position(vg_splitpane_t *split, float position) {
    if (!split)
        return;

    if (position < 0)
        position = 0;
    if (position > 1)
        position = 1;
    if (split->split_position == position && split->collapsed_side == VG_SPLIT_COLLAPSED_NONE)
        return;

    split->split_position = position;
    split->restore_position = position;
    split->collapsed_side = VG_SPLIT_COLLAPSED_NONE;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Return the current normalised split position.
///
/// @param split The split pane to query.
/// @return Fraction in [0.0, 1.0], or 0.5 if split is NULL.
float vg_splitpane_get_position(vg_splitpane_t *split) {
    return split ? split->split_position : 0.5f;
}

/// @brief Set the minimum pixel sizes for each panel.
///
/// @details The divider cannot be dragged to make either panel smaller than
///          its minimum.  Values ≤ 0 are treated as 0 (no minimum).
///
/// @param split      The split pane to configure.
/// @param min_first  Minimum size of the first panel in logical pixels.
/// @param min_second Minimum size of the second panel in logical pixels.
void vg_splitpane_set_min_sizes(vg_splitpane_t *split, float min_first, float min_second) {
    if (!split)
        return;

    min_first = min_first > 0 ? min_first : 0;
    min_second = min_second > 0 ? min_second : 0;
    if (split->min_first_size == min_first && split->min_second_size == min_second)
        return;
    split->min_first_size = min_first;
    split->min_second_size = min_second;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Set the first pane's minimum physical size.
/// @details Non-finite and non-positive inputs are normalized to zero. A real value transition
///          invalidates layout and advances the split pane's general revision.
/// @param split Split pane widget to configure; NULL is ignored.
/// @param size Minimum size in physical pixels.
void vg_splitpane_set_min_first(vg_splitpane_t *split, float size) {
    if (!split)
        return;
    float normalized = isfinite(size) && size > 0.0f ? size : 0.0f;
    if (split->min_first_size == normalized)
        return;
    split->min_first_size = normalized;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Set the second pane's minimum physical size.
/// @details Non-finite and non-positive inputs are normalized to zero. A real value transition
///          invalidates layout and advances the split pane's general revision.
/// @param split Split pane widget to configure; NULL is ignored.
/// @param size Minimum size in physical pixels.
void vg_splitpane_set_min_second(vg_splitpane_t *split, float size) {
    if (!split)
        return;
    float normalized = isfinite(size) && size > 0.0f ? size : 0.0f;
    if (split->min_second_size == normalized)
        return;
    split->min_second_size = normalized;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Return the configured first-pane minimum size.
/// @param split Split pane widget to inspect.
/// @return Minimum first-pane size in physical pixels, or zero for NULL.
float vg_splitpane_get_min_first(const vg_splitpane_t *split) {
    return split ? split->min_first_size : 0.0f;
}

/// @brief Return the configured second-pane minimum size.
/// @param split Split pane widget to inspect.
/// @return Minimum second-pane size in physical pixels, or zero for NULL.
float vg_splitpane_get_min_second(const vg_splitpane_t *split) {
    return split ? split->min_second_size : 0.0f;
}

/// @brief Return the split pane's creation-time direction.
/// @param split Split pane widget to inspect.
/// @return Horizontal or vertical direction; NULL defaults to horizontal.
vg_split_direction_t vg_splitpane_get_direction(const vg_splitpane_t *split) {
    return split ? split->direction : VG_SPLIT_HORIZONTAL;
}

/// @brief Apply an explicit collapsed-side state and retain a restore position.
/// @details The first transition from the uncollapsed state captures the current position. A
///          switch directly between collapsed sides keeps that same original restore position.
/// @param split Split pane widget to update; NULL is ignored.
/// @param side First or second collapsed-side value.
static void splitpane_collapse(vg_splitpane_t *split, vg_split_collapsed_side_t side) {
    if (!split || side == VG_SPLIT_COLLAPSED_NONE || split->collapsed_side == side)
        return;
    if (split->collapsed_side == VG_SPLIT_COLLAPSED_NONE)
        split->restore_position = split->split_position;
    split->collapsed_side = side;
    split->split_position = side == VG_SPLIT_COLLAPSED_FIRST ? 0.0f : 1.0f;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Collapse the first (left or top) pane.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_collapse_first(vg_splitpane_t *split) {
    splitpane_collapse(split, VG_SPLIT_COLLAPSED_FIRST);
}

/// @brief Collapse the second (right or bottom) pane.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_collapse_second(vg_splitpane_t *split) {
    splitpane_collapse(split, VG_SPLIT_COLLAPSED_SECOND);
}

/// @brief Restore both panes to the divider position retained before collapse.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_restore(vg_splitpane_t *split) {
    if (!split || split->collapsed_side == VG_SPLIT_COLLAPSED_NONE)
        return;
    float restored = split->restore_position;
    if (!isfinite(restored))
        restored = 0.5f;
    if (restored < 0.0f)
        restored = 0.0f;
    if (restored > 1.0f)
        restored = 1.0f;
    split->split_position = restored;
    split->collapsed_side = VG_SPLIT_COLLAPSED_NONE;
    vg_widget_invalidate_layout(&split->base);
    vg_widget_note_change(&split->base);
}

/// @brief Return the currently collapsed pane.
/// @param split Split pane widget to inspect.
/// @return None, first, or second collapsed-side value; NULL returns none.
vg_split_collapsed_side_t vg_splitpane_get_collapsed_side(const vg_splitpane_t *split) {
    return split ? split->collapsed_side : VG_SPLIT_COLLAPSED_NONE;
}

/// @brief Return the first (left or top) child widget of the split pane.
///
/// @param split The split pane to query.
/// @return First child widget pointer, or NULL if split is NULL or has no children.
vg_widget_t *vg_splitpane_get_first(vg_splitpane_t *split) {
    return split ? split->base.first_child : NULL;
}

/// @brief Return the second (right or bottom) child widget of the split pane.
///
/// @param split The split pane to query.
/// @return Second child widget pointer, or NULL if split is NULL or has fewer than two children.
vg_widget_t *vg_splitpane_get_second(vg_splitpane_t *split) {
    vg_widget_t *first = split ? split->base.first_child : NULL;
    return first ? first->next_sibling : NULL;
}
