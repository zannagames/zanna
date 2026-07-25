//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_floatingpanel.c
/// @brief Implements an absolute-positioned overlay panel.
///
/// @details A floating panel occupies zero space in normal layout and paints
/// itself during the overlay pass at explicit screen coordinates. Its children
/// remain ordinary widget-tree members so ownership, focus, hit testing, and
/// destruction use the standard widget machinery.
///
/// Recursive rendering temporarily converts each child's coordinates to screen
/// space and restores them immediately afterward. Traversal stops at scroll
/// views and custom widgets with their own overlay renderer so those boundaries
/// can manage their subtrees. The panel supports direct background dragging.
///
/// @see vg_ide_widgets.h
/// @see vg_widget.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widget.h"
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void floatingpanel_destroy(vg_widget_t *widget);
static void floatingpanel_measure(vg_widget_t *widget,
                                  float available_width,
                                  float available_height);
static void floatingpanel_arrange(vg_widget_t *widget, float x, float y, float width, float height);
static void floatingpanel_paint(vg_widget_t *widget, void *canvas);
static void floatingpanel_paint_overlay(vg_widget_t *widget, void *canvas);
static bool floatingpanel_handle_event(vg_widget_t *widget, vg_event_t *event);
static void floatingpanel_render_normal_subtree(vg_widget_t *widget,
                                                void *canvas,
                                                float parent_abs_x,
                                                float parent_abs_y);
static void floatingpanel_render_overlay_subtree(vg_widget_t *widget,
                                                 void *canvas,
                                                 float parent_abs_x,
                                                 float parent_abs_y);
static void floatingpanel_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);
static void floatingpanel_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color);

//=============================================================================
// VTable
//=============================================================================

static vg_widget_vtable_t g_floatingpanel_vtable = {
    .destroy = floatingpanel_destroy,
    .measure = floatingpanel_measure,
    .arrange = floatingpanel_arrange,
    .paint = floatingpanel_paint,
    .paint_overlay = floatingpanel_paint_overlay,
    .handle_event = floatingpanel_handle_event,
    .can_focus = NULL,
    .on_focus = NULL,
};

//=============================================================================
// Implementation
//=============================================================================

/// @brief Fills the panel's rounded rectangle through the shared AA renderer.
///
/// @param win Destination window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width.
/// @param h Height.
/// @param radius Corner radius.
/// @param color Packed fill color.
static void floatingpanel_fill_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    // Delegated to the shared anti-aliased core (Refined Depth).
    vg_draw_round_rect_fill(win, (float)x, (float)y, (float)w, (float)h, (float)radius, color);
}

/// @brief Strokes the panel border through the shared AA renderer.
///
/// @param win Destination window.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width.
/// @param h Height.
/// @param radius Corner radius.
/// @param color Packed one-pixel stroke color.
static void floatingpanel_stroke_round_rect(
    vgfx_window_t win, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint32_t color) {
    vg_draw_round_rect_stroke(win, (float)x, (float)y, (float)w, (float)h, (float)radius, 1.0f,
                              color);
}

/// @brief Creates an initially hidden floating panel.
///
/// @details Theme-derived background and border colors are applied. Attaching
/// the panel to @p root makes it part of that root's overlay dispatch.
///
/// @param root Widget to receive the panel as a child; may be null.
/// @return Newly allocated panel, or null on allocation failure.
vg_floatingpanel_t *vg_floatingpanel_create(vg_widget_t *root) {
    vg_floatingpanel_t *panel = calloc(1, sizeof(vg_floatingpanel_t));
    if (!panel)
        return NULL;

    vg_widget_init(&panel->base, VG_WIDGET_CUSTOM, &g_floatingpanel_vtable);

    vg_theme_t *theme = vg_theme_get_current();
    panel->bg_color =
        theme ? vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.16f)
              : 0x1A2230u;
    panel->border_color = theme ? theme->colors.border_primary : 0x334156u;
    panel->border_width = 1.0f;

    // Initially hidden
    panel->base.visible = false;

    // Attach to root widget so paint_overlay is dispatched
    if (root)
        vg_widget_add_child(root, &panel->base);

    return panel;
}

