//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_layout.c
// Purpose: Layout system implementation — VBox, HBox, Stack, Grid, and
//          Absolute layout containers with measure/arrange two-pass engine.
// Key invariants:
//   - Measure must complete before arrange; arrange uses the measured sizes.
//   - Grow factors distribute remaining space proportionally across children.
// Ownership/Lifetime:
//   - Layout vtable functions operate on caller-owned widget trees.
// Links: lib/gui/include/vg_layout.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#include "../../include/vg_layout.h"
#include "../../include/vg_widget.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements retained VBox, HBox, Flex, Grid, and Dock layout algorithms.
/// @details Each container participates in the widget measure/arrange protocol, normalizes
/// invalid dimensions, applies margins and constraints, and supports manually positioned
/// children. Checked Grid and Dock mutations preserve prior metadata on validation or allocation
/// failure.

//=============================================================================
// Layout-specific vtables
//=============================================================================

static void vbox_measure(vg_widget_t *self, float available_width, float available_height);
static void vbox_arrange(vg_widget_t *self, float x, float y, float width, float height);
static void hbox_measure(vg_widget_t *self, float available_width, float available_height);
static void hbox_arrange(vg_widget_t *self, float x, float y, float width, float height);
static void flex_destroy(vg_widget_t *self);
static void flex_measure(vg_widget_t *self, float available_width, float available_height);
static void flex_arrange(vg_widget_t *self, float x, float y, float width, float height);

/// @brief Computes the initial @p out_offset and per-item @p out_gap_add for a justify-content
/// distribution over @p extra_space.
/// @param justify Main-axis distribution policy to apply.
/// @param visible_count Number of visible, automatically positioned children.
/// @param extra_space Remaining main-axis space available for distribution.
/// @param[out] out_offset Receives the leading offset before the first child when non-NULL.
/// @param[out] out_gap_add Receives the additional spacing between children when non-NULL.
static void compute_justify_distribution(vg_justify_t justify,
                                         int visible_count,
                                         float extra_space,
                                         float *out_offset,
                                         float *out_gap_add) {
    float offset = 0.0f;
    float gap_add = 0.0f;

    if (extra_space <= 0.0f || visible_count <= 0) {
        if (out_offset)
            *out_offset = 0.0f;
        if (out_gap_add)
            *out_gap_add = 0.0f;
        return;
    }

    switch (justify) {
        case VG_JUSTIFY_CENTER:
            offset = extra_space * 0.5f;
            break;
        case VG_JUSTIFY_END:
            offset = extra_space;
            break;
        case VG_JUSTIFY_SPACE_BETWEEN:
            if (visible_count > 1)
                gap_add = extra_space / (float)(visible_count - 1);
            break;
        case VG_JUSTIFY_SPACE_AROUND:
            gap_add = extra_space / (float)visible_count;
            offset = gap_add * 0.5f;
            break;
        case VG_JUSTIFY_SPACE_EVENLY:
            gap_add = extra_space / (float)(visible_count + 1);
            offset = gap_add;
            break;
        case VG_JUSTIFY_START:
        default:
            break;
    }

    if (out_offset)
        *out_offset = offset;
    if (out_gap_add)
        *out_gap_add = gap_add;
}

/// @brief Returns @p value if it is finite and positive, otherwise 0.
/// @param value Dimension or spacing value to normalize.
/// @return @p value when finite and positive; otherwise `0.0f`.
static float layout_nonnegative(float value) {
    return (isfinite(value) && value > 0.0f) ? value : 0.0f;
}

/// @brief Measure one child after clamping available dimensions to finite non-negative values.
/// @param child Child widget to measure.
/// @param available_width Parent-provided available width.
/// @param available_height Parent-provided available height.
static void layout_measure_child(vg_widget_t *child,
                                 float available_width,
                                 float available_height) {
    vg_widget_measure(
        child, layout_nonnegative(available_width), layout_nonnegative(available_height));
}

/// @brief Arrange a manually positioned child at its stored origin and measured size.
/// @param child Manual child to arrange; NULL is ignored.
static void layout_arrange_manual_child(vg_widget_t *child) {
    if (!child)
        return;
    vg_widget_arrange(child,
                      child->x,
                      child->y,
                      layout_nonnegative(child->measured_width),
                      layout_nonnegative(child->measured_height));
}

/// @brief Applies min/max size constraints to @p widget's measured_width/height after measure.
/// @param widget Widget whose measured dimensions are clamped in place.
static void layout_apply_constraints(vg_widget_t *widget) {
    vg_widget_apply_constraints(widget);
}

static const vg_widget_vtable_t g_vbox_vtable = {
    .measure = vbox_measure,
    .arrange = vbox_arrange,
};

static const vg_widget_vtable_t g_hbox_vtable = {
    .measure = hbox_measure,
    .arrange = hbox_arrange,
};

static const vg_widget_vtable_t g_flex_vtable = {
    .destroy = flex_destroy,
    .measure = flex_measure,
    .arrange = flex_arrange,
};

//=============================================================================
// VBox Implementation
//=============================================================================

/// @brief Creates a VBox layout container with the given child spacing; returns NULL on allocation
/// failure.
/// @param spacing Requested gap in pixels between adjacent automatic children.
/// @return Newly allocated VBox widget, or NULL when widget or layout allocation fails.
vg_widget_t *vg_vbox_create(float spacing) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!widget)
        return NULL;

    widget->vtable = &g_vbox_vtable;

    // Allocate layout data
    vg_vbox_layout_t *layout = calloc(1, sizeof(vg_vbox_layout_t));
    if (!layout) {
        vg_widget_destroy(widget);
        return NULL;
    }

    layout->spacing = layout_nonnegative(spacing);
    layout->align = VG_ALIGN_STRETCH;
    layout->justify = VG_JUSTIFY_START;
    widget->impl_data = layout;

    return widget;
}

/// @brief Sets the gap between VBox children in pixels and marks the layout dirty.
/// @param vbox VBox widget to update; invalid or differently typed widgets are ignored.
/// @param spacing New child gap, normalized to a finite non-negative value.
void vg_vbox_set_spacing(vg_widget_t *vbox, float spacing) {
    if (!vbox || vbox->vtable != &g_vbox_vtable || !vbox->impl_data)
        return;
    vg_vbox_layout_t *layout = (vg_vbox_layout_t *)vbox->impl_data;
    spacing = layout_nonnegative(spacing);
    if (layout->spacing == spacing)
        return;
    layout->spacing = spacing;
    vg_widget_invalidate_layout(vbox);
    vg_widget_note_revision(vbox);
}

/// @brief Sets the cross-axis alignment for VBox children (START, CENTER, END, or STRETCH).
/// @param vbox VBox widget to update; invalid or differently typed widgets are ignored.
/// @param align Requested cross-axis alignment; unsupported values fall back to start.
void vg_vbox_set_align(vg_widget_t *vbox, vg_align_t align) {
    if (!vbox || vbox->vtable != &g_vbox_vtable || !vbox->impl_data)
        return;
    if (align < VG_ALIGN_START || align > VG_ALIGN_BASELINE)
        align = VG_ALIGN_START;
    vg_vbox_layout_t *layout = (vg_vbox_layout_t *)vbox->impl_data;
    if (layout->align == align)
        return;
    layout->align = align;
    vg_widget_invalidate_layout(vbox);
    vg_widget_note_revision(vbox);
}

/// @brief Sets the main-axis content justification for VBox children (START, CENTER, END,
/// SPACE_BETWEEN, etc.).
/// @param vbox VBox widget to update; invalid or differently typed widgets are ignored.
/// @param justify Requested main-axis distribution; unsupported values fall back to start.
void vg_vbox_set_justify(vg_widget_t *vbox, vg_justify_t justify) {
    if (!vbox || vbox->vtable != &g_vbox_vtable || !vbox->impl_data)
        return;
    if (justify < VG_JUSTIFY_START || justify > VG_JUSTIFY_SPACE_EVENLY)
        justify = VG_JUSTIFY_START;
    vg_vbox_layout_t *layout = (vg_vbox_layout_t *)vbox->impl_data;
    if (layout->justify == justify)
        return;
    layout->justify = justify;
    vg_widget_invalidate_layout(vbox);
    vg_widget_note_revision(vbox);
}

/// @brief Returns the VBox cross-axis alignment without exposing implementation data.
/// @param vbox VBox widget to query.
/// @return Stored alignment, or @ref VG_ALIGN_START for an invalid or differently typed widget.
vg_align_t vg_vbox_get_align(const vg_widget_t *vbox) {
    if (!vbox || vbox->vtable != &g_vbox_vtable || !vbox->impl_data)
        return VG_ALIGN_START;
    return ((const vg_vbox_layout_t *)vbox->impl_data)->align;
}

/// @brief Returns the VBox main-axis justification without consuming state.
/// @param vbox VBox widget to query.
/// @return Stored justification, or @ref VG_JUSTIFY_START for an invalid or differently typed
///         widget.
vg_justify_t vg_vbox_get_justify(const vg_widget_t *vbox) {
    if (!vbox || vbox->vtable != &g_vbox_vtable || !vbox->impl_data)
        return VG_JUSTIFY_START;
    return ((const vg_vbox_layout_t *)vbox->impl_data)->justify;
}

/// @brief Measures the VBox by summing children heights plus spacing and taking the max child
/// width.
/// @param self VBox widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided vertical space.
static void vbox_measure(vg_widget_t *self, float available_width, float available_height) {
    if (!self || !self->impl_data)
        return;

    vg_vbox_layout_t *layout = (vg_vbox_layout_t *)self->impl_data;

    float padding_h = self->layout.padding_left + self->layout.padding_right;
    float padding_v = self->layout.padding_top + self->layout.padding_bottom;

    float max_width = 0;
    float total_height = 0;
    int visible_count = 0;

    // First pass: measure children
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_measure_child(child, available_width - padding_h, available_height - padding_v);
            continue;
        }
        layout_measure_child(child, available_width - padding_h, available_height - padding_v);

        float child_width =
            child->measured_width + child->layout.margin_left + child->layout.margin_right;
        float child_height =
            child->measured_height + child->layout.margin_top + child->layout.margin_bottom;

        if (child_width > max_width)
            max_width = child_width;
        total_height += child_height;
        visible_count++;
    }

    // Add spacing between children
    if (visible_count > 1) {
        total_height += layout->spacing * (visible_count - 1);
    }

    self->measured_width = max_width + padding_h;
    self->measured_height = total_height + padding_v;
    layout_apply_constraints(self);
}

