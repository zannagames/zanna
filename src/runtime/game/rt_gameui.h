//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_gameui.h
/// @file
/// @brief Declares lightweight, immediate-mode game UI widgets.
//
// Purpose: In-game widgets that draw directly to a Canvas or registered
//   Canvas3D. Includes labels, bars, panels, nine-slices, menus, buttons, text
//   inputs, tables, modals, sliders, dropdowns, and tooltips for HUDs and
//   overlays separate from the desktop ZannaGUI widget system.
//
// Key invariants:
//   - All widgets use runtime-object reference counting.
//   - Draw methods are no-ops when canvas or widget is NULL.
//   - Widgets use immediate-mode rendering (no retained widget tree).
//   - UIBar values are clamped to [0, max].
//   - UIMenuList selection wraps around on MoveUp/MoveDown.
//   - Fixed-size widget text buffers truncate on UTF-8 codepoint boundaries.
//
// Ownership/Lifetime:
//   - Widget objects expose no module-specific destroy entry points.
//   - UILabel/GameButton/MenuList copy text into widget-owned buffers.
//   - UILabel/MenuList retain assigned BitmapFont handles.
//   - UINineSlice retains a reference to its Pixels source (not copied).
//
// Links: src/runtime/game/rt_gameui.c (implementation),
//        src/runtime/graphics/rt_graphics.h (Canvas drawing API),
//        src/runtime/graphics/rt_bitmapfont.h (custom font support)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID for UILabel objects.
#define RT_UILABEL_CLASS_ID INT64_C(-0x510101)
/// @brief Runtime class ID for UIBar objects.
#define RT_UIBAR_CLASS_ID INT64_C(-0x510102)
/// @brief Runtime class ID for UIPanel objects.
#define RT_UIPANEL_CLASS_ID INT64_C(-0x510103)
/// @brief Runtime class ID for UINineSlice objects.
#define RT_UININESLICE_CLASS_ID INT64_C(-0x510104)
/// @brief Runtime class ID for UIMenuList objects.
#define RT_UIMENULIST_CLASS_ID INT64_C(-0x510105)
/// @brief Runtime class ID for GameButton objects.
#define RT_GAMEBUTTON_CLASS_ID INT64_C(-0x510106)
/// @brief Runtime class ID for UITextInput objects.
#define RT_UITEXTINPUT_CLASS_ID INT64_C(-0x510107)
/// @brief Runtime class ID for UITable objects.
#define RT_UITABLE_CLASS_ID INT64_C(-0x510108)
/// @brief Runtime class ID for immutable UITable click-result snapshots.
#define RT_UITABLE_CLICK_RESULT_CLASS_ID INT64_C(-0x51010D)
/// @brief Runtime class ID for UIModal objects.
#define RT_UIMODAL_CLASS_ID INT64_C(-0x510109)
/// @brief Runtime class ID for UISlider objects.
#define RT_UISLIDER_CLASS_ID INT64_C(-0x51010A)
/// @brief Runtime class ID for UIDropdown objects.
#define RT_UIDROPDOWN_CLASS_ID INT64_C(-0x51010B)
/// @brief Runtime class ID for UITooltip objects.
#define RT_UITOOLTIP_CLASS_ID INT64_C(-0x51010C)

/// Click result kind for a table click that did not hit a row or header.
#define RT_UITABLE_CLICK_NONE INT64_C(0)
/// Click result kind for a table click that selected a body row.
#define RT_UITABLE_CLICK_ROW INT64_C(1)
/// Click result kind for a table click that hit a column header.
#define RT_UITABLE_CLICK_HEADER INT64_C(2)

//=========================================================================
// UILabel — Positioned text with optional BitmapFont
//=========================================================================

/// @brief Create a positioned text label with the given color (canvas color format).
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param text Runtime string copied into a 512-byte inline buffer.
/// @param color Packed text color.
/// @return A new visible UILabel reference, or `NULL` if allocation fails.
/// @details The input is not retained; scale begins at one with the default
///          canvas font.
void *rt_uilabel_new(int64_t x, int64_t y, rt_string text, int64_t color);

/// @brief Replace the label's text content.
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param text Runtime string to copy, or `NULL` to clear.
/// @details Copies at most 511 visible bytes without splitting validated UTF-8
///          and retains no input reference. A non-null invalid label traps.
void rt_uilabel_set_text(void *label, rt_string text);

/// @brief Move the label to a new (x, y) position.
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_pos(void *label, int64_t x, int64_t y);

/// @brief Change the label's text color.
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_color(void *label, int64_t color);

/// @brief Bind a custom BitmapFont (NULL falls back to the built-in 8×8 font).
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param font Optional BitmapFont reference.
/// @details Retains a valid replacement and releases the prior font. Invalid
///          label or font classes raise runtime traps.
void rt_uilabel_set_font(void *label, void *font);