/// @brief Reports whether a pointer identifies a live floating panel.
///
/// @param panel Panel pointer to validate.
/// @return `true` when the widget is live and uses the floating-panel vtable.
bool vg_floatingpanel_is_live(const vg_floatingpanel_t *panel) {
    return panel && vg_widget_is_live(&panel->base) &&
           panel->base.vtable == &g_floatingpanel_vtable;
}

/// @brief Releases input capture during panel destruction.
///
/// @param widget Floating-panel base widget being destroyed.
static void floatingpanel_destroy(vg_widget_t *widget) {
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
}

/// @brief Destroy the panel and free all resources, including its child widgets.
///
/// @param panel Panel to destroy; may be NULL (no-op).
void vg_floatingpanel_destroy(vg_floatingpanel_t *panel) {
    if (!panel)
        return;
    vg_widget_destroy(&panel->base);
}

/// @brief Measures the panel as zero-sized in normal layout.
///
/// @param widget Floating-panel base widget whose measured size is cleared.
/// @param available_width Available width; unused.
/// @param available_height Available height; unused.
static void floatingpanel_measure(vg_widget_t *widget,
                                  float available_width,
                                  float available_height) {
    // The panel takes zero space in the normal layout pass.
    (void)available_width;
    (void)available_height;
    widget->measured_width = 0.0f;
    widget->measured_height = 0.0f;
}

/// @brief Applies explicit panel geometry and vertically arranges visible children.
///
/// @param widget Floating-panel base widget to arrange.
/// @param x Parent-provided X coordinate; ignored.
/// @param y Parent-provided Y coordinate; ignored.
/// @param width Parent-provided width; ignored.
/// @param height Parent-provided height; ignored.
static void floatingpanel_arrange(
    vg_widget_t *widget, float x, float y, float width, float height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;

    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)widget;
    widget->x = panel->abs_x;
    widget->y = panel->abs_y;
    widget->width = panel->abs_w;
    widget->height = panel->abs_h;

    float content_x = widget->layout.padding_left;
    float content_y = widget->layout.padding_top;
    float content_w = widget->width - widget->layout.padding_left - widget->layout.padding_right;
    float content_h = widget->height - widget->layout.padding_top - widget->layout.padding_bottom;
    float cursor_y = content_y;

    VG_FOREACH_VISIBLE_CHILD(widget, child) {
        float remaining_h = content_h - (cursor_y - content_y);
        float child_w = content_w - child->layout.margin_left - child->layout.margin_right;
        vg_widget_measure(child, content_w, remaining_h > 0.0f ? remaining_h : content_h);
        float child_h = child->measured_height;
        if (child_h <= 0.0f)
            child_h = remaining_h > 0.0f ? remaining_h : content_h;
        child_h -= child->layout.margin_top + child->layout.margin_bottom;
        if (child_h < 0.0f)
            child_h = 0.0f;
        if (remaining_h > 0.0f &&
            child_h > remaining_h - child->layout.margin_top - child->layout.margin_bottom) {
            child_h = remaining_h - child->layout.margin_top - child->layout.margin_bottom;
        }
        vg_widget_arrange(child,
                          content_x + child->layout.margin_left,
                          cursor_y + child->layout.margin_top,
                          child_w > 0.0f ? child_w : child->measured_width,
                          child_h > 0.0f ? child_h : child->measured_height);
        cursor_y += child->layout.margin_top + child->height + child->layout.margin_bottom;
    }
}

/// @brief Suppresses normal-pass painting for the overlay-only panel.
///
/// @param widget Floating-panel base widget; unused.
/// @param canvas Destination drawing context; unused.
static void floatingpanel_paint(vg_widget_t *widget, void *canvas) {
    (void)widget;
    (void)canvas;
}