/// @brief Arranges VBox children vertically, distributing flex space and applying justify/align
/// offsets.
/// @param self VBox widget and children to arrange.
/// @param x Assigned X origin in the parent coordinate system.
/// @param y Assigned Y origin in the parent coordinate system.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void vbox_arrange(vg_widget_t *self, float x, float y, float width, float height) {
    if (!self || !self->impl_data)
        return;

    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;

    vg_vbox_layout_t *layout = (vg_vbox_layout_t *)self->impl_data;

    float content_x = self->layout.padding_left;
    float content_y = self->layout.padding_top;
    float content_width =
        layout_nonnegative(width - self->layout.padding_left - self->layout.padding_right);
    float content_height =
        layout_nonnegative(height - self->layout.padding_top - self->layout.padding_bottom);

    // Calculate total fixed height and flex
    float total_fixed = 0;
    float total_flex = 0;
    int visible_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        total_fixed += child->layout.margin_top + child->layout.margin_bottom;
        if (child->layout.flex > 0) {
            total_flex += child->layout.flex;
        } else {
            total_fixed += child->measured_height;
        }
        visible_count++;
    }

    // Add spacing
    float total_spacing = (visible_count > 1) ? layout->spacing * (visible_count - 1) : 0;
    float available = content_height - total_fixed - total_spacing;
    float flex_unit = (total_flex > 0 && available > 0) ? available / total_flex : 0;
    float used_height = total_fixed + total_spacing + (flex_unit * total_flex);
    float extra_space = content_height - used_height;
    float justify_offset = 0.0f;
    float justify_gap_add = 0.0f;
    compute_justify_distribution(
        layout->justify, visible_count, extra_space, &justify_offset, &justify_gap_add);

    // Arrange children
    float child_y = content_y + justify_offset;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_arrange_manual_child(child);
            continue;
        }
        float child_height;
        if (child->layout.flex > 0) {
            child_height = flex_unit * child->layout.flex;
        } else {
            child_height = child->measured_height;
        }

        // Calculate child X based on alignment
        float child_x;
        float child_width;

        switch (layout->align) {
            case VG_ALIGN_START:
                child_x = content_x + child->layout.margin_left;
                child_width = child->measured_width;
                break;
            case VG_ALIGN_CENTER:
                child_x = content_x + child->layout.margin_left +
                          (content_width - child->layout.margin_left - child->layout.margin_right -
                           child->measured_width) /
                              2.0f;
                child_width = child->measured_width;
                break;
            case VG_ALIGN_END:
                child_x =
                    content_x + content_width - child->measured_width - child->layout.margin_right;
                child_width = child->measured_width;
                break;
            case VG_ALIGN_STRETCH:
            default:
                child_x = content_x + child->layout.margin_left;
                child_width =
                    content_width - child->layout.margin_left - child->layout.margin_right;
                break;
        }

        child_width = layout_nonnegative(child_width);
        child_height = layout_nonnegative(child_height);
        vg_widget_arrange(
            child, child_x, child_y + child->layout.margin_top, child_width, child_height);
        child_y += child_height + child->layout.margin_top + child->layout.margin_bottom +
                   layout->spacing + justify_gap_add;
    }
}

//=============================================================================
// HBox Implementation
//=============================================================================

/// @brief Creates an HBox layout container with the given child spacing; returns NULL on allocation
/// failure.
/// @param spacing Requested gap in pixels between adjacent automatic children.
/// @return Newly allocated HBox widget, or NULL when widget or layout allocation fails.
vg_widget_t *vg_hbox_create(float spacing) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!widget)
        return NULL;

    widget->vtable = &g_hbox_vtable;

    vg_hbox_layout_t *layout = calloc(1, sizeof(vg_hbox_layout_t));
    if (!layout) {
        vg_widget_destroy(widget);
        return NULL;
    }

    layout->spacing = layout_nonnegative(spacing);
    layout->align = VG_ALIGN_STRETCH;
    layout->justify = VG_JUSTIFY_START;
    widget->impl_data = layout;

    return widget;
}

/// @brief Sets the gap between HBox children in pixels and marks the layout dirty.
/// @param hbox HBox widget to update; invalid or differently typed widgets are ignored.
/// @param spacing New child gap, normalized to a finite non-negative value.
void vg_hbox_set_spacing(vg_widget_t *hbox, float spacing) {
    if (!hbox || hbox->vtable != &g_hbox_vtable || !hbox->impl_data)
        return;
    vg_hbox_layout_t *layout = (vg_hbox_layout_t *)hbox->impl_data;
    spacing = layout_nonnegative(spacing);
    if (layout->spacing == spacing)
        return;
    layout->spacing = spacing;
    vg_widget_invalidate_layout(hbox);
    vg_widget_note_revision(hbox);
}

/// @brief Sets the cross-axis alignment for HBox children (START, CENTER, END, or STRETCH).
/// @param hbox HBox widget to update; invalid or differently typed widgets are ignored.
/// @param align Requested cross-axis alignment; unsupported values fall back to start.
void vg_hbox_set_align(vg_widget_t *hbox, vg_align_t align) {
    if (!hbox || hbox->vtable != &g_hbox_vtable || !hbox->impl_data)
        return;
    if (align < VG_ALIGN_START || align > VG_ALIGN_BASELINE)
        align = VG_ALIGN_START;
    vg_hbox_layout_t *layout = (vg_hbox_layout_t *)hbox->impl_data;
    if (layout->align == align)
        return;
    layout->align = align;
    vg_widget_invalidate_layout(hbox);
    vg_widget_note_revision(hbox);
}

/// @brief Sets the main-axis content justification for HBox children (START, CENTER, END,
/// SPACE_BETWEEN, etc.).
/// @param hbox HBox widget to update; invalid or differently typed widgets are ignored.
/// @param justify Requested main-axis distribution; unsupported values fall back to start.
void vg_hbox_set_justify(vg_widget_t *hbox, vg_justify_t justify) {
    if (!hbox || hbox->vtable != &g_hbox_vtable || !hbox->impl_data)
        return;
    if (justify < VG_JUSTIFY_START || justify > VG_JUSTIFY_SPACE_EVENLY)
        justify = VG_JUSTIFY_START;
    vg_hbox_layout_t *layout = (vg_hbox_layout_t *)hbox->impl_data;
    if (layout->justify == justify)
        return;
    layout->justify = justify;
    vg_widget_invalidate_layout(hbox);
    vg_widget_note_revision(hbox);
}

/// @brief Returns the HBox cross-axis alignment without exposing implementation data.
/// @param hbox HBox widget to query.
/// @return Stored alignment, or @ref VG_ALIGN_START for an invalid or differently typed widget.
vg_align_t vg_hbox_get_align(const vg_widget_t *hbox) {
    if (!hbox || hbox->vtable != &g_hbox_vtable || !hbox->impl_data)
        return VG_ALIGN_START;
    return ((const vg_hbox_layout_t *)hbox->impl_data)->align;
}

/// @brief Returns the HBox main-axis justification without consuming state.
/// @param hbox HBox widget to query.
/// @return Stored justification, or @ref VG_JUSTIFY_START for an invalid or differently typed
///         widget.
vg_justify_t vg_hbox_get_justify(const vg_widget_t *hbox) {
    if (!hbox || hbox->vtable != &g_hbox_vtable || !hbox->impl_data)
        return VG_JUSTIFY_START;
    return ((const vg_hbox_layout_t *)hbox->impl_data)->justify;
}

/// @brief Measures the HBox by summing children widths plus spacing and taking the max child
/// height.
/// @param self HBox widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided vertical space.
static void hbox_measure(vg_widget_t *self, float available_width, float available_height) {
    if (!self || !self->impl_data)
        return;

    vg_hbox_layout_t *layout = (vg_hbox_layout_t *)self->impl_data;

    float padding_h = self->layout.padding_left + self->layout.padding_right;
    float padding_v = self->layout.padding_top + self->layout.padding_bottom;

    float total_width = 0;
    float max_height = 0;
    int visible_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_measure_child(child, available_width - padding_h, available_height - padding_v);
            continue;
        }
        layout_measure_child(child, available_width - padding_h, available_height - padding_v);

        float child_width =
            child->measured_width + child->layout.margin_left + child->layout.margin_right;
        float child_height =
            child->measured_height + child->layout.margin_top + child->layout.margin_bottom;

        total_width += child_width;
        if (child_height > max_height)
            max_height = child_height;
        visible_count++;
    }

    if (visible_count > 1) {
        total_width += layout->spacing * (visible_count - 1);
    }

    self->measured_width = total_width + padding_h;
    self->measured_height = max_height + padding_v;
    layout_apply_constraints(self);
}

/// @brief Arranges HBox children horizontally, distributing flex space and applying justify/align
/// offsets.
/// @param self HBox widget and children to arrange.
/// @param x Assigned X origin in the parent coordinate system.
/// @param y Assigned Y origin in the parent coordinate system.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void hbox_arrange(vg_widget_t *self, float x, float y, float width, float height) {
    if (!self || !self->impl_data)
        return;

    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;

    vg_hbox_layout_t *layout = (vg_hbox_layout_t *)self->impl_data;

    float content_x = self->layout.padding_left;
    float content_y = self->layout.padding_top;
    float content_width =
        layout_nonnegative(width - self->layout.padding_left - self->layout.padding_right);
    float content_height =
        layout_nonnegative(height - self->layout.padding_top - self->layout.padding_bottom);

    float total_fixed = 0;
    float total_flex = 0;
    int visible_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        total_fixed += child->layout.margin_left + child->layout.margin_right;
        if (child->layout.flex > 0) {
            total_flex += child->layout.flex;
        } else {
            total_fixed += child->measured_width;
        }
        visible_count++;
    }

    float total_spacing = (visible_count > 1) ? layout->spacing * (visible_count - 1) : 0;
    float available = content_width - total_fixed - total_spacing;
    float flex_unit = (total_flex > 0 && available > 0) ? available / total_flex : 0;
    float used_width = total_fixed + total_spacing + (flex_unit * total_flex);
    float extra_space = content_width - used_width;
    float justify_offset = 0.0f;
    float justify_gap_add = 0.0f;
    compute_justify_distribution(
        layout->justify, visible_count, extra_space, &justify_offset, &justify_gap_add);

    float child_x = content_x + justify_offset;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_arrange_manual_child(child);
            continue;
        }
        float child_width;
        if (child->layout.flex > 0) {
            child_width = flex_unit * child->layout.flex;
        } else {
            child_width = child->measured_width;
        }

        float child_y;
        float child_height;

        switch (layout->align) {
            case VG_ALIGN_START:
                child_y = content_y + child->layout.margin_top;
                child_height = child->measured_height;
                break;
            case VG_ALIGN_CENTER:
                child_y = content_y + child->layout.margin_top +
                          (content_height - child->layout.margin_top - child->layout.margin_bottom -
                           child->measured_height) /
                              2.0f;
                child_height = child->measured_height;
                break;
            case VG_ALIGN_END:
                child_y = content_y + content_height - child->measured_height -
                          child->layout.margin_bottom;
                child_height = child->measured_height;
                break;
            case VG_ALIGN_STRETCH:
            default:
                child_y = content_y + child->layout.margin_top;
                child_height =
                    content_height - child->layout.margin_top - child->layout.margin_bottom;
                break;
        }

        child_width = layout_nonnegative(child_width);
        child_height = layout_nonnegative(child_height);
        vg_widget_arrange(
            child, child_x + child->layout.margin_left, child_y, child_width, child_height);
        child_x += child_width + child->layout.margin_left + child->layout.margin_right +
                   layout->spacing + justify_gap_add;
    }
}

//=============================================================================
// Flex Layout Implementation
//=============================================================================

/// @brief Creates a Flex layout container with default row direction, stretch alignment, and no
/// wrapping.
vg_widget_t *vg_flex_create(void) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!widget)
        return NULL;

    widget->vtable = &g_flex_vtable;

    vg_flex_layout_t *layout = calloc(1, sizeof(vg_flex_layout_t));
    if (!layout) {
        vg_widget_destroy(widget);
        return NULL;
    }

    layout->direction = VG_DIRECTION_ROW;
    layout->align_items = VG_ALIGN_STRETCH;
    layout->justify_content = VG_JUSTIFY_START;
    layout->align_content = VG_ALIGN_START;
    layout->gap = 0;
    layout->wrap = VG_FLEX_NO_WRAP;
    widget->impl_data = layout;

    return widget;
}

