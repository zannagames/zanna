//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_color.c
// Purpose: Runtime bindings for public ColorSwatch, ColorPalette, and
//          ColorPicker controls.
// Key invariants:
//   - The public color contract is 0x00RRGGBB; lower toolkit controls receive
//     0xFFRRGGBB and ColorPicker alpha remains a separate component.
//   - Every graphics-enabled operation validates live handle type and executes
//     on the GUI main thread.
//   - Graphics-disabled twins preserve deterministic null/zero/no-op behavior.
// Ownership/Lifetime:
//   - Parent widgets own attached controls. Detached controls remain owned by
//     the caller and are destroyed through the common Widget surface.
// Links: src/runtime/graphics/gui/rt_gui.h,
//        src/runtime/graphics/gui/rt_gui_internal.h,
//        src/lib/gui/include/vg_widgets.h
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_color.c
/// @brief Implements runtime bindings for swatch, palette, and picker color controls.
///
/// @details
/// The bindings translate the public `0x00RRGGBB` representation to the
/// widget layer's opaque ARGB form, validate live handles, and provide neutral
/// ABI-compatible behavior when graphics support is disabled.

#include "rt_gui_internal.h"
#include "rt_platform.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Resolve an optional runtime parent-container handle.
/// @details A NULL parent deliberately creates a detached widget. A non-NULL invalid or stale
///          handle is rejected rather than silently creating a detached widget.
/// @param parent Candidate runtime parent handle.
/// @return Live lower-toolkit parent widget, or NULL for either a deliberate NULL parent or an
///         invalid handle; callers distinguish those cases by inspecting @p parent.
static vg_widget_t *rt_color_parent_or_null_if_invalid(void *parent) {
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    return parent_widget;
}

/// @brief Convert a runtime integer color to an opaque lower-toolkit color.
/// @param color Runtime integer whose low 24 bits are public RGB.
/// @return Packed lower-toolkit `0xFFRRGGBB` color.
static uint32_t rt_color_to_opaque_argb(int64_t color) {
    return 0xFF000000u | ((uint32_t)color & 0x00FFFFFFu);
}

/// @brief Convert a lower-toolkit color to the public runtime RGB contract.
/// @param color Packed lower-toolkit color.
/// @return Non-negative `0x00RRGGBB` runtime integer.
static int64_t rt_color_from_argb(uint32_t color) {
    return (int64_t)(color & 0x00FFFFFFu);
}

/// @brief Validate and cast a runtime handle to a live ColorSwatch.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed live ColorSwatch pointer, or NULL for invalid, stale, or wrong-type handles.
static vg_colorswatch_t *rt_colorswatch_checked(void *handle) {
    return (vg_colorswatch_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_COLORSWATCH);
}

/// @brief Validate and cast a runtime handle to a live ColorPalette.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed live ColorPalette pointer, or NULL for invalid, stale, or wrong-type handles.
static vg_colorpalette_t *rt_colorpalette_checked(void *handle) {
    return (vg_colorpalette_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_COLORPALETTE);
}

/// @brief Validate and cast a runtime handle to a live ColorPicker.
/// @param handle Candidate runtime widget handle.
/// @return Borrowed live ColorPicker pointer, or NULL for invalid, stale, or wrong-type handles.
static vg_colorpicker_t *rt_colorpicker_checked(void *handle) {
    return (vg_colorpicker_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_COLORPICKER);
}

/// @brief Create a focusable opaque RGB swatch.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @param color Initial public RGB value in the low 24 bits.
/// @return Live ColorSwatch handle, or NULL for invalid parent/allocation failure.
void *rt_colorswatch_new(void *parent, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_color_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_colorswatch_create(parent_widget, rt_color_to_opaque_argb(color));
}

/// @brief Set one swatch's opaque RGB value.
/// @param swatch Live ColorSwatch handle; invalid handles are ignored.
/// @param color Public RGB value in the low 24 bits.
void rt_colorswatch_set_color(void *swatch, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    if (value)
        vg_colorswatch_set_color(value, rt_color_to_opaque_argb(color));
}

/// @brief Read one swatch's public RGB value.
/// @param swatch Live ColorSwatch handle.
/// @return `0x00RRGGBB`, or zero for an invalid handle.
int64_t rt_colorswatch_get_color(void *swatch) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    return value ? rt_color_from_argb(vg_colorswatch_get_color(value)) : 0;
}