/// @brief Set integer scale (1 = native, 2 = double-size, etc.).
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param scale Requested multiplier clamped to `[1, 16]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_scale(void *label, int64_t scale);

/// @brief Toggle the label's visibility (Draw becomes a no-op when hidden).
/// @param label UILabel to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_visible(void *label, int8_t visible);

/// @brief Render the label onto @p canvas.
/// @param label UILabel to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Null arguments, hidden labels, and empty text are no-ops. Invalid
///          non-null widget or canvas handles raise runtime traps.
void rt_uilabel_draw(void *label, void *canvas);

/// @brief Get the label's x-coordinate.
/// @param label UILabel to query.
/// @return Stored X coordinate, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uilabel_get_x(void *label);

/// @brief Get the label's y-coordinate.
/// @param label UILabel to query.
/// @return Stored Y coordinate, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uilabel_get_y(void *label);

//=========================================================================
// UIBar — Progress/health/XP bar
//=========================================================================

/// @brief Create a progress bar widget at (x, y) of given size with foreground/background colors.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param fg_color Filled-region color.
/// @param bg_color Empty-region color.
/// @return A new visible UIBar reference, or `NULL` if allocation fails.
/// @details Value begins at zero of 100, fill is left-to-right, and border is
///          disabled.
void *rt_uibar_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t fg_color, int64_t bg_color);

/// @brief Update the bar's current value (clamped to [0, max_value]).
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param value Requested current value.
/// @param max_value Denominator forced to at least one before value clamping.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_value(void *bar, int64_t value, int64_t max_value);

/// @brief Move the bar to a new (x, y) position.
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_pos(void *bar, int64_t x, int64_t y);

/// @brief Resize the bar.
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_size(void *bar, int64_t w, int64_t h);

/// @brief Change the foreground (filled) and background (empty) colors.
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param fg Packed fill color.
/// @param bg Packed empty-region color.
/// @details Values are stored verbatim. A non-null invalid handle traps.
void rt_uibar_set_colors(void *bar, int64_t fg, int64_t bg);

/// @brief Set the border color (use 0 to disable the border).
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param color Packed border value, or zero.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_border(void *bar, int64_t color);

/// @brief Set the fill direction (0 = left-to-right, 1 = right-to-left, 2 = bottom-up, 3 =
/// top-down).
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param dir Direction value; values outside `[0, 3]` select left-to-right.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_direction(void *bar, int64_t dir);

/// @brief Toggle visibility.
/// @param bar UIBar to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_visible(void *bar, int8_t visible);

/// @brief Render the bar onto @p canvas.
/// @param bar UIBar to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Any positive fraction fills at least one pixel. Null arguments and
///          hidden bars are no-ops; invalid non-null handles trap.
void rt_uibar_draw(void *bar, void *canvas);

/// @brief Get the current value.
/// @param bar UIBar to query.
/// @return Stored current value, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uibar_get_value(void *bar);

/// @brief Get the maximum value.
/// @param bar UIBar to query.
/// @return Positive stored maximum, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uibar_get_max(void *bar);

//=========================================================================
// UIPanel — Semi-transparent background panel
//=========================================================================

/// @brief Create a semi-transparent rectangular panel with optional border and corner radius.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param bg_color Packed fill color.
/// @param alpha Opacity clamped to `[0, 255]`.
/// @return A new visible UIPanel reference, or `NULL` if allocation fails.
/// @details Border begins disabled and corner radius begins zero.
void *rt_uipanel_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t bg_color, int64_t alpha);

/// @brief Move the panel to (x, y).
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_pos(void *panel, int64_t x, int64_t y);

/// @brief Resize the panel.
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_size(void *panel, int64_t w, int64_t h);

/// @brief Change the panel's background color and per-pixel alpha (0–255).
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param bg_color Packed fill color.
/// @param alpha Opacity clamped to `[0, 255]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_color(void *panel, int64_t bg_color, int64_t alpha);

/// @brief Set border color and thickness in pixels (color 0 disables the border).
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param color Packed border color, with zero disabling.
/// @param thickness Positive thickness clamped to 16384 and further limited at
///        draw time to half the smaller panel extent.
/// @details Nonpositive thickness disables the border. A non-null invalid
///          handle raises a runtime trap.
void rt_uipanel_set_border(void *panel, int64_t color, int64_t thickness);

/// @brief Set rounded-corner radius in pixels (0 = sharp corners).
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param radius Negative values become zero; drawing clamps oversized radii.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_corner_radius(void *panel, int64_t radius);

/// @brief Toggle visibility.
/// @param panel UIPanel to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_visible(void *panel, int8_t visible);

/// @brief Render the panel onto @p canvas.
/// @param panel UIPanel to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Zero alpha suppresses only the fill; an enabled border still draws.
///          Null arguments and hidden panels are no-ops. Invalid handles trap.
void rt_uipanel_draw(void *panel, void *canvas);

//=========================================================================
// UINineSlice — Scalable bordered UI element
//=========================================================================

/// @brief Create a 9-slice scalable bordered widget from a Pixels source with corner insets.
/// @param pixels Required Pixels source retained by the widget.
/// @param left Left source inset.
/// @param top Top source inset.
/// @param right Right source inset.
/// @param bottom Bottom source inset.
/// @return A new UINineSlice reference, or `NULL` for null Pixels or allocation
///         failure.
/// @details Negative insets become zero. Opposing insets that exceed a source
///          dimension are replaced with a half-and-remainder split. Wrong
///          source classes trap.
void *rt_uinineslice_new(void *pixels, int64_t left, int64_t top, int64_t right, int64_t bottom);

/// @brief Render the 9-slice at (x, y) tiled to fill (w × h).
/// @param ns UINineSlice to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @param x Destination left coordinate.
/// @param y Destination top coordinate.
/// @param w Positive destination width, clamped to 16384.
/// @param h Positive destination height, clamped to 16384.
/// @details Corners stay natural-sized unless clipped by a small destination;
///          edges and center tile. Null/nonpositive inputs are no-ops. Invalid
///          non-null handles trap.
void rt_uinineslice_draw(void *ns, void *canvas, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Apply a per-channel multiplicative tint to subsequent draws.
/// @param ns UINineSlice to mutate; `NULL` is a no-op.
/// @param color Tint color, or zero to use the original source.
/// @details Changing tint releases the cached tinted Pixels. The cache rebuilds
///          when the source generation or tint changes. Invalid handles trap.
void rt_uinineslice_set_tint(void *ns, int64_t color);

//=========================================================================
// UIMenuList — Vertical menu with selection highlight
//=========================================================================

/// @brief Fixed item capacity of each UIMenuList.
#define RT_UIMENULIST_MAX_ITEMS 64

/// @brief Create a vertical menu list at (x, y) with the given per-row pixel height.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param item_height Row height; nonpositive values select 16 and positive
///        values clamp to `[1, 16384]`.
/// @return A new empty, visible UIMenuList reference, or `NULL` on allocation
///         failure.
void *rt_uimenulist_new(int64_t x, int64_t y, int64_t item_height);

/// @brief Append a menu item (string is copied; max RT_UIMENULIST_MAX_ITEMS).
/// @param menu UIMenuList to mutate.
/// @param text Required runtime string copied to a 128-byte inline buffer.
/// @details Null arguments are ignored. The string is not retained and UTF-8
///          truncation preserves validated boundaries. Capacity overflow and
///          invalid non-null menu handles raise runtime traps.
void rt_uimenulist_add_item(void *menu, rt_string text);

/// @brief Remove all items and reset selection to zero.
/// @param menu UIMenuList to clear; `NULL` is a no-op.
/// @details Item strings are inline buffers, so no per-item references are
///          released. Styling and font remain unchanged. Invalid handles trap.
void rt_uimenulist_clear(void *menu);

/// @brief Set which item is highlighted (clamped to valid range).
/// @param menu UIMenuList to mutate; `NULL` is a no-op.
/// @param index Requested index; an empty menu stores zero.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_set_selected(void *menu, int64_t index);

/// @brief Get the index of the currently selected item.
/// @param menu UIMenuList to query.
/// @return Stored index, or zero for an empty/null/invalid menu.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uimenulist_get_selected(void *menu);

/// @brief Move selection up by one item, wrapping to the last item past the top.
/// @param menu UIMenuList to mutate; null and empty menus are no-ops.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_move_up(void *menu);

/// @brief Move selection down by one item, wrapping to the first item past the bottom.
/// @param menu UIMenuList to mutate; null and empty menus are no-ops.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_move_down(void *menu);

/// @brief Set normal text color, selected-item text color, and highlight background color.
/// @param menu UIMenuList to mutate; `NULL` is a no-op.
/// @param text_color Packed normal label color.
/// @param selected_color Packed highlighted label color.
/// @param highlight_bg Packed selected-row fill color.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_set_colors(void *menu,
                              int64_t text_color,
                              int64_t selected_color,
                              int64_t highlight_bg);