/// @brief Sets the flex main-axis direction (ROW, COLUMN, ROW_REVERSE, or COLUMN_REVERSE).
/// @param flex Flex container to update; invalid or differently typed widgets are ignored.
/// @param direction Requested main-axis direction; unsupported values fall back to row.
void vg_flex_set_direction(vg_widget_t *flex, vg_direction_t direction) {
    if (!flex || flex->vtable != &g_flex_vtable || !flex->impl_data)
        return;
    if (direction < VG_DIRECTION_ROW || direction > VG_DIRECTION_COLUMN_REVERSE)
        direction = VG_DIRECTION_ROW;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)flex->impl_data;
    if (layout->direction == direction)
        return;
    layout->direction = direction;
    vg_widget_invalidate_layout(flex);
    vg_widget_note_revision(flex);
}

/// @brief Sets the cross-axis alignment applied to all children in a flex line.
/// @param flex Flex container to update; invalid or differently typed widgets are ignored.
/// @param align Requested item alignment; unsupported values fall back to start.
void vg_flex_set_align_items(vg_widget_t *flex, vg_align_t align) {
    if (!flex || flex->vtable != &g_flex_vtable || !flex->impl_data)
        return;
    if (align < VG_ALIGN_START || align > VG_ALIGN_BASELINE)
        align = VG_ALIGN_START;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)flex->impl_data;
    if (layout->align_items == align)
        return;
    layout->align_items = align;
    vg_widget_invalidate_layout(flex);
    vg_widget_note_revision(flex);
}

/// @brief Sets how children are distributed along the main axis within each flex line.
/// @param flex Flex container to update; invalid or differently typed widgets are ignored.
/// @param justify Requested distribution policy; unsupported values fall back to start.
void vg_flex_set_justify_content(vg_widget_t *flex, vg_justify_t justify) {
    if (!flex || flex->vtable != &g_flex_vtable || !flex->impl_data)
        return;
    if (justify < VG_JUSTIFY_START || justify > VG_JUSTIFY_SPACE_EVENLY)
        justify = VG_JUSTIFY_START;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)flex->impl_data;
    if (layout->justify_content == justify)
        return;
    layout->justify_content = justify;
    vg_widget_invalidate_layout(flex);
    vg_widget_note_revision(flex);
}

/// @brief Sets the gap between children on the main axis (and between flex lines when wrapping).
/// @param flex Flex container to update; invalid or differently typed widgets are ignored.
/// @param gap Requested item and line gap, normalized to a finite non-negative value.
void vg_flex_set_gap(vg_widget_t *flex, float gap) {
    if (!flex || flex->vtable != &g_flex_vtable || !flex->impl_data)
        return;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)flex->impl_data;
    gap = layout_nonnegative(gap);
    if (layout->gap == gap)
        return;
    layout->gap = gap;
    vg_widget_invalidate_layout(flex);
    vg_widget_note_revision(flex);
}

/// @brief Polymorphic spacing setter — dispatches to vg_vbox_set_spacing, vg_hbox_set_spacing, or
/// vg_flex_set_gap.
/// @param container VBox, HBox, or Flex container to update.
/// @param spacing Requested child spacing; normalization follows the concrete container type.
void vg_container_set_spacing(vg_widget_t *container, float spacing) {
    if (!container || !container->impl_data || !container->vtable)
        return;

    if (container->vtable == &g_vbox_vtable) {
        vg_vbox_set_spacing(container, spacing);
        return;
    }
    if (container->vtable == &g_hbox_vtable) {
        vg_hbox_set_spacing(container, spacing);
        return;
    }
    if (container->vtable == &g_flex_vtable) {
        vg_flex_set_gap(container, spacing);
    }
}

/// @brief Enables or disables line wrapping; when enabled, overflow children start a new flex line.
/// @param flex Flex container to update.
/// @param wrap True to use normal wrapping, or false to keep all children on one line.
void vg_flex_set_wrap(vg_widget_t *flex, bool wrap) {
    vg_flex_set_wrap_mode(flex, wrap ? VG_FLEX_WRAP : VG_FLEX_NO_WRAP);
}

/// @brief Sets no-wrap, normal-wrap, or reverse-wrap line placement on a Flex container.
/// @param flex Flex container to update; invalid or differently typed widgets are ignored.
/// @param wrap Requested wrapping mode; unsupported values fall back to no-wrap.
void vg_flex_set_wrap_mode(vg_widget_t *flex, vg_flex_wrap_t wrap) {
    if (!flex || flex->vtable != &g_flex_vtable || !flex->impl_data)
        return;
    if (wrap < VG_FLEX_NO_WRAP || wrap > VG_FLEX_WRAP_REVERSE)
        wrap = VG_FLEX_NO_WRAP;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)flex->impl_data;
    if (layout->wrap == wrap)
        return;
    layout->wrap = wrap;
    vg_widget_invalidate_layout(flex);
    vg_widget_note_revision(flex);
}

typedef struct flex_line {
    vg_widget_t **children;
    int count;
    float main_outer;
    float cross_outer;
    float total_flex;
} flex_line_t;

/// @brief Destroy a flex layout implementation and its reusable scratch buffers.
/// @param self Flex container widget whose impl_data is a vg_flex_layout_t.
static void flex_destroy(vg_widget_t *self) {
    if (!self || !self->impl_data)
        return;
    vg_flex_layout_t *layout = (vg_flex_layout_t *)vg_widget_take_impl_data(self);
    free(layout->scratch_children);
    free(layout->scratch_lines);
    free(layout);
}

/// @brief Ensure the flex layout has a reusable child-pointer scratch buffer.
/// @param layout Flex layout state that owns the scratch buffer.
/// @param needed Number of child pointers required.
/// @return Scratch child-pointer array, or NULL on allocation failure.
static vg_widget_t **flex_ensure_child_scratch(vg_flex_layout_t *layout, int needed) {
    if (!layout || needed <= 0)
        return NULL;
    if (layout->scratch_child_capacity >= needed)
        return (vg_widget_t **)layout->scratch_children;
    if ((size_t)needed > SIZE_MAX / sizeof(vg_widget_t *))
        return NULL;
    vg_widget_t **children =
        (vg_widget_t **)realloc(layout->scratch_children, (size_t)needed * sizeof(vg_widget_t *));
    if (!children)
        return NULL;
    layout->scratch_children = children;
    layout->scratch_child_capacity = needed;
    return children;
}

/// @brief Ensure the flex layout has a reusable wrapped-line scratch buffer.
/// @param layout Flex layout state that owns the scratch buffer.
/// @param needed Number of line records required.
/// @return Zeroed scratch line array, or NULL on allocation failure.
static flex_line_t *flex_ensure_line_scratch(vg_flex_layout_t *layout, int needed) {
    if (!layout || needed <= 0)
        return NULL;
    if (layout->scratch_line_capacity < needed) {
        if ((size_t)needed > SIZE_MAX / sizeof(flex_line_t))
            return NULL;
        flex_line_t *lines =
            (flex_line_t *)realloc(layout->scratch_lines, (size_t)needed * sizeof(flex_line_t));
        if (!lines)
            return NULL;
        layout->scratch_lines = lines;
        layout->scratch_line_capacity = needed;
    }
    memset(layout->scratch_lines, 0, (size_t)needed * sizeof(flex_line_t));
    return (flex_line_t *)layout->scratch_lines;
}

/// @brief Returns the child's measured size on the main axis (width for row, height for column).
/// @param child Child widget to inspect; NULL contributes zero.
/// @param is_row True when the flex main axis is horizontal.
/// @return Normalized measured size on the selected main axis.
static float flex_child_main_size(vg_widget_t *child, bool is_row) {
    if (!child)
        return 0.0f;
    return is_row ? child->measured_width : child->measured_height;
}

/// @brief Returns the child's measured size on the cross axis (height for row, width for column).
/// @param child Child widget to inspect; NULL contributes zero.
/// @param is_row True when the flex main axis is horizontal.
/// @return Normalized measured size on the selected cross axis.
static float flex_child_cross_size(vg_widget_t *child, bool is_row) {
    if (!child)
        return 0.0f;
    return is_row ? child->measured_height : child->measured_width;
}

/// @brief Returns the child's main-axis measured size plus its surrounding margins.
/// @param child Child widget to inspect; NULL contributes zero.
/// @param is_row True when the flex main axis is horizontal.
/// @return Main-axis measured extent including the corresponding margins.
static float flex_child_main_outer(vg_widget_t *child, bool is_row) {
    if (!child)
        return 0.0f;
    return flex_child_main_size(child, is_row) +
           (is_row ? child->layout.margin_left + child->layout.margin_right
                   : child->layout.margin_top + child->layout.margin_bottom);
}

/// @brief Returns the child's cross-axis measured size plus its surrounding margins.
/// @param child Child widget to inspect; NULL contributes zero.
/// @param is_row True when the flex main axis is horizontal.
/// @return Cross-axis measured extent including the corresponding margins.
static float flex_child_cross_outer(vg_widget_t *child, bool is_row) {
    if (!child)
        return 0.0f;
    return flex_child_cross_size(child, is_row) +
           (is_row ? child->layout.margin_top + child->layout.margin_bottom
                   : child->layout.margin_left + child->layout.margin_right);
}

/// @brief Counts and optionally fills the reusable scratch array of all visible children of @p
/// self.
/// @param self Flex container whose layout-managed children should be collected.
/// @param layout Flex implementation state that owns the scratch child array.
/// @param out_children Optional output pointer receiving the reusable scratch array.
/// @return Number of visible layout-managed children, or -1 on allocation failure.
static int flex_collect_visible_children(vg_widget_t *self,
                                         vg_flex_layout_t *layout,
                                         vg_widget_t ***out_children) {
    int count = 0;
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        count++;
    }

    if (!out_children)
        return count;
    *out_children = NULL;
    if (count <= 0)
        return 0;

    vg_widget_t **children = flex_ensure_child_scratch(layout, count);
    if (!children)
        return -1;

    int index = 0;
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        children[index++] = child;
    }
    *out_children = children;
    return count;
}