/// @brief Set one swatch's selected state.
/// @param swatch Live ColorSwatch handle; invalid handles are ignored.
/// @param selected Non-zero to select, zero to clear.
void rt_colorswatch_set_selected(void *swatch, int64_t selected) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    if (value)
        vg_colorswatch_set_selected(value, selected != 0);
}

/// @brief Query one swatch's selected state.
/// @param swatch Live ColorSwatch handle.
/// @return 1 when selected, otherwise zero.
int64_t rt_colorswatch_is_selected(void *swatch) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    return value && vg_colorswatch_is_selected(value) ? 1 : 0;
}

/// @brief Consume one swatch color-or-selection change edge.
/// @param swatch Live ColorSwatch handle.
/// @return 1 once after unreported effective changes, otherwise zero.
int64_t rt_colorswatch_was_changed(void *swatch) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    return value && vg_widget_was_changed(&value->base) ? 1 : 0;
}

/// @brief Return one swatch's non-consuming state revision.
/// @param swatch Live ColorSwatch handle.
/// @return Monotonic revision, or zero for an invalid handle.
int64_t rt_colorswatch_get_revision(void *swatch) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorswatch_t *value = rt_colorswatch_checked(swatch);
    return value ? rt_widget_get_revision(&value->base) : 0;
}

/// @brief Create an empty keyboard-navigable color palette.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @return Live ColorPalette handle, or NULL for invalid parent/allocation failure.
void *rt_colorpalette_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_color_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_colorpalette_create(parent_widget);
}

/// @brief Append one opaque RGB entry to a palette.
/// @param palette Live ColorPalette handle; invalid handles are ignored.
/// @param color Public RGB value in the low 24 bits.
void rt_colorpalette_add_color(void *palette, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    if (value)
        vg_colorpalette_add_color(value, rt_color_to_opaque_argb(color));
}

/// @brief Remove one palette entry by zero-based index.
/// @param palette Live ColorPalette handle.
/// @param index Zero-based entry index.
/// @return 1 when removed, otherwise zero.
int64_t rt_colorpalette_remove_color(void *palette, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    if (!value || index < 0 || index > INT_MAX)
        return 0;
    return vg_colorpalette_remove_color(value, (int)index) ? 1 : 0;
}

/// @brief Clear all palette entries and selection.
/// @param palette Live ColorPalette handle; invalid handles are ignored.
void rt_colorpalette_clear(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    if (value)
        vg_colorpalette_clear(value);
}

/// @brief Return the number of palette entries.
/// @param palette Live ColorPalette handle.
/// @return Non-negative entry count, or zero for invalid handles.
int64_t rt_colorpalette_get_color_count(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    return value ? (int64_t)vg_colorpalette_get_color_count(value) : 0;
}

/// @brief Return one palette entry under the public RGB contract.
/// @param palette Live ColorPalette handle.
/// @param index Zero-based entry index.
/// @return `0x00RRGGBB`, or zero for invalid input.
int64_t rt_colorpalette_get_color_at(void *palette, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    if (!value || index < 0 || index > INT_MAX)
        return 0;
    uint32_t color = 0;
    return vg_colorpalette_get_color_at(value, (int)index, &color) ? rt_color_from_argb(color) : 0;
}

/// @brief Select or clear one palette index.
/// @param palette Live ColorPalette handle; invalid handles are ignored.
/// @param index Zero-based entry index, or -1 to clear.
void rt_colorpalette_set_selected_index(void *palette, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    if (!value)
        return;
    if (index < INT_MIN || index > INT_MAX)
        index = -1;
    vg_colorpalette_set_selected(value, (int)index);
}

/// @brief Return one palette's selected index.
/// @param palette Live ColorPalette handle.
/// @return Zero-based index, or -1 when absent/invalid.
int64_t rt_colorpalette_get_selected_index(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    return value ? (int64_t)vg_colorpalette_get_selected(value) : -1;
}

/// @brief Consume one palette structural-or-selection change edge.
/// @param palette Live ColorPalette handle.
/// @return 1 once after unreported changes, otherwise zero.
int64_t rt_colorpalette_was_changed(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    return value && vg_widget_was_changed(&value->base) ? 1 : 0;
}

/// @brief Return one palette's non-consuming state revision.
/// @param palette Live ColorPalette handle.
/// @return Monotonic revision, or zero for invalid handles.
int64_t rt_colorpalette_get_revision(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpalette_t *value = rt_colorpalette_checked(palette);
    return value ? rt_widget_get_revision(&value->base) : 0;
}