/// @brief Paints the elevated panel and its child subtrees in screen space.
///
/// @param widget Floating-panel base widget to render.
/// @param canvas Destination drawing context.
static void floatingpanel_paint_overlay(vg_widget_t *widget, void *canvas) {
    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    if (!panel->base.visible || panel->abs_w <= 0.0f || panel->abs_h <= 0.0f)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;

    int32_t px = (int32_t)widget->x;
    int32_t py = (int32_t)widget->y;
    int32_t pw = (int32_t)widget->width;
    int32_t ph = (int32_t)widget->height;
    int32_t radius = 8;

    uint32_t bg_color =
        panel->bg_color
            ? panel->bg_color
            : (theme ? vg_color_blend(theme->colors.bg_primary, theme->colors.bg_secondary, 0.16f)
                     : 0x1A2230u);
    uint32_t border_color = panel->border_color
                                ? panel->border_color
                                : (theme ? theme->colors.border_primary : 0x334156u);
    uint32_t highlight = vg_color_lighten(bg_color, 0.06f);
    uint32_t accent = theme ? theme->colors.accent_primary : border_color;

    // Real soft drop shadow (floating elevation) + anti-aliased panel body.
    vg_elevation_t el = theme ? theme->elevation.level3 : (vg_elevation_t){20.0f, 0, 8, 90};
    uint32_t shadow_rgb = theme ? theme->elevation.shadow_rgb : 0x000000u;
    vg_draw_round_rect_shadow(win, (float)px, (float)py, (float)pw, (float)ph, (float)radius,
                              el.blur, el.dx, el.dy, el.alpha, shadow_rgb);
    floatingpanel_fill_round_rect(win, px, py, pw, ph, radius, bg_color);
    if (pw > radius * 2)
        vgfx_fill_rect(win, px + radius, py + 1, pw - radius * 2, 1, highlight);
    if (pw > 24)
        vgfx_fill_rect(win, px + radius, py + 1, pw - radius * 2, 3, accent);
    if (panel->border_width > 0.0f)
        floatingpanel_stroke_round_rect(win, px, py, pw, ph, radius, border_color);

    if (pw > 0 && ph > 0) {
        vgfx_set_clip(win, px, py, pw, ph);
        VG_FOREACH_VISIBLE_CHILD(widget, child) {
            floatingpanel_render_normal_subtree(child, canvas, widget->x, widget->y);
            vgfx_set_clip(win, px, py, pw, ph);
        }
        vgfx_clear_clip(win);
    }

    // Nested overlays inherit the panel clip so large forms and scroll views stay contained.
    if (pw > 0 && ph > 0) {
        vgfx_set_clip(win, px, py, pw, ph);
        VG_FOREACH_VISIBLE_CHILD(widget, child) {
            floatingpanel_render_overlay_subtree(child, canvas, widget->x, widget->y);
            vgfx_set_clip(win, px, py, pw, ph);
        }
        vgfx_clear_clip(win);
    }
}

/// @brief Handles direct manipulation of panel position by pointer dragging.
///
/// @param widget Floating-panel base widget receiving the event.
/// @param event Pointer event to inspect.
/// @return `true` when the panel begins, continues, or ends a drag.
static bool floatingpanel_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_floatingpanel_t *panel = (vg_floatingpanel_t *)widget;
    if (!widget->visible)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN:
            if (event->target == widget) {
                panel->dragging = true;
                panel->drag_offset_x = event->mouse.x;
                panel->drag_offset_y = event->mouse.y;
                vg_widget_set_input_capture(widget);
                widget->needs_paint = true;
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_MOVE:
            if (panel->dragging) {
                vg_floatingpanel_set_position(panel,
                                              event->mouse.screen_x - panel->drag_offset_x,
                                              event->mouse.screen_y - panel->drag_offset_y);
                return true;
            }
            return false;

        case VG_EVENT_MOUSE_UP:
            if (panel->dragging) {
                panel->dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }
            return false;

        default:
            return false;
    }
}

/// @brief Tests whether a widget owns rendering of its child subtree.
///
/// @param widget Widget to inspect.
/// @return `true` for scroll views and custom overlay-rendering widgets.
static bool floatingpanel_child_render_boundary(const vg_widget_t *widget) {
    if (!widget)
        return false;
    if (widget->type == VG_WIDGET_SCROLLVIEW)
        return true;
    return widget->type == VG_WIDGET_CUSTOM && widget->vtable && widget->vtable->paint_overlay;
}