/// @brief Partitions @p children into wrap lines based on main-axis size; returns the number of
/// lines.
/// @param self Flex container that owns the child sequence.
/// @param layout Flex state providing wrap mode, gap, and reusable line storage.
/// @param is_row True when the flex main axis is horizontal.
/// @param main_limit Available main-axis extent used to decide line breaks.
/// @param children Visible layout-managed children in traversal order.
/// @param child_count Number of entries in @p children.
/// @param[out] out_lines Receives the reusable line array, or NULL when no lines are produced.
/// @return Number of populated line records, or zero for empty input or allocation failure.
static int flex_build_lines(vg_widget_t *self,
                            vg_flex_layout_t *layout,
                            bool is_row,
                            float main_limit,
                            vg_widget_t **children,
                            int child_count,
                            flex_line_t **out_lines) {
    if (!out_lines) {
        return 0;
    }
    *out_lines = NULL;
    if (!children || child_count <= 0)
        return 0;

    flex_line_t *lines = flex_ensure_line_scratch(layout, child_count);
    if (!lines)
        return 0;

    int line_count = 0;
    for (int i = 0; i < child_count; i++) {
        vg_widget_t *child = children[i];
        float child_main_outer = flex_child_main_outer(child, is_row);
        float child_cross_outer = flex_child_cross_outer(child, is_row);

        if (line_count == 0)
            line_count = 1;
        flex_line_t *line = &lines[line_count - 1];
        bool wraps = layout->wrap && line->count > 0 && main_limit > 0.0f &&
                     (line->main_outer + layout->gap + child_main_outer) > main_limit;
        if (wraps) {
            line_count++;
            line = &lines[line_count - 1];
        }

        if (!line->children)
            line->children = children + i;
        if (line->count > 0)
            line->main_outer += layout->gap;
        line->count++;
        line->main_outer += child_main_outer;
        if (child_cross_outer > line->cross_outer)
            line->cross_outer = child_cross_outer;
        line->total_flex += child->layout.flex;
    }

    *out_lines = lines;
    return line_count;
}

/// @brief Measures the flex container by summing child main sizes and taking the max cross size,
/// accounting for wrap lines.
/// @param self Flex container whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided vertical space.
static void flex_measure(vg_widget_t *self, float available_width, float available_height) {
    if (!self || !self->impl_data)
        return;

    vg_flex_layout_t *layout = (vg_flex_layout_t *)self->impl_data;
    bool is_row =
        (layout->direction == VG_DIRECTION_ROW || layout->direction == VG_DIRECTION_ROW_REVERSE);

    float padding_h = self->layout.padding_left + self->layout.padding_right;
    float padding_v = self->layout.padding_top + self->layout.padding_bottom;

    float main_size = 0;
    float cross_size = 0;
    int visible_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_measure_child(child, available_width - padding_h, available_height - padding_v);
            continue;
        }
        layout_measure_child(child, available_width - padding_h, available_height - padding_v);

        float child_main =
            is_row
                ? child->measured_width + child->layout.margin_left + child->layout.margin_right
                : child->measured_height + child->layout.margin_top + child->layout.margin_bottom;
        float child_cross =
            is_row ? child->measured_height + child->layout.margin_top + child->layout.margin_bottom
                   : child->measured_width + child->layout.margin_left + child->layout.margin_right;

        main_size += child_main;
        if (child_cross > cross_size)
            cross_size = child_cross;
        visible_count++;
    }

    if (layout->wrap) {
        vg_widget_t **children = NULL;
        int child_count = flex_collect_visible_children(self, layout, &children);
        if (child_count < 0) {
            self->measured_width = self->constraints.min_width;
            self->measured_height = self->constraints.min_height;
            return;
        }
        float available_main =
            layout_nonnegative(is_row ? available_width - padding_h : available_height - padding_v);
        flex_line_t *lines = NULL;
        int line_count =
            flex_build_lines(self, layout, is_row, available_main, children, child_count, &lines);

        float measured_main = 0.0f;
        float measured_cross = 0.0f;
        for (int i = 0; i < line_count; i++) {
            if (lines[i].main_outer > measured_main)
                measured_main = lines[i].main_outer;
            measured_cross += lines[i].cross_outer;
            if (i + 1 < line_count)
                measured_cross += layout->gap;
        }

        if (is_row) {
            self->measured_width = measured_main + padding_h;
            self->measured_height = measured_cross + padding_v;
        } else {
            self->measured_width = measured_cross + padding_h;
            self->measured_height = measured_main + padding_v;
        }

    } else {
        if (visible_count > 1) {
            main_size += layout->gap * (visible_count - 1);
        }

        if (is_row) {
            self->measured_width = main_size + padding_h;
            self->measured_height = cross_size + padding_v;
        } else {
            self->measured_width = cross_size + padding_h;
            self->measured_height = main_size + padding_v;
        }
    }

    layout_apply_constraints(self);
}

/// @brief Arranges flex children along the main axis with flex-grow resolution, justify-content,
/// and align-items; handles wrap and reverse directions.
/// @param self Flex container and children to arrange.
/// @param x Assigned X origin in the parent coordinate system.
/// @param y Assigned Y origin in the parent coordinate system.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void flex_arrange(vg_widget_t *self, float x, float y, float width, float height) {
    if (!self || !self->impl_data)
        return;

    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;

    vg_flex_layout_t *layout = (vg_flex_layout_t *)self->impl_data;
    bool is_row =
        (layout->direction == VG_DIRECTION_ROW || layout->direction == VG_DIRECTION_ROW_REVERSE);
    bool is_reverse = (layout->direction == VG_DIRECTION_ROW_REVERSE ||
                       layout->direction == VG_DIRECTION_COLUMN_REVERSE);

    float content_x = self->layout.padding_left;
    float content_y = self->layout.padding_top;
    float content_width =
        layout_nonnegative(width - self->layout.padding_left - self->layout.padding_right);
    float content_height =
        layout_nonnegative(height - self->layout.padding_top - self->layout.padding_bottom);

    float main_size = is_row ? content_width : content_height;
    float cross_size = is_row ? content_height : content_width;

    if (layout->wrap) {
        vg_widget_t **children = NULL;
        int child_count = flex_collect_visible_children(self, layout, &children);
        if (child_count < 0)
            return;
        flex_line_t *lines = NULL;
        int line_count =
            flex_build_lines(self, layout, is_row, main_size, children, child_count, &lines);
        if (line_count > 0 && lines) {
            float total_cross = 0.0f;
            for (int i = 0; i < line_count; i++) {
                total_cross += lines[i].cross_outer;
                if (i + 1 < line_count)
                    total_cross += layout->gap;
            }

            float extra_cross = layout_nonnegative(cross_size - total_cross);
            float line_cross_extra = 0.0f;
            float line_cross_offset = 0.0f;
            switch (layout->align_content) {
                case VG_ALIGN_CENTER:
                    line_cross_offset = extra_cross * 0.5f;
                    break;
                case VG_ALIGN_END:
                    line_cross_offset = extra_cross;
                    break;
                case VG_ALIGN_STRETCH:
                    if (line_count > 0)
                        line_cross_extra = extra_cross / (float)line_count;
                    break;
                case VG_ALIGN_START:
                default:
                    break;
            }

            const bool reverse_lines = layout->wrap == VG_FLEX_WRAP_REVERSE;
            float cross_pos = reverse_lines ? cross_size - line_cross_offset : line_cross_offset;
            for (int line_index = 0; line_index < line_count; line_index++) {
                flex_line_t *line = &lines[line_index];
                float line_cross_size = line->cross_outer + line_cross_extra;
                if (reverse_lines)
                    cross_pos -= line_cross_size;
                float available_main = layout_nonnegative(main_size - line->main_outer);
                float flex_unit = (line->total_flex > 0.0f && available_main > 0.0f)
                                      ? available_main / line->total_flex
                                      : 0.0f;
                float used_main = line->main_outer + flex_unit * line->total_flex;
                float extra_main = main_size - used_main;
                float justify_offset = 0.0f;
                float justify_gap_add = 0.0f;
                compute_justify_distribution(layout->justify_content,
                                             line->count,
                                             extra_main,
                                             &justify_offset,
                                             &justify_gap_add);

                float main_pos = is_reverse ? (main_size - justify_offset) : justify_offset;
                for (int child_index = 0; child_index < line->count; child_index++) {
                    vg_widget_t *child = line->children[child_index];
                    float child_main_size = flex_child_main_size(child, is_row);
                    if (child->layout.flex > 0.0f)
                        child_main_size += flex_unit * child->layout.flex;

                    float child_cross_size =
                        layout->align_items == VG_ALIGN_STRETCH
                            ? line_cross_size -
                                  (is_row ? child->layout.margin_top + child->layout.margin_bottom
                                          : child->layout.margin_left + child->layout.margin_right)
                            : flex_child_cross_size(child, is_row);
                    child_cross_size = layout_nonnegative(child_cross_size);

                    float child_x = content_x;
                    float child_y = content_y;
                    float child_w = 0.0f;
                    float child_h = 0.0f;

                    if (is_row) {
                        child_w = child_main_size;
                        child_h = child_cross_size;

                        if (is_reverse) {
                            main_pos -= child_main_size + child->layout.margin_right;
                            child_x = content_x + main_pos;
                            main_pos -= child->layout.margin_left + layout->gap + justify_gap_add;
                        } else {
                            child_x = content_x + main_pos + child->layout.margin_left;
                            main_pos += child_main_size + child->layout.margin_left +
                                        child->layout.margin_right + layout->gap + justify_gap_add;
                        }

                        float cross_inner = line_cross_size - child->layout.margin_top -
                                            child->layout.margin_bottom - child_h;
                        if (cross_inner < 0.0f)
                            cross_inner = 0.0f;
                        switch (layout->align_items) {
                            case VG_ALIGN_CENTER:
                                child_y = content_y + cross_pos + child->layout.margin_top +
                                          cross_inner * 0.5f;
                                break;
                            case VG_ALIGN_END:
                                child_y = content_y + cross_pos + line_cross_size -
                                          child->layout.margin_bottom - child_h;
                                break;
                            case VG_ALIGN_START:
                            case VG_ALIGN_STRETCH:
                            default:
                                child_y = content_y + cross_pos + child->layout.margin_top;
                                break;
                        }
                    } else {
                        child_h = child_main_size;
                        child_w = child_cross_size;

                        if (is_reverse) {
                            main_pos -= child_main_size + child->layout.margin_bottom;
                            child_y = content_y + main_pos;
                            main_pos -= child->layout.margin_top + layout->gap + justify_gap_add;
                        } else {
                            child_y = content_y + main_pos + child->layout.margin_top;
                            main_pos += child_main_size + child->layout.margin_top +
                                        child->layout.margin_bottom + layout->gap + justify_gap_add;
                        }

                        float cross_inner = line_cross_size - child->layout.margin_left -
                                            child->layout.margin_right - child_w;
                        if (cross_inner < 0.0f)
                            cross_inner = 0.0f;
                        switch (layout->align_items) {
                            case VG_ALIGN_CENTER:
                                child_x = content_x + cross_pos + child->layout.margin_left +
                                          cross_inner * 0.5f;
                                break;
                            case VG_ALIGN_END:
                                child_x = content_x + cross_pos + line_cross_size -
                                          child->layout.margin_right - child_w;
                                break;
                            case VG_ALIGN_START:
                            case VG_ALIGN_STRETCH:
                            default:
                                child_x = content_x + cross_pos + child->layout.margin_left;
                                break;
                        }
                    }

                    vg_widget_arrange(child,
                                      child_x,
                                      child_y,
                                      layout_nonnegative(child_w),
                                      layout_nonnegative(child_h));
                }

                if (reverse_lines)
                    cross_pos -= layout->gap;
                else
                    cross_pos += line_cross_size + layout->gap;
            }
        }

        return;
    }

    // Calculate total flex-basis and flex grow. The basis includes every visible
    // child's measured main size and margins; flex grow distributes only the
    // remaining space so flex children keep their preferred size.
    float total_basis = 0;
    float total_flex = 0;
    int visible_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        float child_main =
            is_row
                ? child->measured_width + child->layout.margin_left + child->layout.margin_right
                : child->measured_height + child->layout.margin_top + child->layout.margin_bottom;

        total_basis += child_main;
        if (child->layout.flex > 0) {
            total_flex += child->layout.flex;
        }
        visible_count++;
    }

    float gap_total = (visible_count > 1) ? layout->gap * (visible_count - 1) : 0;
    float available = main_size - total_basis - gap_total;
    float flex_unit = (total_flex > 0 && available > 0) ? available / total_flex : 0;
    float used_main = total_basis + gap_total + (flex_unit * total_flex);
    float extra_space = main_size - used_main;
    float justify_offset = 0.0f;
    float justify_gap_add = 0.0f;
    compute_justify_distribution(
        layout->justify_content, visible_count, extra_space, &justify_offset, &justify_gap_add);

    float main_pos = is_reverse ? (main_size - justify_offset) : justify_offset;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_arrange_manual_child(child);
            continue;
        }
        float child_main_size;
        if (child->layout.flex > 0) {
            child_main_size = (is_row ? child->measured_width : child->measured_height) +
                              flex_unit * child->layout.flex;
        } else {
            child_main_size = is_row ? child->measured_width : child->measured_height;
        }
        child_main_size = layout_nonnegative(child_main_size);

        float child_cross_size;
        if (layout->align_items == VG_ALIGN_STRETCH) {
            child_cross_size = cross_size;
        } else {
            child_cross_size = is_row ? child->measured_height : child->measured_width;
        }

        float child_x, child_y, child_w, child_h;

        if (is_row) {
            child_w = child_main_size;
            // In STRETCH mode, child_cross_size is the full cross axis — subtract
            // margins so the child fills the inset rect. In other modes,
            // child_cross_size is the child's own measured size and must not be
            // shrunk further (the previous behavior clipped descenders/icons by
            // a few pixels when margins were nonzero).
            if (layout->align_items == VG_ALIGN_STRETCH)
                child_h = child_cross_size - child->layout.margin_top - child->layout.margin_bottom;
            else
                child_h = child_cross_size;

            if (is_reverse) {
                main_pos -= child_main_size + child->layout.margin_right;
                child_x = content_x + main_pos;
                main_pos -= child->layout.margin_left + layout->gap + justify_gap_add;
            } else {
                child_x = content_x + main_pos + child->layout.margin_left;
                main_pos += child_main_size + child->layout.margin_left +
                            child->layout.margin_right + layout->gap + justify_gap_add;
            }

            switch (layout->align_items) {
                case VG_ALIGN_START:
                    child_y = content_y + child->layout.margin_top;
                    break;
                case VG_ALIGN_CENTER:
                    child_y = content_y + child->layout.margin_top +
                              (cross_size - child->layout.margin_top - child->layout.margin_bottom -
                               child_h) /
                                  2.0f;
                    break;
                case VG_ALIGN_END:
                    child_y = content_y + cross_size - child_h - child->layout.margin_bottom;
                    break;
                default:
                    child_y = content_y + child->layout.margin_top;
                    break;
            }
        } else {
            child_h = child_main_size;
            if (layout->align_items == VG_ALIGN_STRETCH)
                child_w = child_cross_size - child->layout.margin_left - child->layout.margin_right;
            else
                child_w = child_cross_size;

            if (is_reverse) {
                main_pos -= child_main_size + child->layout.margin_bottom;
                child_y = content_y + main_pos;
                main_pos -= child->layout.margin_top + layout->gap + justify_gap_add;
            } else {
                child_y = content_y + main_pos + child->layout.margin_top;
                main_pos += child_main_size + child->layout.margin_top +
                            child->layout.margin_bottom + layout->gap + justify_gap_add;
            }

            switch (layout->align_items) {
                case VG_ALIGN_START:
                    child_x = content_x + child->layout.margin_left;
                    break;
                case VG_ALIGN_CENTER:
                    child_x = content_x + child->layout.margin_left +
                              (cross_size - child->layout.margin_left - child->layout.margin_right -
                               child_w) /
                                  2.0f;
                    break;
                case VG_ALIGN_END:
                    child_x = content_x + cross_size - child_w - child->layout.margin_right;
                    break;
                default:
                    child_x = content_x + child->layout.margin_left;
                    break;
            }
        }

        vg_widget_arrange(
            child, child_x, child_y, layout_nonnegative(child_w), layout_nonnegative(child_h));
    }
}