/// @brief Create a complete RGB picker with separately enabled alpha editing.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @return Live ColorPicker handle, or NULL for invalid parent/allocation failure.
void *rt_colorpicker_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_color_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_colorpicker_t *picker = vg_colorpicker_create(parent_widget);
    if (picker)
        rt_gui_apply_default_font(&picker->base);
    return picker;
}

/// @brief Set picker RGB while preserving its separate alpha component.
/// @param picker Live ColorPicker handle; invalid handles are ignored.
/// @param color Public RGB value in the low 24 bits.
void rt_colorpicker_set_color(void *picker, int64_t color) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    if (!value)
        return;
    uint32_t rgb = (uint32_t)color & 0x00FFFFFFu;
    vg_colorpicker_set_rgb(value,
                           (uint8_t)((rgb >> 16) & 0xFFu),
                           (uint8_t)((rgb >> 8) & 0xFFu),
                           (uint8_t)(rgb & 0xFFu));
}

/// @brief Return picker RGB under the public color contract.
/// @param picker Live ColorPicker handle.
/// @return `0x00RRGGBB`, or zero for invalid handles.
int64_t rt_colorpicker_get_color(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? rt_color_from_argb(vg_colorpicker_get_color(value)) : 0;
}

/// @brief Enable or disable picker alpha-channel editing.
/// @param picker Live ColorPicker handle; invalid handles are ignored.
/// @param enabled Non-zero to enable, zero to disable.
void rt_colorpicker_set_alpha_enabled(void *picker, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    if (value)
        vg_colorpicker_show_alpha(value, enabled != 0);
}

/// @brief Query whether picker alpha-channel editing is enabled.
/// @param picker Live ColorPicker handle.
/// @return 1 when enabled, otherwise zero.
int64_t rt_colorpicker_is_alpha_enabled(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value && vg_colorpicker_is_alpha_enabled(value) ? 1 : 0;
}

/// @brief Return the picker red component.
/// @param picker Live ColorPicker handle.
/// @return Value in `[0,255]`, or zero for invalid handles.
int64_t rt_colorpicker_get_red(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? (int64_t)value->r : 0;
}

/// @brief Return the picker green component.
/// @param picker Live ColorPicker handle.
/// @return Value in `[0,255]`, or zero for invalid handles.
int64_t rt_colorpicker_get_green(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? (int64_t)value->g : 0;
}

/// @brief Return the picker blue component.
/// @param picker Live ColorPicker handle.
/// @return Value in `[0,255]`, or zero for invalid handles.
int64_t rt_colorpicker_get_blue(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? (int64_t)value->b : 0;
}

/// @brief Return the picker alpha component.
/// @param picker Live ColorPicker handle.
/// @return Value in `[0,255]`, or zero for invalid handles.
int64_t rt_colorpicker_get_alpha(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? (int64_t)vg_colorpicker_get_alpha(value) : 0;
}

/// @brief Consume one picker component-change edge.
/// @param picker Live ColorPicker handle.
/// @return 1 once after unreported RGB/alpha changes, otherwise zero.
int64_t rt_colorpicker_was_changed(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value && vg_widget_was_changed(&value->base) ? 1 : 0;
}

/// @brief Return one picker's non-consuming state revision.
/// @param picker Live ColorPicker handle.
/// @return Monotonic revision, or zero for invalid handles.
int64_t rt_colorpicker_get_revision(void *picker) {
    RT_ASSERT_MAIN_THREAD();
    vg_colorpicker_t *value = rt_colorpicker_checked(picker);
    return value ? rt_widget_get_revision(&value->base) : 0;
}

#else

/// @brief Graphics-disabled ColorSwatch constructor twin.
/// @param parent Ignored parent handle.
/// @param color Ignored RGB value.
/// @return Always NULL because GUI controls are unavailable.
void *rt_colorswatch_new(void *parent, int64_t color) {
    (void)parent;
    (void)color;
    return NULL;
}

/// @brief Graphics-disabled ColorSwatch setter twin.
/// @param swatch Ignored handle.
/// @param color Ignored RGB value.
void rt_colorswatch_set_color(void *swatch, int64_t color) {
    (void)swatch;
    (void)color;
}

/// @brief Graphics-disabled ColorSwatch getter twin.
/// @param swatch Ignored handle.
/// @return Always zero.
int64_t rt_colorswatch_get_color(void *swatch) {
    (void)swatch;
    return 0;
}