/// @brief Paints a normal child subtree using temporary screen coordinates.
///
/// @details Each widget's relative coordinates and `_paint_screen_space` flag
/// are restored before recursion continues or returns.
///
/// @param widget Subtree root.
/// @param canvas Destination drawing context.
/// @param parent_abs_x Absolute X coordinate of the parent.
/// @param parent_abs_y Absolute Y coordinate of the parent.
static void floatingpanel_render_normal_subtree(vg_widget_t *widget,
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

    if (floatingpanel_child_render_boundary(widget))
        return;

    VG_FOREACH_CHILD(widget, child) {
        floatingpanel_render_normal_subtree(child, canvas, abs_x, abs_y);
    }
}

/// @brief Paints a child overlay subtree using temporary screen coordinates.
///
/// @param widget Subtree root.
/// @param canvas Destination drawing context.
/// @param parent_abs_x Absolute X coordinate of the parent.
/// @param parent_abs_y Absolute Y coordinate of the parent.
static void floatingpanel_render_overlay_subtree(vg_widget_t *widget,
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

    if (floatingpanel_child_render_boundary(widget))
        return;

    VG_FOREACH_CHILD(widget, child) {
        floatingpanel_render_overlay_subtree(child, canvas, abs_x, abs_y);
    }
}

/// @brief Set the top-left screen-space position of the panel and request a layout pass.
///
/// @param panel Panel to reposition; may be NULL (no-op).
/// @param x     Horizontal screen coordinate in logical pixels.
/// @param y     Vertical screen coordinate in logical pixels.
void vg_floatingpanel_set_position(vg_floatingpanel_t *panel, float x, float y) {
    if (!panel)
        return;
    if (panel->abs_x == x && panel->abs_y == y)
        return;
    panel->abs_x = x;
    panel->abs_y = y;
    panel->base.needs_layout = true;
    panel->base.needs_paint = true;
}

/// @brief Set the explicit size of the panel and request a layout pass.
///
/// @param panel Panel to resize; may be NULL (no-op).
/// @param w     Width in logical pixels.
/// @param h     Height in logical pixels.
void vg_floatingpanel_set_size(vg_floatingpanel_t *panel, float w, float h) {
    if (!panel)
        return;
    if (panel->abs_w == w && panel->abs_h == h)
        return;
    panel->abs_w = w;
    panel->abs_h = h;
    panel->base.needs_layout = true;
    panel->base.needs_paint = true;
}

/// @brief Center the panel within its connected root's bounds, clamped to the top-left.
///
/// @details Uses the root widget's arranged size (the same coordinate space the panel's
///          position lives in) as the reference frame, so a modal/dialog panel can be centered
///          without the caller re-deriving the window size and offset by hand. If the panel is
///          larger than the root, it clamps to the (0,0) origin. No-op when the panel has no
///          parent yet (size the panel and attach it before centering).
///
/// @param panel Panel to center; may be NULL (no-op).
void vg_floatingpanel_center_in_parent(vg_floatingpanel_t *panel) {
    if (!panel)
        return;
    vg_widget_t *root = panel->base.parent;
    float avail_w = root ? root->width : 0.0f;
    float avail_h = root ? root->height : 0.0f;
    float x = (avail_w - panel->abs_w) * 0.5f;
    float y = (avail_h - panel->abs_h) * 0.5f;
    if (x < 0.0f)
        x = 0.0f;
    if (y < 0.0f)
        y = 0.0f;
    vg_floatingpanel_set_position(panel, x, y);
}

/// @brief Show or hide the panel, clearing the dragging state when hiding.
///
/// @details Hiding while a drag is active releases dragging without releasing input
///          capture — callers should follow with vg_widget_release_input_capture if needed.
///
/// @param panel   Panel to show or hide; may be NULL (no-op).
/// @param visible Non-zero to make visible, zero to hide.
void vg_floatingpanel_set_visible(vg_floatingpanel_t *panel, int visible) {
    if (!panel)
        return;
    if (!visible)
        panel->dragging = false;
    vg_widget_set_visible(&panel->base, visible != 0);
}

/// @brief Append a child widget to the panel and invalidate layout.
///
/// @param panel Panel to add to; may be NULL (no-op).
/// @param child Widget to append; may be NULL (no-op). Ownership transfers to the panel.
void vg_floatingpanel_add_child(vg_floatingpanel_t *panel, vg_widget_t *child) {
    if (!panel || !child)
        return;
    vg_widget_add_child(&panel->base, child);
    panel->base.needs_layout = true;
    panel->base.needs_paint = true;
}