//=============================================================================
// Grid Layout Implementation
//=============================================================================

#define VG_GRID_MAX_TRACKS 4096

/// @brief Per-child grid placement entry stored inside grid_impl_t.
typedef struct grid_placement {
    vg_widget_t *child;
    vg_grid_item_t item;
} grid_placement_t;

/// @brief Grid container internal state (stored in impl_data).
typedef struct grid_impl {
    vg_grid_layout_t layout;      ///< column/row counts, gaps, width/height arrays
    grid_placement_t *placements; ///< per-child placement records
    int placement_count;
    int placement_capacity;
} grid_impl_t;

/// @brief Frees the grid's column_widths, row_heights, and placements arrays and then the impl
/// struct.
/// @param self Grid widget whose implementation storage is released.
static void grid_destroy(vg_widget_t *self) {
    if (!self || !self->impl_data)
        return;
    grid_impl_t *g = (grid_impl_t *)vg_widget_take_impl_data(self);
    free(g->layout.column_widths);
    free(g->layout.row_heights);
    free(g->placements);
    free(g);
}

/// @brief Finds and returns the placement record for @p child, or NULL if none exists.
/// @param g Grid implementation whose placement array is searched.
/// @param child Direct child whose explicit placement is requested.
/// @return Mutable placement record for @p child, or NULL when it is auto-placed.
static grid_placement_t *grid_find_placement(grid_impl_t *g, vg_widget_t *child) {
    for (int i = 0; i < g->placement_count; i++) {
        if (g->placements[i].child == child)
            return &g->placements[i];
    }
    return NULL;
}

/// @brief Removes the placement record for @p child from the grid, shifting the array to fill the
/// gap.
/// @param g Grid implementation whose placement metadata is updated.
/// @param child Child whose explicit placement should be forgotten.
static void grid_remove_placement(grid_impl_t *g, vg_widget_t *child) {
    if (!g || !child)
        return;
    for (int i = 0; i < g->placement_count; i++) {
        if (g->placements[i].child != child)
            continue;
        int remaining = g->placement_count - i - 1;
        if (remaining > 0)
            memmove(&g->placements[i],
                    &g->placements[i + 1],
                    (size_t)remaining * sizeof(grid_placement_t));
        g->placement_count--;
        if (g->placement_count >= 0)
            memset(&g->placements[g->placement_count], 0, sizeof(grid_placement_t));
        return;
    }
}

/// @brief Clamps a track count to [1, VG_GRID_MAX_TRACKS].
/// @param count Requested number of rows or columns.
/// @return Valid track count within the implementation limit.
static int grid_clamp_track_count(int count) {
    if (count < 1)
        return 1;
    if (count > VG_GRID_MAX_TRACKS)
        return VG_GRID_MAX_TRACKS;
    return count;
}

/// @brief Clamps a track index to [0, VG_GRID_MAX_TRACKS - 1].
/// @param index Requested zero-based row or column index.
/// @return Index clamped to the representable track range.
static int grid_clamp_track_index(int index) {
    if (index < 0)
        return 0;
    if (index >= VG_GRID_MAX_TRACKS)
        return VG_GRID_MAX_TRACKS - 1;
    return index;
}

/// @brief Clamps a column or row span to [1, VG_GRID_MAX_TRACKS].
/// @param span Requested number of tracks covered by a child.
/// @return Span clamped to the supported nonzero range.
static int grid_clamp_span(int span) {
    if (span < 1)
        return 1;
    if (span > VG_GRID_MAX_TRACKS)
        return VG_GRID_MAX_TRACKS;
    return span;
}

/// @brief Computes the effective row count: max of declared rows, auto-flow rows needed, and
/// explicit row+span extents.
/// @param g Grid implementation containing declared and explicit placement metadata.
/// @param self Grid widget whose visible children contribute auto-flow rows.
/// @param cols Effective nonzero column count.
/// @return Required row count, clamped to the supported track range.
static int grid_effective_rows(grid_impl_t *g, vg_widget_t *self, int cols) {
    int rows = grid_clamp_track_count(g->layout.rows);
    int auto_count = 0;

    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        grid_placement_t *p = grid_find_placement(g, child);
        if (!p) {
            if (auto_count < VG_GRID_MAX_TRACKS)
                auto_count++;
            continue;
        }

        int row = grid_clamp_track_index(p->item.row);
        int row_span = grid_clamp_span(p->item.row_span);
        if (row_span > VG_GRID_MAX_TRACKS - row)
            row_span = VG_GRID_MAX_TRACKS - row;
        if (row + row_span > rows)
            rows = row + row_span;
    }

    int auto_rows = (auto_count + cols - 1) / cols;
    if (auto_rows > rows)
        rows = auto_rows;
    return grid_clamp_track_count(rows);
}

/// @brief Allocates or reallocates @p tracks to @p new_count entries, zero-filling any newly added
/// slots.
/// @param[in,out] tracks Address of the owned track-definition array.
/// @param old_count Number of initialized entries in the existing array.
/// @param new_count Required number of entries after resizing.
/// @param default_definition Value assigned to each newly created entry.
/// @return True on success; false for invalid sizes, overflow, or allocation failure.
static bool grid_resize_track_array(float **tracks,
                                    int old_count,
                                    int new_count,
                                    float default_definition) {
    if (!tracks || new_count <= 0)
        return false;
    if ((size_t)new_count > SIZE_MAX / sizeof(float))
        return false;
    if (!*tracks) {
        float *created = malloc((size_t)new_count * sizeof(float));
        if (!created)
            return false;
        for (int i = 0; i < new_count; i++)
            created[i] = default_definition;
        *tracks = created;
        return true;
    }
    float *resized = realloc(*tracks, (size_t)new_count * sizeof(float));
    if (!resized)
        return false;
    if (new_count > old_count) {
        for (int i = old_count; i < new_count; i++)
            resized[i] = default_definition;
    }
    *tracks = resized;
    return true;
}

/// @brief Return one declared track definition or the default one-fraction track.
/// @param definitions Optional declared track-definition array.
/// @param index Track index to query.
/// @param declared_count Number of entries available in @p definitions.
/// @return Finite declared definition, or `-1.0f` for an absent or invalid entry.
static float grid_track_definition(const float *definitions, int index, int declared_count) {
    if (!definitions || index < 0 || index >= declared_count || !isfinite(definitions[index]))
        return -1.0f;
    return definitions[index];
}

/// @brief Seed resolved track sizes with positive fixed definitions.
/// @param[out] sizes Resolved-size array with @p count writable entries.
/// @param count Number of tracks to initialize.
/// @param definitions Optional declared fixed, auto, or fractional definitions.
/// @param declared_count Number of entries available in @p definitions.
static void grid_seed_track_sizes(float *sizes,
                                  int count,
                                  const float *definitions,
                                  int declared_count) {
    for (int i = 0; i < count; i++) {
        float definition = grid_track_definition(definitions, i, declared_count);
        sizes[i] = definition > 0.0f ? definition : 0.0f;
    }
}