/// @brief Graphics-disabled ColorSwatch selection setter twin.
/// @param swatch Ignored handle.
/// @param selected Ignored selection state.
void rt_colorswatch_set_selected(void *swatch, int64_t selected) {
    (void)swatch;
    (void)selected;
}

/// @brief Graphics-disabled ColorSwatch selection getter twin.
/// @param swatch Ignored handle.
/// @return Always zero.
int64_t rt_colorswatch_is_selected(void *swatch) {
    (void)swatch;
    return 0;
}

/// @brief Graphics-disabled ColorSwatch edge twin.
/// @param swatch Ignored handle.
/// @return Always zero.
int64_t rt_colorswatch_was_changed(void *swatch) {
    (void)swatch;
    return 0;
}

/// @brief Graphics-disabled ColorSwatch revision twin.
/// @param swatch Ignored handle.
/// @return Always zero.
int64_t rt_colorswatch_get_revision(void *swatch) {
    (void)swatch;
    return 0;
}

/// @brief Graphics-disabled ColorPalette constructor twin.
/// @param parent Ignored parent handle.
/// @return Always NULL because GUI controls are unavailable.
void *rt_colorpalette_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Graphics-disabled ColorPalette append twin.
/// @param palette Ignored handle.
/// @param color Ignored RGB value.
void rt_colorpalette_add_color(void *palette, int64_t color) {
    (void)palette;
    (void)color;
}

/// @brief Graphics-disabled ColorPalette removal twin.
/// @param palette Ignored handle.
/// @param index Ignored index.
/// @return Always zero.
int64_t rt_colorpalette_remove_color(void *palette, int64_t index) {
    (void)palette;
    (void)index;
    return 0;
}

/// @brief Graphics-disabled ColorPalette clear twin.
/// @param palette Ignored handle.
void rt_colorpalette_clear(void *palette) {
    (void)palette;
}

/// @brief Graphics-disabled ColorPalette count twin.
/// @param palette Ignored handle.
/// @return Always zero.
int64_t rt_colorpalette_get_color_count(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Graphics-disabled ColorPalette indexed getter twin.
/// @param palette Ignored handle.
/// @param index Ignored index.
/// @return Always zero.
int64_t rt_colorpalette_get_color_at(void *palette, int64_t index) {
    (void)palette;
    (void)index;
    return 0;
}

/// @brief Graphics-disabled ColorPalette selection setter twin.
/// @param palette Ignored handle.
/// @param index Ignored index.
void rt_colorpalette_set_selected_index(void *palette, int64_t index) {
    (void)palette;
    (void)index;
}

/// @brief Graphics-disabled ColorPalette selection getter twin.
/// @param palette Ignored handle.
/// @return Always -1 because no selection exists.
int64_t rt_colorpalette_get_selected_index(void *palette) {
    (void)palette;
    return -1;
}

/// @brief Graphics-disabled ColorPalette edge twin.
/// @param palette Ignored handle.
/// @return Always zero.
int64_t rt_colorpalette_was_changed(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Graphics-disabled ColorPalette revision twin.
/// @param palette Ignored handle.
/// @return Always zero.
int64_t rt_colorpalette_get_revision(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Graphics-disabled ColorPicker constructor twin.
/// @param parent Ignored parent handle.
/// @return Always NULL because GUI controls are unavailable.
void *rt_colorpicker_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Graphics-disabled ColorPicker RGB setter twin.
/// @param picker Ignored handle.
/// @param color Ignored RGB value.
void rt_colorpicker_set_color(void *picker, int64_t color) {
    (void)picker;
    (void)color;
}

/// @brief Graphics-disabled ColorPicker RGB getter twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_color(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker alpha-enabled setter twin.
/// @param picker Ignored handle.
/// @param enabled Ignored state.
void rt_colorpicker_set_alpha_enabled(void *picker, int64_t enabled) {
    (void)picker;
    (void)enabled;
}

/// @brief Graphics-disabled ColorPicker alpha-enabled getter twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_is_alpha_enabled(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker red-component twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_red(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker green-component twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_green(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker blue-component twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_blue(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker alpha-component twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_alpha(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker edge twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_was_changed(void *picker) {
    (void)picker;
    return 0;
}

/// @brief Graphics-disabled ColorPicker revision twin.
/// @param picker Ignored handle.
/// @return Always zero.
int64_t rt_colorpicker_get_revision(void *picker) {
    (void)picker;
    return 0;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