/// @brief Bind a custom BitmapFont (NULL falls back to built-in 8×8).
/// @param menu UIMenuList to mutate; `NULL` is a no-op.
/// @param font Optional BitmapFont reference.
/// @details Retains a valid replacement and releases the prior font. Wrong
///          widget or font classes raise runtime traps.
void rt_uimenulist_set_font(void *menu, void *font);

/// @brief Toggle visibility.
/// @param menu UIMenuList to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_set_visible(void *menu, int8_t visible);

/// @brief Get the number of items currently in the menu.
/// @param menu UIMenuList to query.
/// @return Stored count, or zero for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uimenulist_get_count(void *menu);

/// @brief Render the menu onto @p canvas.
/// @param menu UIMenuList to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Null arguments, hidden menus, and empty menus are no-ops. Invalid
///          non-null handles raise runtime traps.
void rt_uimenulist_draw(void *menu, void *canvas);

/// @brief Process input flags and return the selected index when @p confirm is set, else -1.
/// @param menu UIMenuList to update.
/// @param up Nonzero requests one wrapped upward move.
/// @param down Nonzero requests one wrapped downward move.
/// @param confirm Nonzero returns the resulting selection.
/// @return Selected index on confirmation of a visible, nonempty menu;
///         otherwise `-1`.
/// @details Simultaneous up and down cancel navigation. Invalid handles trap.
int64_t rt_uimenulist_handle_input(void *menu, int8_t up, int8_t down, int8_t confirm);

//=========================================================================
// GameButton
//=========================================================================

/// @brief Create a clickable in-game button at (x, y) of size (w × h) with label text.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param text Optional runtime string copied into a 64-byte inline buffer.
/// @return A new visible GameButton reference, or `NULL` on allocation failure.
/// @details The input is not retained; default styling includes a one-pixel
///          border and scale-one default-font text.
void *rt_gamebutton_new(int64_t x, int64_t y, int64_t w, int64_t h, void *text);

/// @brief Replace the button's label.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param text Optional runtime string copied without retaining.
/// @details At most 63 visible bytes are stored on validated UTF-8 boundaries.
///          A non-null invalid button raises a runtime trap.
void rt_gamebutton_set_text(void *btn, void *text);

/// @brief Set background colors for normal vs selected (focused) state.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param normal Packed normal fill color.
/// @param selected Packed selected fill color.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_colors(void *btn, int64_t normal, int64_t selected);

/// @brief Set text colors for normal vs selected state.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param normal Packed normal text color.
/// @param selected Packed selected text color.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_text_colors(void *btn, int64_t normal, int64_t selected);

/// @brief Set border thickness in pixels and border color (width 0 disables border).
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param width Nonpositive values disable; drawing caps positive values to
///        half the smaller button extent.
/// @param color Packed border color; zero suppresses drawing.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_border(void *btn, int64_t width, int64_t color);

/// @brief Render the button; @p is_selected picks the selected colorway when nonzero.
/// @param btn GameButton to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @param is_selected Nonzero selects highlighted colors.
/// @details Text clips by scale-adjusted eight-pixel cells and is centered.
///          Null/hidden inputs are no-ops; invalid non-null handles trap.
void rt_gamebutton_draw(void *btn, void *canvas, int8_t is_selected);

/// @brief Get the button's x-coordinate.
/// @param btn GameButton to query.
/// @return Stored X coordinate, or zero for a null/invalid handle.
int64_t rt_gamebutton_get_x(void *btn);

/// @brief Get the button's y-coordinate.
/// @param btn GameButton to query.
/// @return Stored Y coordinate, or zero for a null/invalid handle.
int64_t rt_gamebutton_get_y(void *btn);

/// @brief Get the button's width.
/// @param btn GameButton to query.
/// @return Positive stored width, or zero for a null/invalid handle.
int64_t rt_gamebutton_get_width(void *btn);

/// @brief Get the button's height.
/// @param btn GameButton to query.
/// @return Positive stored height, or zero for a null/invalid handle.
int64_t rt_gamebutton_get_height(void *btn);

/// @brief Set the button's x-coordinate.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param x New coordinate stored verbatim.
void rt_gamebutton_set_x(void *btn, int64_t x);

/// @brief Set the button's y-coordinate.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param y New coordinate stored verbatim.
void rt_gamebutton_set_y(void *btn, int64_t y);

/// @brief Set the button's width and height.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
void rt_gamebutton_set_size(void *btn, int64_t w, int64_t h);

/// @brief Toggle the button's visibility.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param visible Zero hides; any nonzero value shows.
void rt_gamebutton_set_visible(void *btn, int8_t visible);

/// @brief Return whether the button is visible.
/// @param btn GameButton to query.
/// @return Normalized visible flag, or zero for a null/invalid handle.
int8_t rt_gamebutton_get_visible(void *btn);

/// @brief Set the button text scale.
/// @param btn GameButton to mutate; `NULL` is a no-op.
/// @param scale Requested multiplier clamped to `[1, 16]`.
void rt_gamebutton_set_text_scale(void *btn, int64_t scale);

/// @brief Get the button text scale.
/// @param btn GameButton to query.
/// @return Stored scale, or one for a null/invalid handle.
/// @details Every non-null invalid GameButton accessor/mutator above raises a
///          runtime trap.
int64_t rt_gamebutton_get_text_scale(void *btn);

//=========================================================================
// TextInput
//=========================================================================