/// @brief Grow auto/content tracks in a span until an intrinsic child extent fits.
/// @param[in,out] sizes Resolved track sizes to grow.
/// @param count Number of entries in @p sizes.
/// @param definitions Optional declared track definitions.
/// @param declared_count Number of entries available in @p definitions.
/// @param start First track covered by the child.
/// @param span Requested number of covered tracks.
/// @param gap Gap between adjacent covered tracks.
/// @param required Intrinsic child extent that the covered tracks must accommodate.
static void grid_grow_tracks_for_intrinsic(float *sizes,
                                           int count,
                                           const float *definitions,
                                           int declared_count,
                                           int start,
                                           int span,
                                           float gap,
                                           float required) {
    if (!sizes || count <= 0 || start < 0 || start >= count || span <= 0)
        return;
    if (span > count - start)
        span = count - start;

    float current = gap * (float)(span - 1);
    float total_weight = 0.0f;
    for (int i = start; i < start + span; i++) {
        current += sizes[i];
        float definition = grid_track_definition(definitions, i, declared_count);
        if (definition == 0.0f)
            total_weight += 1.0f;
    }
    if (required <= current || total_weight <= 0.0f)
        return;

    float deficit = required - current;
    for (int i = start; i < start + span; i++) {
        float definition = grid_track_definition(definitions, i, declared_count);
        if (definition != 0.0f)
            continue;
        sizes[i] += deficit / total_weight;
    }
}

/// @brief Distribute remaining available space among negative fractional tracks.
/// @param[in,out] sizes Resolved track sizes to extend.
/// @param count Number of entries in @p sizes.
/// @param definitions Optional declared track definitions.
/// @param declared_count Number of entries available in @p definitions.
/// @param gap Gap between adjacent tracks.
/// @param available Total content-axis extent available to the grid.
static void grid_distribute_fractional_space(float *sizes,
                                             int count,
                                             const float *definitions,
                                             int declared_count,
                                             float gap,
                                             float available) {
    if (!sizes || count <= 0)
        return;
    float used = gap * (float)(count - 1);
    float total_weight = 0.0f;
    for (int i = 0; i < count; i++) {
        used += sizes[i];
        float definition = grid_track_definition(definitions, i, declared_count);
        if (definition < 0.0f)
            total_weight += -definition;
    }
    float remaining = available - used;
    if (remaining <= 0.0f || total_weight <= 0.0f)
        return;
    for (int i = 0; i < count; i++) {
        float definition = grid_track_definition(definitions, i, declared_count);
        if (definition < 0.0f)
            sizes[i] += remaining * (-definition) / total_weight;
    }
}

/// @brief Resolve fixed, auto/content, and fractional row and column sizes from measured children.
/// @param g Grid implementation containing track definitions and gaps.
/// @param self Grid widget whose measured children supply intrinsic extents.
/// @param cols Effective column count.
/// @param rows Effective row count.
/// @param content_width Available width inside grid padding.
/// @param content_height Available height inside grid padding.
/// @param[out] column_sizes Array receiving @p cols resolved widths.
/// @param[out] row_sizes Array receiving @p rows resolved heights.
static void grid_resolve_tracks(grid_impl_t *g,
                                vg_widget_t *self,
                                int cols,
                                int rows,
                                float content_width,
                                float content_height,
                                float *column_sizes,
                                float *row_sizes) {
    grid_seed_track_sizes(column_sizes, cols, g->layout.column_widths, g->layout.columns);
    grid_seed_track_sizes(row_sizes, rows, g->layout.row_heights, g->layout.rows);

    int auto_index = 0;
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position)
            continue;
        grid_placement_t *placement = grid_find_placement(g, child);
        int column = 0;
        int row = 0;
        int column_span = 1;
        int row_span = 1;
        if (placement) {
            column = placement->item.column;
            row = placement->item.row;
            column_span = placement->item.col_span;
            row_span = placement->item.row_span;
        } else {
            column = auto_index % cols;
            row = auto_index / cols;
            auto_index++;
        }
        if (column < 0 || column >= cols)
            column = 0;
        if (row < 0 || row >= rows)
            row = 0;
        column_span = grid_clamp_span(column_span);
        row_span = grid_clamp_span(row_span);
        if (column_span > cols - column)
            column_span = cols - column;
        if (row_span > rows - row)
            row_span = rows - row;

        grid_grow_tracks_for_intrinsic(column_sizes,
                                       cols,
                                       g->layout.column_widths,
                                       g->layout.columns,
                                       column,
                                       column_span,
                                       g->layout.column_gap,
                                       layout_nonnegative(child->measured_width));
        grid_grow_tracks_for_intrinsic(row_sizes,
                                       rows,
                                       g->layout.row_heights,
                                       g->layout.rows,
                                       row,
                                       row_span,
                                       g->layout.row_gap,
                                       layout_nonnegative(child->measured_height));
    }

    grid_distribute_fractional_space(column_sizes,
                                     cols,
                                     g->layout.column_widths,
                                     g->layout.columns,
                                     g->layout.column_gap,
                                     content_width);
    grid_distribute_fractional_space(
        row_sizes, rows, g->layout.row_heights, g->layout.rows, g->layout.row_gap, content_height);
}

/// @brief Measures the grid by distributing available space across columns/rows and measuring each
/// child in its cell.
/// @param self Grid widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided vertical space.
static void grid_measure(vg_widget_t *self, float available_width, float available_height) {
    if (!self || !self->impl_data)
        return;

    grid_impl_t *g = (grid_impl_t *)self->impl_data;
    int cols = grid_clamp_track_count(g->layout.columns);
    int rows = grid_effective_rows(g, self, cols);

    float padding_h = self->layout.padding_left + self->layout.padding_right;
    float padding_v = self->layout.padding_top + self->layout.padding_bottom;
    float content_w = layout_nonnegative(available_width - padding_h);
    float content_h = layout_nonnegative(available_height - padding_v);

    /* Compute column widths */
    float total_col_gap = g->layout.column_gap * (cols - 1);
    float auto_col_w = (content_w - total_col_gap) / (float)cols;
    if (auto_col_w < 0)
        auto_col_w = 0;

    /* Compute row heights */
    float total_row_gap = g->layout.row_gap * (rows - 1);
    float auto_row_h = (content_h - total_row_gap) / (float)rows;
    if (auto_row_h < 0)
        auto_row_h = 0;

    /* Measure each child at its cell size */
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_measure_child(child, content_w, content_h);
            continue;
        }
        grid_placement_t *p = grid_find_placement(g, child);
        int col = p ? ((p->item.column >= 0 && p->item.column < cols) ? p->item.column : 0) : 0;
        int row = p ? grid_clamp_track_index(p->item.row) : 0;
        if (row >= rows)
            row = rows - 1;
        int cs = p ? grid_clamp_span(p->item.col_span) : 1;
        int rs = p ? grid_clamp_span(p->item.row_span) : 1;
        if (col + cs > cols)
            cs = cols - col;
        if (row + rs > rows)
            rs = rows - row;

        float cell_w = auto_col_w * cs + g->layout.column_gap * (cs - 1);
        float cell_h = auto_row_h * rs + g->layout.row_gap * (rs - 1);

        layout_measure_child(child, cell_w, cell_h);
    }

    size_t scratch_count = (size_t)cols + (size_t)rows;
    if (scratch_count > SIZE_MAX / sizeof(float))
        return;
    float scratch_stack[128];
    float *scratch = scratch_count <= (sizeof(scratch_stack) / sizeof(scratch_stack[0]))
                         ? scratch_stack
                         : malloc(scratch_count * sizeof(float));
    if (!scratch)
        return;
    float *column_sizes = scratch;
    float *row_sizes = column_sizes + cols;
    grid_resolve_tracks(g, self, cols, rows, content_w, content_h, column_sizes, row_sizes);

    float grid_w = g->layout.column_gap * (float)(cols - 1);
    float grid_h = g->layout.row_gap * (float)(rows - 1);
    for (int c = 0; c < cols; c++)
        grid_w += column_sizes[c];
    for (int r = 0; r < rows; r++)
        grid_h += row_sizes[r];
    if (scratch != scratch_stack)
        free(scratch);

    self->measured_width = grid_w + padding_h;
    self->measured_height = grid_h + padding_v;
    layout_apply_constraints(self);
}

/// @brief Arranges each child in its grid cell, supporting explicit placement, column/row spanning,
/// and auto-flow.
/// @param self Grid widget and children to arrange.
/// @param x Assigned X origin in the parent coordinate system.
/// @param y Assigned Y origin in the parent coordinate system.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void grid_arrange(vg_widget_t *self, float x, float y, float width, float height) {
    if (!self || !self->impl_data)
        return;

    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;

    grid_impl_t *g = (grid_impl_t *)self->impl_data;
    int cols = grid_clamp_track_count(g->layout.columns);
    int rows = grid_effective_rows(g, self, cols);

    float content_x = self->layout.padding_left;
    float content_y = self->layout.padding_top;
    float content_w =
        layout_nonnegative(width - self->layout.padding_left - self->layout.padding_right);
    float content_h =
        layout_nonnegative(height - self->layout.padding_top - self->layout.padding_bottom);

    size_t scratch_count = ((size_t)cols + (size_t)rows) * 2u;
    if (scratch_count > SIZE_MAX / sizeof(float))
        return;
    float scratch_stack[128];
    float *scratch = scratch_count <= (sizeof(scratch_stack) / sizeof(scratch_stack[0]))
                         ? scratch_stack
                         : malloc(scratch_count * sizeof(float));
    if (!scratch)
        return;
    float *col_x = scratch;
    float *col_w = col_x + cols;
    float *row_y = col_w + cols;
    float *row_h = row_y + rows;

    grid_resolve_tracks(g, self, cols, rows, content_w, content_h, col_w, row_h);

    float cursor = content_x;
    for (int c = 0; c < cols; c++) {
        col_x[c] = cursor;
        cursor += col_w[c] + g->layout.column_gap;
    }

    /* Compute row Y positions */
    cursor = content_y;
    for (int r = 0; r < rows; r++) {
        row_y[r] = cursor;
        cursor += row_h[r] + g->layout.row_gap;
    }

    /* Arrange each child at its cell */
    int auto_idx = 0; /* sequential auto-placement counter */
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_arrange_manual_child(child);
            continue;
        }
        grid_placement_t *p = grid_find_placement(g, child);

        int col, row, cs, rs;
        if (p) {
            col = (p->item.column >= 0 && p->item.column < cols) ? p->item.column : 0;
            row = grid_clamp_track_index(p->item.row);
            if (row >= rows)
                row = rows - 1;
            cs = grid_clamp_span(p->item.col_span);
            rs = grid_clamp_span(p->item.row_span);
        } else {
            /* Auto-flow: place sequentially left-to-right, top-to-bottom */
            col = auto_idx % cols;
            row = auto_idx / cols;
            if (row >= rows) {
                vg_widget_arrange(child, content_x, content_y, 0.0f, 0.0f);
                auto_idx++;
                continue;
            }
            cs = 1;
            rs = 1;
            auto_idx++;
        }

        /* Clamp spans to grid bounds */
        if (col + cs > cols)
            cs = cols - col;
        if (row + rs > rows)
            rs = rows - row;
        if (cs < 1)
            cs = 1;
        if (rs < 1)
            rs = 1;

        /* Compute cell bounds spanning multiple columns/rows */
        float cell_x = col < cols ? col_x[col] : 0;
        float cell_y = row < rows ? row_y[row] : 0;
        float cell_w = 0;
        float cell_h = 0;

        for (int c = col; c < col + cs && c < cols; c++)
            cell_w += col_w[c] + (c < col + cs - 1 ? g->layout.column_gap : 0);
        for (int r = row; r < row + rs && r < rows; r++)
            cell_h += row_h[r] + (r < row + rs - 1 ? g->layout.row_gap : 0);

        vg_widget_arrange(
            child, cell_x, cell_y, layout_nonnegative(cell_w), layout_nonnegative(cell_h));

        if (!p)
            auto_idx = row * cols + col + cs; /* advance past spanned cells */
    }

    if (scratch != scratch_stack)
        free(scratch);
}

static const vg_widget_vtable_t g_grid_vtable = {
    .destroy = grid_destroy,
    .measure = grid_measure,
    .arrange = grid_arrange,
};

/// @brief Creates a grid layout container with the specified column and row counts; returns NULL on
/// allocation failure.
/// @param columns Requested declared column count, clamped to the supported range.
/// @param rows Requested declared row count, clamped to the supported range.
/// @return Newly allocated Grid widget, or NULL on allocation failure.
vg_widget_t *vg_grid_create(int columns, int rows) {
    columns = grid_clamp_track_count(columns);
    rows = grid_clamp_track_count(rows);

    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!widget)
        return NULL;

    widget->vtable = &g_grid_vtable;

    grid_impl_t *g = calloc(1, sizeof(grid_impl_t));
    if (!g) {
        vg_widget_destroy(widget);
        return NULL;
    }

    g->layout.columns = columns;
    g->layout.rows = rows;
    g->layout.column_gap = 0;
    g->layout.row_gap = 0;
    g->layout.column_widths = NULL;
    g->layout.row_heights = NULL;
    g->placement_capacity = 8;
    g->placement_count = 0;
    g->placements = calloc(g->placement_capacity, sizeof(grid_placement_t));
    if (!g->placements) {
        free(g);
        vg_widget_destroy(widget);
        return NULL;
    }

    widget->impl_data = g;
    return widget;
}

/// @brief Sets the grid's column count, resizing the column_widths array if it exists.
/// @param grid Grid widget to update; invalid or differently typed widgets are ignored.
/// @param columns Requested declared column count, clamped to the supported range.
void vg_grid_set_columns(vg_widget_t *grid, int columns) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data)
        return;
    columns = grid_clamp_track_count(columns);
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    int old_columns = g->layout.columns;
    if (old_columns == columns)
        return;
    if (g->layout.column_widths &&
        !grid_resize_track_array(&g->layout.column_widths, old_columns, columns, -1.0f))
        return;
    g->layout.columns = columns;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
}

/// @brief Sets the grid's row count, resizing the row_heights array if it exists.
/// @param grid Grid widget to update; invalid or differently typed widgets are ignored.
/// @param rows Requested declared row count, clamped to the supported range.
void vg_grid_set_rows(vg_widget_t *grid, int rows) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data)
        return;
    rows = grid_clamp_track_count(rows);
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    int old_rows = g->layout.rows;
    if (old_rows == rows)
        return;
    if (g->layout.row_heights &&
        !grid_resize_track_array(&g->layout.row_heights, old_rows, rows, -1.0f))
        return;
    g->layout.rows = rows;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
}

/// @brief Sets the column and row gap sizes for the grid layout.
/// @param grid Grid widget to update; invalid or differently typed widgets are ignored.
/// @param column_gap Horizontal inter-track gap, normalized to a finite non-negative value.
/// @param row_gap Vertical inter-track gap, normalized to a finite non-negative value.
void vg_grid_set_gap(vg_widget_t *grid, float column_gap, float row_gap) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data)
        return;
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    column_gap = layout_nonnegative(column_gap);
    row_gap = layout_nonnegative(row_gap);
    if (g->layout.column_gap == column_gap && g->layout.row_gap == row_gap)
        return;
    g->layout.column_gap = column_gap;
    g->layout.row_gap = row_gap;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
}

/// @brief Sets an explicit pixel width for the given column index, allocating the widths array if
/// necessary.
/// @param grid Grid widget to update; invalid or differently typed widgets are ignored.
/// @param column Zero-based declared column index.
/// @param width Track definition to store; non-finite values become auto/content sizing.
void vg_grid_set_column_width(vg_widget_t *grid, int column, float width) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data || column < 0)
        return;
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    int cols = g->layout.columns;
    if (column >= cols)
        return;
    if (!isfinite(width))
        width = 0.0f;
    if (!g->layout.column_widths &&
        !grid_resize_track_array(&g->layout.column_widths, 0, cols, -1.0f))
        return;
    if (g->layout.column_widths[column] == width)
        return;
    g->layout.column_widths[column] = width;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
}

/// @brief Sets an explicit pixel height for the given row index, allocating the heights array if
/// necessary.
/// @param grid Grid widget to update; invalid or differently typed widgets are ignored.
/// @param row Zero-based declared row index.
/// @param height Track definition to store; non-finite values become auto/content sizing.
void vg_grid_set_row_height(vg_widget_t *grid, int row, float height) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data || row < 0)
        return;
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    int rows = g->layout.rows;
    if (row >= rows)
        return;
    if (!isfinite(height))
        height = 0.0f;
    if (!g->layout.row_heights && !grid_resize_track_array(&g->layout.row_heights, 0, rows, -1.0f))
        return;
    if (g->layout.row_heights[row] == height)
        return;
    g->layout.row_heights[row] = height;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
}

/// @brief Commit already-validated grid placement metadata, growing storage atomically.
/// @param grid Grid widget whose placement table is updated.
/// @param child Direct child receiving explicit placement metadata.
/// @param column Valid zero-based starting column.
/// @param row Valid zero-based starting row.
/// @param col_span Valid number of covered columns.
/// @param row_span Valid number of covered rows.
/// @return True when the existing record was updated or a new record was committed.
static bool grid_commit_placement(
    vg_widget_t *grid, vg_widget_t *child, int column, int row, int col_span, int row_span) {
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;

    grid_placement_t *existing = grid_find_placement(g, child);
    if (existing) {
        if (existing->item.column == column && existing->item.row == row &&
            existing->item.col_span == col_span && existing->item.row_span == row_span) {
            return true;
        }
        existing->item.column = column;
        existing->item.row = row;
        existing->item.col_span = col_span;
        existing->item.row_span = row_span;
        vg_widget_invalidate_layout(grid);
        vg_widget_note_revision(grid);
        return true;
    }

    if (g->placement_count >= g->placement_capacity) {
        if (g->placement_capacity > INT32_MAX / 2)
            return false;
        int new_cap = g->placement_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(grid_placement_t))
            return false;
        grid_placement_t *new_p =
            realloc(g->placements, (size_t)new_cap * sizeof(grid_placement_t));
        if (!new_p)
            return false;
        g->placements = new_p;
        g->placement_capacity = new_cap;
    }

    grid_placement_t *p = &g->placements[g->placement_count++];
    p->child = child;
    p->item.column = column;
    p->item.row = row;
    p->item.col_span = col_span;
    p->item.row_span = row_span;
    vg_widget_invalidate_layout(grid);
    vg_widget_note_revision(grid);
    return true;
}

/// @brief Validate and atomically place an existing direct child in declared grid tracks.
/// @param grid Grid widget that already owns @p child.
/// @param child Direct child receiving explicit placement.
/// @param column Zero-based starting column.
/// @param row Zero-based starting row.
/// @param col_span Number of declared columns to cover.
/// @param row_span Number of declared rows to cover.
/// @return True when all bounds and ownership checks pass and placement is committed.
bool vg_grid_place_checked(
    vg_widget_t *grid, vg_widget_t *child, int column, int row, int col_span, int row_span) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data || !child ||
        child->parent != grid || column < 0 || row < 0 || col_span < 1 || row_span < 1) {
        return false;
    }
    grid_impl_t *g = (grid_impl_t *)grid->impl_data;
    if (column >= g->layout.columns || row >= g->layout.rows ||
        col_span > g->layout.columns - column || row_span > g->layout.rows - row) {
        return false;
    }
    return grid_commit_placement(grid, child, column, row, col_span, row_span);
}

/// @brief Compatibility placement API that preserves legacy index clamping and implicit rows.
/// @param grid Grid widget whose metadata is updated.
/// @param child Child receiving explicit placement metadata.
/// @param column Requested starting column, clamped to the supported index range.
/// @param row Requested starting row, clamped to the supported index range.
/// @param col_span Requested column span, clamped to a supported nonzero value.
/// @param row_span Requested row span, clamped to a supported nonzero value.
void vg_grid_place(
    vg_widget_t *grid, vg_widget_t *child, int column, int row, int col_span, int row_span) {
    if (!grid || grid->vtable != &g_grid_vtable || !grid->impl_data || !child)
        return;
    (void)grid_commit_placement(grid,
                                child,
                                grid_clamp_track_index(column),
                                grid_clamp_track_index(row),
                                grid_clamp_span(col_span),
                                grid_clamp_span(row_span));
}

//=============================================================================
// Dock Layout Implementation
//=============================================================================

/// @brief Per-child dock position stored as a tagged entry in dock_impl_t.
typedef struct dock_entry {
    vg_widget_t *child;
    vg_dock_t position;
} dock_entry_t;

/// @brief Dock container internal state.
typedef struct dock_impl {
    dock_entry_t *entries;
    int entry_count;
    int entry_capacity;
    float gap; ///< Physical space reserved between each claimed edge and the remainder.
} dock_impl_t;

/// @brief Frees the dock's entry array and impl struct.
/// @param self Dock widget whose implementation storage is released.
static void dock_destroy(vg_widget_t *self) {
    if (!self || !self->impl_data)
        return;
    dock_impl_t *d = (dock_impl_t *)vg_widget_take_impl_data(self);
    free(d->entries);
    free(d);
}

/// @brief Measures the dock container to fill the full available area (children determine content,
/// not the container).
/// @param self Dock widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space, or a non-positive value if unbounded.
/// @param available_height Parent-provided vertical space, or a non-positive value if unbounded.
static void dock_measure(vg_widget_t *self, float available_width, float available_height) {
    if (!self || !self->impl_data)
        return;

    /* Dock measures as available space; children determine actual content */
    self->measured_width = available_width > 0 ? available_width : 100.0f;
    self->measured_height = available_height > 0 ? available_height : 100.0f;
    layout_apply_constraints(self);
}