/// @brief Create a single-line text-input widget at (x, y) sized w x h.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new visible, enabled UITextInput reference, or `NULL` if object or
///         initial-buffer allocation fails.
/// @details The field begins empty, unfocused, unselected, and unlimited in
///          codepoint count. Its owned text and placeholder buffers begin at
///          512 bytes each.
void *rt_uitextinput_new(int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Replace the field's contents with @p text.
/// @param ti UITextInput to mutate; `NULL` is a no-op.
/// @param text Runtime string to copy, or `NULL` for empty text.
/// @details Copying stops at embedded NUL and respects the configured maximum
///          codepoints. The caret moves to the end, selection and horizontal
///          scroll reset, and the input is not retained. Allocation failure
///          traps; a non-null invalid field handle also traps.
void rt_uitextinput_set_text(void *ti, rt_string text);

/// @brief Get the current field contents.
/// @param ti UITextInput to query.
/// @return A caller-owned runtime-string copy, or the immortal empty singleton
///         for a null/invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
rt_string rt_uitextinput_get_text(void *ti);

/// @brief Number of codepoints currently in the field.
/// @param ti UITextInput to query.
/// @return Count of validated UTF-8 codepoints plus malformed single-byte
///         units, or zero for a null/invalid handle.
int64_t rt_uitextinput_text_length(void *ti);

/// @brief Get the caret position (codepoint index).
/// @param ti UITextInput to query.
/// @return Codepoint boundary count before the caret, or zero for a
///         null/invalid handle.
int64_t rt_uitextinput_get_cursor(void *ti);

/// @brief Move the caret to codepoint index @p pos (clamped).
/// @param ti UITextInput to mutate; `NULL` is a no-op.
/// @param pos Requested codepoint index; nonpositive values select the start
///        and oversized values select the end.
/// @details Clears selection and restarts caret blinking. Invalid handles trap.
void rt_uitextinput_set_cursor(void *ti, int64_t pos);

/// @brief Select the entire field contents.
/// @param ti UITextInput to mutate; `NULL` is a no-op.
/// @details Anchors at byte zero and moves the caret to the byte end. An empty
///          field still has no nonempty selection. Invalid handles trap.
void rt_uitextinput_select_all(void *ti);

/// @brief Clear any active text selection.
/// @param ti UITextInput to mutate; `NULL` is a no-op.
/// @details Leaves the caret and text unchanged. Invalid handles trap.
void rt_uitextinput_clear_selection(void *ti);

/// @brief Whether a non-empty selection exists.
/// @param ti UITextInput to query.
/// @return `1` for distinct in-range anchor and caret positions; otherwise
///         `0`.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_uitextinput_has_selection(void *ti);

/// @brief Get the currently selected substring (empty if none).
/// @param ti UITextInput to query.
/// @return A caller-owned copy of the selected byte range, the immortal empty
///         singleton when no selection exists, or `NULL` on allocation failure.
/// @details A non-null invalid handle raises a runtime trap.
rt_string rt_uitextinput_get_selected_text(void *ti);

/// @brief Delete the current selection, if any.
/// @param ti UITextInput to mutate; `NULL` is a no-op.
/// @details Compacts the owned buffer, moves the caret to the former range
///          start, clears selection, and adjusts horizontal scroll. Invalid
///          handles trap.
void rt_uitextinput_delete_selection(void *ti);

/// @brief Process a key press (arrows/editing/shortcuts); @p shift_held
///        extends the selection.
/// @param ti UITextInput to update.
/// @param key_code Shared GameUI key code; value 1 selects all.
/// @param shift_held Nonzero extends navigation from the existing/new anchor.
/// @return `1` only when Backspace or Delete changes text; navigation,
///         select-all, ignored keys, disabled, and unfocused states return zero.
/// @details Unshifted left/right collapse a selection to its near/far edge.
///          Invalid non-null handles raise a runtime trap.
int64_t rt_uitextinput_handle_key(void *ti, int64_t key_code, int8_t shift_held);

/// @brief Insert typed text at the caret (respects max-codepoints).
/// @param ti UITextInput to update.
/// @param typed_text Runtime string whose bytes are inserted.
/// @return Nonzero when text changed, including deletion of an active selection
///         even if the codepoint limit accepts no replacement bytes.
/// @details Disabled, unfocused, null, or empty inputs return zero. The input is
///          not retained. Buffer-allocation failure traps.
int64_t rt_uitextinput_handle_text(void *ti, rt_string typed_text);

/// @brief Place the caret/selection from a mouse click at (mx, my).
/// @param ti UITextInput to update.
/// @param mx Mouse X coordinate.
/// @param my Mouse Y coordinate.
/// @param shift_held Nonzero extends selection to the measured byte position.
/// @details A visible, enabled field gains focus only when the click is inside;
///          clicking outside clears focus. Invalid handles trap.
void rt_uitextinput_handle_mouse_click(void *ti, int64_t mx, int64_t my, int8_t shift_held);

/// @brief Extend the selection by dragging to (mx, my).
/// @param ti UITextInput to update.
/// @param mx Mouse X coordinate mapped to the nearest text boundary.
/// @param my Mouse Y coordinate, currently ignored.
/// @details Only enabled, focused fields react. A missing anchor starts at the
///          prior caret. Invalid handles trap.
void rt_uitextinput_handle_mouse_drag(void *ti, int64_t mx, int64_t my);

/// @brief Advance time-based state (caret blink) by @p delta_ms.
/// @param ti UITextInput to update.
/// @param delta_ms Positive elapsed milliseconds; nonpositive values are
///        ignored.
/// @details Advances the internal blink phase with overflow recovery. Invalid
///          non-null handles raise a runtime trap.
void rt_uitextinput_update(void *ti, int64_t delta_ms);

/// @brief Render the field onto @p canvas.
/// @param ti UITextInput to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Hidden/null inputs are no-ops. Draws background, focus-dependent
///          border, selection, placeholder or password-masked text, and a
///          blinking caret while focused and enabled. Invalid handles trap.
void rt_uitextinput_draw(void *ti, void *canvas);

/// @brief Set the text color (packed RGB).
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_text_color(void *ti, int64_t color);

/// @brief Get the text color.
/// @param ti UITextInput to query.
/// @return Stored color, or zero for a null/invalid handle.
int64_t rt_uitextinput_get_text_color(void *ti);

/// @brief Set the background color.
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_bg_color(void *ti, int64_t color);

/// @brief Get the background color.
/// @param ti UITextInput to query.
/// @return Stored color, or zero for a null/invalid handle.
int64_t rt_uitextinput_get_bg_color(void *ti);

/// @brief Set the caret color.
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_cursor_color(void *ti, int64_t color);

/// @brief Set the selection-highlight color.
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim; drawing uses alpha 160.
void rt_uitextinput_set_selection_color(void *ti, int64_t color);

/// @brief Set the border color (unfocused).
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_border_color(void *ti, int64_t color);

/// @brief Set the border color used while the field is focused.
/// @param ti UITextInput to mutate.
/// @param color Packed value stored verbatim.
void rt_uitextinput_set_border_color_focused(void *ti, int64_t color);

/// @brief Set the font used to render the text.
/// @param ti UITextInput to mutate.
/// @param font Optional BitmapFont reference; `NULL` selects default metrics.
/// @details Retains a valid replacement and releases the prior font. Wrong font
///          or field classes trap.
void rt_uitextinput_set_font(void *ti, void *font);

/// @brief Show/hide the widget.
/// @param ti UITextInput to mutate.
/// @param visible Zero hides; nonzero shows.
void rt_uitextinput_set_visible(void *ti, int8_t visible);

/// @brief Query visibility.
/// @param ti UITextInput to query.
/// @return Normalized visible flag, or zero for a null/invalid handle.
int8_t rt_uitextinput_get_visible(void *ti);

/// @brief Enable/disable input handling.
/// @param ti UITextInput to mutate.
/// @param enabled Zero disables; nonzero enables.
void rt_uitextinput_set_enabled(void *ti, int8_t enabled);

/// @brief Query enabled state.
/// @param ti UITextInput to query.
/// @return Normalized enabled flag, or zero for a null/invalid handle.
int8_t rt_uitextinput_get_enabled(void *ti);

/// @brief Set keyboard focus on/off.
/// @param ti UITextInput to mutate.
/// @param focused Zero clears focus; nonzero sets it.
/// @details Restarts blink timing and clears selection when focus is removed.
void rt_uitextinput_set_focused(void *ti, int8_t focused);

/// @brief Query focus state.
/// @param ti UITextInput to query.
/// @return Normalized focus flag, or zero for a null/invalid handle.
int8_t rt_uitextinput_get_focused(void *ti);

/// @brief Toggle password masking of the displayed text.
/// @param ti UITextInput to mutate.
/// @param password Zero shows text; nonzero draws one `*` per codepoint.
void rt_uitextinput_set_password_mode(void *ti, int8_t password);

/// @brief Set the placeholder shown when the field is empty.
/// @param ti UITextInput to mutate.
/// @param placeholder Runtime string copied into owned dynamic storage, or
///        `NULL` to clear.
/// @details The placeholder is shown only while empty and unfocused. Embedded
///          NUL ends the copy. Allocation or invalid-handle errors trap.
void rt_uitextinput_set_placeholder(void *ti, rt_string placeholder);

/// @brief Cap the maximum number of codepoints accepted.
/// @param ti UITextInput to mutate.
/// @param max_cps Positive limit, or nonpositive for unlimited input.
/// @details Lowering the limit immediately truncates existing text and resets
///          caret/selection through the ordinary set-text path. All simple
///          UITextInput setters/getters above trap on a non-null wrong class.
void rt_uitextinput_set_max_codepoints(void *ti, int64_t max_cps);

//=========================================================================
// Table
//=========================================================================

/// @brief Create a scrollable, sortable data table at (x, y) sized w x h.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new visible UITable reference, or `NULL` if object or initial
///         column/row-capacity allocation fails.
/// @details Begins with no columns or rows, a visible 22-pixel header,
///          20-pixel rows, striped backgrounds, borders, and no selection/sort.
void *rt_uitable_new(int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Add a column with @p title, pixel @p width and @p align; returns its index.
/// @param table UITable to mutate.
/// @param title Runtime string copied into a 64-byte inline title.
/// @param width Positive column width, or 80 when nonpositive.
/// @param align Alignment code `0..2`; out-of-range values become zero.
/// @return New zero-based column index, or `-1` on invalid table/allocation
///         failure.
/// @details The new column begins nonsortable and lexicographic. Allocation and
///          wrong-class errors trap; title is not retained.
int64_t rt_uitable_add_column(void *table, rt_string title, int64_t width, int64_t align);

/// @brief Mark column @p col sortable, and whether it sorts numerically.
/// @param table UITable to mutate.
/// @param col Existing zero-based column index.
/// @param sortable Zero disables sorting; nonzero enables.
/// @param numeric Zero selects bytewise text ordering; nonzero parses finite
///        numeric cell values.
/// @details Invalid indices are ignored. A non-null invalid table traps.
void rt_uitable_set_column_sortable(void *table, int64_t col, int8_t sortable, int8_t numeric);

/// @brief Number of columns.
/// @param table UITable to query.
/// @return Stored column count, or zero for a null/invalid handle.
int64_t rt_uitable_column_count(void *table);

/// @brief Append an empty row; returns its index.
/// @param table UITable to mutate.
/// @return New zero-based row index, or `-1` for invalid handle/allocation
///         failure.
/// @details Allocates zeroed fixed-size cell storage for current column
///          capacity. The first row becomes selected. Failures trap.
int64_t rt_uitable_add_row(void *table);

/// @brief Set the text of cell (row, col).
/// @param table UITable to mutate.
/// @param row Existing row index.
/// @param col Existing column index.
/// @param text Runtime string copied into a 64-byte cell buffer.
/// @details Invalid indices are ignored. The input is not retained and valid
///          UTF-8 is not split by truncation. Wrong-class handles trap.
void rt_uitable_set_cell(void *table, int64_t row, int64_t col, rt_string text);

/// @brief Get the text of cell (row, col).
/// @param table UITable to query.
/// @param row Existing row index.
/// @param col Existing column index.
/// @return A caller-owned copy of the cell, or the immortal empty singleton for
///         invalid indices/null handles.
/// @details A non-null wrong-class handle traps.
rt_string rt_uitable_get_cell(void *table, int64_t row, int64_t col);

/// @brief Remove the row at @p row.
/// @param table UITable to mutate.
/// @param row Existing index to remove.
/// @details Frees that row's cells, shifts later row records left, clears the
///          spare tail record, and reclamps scroll/selection. Invalid indices
///          are ignored; wrong-class handles trap.
void rt_uitable_remove_row(void *table, int64_t row);

/// @brief Remove all rows (columns are kept).
/// @param table UITable to mutate.
/// @details Frees every row's cell storage and resets row count, scroll to zero,
///          and selection to `-1`. Columns and styling remain. Invalid handles
///          trap.
void rt_uitable_clear_rows(void *table);

/// @brief Number of rows.
/// @param table UITable to query.
/// @return Stored row count, or zero for a null/invalid handle.
int64_t rt_uitable_row_count(void *table);

/// @brief Sort rows by column @p col, ascending or @p descending.
/// @param table UITable to mutate.
/// @param col Existing sort-column index.
/// @param descending Zero selects ascending; nonzero descending.
/// @details Forces the chosen column sortable and performs an in-place stable
///          insertion sort. Numeric columns order finite parsed values before
///          invalid values in ascending mode; two invalid values compare
///          lexicographically. Invalid indices are ignored.
void rt_uitable_sort_by(void *table, int64_t col, int8_t descending);

/// @brief Column the table is currently sorted by (-1 if none).
/// @param table UITable to query.
/// @return Active sort column, or `-1` for none/null/invalid.
int64_t rt_uitable_get_sort_column(void *table);

/// @brief Whether the current sort is descending.
/// @param table UITable to query.
/// @return Normalized descending flag, or zero for null/invalid.
int8_t rt_uitable_get_sort_descending(void *table);

/// @brief Scroll so that @p row is the first visible row.
/// @param table UITable to mutate.
/// @param row Requested scroll offset.
/// @details Clamps to `[0, max(row_count - visible_rows, 0)]` and repairs an
///          out-of-range selection. Wrong-class handles trap.
void rt_uitable_set_scroll(void *table, int64_t row);

/// @brief First visible row index.
/// @param table UITable to query.
/// @return Clamped scroll offset, or zero for null/invalid.
int64_t rt_uitable_get_scroll(void *table);

/// @brief Set the highlighted/selected row.
/// @param table UITable to mutate.
/// @param row Existing index, or any invalid value to clear selection to `-1`.
void rt_uitable_set_selected_row(void *table, int64_t row);

/// @brief Get the selected row index (-1 if none).
/// @param table UITable to query.
/// @return Selected index, or `-1` for none/null/invalid.
int64_t rt_uitable_get_selected_row(void *table);

/// @brief Process a click at (mx, my); @return the clicked row index, -2 for a header click, or -1.
/// @param table UITable to update.
/// @param mx Click X coordinate.
/// @param my Click Y coordinate.
/// @return Body-row index, `-2` for any hit column header, or `-1` for a miss,
///         hidden table, null, or invalid handle.
/// @details A body hit selects its row. A header hit records its column and
///          toggles sorting only when sortable. A non-null wrong class traps.
int64_t rt_uitable_handle_click(void *table, int64_t mx, int64_t my);
/// @brief Process a click at (mx, my) and return a structured snapshot.
/// @details This is the sentinel-free replacement for rt_uitable_handle_click()
///          plus rt_uitable_last_header_click(). It performs the same state
///          updates as rt_uitable_handle_click(): row clicks select rows and
///          sortable header clicks toggle sort order.
/// @param table Opaque Zanna.Game.UI.Table object.
/// @param mx Click x-coordinate in canvas pixels.
/// @param my Click y-coordinate in canvas pixels.
/// @return New immutable TableClickResult reference, or `NULL` if result
///         allocation fails.
/// @details Even misses and invalid/null tables produce a NONE snapshot when
///          allocation succeeds. Allocation and wrong-class failures trap.
void *rt_uitable_handle_click_result(void *table, int64_t mx, int64_t my);

/// @brief Index of the column header last clicked (for sort toggling), or -1.
/// @param table UITable to query.
/// @return Last recorded header index, or `-1` for no header/null/invalid.
int64_t rt_uitable_last_header_click(void *table);
/// @brief Return the click kind stored in a Zanna.Game.UI.TableClickResult.
/// @param result Opaque TableClickResult object.
/// @return One of RT_UITABLE_CLICK_NONE, RT_UITABLE_CLICK_ROW, or RT_UITABLE_CLICK_HEADER.
/// @details Null returns NONE; a non-null wrong result class traps.
int64_t rt_table_click_result_kind(void *result);
/// @brief Query whether a TableClickResult represents no hit.
/// @param result Opaque TableClickResult object.
/// @return 1 when the click hit neither a row nor a header, otherwise 0.
/// @details Null results classify as NONE; wrong non-null classes trap.
int8_t rt_table_click_result_is_none(void *result);
/// @brief Query whether a TableClickResult represents a body row hit.
/// @param result Opaque TableClickResult object.
/// @return 1 when the click selected a row, otherwise 0.
/// @details Wrong non-null result classes trap.
int8_t rt_table_click_result_is_row(void *result);
/// @brief Query whether a TableClickResult represents a header hit.
/// @param result Opaque TableClickResult object.
/// @return 1 when the click hit a column header, otherwise 0.
/// @details Wrong non-null result classes trap.
int8_t rt_table_click_result_is_header(void *result);
/// @brief Return the selected row as an Option.
/// @details Row clicks return Some(row). Header clicks and misses return None.
/// @param result Opaque TableClickResult object.
/// @return Opaque Zanna.Option containing an i64 row index, or None.
/// @details Returns a fresh Some option for a row hit and the runtime None
///          option for every other kind. Wrong non-null result classes trap.
void *rt_table_click_result_row_option(void *result);
/// @brief Return the clicked header column as an Option.
/// @details Header clicks return Some(column). Row clicks and misses return None.
/// @param result Opaque TableClickResult object.
/// @return Opaque Zanna.Option containing an i64 column index, or None.
/// @details Returns a fresh Some option for a header hit and the runtime None
///          option otherwise. Wrong non-null result classes trap.
void *rt_table_click_result_column_option(void *result);

/// @brief Scroll the view by @p delta rows.
/// @param table UITable to mutate.
/// @param delta Signed relative row count, added with saturation then clamped.
void rt_uitable_handle_scroll(void *table, int64_t delta);

/// @brief Process a key press (row navigation).
/// @param table UITable to update.
/// @param key_code Up, Down, Home, End, PageUp, or PageDown shared key code.
/// @details Empty tables are unchanged. Navigation clamps selection, keeps it
///          visible, and reclamps scroll; visibility does not gate keyboard
///          handling.
void rt_uitable_handle_key(void *table, int64_t key_code);

/// @brief Render the table onto @p canvas.
/// @param table UITable to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Null/hidden tables are no-ops. Draws the optional header, visible
///          scrolled rows, selection/striping, text, and configured borders.
///          Invalid non-null handles raise runtime traps.
void rt_uitable_draw(void *table, void *canvas);

//=========================================================================
// Modal
//=========================================================================

/// @brief Create a centered modal dialog sized w x h.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new closed UIModal at `(0, 0)`, or `NULL` if allocation fails.
/// @note Despite the historical “centered” name, this constructor does not
///       inspect a canvas and therefore initializes the position to the origin.
void *rt_uimodal_new(int64_t w, int64_t h);

/// @brief Create a modal dialog at an explicit (x, y) position.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new closed UIModal, or `NULL` if object or initial child/button
///         capacity allocation fails.
/// @details Result and selected button begin at `-1`; the modal owns retained
///          child references and dynamic button/child arrays.
void *rt_uimodal_new_at(int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Set the modal's title-bar text.
/// @param modal UIModal to mutate.
/// @param title Runtime string copied into a 128-byte inline buffer.
/// @details The input is not retained; invalid handles trap.
void rt_uimodal_set_title(void *modal, rt_string title);

/// @brief Set the modal's body content text.
/// @param modal UIModal to mutate.
/// @param text Runtime string copied into a 512-byte inline buffer.
/// @details The input is not retained; invalid handles trap.
void rt_uimodal_set_content(void *modal, rt_string text);

/// @brief Add a button labeled @p text that closes with @p return_value;
///        returns the button index.
/// @param modal UIModal to mutate.
/// @param text Runtime label copied into a 64-byte inline button buffer.
/// @param return_value Result stored when this button is triggered.
/// @return New zero-based button index, or `-1` on invalid handle/allocation
///         failure.
/// @details The first button becomes selected. Array-allocation and class
///          errors trap; text is not retained.
int64_t rt_uimodal_add_button(void *modal, rt_string text, int64_t return_value);

/// @brief Designate the default button (activated by Enter).
/// @param modal UIModal to mutate.
/// @param index Existing button index.
/// @details Clears every prior default flag, marks this button, and selects it.
///          Invalid indices are ignored; wrong-class handles trap.
void rt_uimodal_set_default_button(void *modal, int64_t index);

/// @brief Designate the cancel button (activated by Escape).
/// @param modal UIModal to mutate.
/// @param index Existing button index.
/// @details Clears every prior cancel flag. Invalid indices are ignored.
void rt_uimodal_set_cancel_button(void *modal, int64_t index);

/// @brief Embed a child widget in the modal's content area.
/// @param modal UIModal to mutate.
/// @param child_widget Non-null runtime object retained by the modal.
/// @details Drawing recognizes TextInput, Slider, Dropdown, Table, and MenuList
///          child classes; keyboard/click forwarding currently targets only
///          TextInput. Capacity-allocation or class errors trap.
void rt_uimodal_add_child(void *modal, void *child_widget);

/// @brief Open (show) the modal.
/// @param modal UIModal to mutate.
/// @details Sets open and visible, and resets result to `-1`; button selection
///          is preserved. Invalid handles trap.
void rt_uimodal_open(void *modal);

/// @brief Close (hide) the modal.
/// @param modal UIModal to mutate.
/// @details Clears only the open flag; result and visible state are preserved.
void rt_uimodal_close(void *modal);

/// @brief Whether the modal is currently open.
/// @param modal UIModal to query.
/// @return Normalized open flag, or zero for null/invalid.
int8_t rt_uimodal_is_open(void *modal);

/// @brief The return value of the button that closed the modal (-1 if open).
/// @param modal UIModal to query.
/// @return Stored result, initially/reset to `-1`, or `-1` for null/invalid.
int64_t rt_uimodal_get_result(void *modal);

/// @brief Process Enter, Escape, Tab, or child-text-input keyboard input.
/// @param modal Open UIModal to update.
/// @param key_code Shared GameUI key code.
/// @param shift_held Reverses Tab traversal and is forwarded to text inputs.
/// @return Triggered button's configured result, or `-1` when no button closes
///         the modal.
/// @details Tab wraps button selection. Enter triggers the selected button;
///          Escape triggers the cancel-marked button. Other/unhandled input is
///          forwarded to every TextInput child. Invalid handles trap.
int64_t rt_uimodal_handle_key(void *modal, int64_t key_code, int8_t shift_held);

/// @brief Process a click against modal buttons and TextInput children.
/// @param modal Open UIModal to update.
/// @param mx Click X coordinate.
/// @param my Click Y coordinate.
/// @return Triggered button result, or `-1` when no button was hit.
/// @details A non-button click is forwarded to all TextInput children and still
///          returns `-1`. Invalid handles trap.
int64_t rt_uimodal_handle_click(void *modal, int64_t mx, int64_t my);

/// @brief Render the modal (and its dimmed backdrop) onto @p canvas.
/// @param modal UIModal to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Closed/invisible/null modals are no-ops. Draws the full-canvas
///          overlay, panel/title/content, recognized children, and fixed-size
///          buttons. Invalid non-null handles trap.
void rt_uimodal_draw(void *modal, void *canvas);

//=========================================================================
// Slider
//=========================================================================

/// @brief Create a horizontal slider over [min_v, max_v] at (x, y) sized w x h.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param min_v First endpoint.
/// @param max_v Second endpoint; endpoints are swapped when reversed.
/// @return A new visible, enabled UISlider reference, or `NULL` on allocation
///         failure.
/// @details Value begins at the lower endpoint, step at one, and dragging off.
void *rt_uislider_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t min_v, int64_t max_v);

/// @brief Set the slider value (clamped to its range).
/// @param s UISlider to mutate.
/// @param v Requested value, snapped to the nearest configured step from the
///        minimum and then range-clamped.
void rt_uislider_set_value(void *s, int64_t v);

/// @brief Get the current slider value.
/// @param s UISlider to query.
/// @return Stored value, or zero for null/invalid.
int64_t rt_uislider_get_value(void *s);

/// @brief Set the increment applied by key/scroll adjustments.
/// @param s UISlider to mutate.
/// @param step Positive step, or one when nonpositive.
/// @details Immediately resnaps the current value. Invalid handles trap.
void rt_uislider_set_step(void *s, int64_t step);

/// @brief Set the text label drawn alongside the slider.
/// @param s UISlider to mutate.
/// @param label Runtime string copied into a 64-byte inline buffer.
/// @details Empty/null text hides the label; input is not retained.
void rt_uislider_set_label(void *s, rt_string label);

/// @brief Process a key press (arrows adjust by step). @return non-zero if consumed.
/// @param s UISlider to update.
/// @param key_code Left/Down decrement, Right/Up increment, Home selects
///        minimum, and End selects maximum.
/// @return `1` only when the value changes; otherwise `0`.
/// @details Hidden or disabled sliders ignore keys. Invalid handles trap.
int8_t rt_uislider_handle_key(void *s, int64_t key_code);

/// @brief Begin dragging when a mouse press hits the slider rectangle.
/// @param s UISlider to update.
/// @param mx Mouse X coordinate mapped across the full width.
/// @param my Mouse Y coordinate used for hit testing.
/// @return `1` only if beginning the drag also changes the value; `0` may still
///         mean a drag began at the existing value.
int8_t rt_uislider_handle_mouse_down(void *s, int64_t mx, int64_t my);

/// @brief Update the value while dragging to x=@p mx. @return non-zero if dragging.
/// @param s UISlider to update.
/// @param mx Mouse X coordinate, clamped across the track.
/// @return `1` when an active, enabled drag changes value; otherwise `0`.
int8_t rt_uislider_handle_mouse_drag(void *s, int64_t mx);

/// @brief End an active drag. @return non-zero if a drag was in progress.
/// @param s UISlider to update.
/// @return Previous dragging flag, then clears it.
int8_t rt_uislider_handle_mouse_up(void *s);

/// @brief Render the slider onto @p canvas.
/// @param s UISlider to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Hidden/null sliders are no-ops. Draws track, proportional fill,
///          eight-pixel thumb, and optional label. Invalid handles trap.
void rt_uislider_draw(void *s, void *canvas);

//=========================================================================
// Dropdown
//=========================================================================

/// @brief Create a dropdown (combo-box) at (x, y) sized w x h.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @return A new visible, enabled, closed UIDropdown reference, or `NULL` if
///         object or initial 32-option capacity allocation fails.
/// @details Selection begins at `-1`; options are owned 64-byte inline records
///          in a dynamically grown array.
void *rt_uidropdown_new(int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Append a selectable option labeled @p text.
/// @param dd UIDropdown to mutate.
/// @param text Runtime string copied into a 64-byte option record; null creates
///        an empty label.
/// @details The first option becomes selected. Input is not retained.
///          Allocation and wrong-class failures trap.
void rt_uidropdown_add_option(void *dd, rt_string text);

/// @brief Remove all options.
/// @param dd UIDropdown to mutate.
/// @details Zeroes allocated option storage, resets count and selection to
///          `-1`, and closes the list without releasing capacity.
void rt_uidropdown_clear_options(void *dd);

/// @brief Index of the selected option (-1 if none).
/// @param dd UIDropdown to query.
/// @return Selected index, or `-1` for none/null/invalid.
int64_t rt_uidropdown_get_selected(void *dd);

/// @brief Select the option at @p index.
/// @param dd UIDropdown to mutate.
/// @param index Existing index; invalid values clear selection to `-1`.
void rt_uidropdown_set_selected(void *dd, int64_t index);

/// @brief Text of the selected option (empty if none).
/// @param dd UIDropdown to query.
/// @return Caller-owned copy of the selected label, the immortal empty
///         singleton when unavailable, or `NULL` on copy failure.
rt_string rt_uidropdown_get_selected_text(void *dd);

/// @brief Whether the option list is currently expanded.
/// @param dd UIDropdown to query.
/// @return Normalized open flag, or zero for null/invalid.
int8_t rt_uidropdown_is_open(void *dd);

/// @brief Expand the option list.
/// @param dd UIDropdown to mutate.
/// @details Opens only while enabled and visible; an empty list may still open.
void rt_uidropdown_open(void *dd);

/// @brief Collapse the option list.
/// @param dd UIDropdown to mutate.
/// @details Clears open regardless of enabled/visible state.
void rt_uidropdown_close(void *dd);

/// @brief Process a click at (mx, my) (toggle/select). @return non-zero if consumed.
/// @param dd UIDropdown to update.
/// @param mx Click X coordinate.
/// @param my Click Y coordinate.
/// @return `1` for a header toggle or expanded-option hit; otherwise `0`.
/// @details An outside click closes the list. Hidden/disabled dropdowns ignore
///          clicks. Invalid handles trap.
int8_t rt_uidropdown_handle_click(void *dd, int64_t mx, int64_t my);

/// @brief Process a key press (open/navigate/select). @return non-zero if consumed.
/// @param dd UIDropdown to update.
/// @param key_code Escape closes, Enter toggles, and Up/Down wrap selection.
/// @return `1` for one of those keys on a visible, enabled, nonempty dropdown;
///         otherwise `0`.
int8_t rt_uidropdown_handle_key(void *dd, int64_t key_code);

/// @brief Render the dropdown (and expanded list) onto @p canvas.
/// @param dd UIDropdown to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Hidden/null dropdowns are no-ops. Draws the closed field/caret and,
///          while open, one same-height row per option. Invalid handles trap.
void rt_uidropdown_draw(void *dd, void *canvas);

//=========================================================================
// Tooltip
//=========================================================================

/// @brief Create a hover tooltip widget.
/// @return A new hidden UITooltip reference, or `NULL` if allocation fails.
/// @details Text begins empty with six-pixel padding and a 500-millisecond
///          hover delay.
void *rt_uitooltip_new(void);

/// @brief Set the tooltip text.
/// @param t UITooltip to mutate.
/// @param text Runtime string copied into a 256-byte inline buffer.
/// @details The input is not retained and UTF-8 truncation preserves validated
///          boundaries. Invalid handles trap.
void rt_uitooltip_set_text(void *t, rt_string text);

/// @brief Set the hover dwell time (ms) before the tooltip appears.
/// @param t UITooltip to mutate.
/// @param ms Delay; negative values become zero.
void rt_uitooltip_set_hover_delay_ms(void *t, int64_t ms);

/// @brief Update tooltip timing/position from the cursor and whether its
///        target is hovered; advances the dwell timer by @p delta_ms.
/// @param t UITooltip to update.
/// @param mx Cursor X coordinate; tooltip target X becomes `mx + 14` with
///        saturation.
/// @param my Cursor Y coordinate; tooltip target Y becomes `my + 18`.
/// @param hovered_target Zero immediately hides and resets dwell; nonzero
///        accumulates hover time.
/// @param delta_ms Positive elapsed milliseconds added with saturation;
///        nonpositive values do not reduce time.
/// @details Visibility becomes true when accumulated dwell reaches the delay.
///          Invalid handles trap.
void rt_uitooltip_update(void *t, int64_t mx, int64_t my, int8_t hovered_target, int64_t delta_ms);

/// @brief Render the tooltip onto @p canvas if currently shown.
/// @param t UITooltip to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Hidden or empty tooltips are no-ops. The measured box is shifted
///          back inside the canvas and clamped to nonnegative coordinates, then
///          background, border, and text are drawn. Invalid handles trap.
void rt_uitooltip_draw(void *t, void *canvas);

#ifdef __cplusplus
}
#endif