/// @brief Arranges docked children in order; each child claims its edge (TOP/BOTTOM/LEFT/RIGHT)
/// from the remaining rect; FILL children get the remainder.
/// @param self Dock widget and children to arrange.
/// @param x Assigned X origin in the parent coordinate system.
/// @param y Assigned Y origin in the parent coordinate system.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
static void dock_arrange(vg_widget_t *self, float x, float y, float width, float height) {
    if (!self || !self->impl_data)
        return;

    self->x = x;
    self->y = y;
    self->width = width;
    self->height = height;

    dock_impl_t *d = (dock_impl_t *)self->impl_data;

    /* Remaining area after docked children claim their edges */
    float rem_x = self->layout.padding_left;
    float rem_y = self->layout.padding_top;
    float rem_w =
        layout_nonnegative(width - self->layout.padding_left - self->layout.padding_right);
    float rem_h =
        layout_nonnegative(height - self->layout.padding_top - self->layout.padding_bottom);

    /* Process children in order; each docked child takes from the remaining area */
    VG_FOREACH_VISIBLE_CHILD(self, child) {
        if (child->manual_position) {
            layout_measure_child(child, rem_w, rem_h);
            layout_arrange_manual_child(child);
            continue;
        }
        /* Look up this child's dock position */
        vg_dock_t pos = VG_DOCK_FILL;
        for (int i = 0; i < d->entry_count; i++) {
            if (d->entries[i].child == child) {
                pos = d->entries[i].position;
                break;
            }
        }

        /* Measure child in the remaining area */
        layout_measure_child(child, rem_w, rem_h);

        switch (pos) {
            case VG_DOCK_LEFT: {
                float cw = layout_nonnegative(child->measured_width);
                if (cw > rem_w)
                    cw = rem_w;
                vg_widget_arrange(child, rem_x, rem_y, cw, layout_nonnegative(rem_h));
                float consumed_gap = d->gap < rem_w - cw ? d->gap : rem_w - cw;
                if (consumed_gap < 0.0f)
                    consumed_gap = 0.0f;
                rem_x += cw + consumed_gap;
                rem_w -= cw + consumed_gap;
                if (rem_w < 0)
                    rem_w = 0;
                break;
            }
            case VG_DOCK_RIGHT: {
                float cw = layout_nonnegative(child->measured_width);
                if (cw > rem_w)
                    cw = rem_w;
                vg_widget_arrange(child, rem_x + rem_w - cw, rem_y, cw, layout_nonnegative(rem_h));
                float consumed_gap = d->gap < rem_w - cw ? d->gap : rem_w - cw;
                if (consumed_gap < 0.0f)
                    consumed_gap = 0.0f;
                rem_w -= cw + consumed_gap;
                if (rem_w < 0)
                    rem_w = 0;
                break;
            }
            case VG_DOCK_TOP: {
                float ch = layout_nonnegative(child->measured_height);
                if (ch > rem_h)
                    ch = rem_h;
                vg_widget_arrange(child, rem_x, rem_y, layout_nonnegative(rem_w), ch);
                float consumed_gap = d->gap < rem_h - ch ? d->gap : rem_h - ch;
                if (consumed_gap < 0.0f)
                    consumed_gap = 0.0f;
                rem_y += ch + consumed_gap;
                rem_h -= ch + consumed_gap;
                if (rem_h < 0)
                    rem_h = 0;
                break;
            }
            case VG_DOCK_BOTTOM: {
                float ch = layout_nonnegative(child->measured_height);
                if (ch > rem_h)
                    ch = rem_h;
                vg_widget_arrange(child, rem_x, rem_y + rem_h - ch, layout_nonnegative(rem_w), ch);
                float consumed_gap = d->gap < rem_h - ch ? d->gap : rem_h - ch;
                if (consumed_gap < 0.0f)
                    consumed_gap = 0.0f;
                rem_h -= ch + consumed_gap;
                if (rem_h < 0)
                    rem_h = 0;
                break;
            }
            case VG_DOCK_FILL:
            default:
                vg_widget_arrange(
                    child, rem_x, rem_y, layout_nonnegative(rem_w), layout_nonnegative(rem_h));
                break;
        }
    }
}

static const vg_widget_vtable_t g_dock_vtable = {
    .destroy = dock_destroy,
    .measure = dock_measure,
    .arrange = dock_arrange,
};

/// @brief Creates a dock layout container with an initial 8-entry dock registry; returns NULL on
/// allocation failure.
vg_widget_t *vg_dock_create(void) {
    vg_widget_t *widget = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!widget)
        return NULL;

    widget->vtable = &g_dock_vtable;

    dock_impl_t *d = calloc(1, sizeof(dock_impl_t));
    if (!d) {
        vg_widget_destroy(widget);
        return NULL;
    }

    d->entry_capacity = 8;
    d->entries = calloc(d->entry_capacity, sizeof(dock_entry_t));
    if (!d->entries) {
        free(d);
        vg_widget_destroy(widget);
        return NULL;
    }

    widget->impl_data = d;
    return widget;
}

/// @brief Commit a dock assignment, optionally preserving legacy cross-parent reparenting.
/// @param dock Dock widget that should own and arrange @p child.
/// @param child Widget receiving a dock position.
/// @param position Edge or fill position to assign.
/// @param allow_reparent True to detach @p child from a different parent when necessary.
/// @return True when the assignment is valid and stored; false on validation or allocation
///         failure.
static bool dock_add_internal(vg_widget_t *dock,
                              vg_widget_t *child,
                              vg_dock_t position,
                              bool allow_reparent) {
    if (!dock || dock->vtable != &g_dock_vtable || !dock->impl_data || !child ||
        position < VG_DOCK_LEFT || position > VG_DOCK_FILL || child == dock ||
        (!allow_reparent && child->parent && child->parent != dock)) {
        return false;
    }
    dock_impl_t *d = (dock_impl_t *)dock->impl_data;

    for (int i = 0; i < d->entry_count; i++) {
        if (d->entries[i].child == child) {
            if (d->entries[i].position == position)
                return true;
            d->entries[i].position = position;
            vg_widget_invalidate_layout(dock);
            vg_widget_note_revision(dock);
            return true;
        }
    }

    if (d->entry_count >= d->entry_capacity) {
        if (d->entry_capacity > INT32_MAX / 2)
            return false;
        int new_cap = d->entry_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(dock_entry_t))
            return false;
        dock_entry_t *new_e = realloc(d->entries, (size_t)new_cap * sizeof(dock_entry_t));
        if (!new_e)
            return false;
        d->entries = new_e;
        d->entry_capacity = new_cap;
    }

    if (child->parent != dock) {
        vg_widget_add_child(dock, child);
        if (child->parent != dock)
            return false;
    }

    d->entries[d->entry_count].child = child;
    d->entries[d->entry_count].position = position;
    d->entry_count++;
    vg_widget_invalidate_layout(dock);
    vg_widget_note_revision(dock);
    return true;
}

/// @brief Add or update a child while rejecting cross-parent ownership changes.
/// @param dock Dock widget that should already own @p child or accept an unparented child.
/// @param child Widget receiving a dock position.
/// @param position Edge or fill position to assign.
/// @return True when the child is assigned without stealing it from another parent.
bool vg_dock_add_checked(vg_widget_t *dock, vg_widget_t *child, vg_dock_t position) {
    return dock_add_internal(dock, child, position, false);
}

/// @brief Compatibility docking API that retains legacy automatic reparenting.
/// @param dock Dock widget that should own and arrange @p child.
/// @param child Widget receiving a dock position.
/// @param position Edge or fill position to assign.
void vg_dock_add(vg_widget_t *dock, vg_widget_t *child, vg_dock_t position) {
    (void)dock_add_internal(dock, child, position, true);
}

/// @brief Set the gap between claimed dock regions and the remaining rectangle.
/// @param dock Dock widget to update; invalid or differently typed widgets are ignored.
/// @param gap Requested inter-region gap, normalized to a finite non-negative value.
void vg_dock_set_gap(vg_widget_t *dock, float gap) {
    if (!dock || dock->vtable != &g_dock_vtable || !dock->impl_data)
        return;
    gap = layout_nonnegative(gap);
    dock_impl_t *d = (dock_impl_t *)dock->impl_data;
    if (d->gap == gap)
        return;
    d->gap = gap;
    vg_widget_invalidate_layout(dock);
    vg_widget_note_revision(dock);
}

/// @brief Removes the dock entry for @p child, shifting remaining entries to fill the gap.
/// @param d Dock implementation whose metadata is updated.
/// @param child Child whose dock assignment should be forgotten.
static void dock_remove_entry(dock_impl_t *d, vg_widget_t *child) {
    if (!d || !child)
        return;
    for (int i = 0; i < d->entry_count; i++) {
        if (d->entries[i].child != child)
            continue;
        int remaining = d->entry_count - i - 1;
        if (remaining > 0)
            memmove(&d->entries[i], &d->entries[i + 1], (size_t)remaining * sizeof(dock_entry_t));
        d->entry_count--;
        if (d->entry_count >= 0)
            memset(&d->entries[d->entry_count], 0, sizeof(dock_entry_t));
        return;
    }
}

/// @brief Called when a child is detached from a layout container; removes grid/dock metadata so
/// stale entries don't survive re-parenting.
/// @param parent Former Grid or Dock parent whose private metadata may reference the child.
/// @param child Detached child whose metadata should be removed.
void vg_layout_on_child_detached(vg_widget_t *parent, vg_widget_t *child) {
    if (!parent || !child || !parent->impl_data || !parent->vtable)
        return;

    if (parent->vtable == &g_grid_vtable) {
        grid_remove_placement((grid_impl_t *)parent->impl_data, child);
        parent->needs_layout = true;
    } else if (parent->vtable == &g_dock_vtable) {
        dock_remove_entry((dock_impl_t *)parent->impl_data, child);
        parent->needs_layout = true;
    }
}

/// @brief Return the concrete layout kind represented by a widget's private vtable.
/// @param widget Widget whose registered layout vtable is inspected.
/// @return Matching layout kind, or @ref VG_LAYOUT_NONE for null or non-layout widgets.
vg_layout_type_t vg_layout_get_type(const vg_widget_t *widget) {
    if (!widget || !widget->vtable)
        return VG_LAYOUT_NONE;
    if (widget->vtable == &g_vbox_vtable)
        return VG_LAYOUT_VBOX;
    if (widget->vtable == &g_hbox_vtable)
        return VG_LAYOUT_HBOX;
    if (widget->vtable == &g_flex_vtable)
        return VG_LAYOUT_FLEX;
    if (widget->vtable == &g_grid_vtable)
        return VG_LAYOUT_GRID;
    if (widget->vtable == &g_dock_vtable)
        return VG_LAYOUT_DOCK;
    return VG_LAYOUT_NONE;
}

//=============================================================================
// Layout Engine Entry Points
//=============================================================================

/// @brief Entry-point: arranges @p container as a VBox at its current position with the given
/// dimensions.
/// @param container VBox container to arrange.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
void vg_layout_vbox(vg_widget_t *container, float width, float height) {
    vbox_arrange(container, container->x, container->y, width, height);
}

/// @brief Entry-point: arranges @p container as an HBox at its current position with the given
/// dimensions.
/// @param container HBox container to arrange.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
void vg_layout_hbox(vg_widget_t *container, float width, float height) {
    hbox_arrange(container, container->x, container->y, width, height);
}

/// @brief Entry-point: arranges @p container as a Flex layout at its current position with the
/// given dimensions.
/// @param container Flex container to arrange.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
void vg_layout_flex(vg_widget_t *container, float width, float height) {
    flex_arrange(container, container->x, container->y, width, height);
}

/// @brief Entry-point: arranges @p container as a Grid layout at its current position with the
/// given dimensions.
/// @param container Grid container to arrange.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
void vg_layout_grid(vg_widget_t *container, float width, float height) {
    grid_arrange(container, container->x, container->y, width, height);
}

/// @brief Entry-point: arranges @p container as a Dock layout at its current position with the
/// given dimensions.
/// @param container Dock container to arrange.
/// @param width Assigned outer width.
/// @param height Assigned outer height.
void vg_layout_dock(vg_widget_t *container, float width, float height) {
    dock_arrange(container, container->x, container->y, width, height);
}
