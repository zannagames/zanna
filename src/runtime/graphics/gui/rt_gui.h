//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_gui.h
/// @brief Declares the public C ABI for retained GUI applications and widgets.
///
/// @details
/// This header exposes opaque application, widget, font, layout, event,
/// accessibility, editor, dialog, command, and language-service bridge
/// handles. Parent/child ownership and runtime-managed return values let
/// frontends use the GUI without depending on the lower toolkit's C types.
///
// File: src/runtime/graphics/gui/rt_gui.h
// Purpose: Runtime bridge functions for the ZannaGUI widget library, providing widget creation,
// layout, event handling, and rendering for GUI application development.
//
// Key invariants:
//   - All widget pointers are opaque handles from widget constructor functions.
//   - Widgets are organized in a parent-child hierarchy; destroying a parent destroys children.
//   - Event callbacks must remain valid while the widget exists.
//   - Layout is computed automatically after the widget tree is assembled.
//
// Ownership/Lifetime:
//   - Widget objects must be destroyed with their respective destroy functions.
//   - The root widget is owned by the GUI application; leaf widgets are owned by their parents.
//
// Links: src/runtime/graphics/gui/rt_gui_widgets.c,
//        src/runtime/graphics/common/rt_zia_completion.h,
//        src/runtime/graphics/common/rt_basic_completion.h,
//        src/lib/gui/include/vg_widget.h,
//        src/lib/gui/include/vg_widgets.h,
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md,
//        docs/adr/0165-scrollview-descendant-reveal.md,
//        docs/adr/0167-spinner-mixed-value-state.md,
//        docs/adr/0281-event-driven-process-pty-gui-wakes.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_basic_completion.h"
#include "rt_string.h"
#include "rt_zia_completion.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Button style constants.
typedef enum {
    RT_BTN_DEFAULT = 0,   ///< Standard button.
    RT_BTN_PRIMARY = 1,   ///< Primary action (highlighted).
    RT_BTN_SECONDARY = 2, ///< Secondary action.
    RT_BTN_DANGER = 3,    ///< Destructive action (red).
    RT_BTN_TEXT = 4,      ///< Text-only (no background).
} rt_button_style_t;

/// Icon position constants.
typedef enum {
    RT_ICON_LEFT = 0,  ///< Icon to the left of label (default).
    RT_ICON_RIGHT = 1, ///< Icon to the right of label.
} rt_icon_pos_t;

/// Image scale mode constants.
typedef enum {
    RT_SCALE_NONE = 0,    ///< No scaling (original size).
    RT_SCALE_FIT = 1,     ///< Scale to fit within bounds (preserve aspect ratio).
    RT_SCALE_FILL = 2,    ///< Scale to fill bounds (may crop).
    RT_SCALE_STRETCH = 3, ///< Stretch to fill bounds (distort aspect ratio).
} rt_image_scale_t;

/// Image resize filter constants.
typedef enum {
    RT_IMAGE_FILTER_NEAREST = 0, ///< Deterministic nearest-neighbour sampling.
    RT_IMAGE_FILTER_BILINEAR = 1 ///< Four-sample bilinear interpolation.
} rt_image_filter_t;

/// @brief Validate an opaque handle for an internal cross-language GUI binding.
/// @details This bridge lets C++ runtime helpers validate a C widget without including the
///          C-only private GUI header. It rejects applications, destroyed widgets, and mismatched
///          widget types. The returned pointer is borrowed and must never be freed or retained by
///          the caller. Graphics-disabled builds always return NULL.
/// @param handle Candidate opaque GUI handle.
/// @param widget_type Numeric lower-toolkit widget kind (`vg_widget_type_t`).
/// @return Borrowed live lower widget pointer when the type matches, otherwise NULL.
/// @internal
void *rt_gui_widget_checked_for_binding(void *handle, int64_t widget_type);

//=========================================================================
// GUI Application
//=========================================================================

/// @brief Report whether GUI support is compiled into the current runtime.
/// @details This query is side-effect free: it does not initialize the graphics backend, connect
///          to a display server, or create a native window. A true result means app construction
///          can be attempted; backend-specific construction can still fail and is reported by
///          @ref rt_gui_app_try_new.
/// @return 1 when the GUI implementation is present, otherwise 0.
int64_t rt_gui_system_is_available(void);

/// @brief Describe why GUI support is unavailable in the current runtime.
/// @details The returned runtime string is empty whenever @ref rt_gui_system_is_available returns
///          1. Graphics-disabled builds return the stable text
///          `GUI support is not available in this build`.
/// @return Caller-owned runtime string containing the capability reason or an empty string.
rt_string rt_gui_system_get_unavailable_reason(void);

/// @brief Create a new GUI application with window.
/// @param title Window title.
/// @param width Window width in pixels.
/// @param height Window height in pixels.
/// @return GUI application handle, or NULL on failure.
void *rt_gui_app_new(rt_string title, int64_t width, int64_t height);

/// @brief Attempt to create a GUI application without trapping on expected backend failures.
/// @details This is the fallible companion to @ref rt_gui_app_new. On success the returned
///          `Zanna.Result` owns the application handle. On failure it contains one stable error
///          string describing unavailable GUI support, application-state allocation, native-window
///          creation, or root-widget allocation. Width and height use the same clamping rules as
///          the compatibility constructor.
/// @param title Window title copied during construction; NULL selects `Zanna GUI`.
/// @param width Initial logical width, clamped to the native signed 32-bit range.
/// @param height Initial logical height, clamped to the native signed 32-bit range.
/// @return Caller-owned opaque `Zanna.Result` containing the app or an error string.
void *rt_gui_app_try_new(rt_string title, int64_t width, int64_t height);

/// @brief Destroy a GUI application and free resources.
/// @details Safe to call more than once. After destruction, app methods reject
///          the stale handle without dereferencing it.
/// @param app GUI application handle.
void rt_gui_app_destroy(void *app);

/// @brief Check if application should close.
/// @param app GUI application handle.
/// @return 1 if should close, 0 otherwise.
int64_t rt_gui_app_should_close(void *app);

/// @brief Poll and process events, update widget states.
/// @param app GUI application handle.
void rt_gui_app_poll(void *app);

/// @brief Block up to timeout_ms for OS events, then poll (App.PollWait).
/// @param app GUI application handle.
/// @param timeout_ms Maximum idle wait in ms (clamped to [0, 1000]).
/// @return 1 if events arrived, 0 on timeout.
int64_t rt_gui_app_poll_wait(void *app, int64_t timeout_ms);

/// @brief Wake this app's event wait when the selected Process has output or exits.
int64_t rt_gui_app_watch_process(void *app, void *process);

/// @brief Wake this app's event wait when the selected PTY has output or exits.
int64_t rt_gui_app_watch_pty(void *app, void *pty);

/// @brief Render all widgets to the window.
/// @param app GUI application handle.
void rt_gui_app_render(void *app);

/// @brief Poll events, advance scheduled GUI work, render damage, and present one frame.
/// @details This is the preferred compatibility-preserving frame-loop primitive. It returns false
///          for invalid/destroyed apps and once a close request has been accepted. Timing uses the
///          runtime's monotonic clock, and all app-owned retained surfaces are advanced together.
/// @param app GUI application handle; may be NULL or stale.
/// @return 1 while another frame may run, or 0 when the app is invalid/closing.
int64_t rt_gui_app_run_frame(void *app);

/// @brief Run one GUI frame with an explicit deterministic elapsed time.
/// @details Negative and non-finite deltas become zero. The full sanitized delta is accumulated by
///          the scheduler, while animation advancement in one call is capped at 250 ms to avoid
///          unstable jumps. Platform events are still polled before scheduled work is advanced.
/// @param app GUI application handle; may be NULL or stale.
/// @param delta_ms Deterministic elapsed time in milliseconds.
/// @return 1 while another frame may run, or 0 when the app is invalid/closing.
int64_t rt_gui_app_run_frame_with_delta(void *app, double delta_ms);

/// @brief Return how soon the app needs another scheduler/render pass.
/// @details A zero result means retained layout, paint, or animation work is already pending. A
///          positive result is the delay to the next known timer, and -1 means no deadline is
///          currently scheduled. Invalid or destroyed app handles also return -1.
/// @param app GUI application handle; may be NULL or stale.
/// @return Milliseconds until the next GUI deadline, 0 for immediate work, or -1 for none.
int64_t rt_gui_app_get_next_deadline_ms(void *app);

/// @brief Make an application the current target for app-scoped GUI services.
/// @details Saves the outgoing app's focus/capture/tooltip state and restores the incoming app's
///          state, theme, native menu ownership, cursor context, and constructor default font.
///          Invalid or destroyed handles are ignored without changing the current app.
/// @param app GUI application handle; may be NULL or stale.
void rt_gui_app_make_current(void *app);

/// @brief Get the root widget container.
/// @details The root widget is owned by the app. Destroy the app with
///          rt_gui_app_destroy rather than destroying the root widget directly.
/// @param app GUI application handle.
/// @return Root widget handle.
void *rt_gui_app_get_root(void *app);

/// @brief Set default font for all widgets.
/// @param app GUI application handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_gui_app_set_font(void *app, void *font, double size);

//=========================================================================
// Font Functions
//=========================================================================

/// @brief Load a font from a file.
/// @param path Path to TTF font file.
/// @return Opaque font handle, or NULL on failure.
void *rt_font_load(rt_string path);

/// @brief Load the host's regular proportional system UI font at a logical size.
/// @details Resolution uses Zanna's zero-dependency platform adapter and falls back to the
///          embedded deterministic font when host candidates are unavailable. The successful
///          Font payload is runtime-managed and safe to retain in Result, themes, and user code.
///          Graphics-disabled builds return Result.ErrStr with the stable capability reason.
/// @param size Finite logical-point size in the inclusive range [1,512].
/// @return Caller-owned Zanna.Result containing a managed Font or a stable error string.
void *rt_font_load_system_ui(double size);

/// @brief Load the host's bold proportional system UI font at a logical size.
/// @details This follows the same managed ownership, deterministic fallback, validation, and
///          graphics-disabled contract as @ref rt_font_load_system_ui, but prefers bold faces.
/// @param size Finite logical-point size in the inclusive range [1,512].
/// @return Caller-owned Zanna.Result containing a managed Font or a stable error string.
void *rt_font_load_system_ui_bold(double size);

/// @brief Return the unscaled logical-point size preserved by a Font.
/// @details System-role loaders set this metadata before wrapping the font. Legacy path-loaded
///          fonts report zero unless annotated internally. Null, stale, explicitly destroyed,
///          graphics-disabled, and unrelated handles return zero without trapping.
/// @param font Managed Font wrapper or legacy raw GUI font handle.
/// @return Finite positive logical-point size, or zero when unavailable.
double rt_font_get_logical_size(void *font);

/// @brief Destroy a font and free resources.
/// @details Managed wrappers become inert immediately. If the backing font is still referenced by
///          a live app/widget/service, destruction is deferred through a safe presentation-frame
///          generation and reclaimed automatically after its last render reference disappears.
///          Legacy raw Font handles remain accepted for compatibility.
/// @param font Managed or legacy Font handle; NULL/stale values are ignored.
void rt_font_destroy(void *font);

//=========================================================================
// Widget Functions
//=========================================================================

/// @brief Destroy a widget and all its children.
/// @details App handles and app-owned root widgets are ignored; use App.Destroy
///          for app teardown.
/// @param widget Widget handle.
void rt_widget_destroy(void *widget);

/// @brief Set widget visibility.
/// @param widget Widget handle.
/// @param visible 1 for visible, 0 for hidden.
void rt_widget_set_visible(void *widget, int64_t visible);

/// @brief Set widget enabled state.
/// @param widget Widget handle.
/// @param enabled 1 for enabled, 0 for disabled.
void rt_widget_set_enabled(void *widget, int64_t enabled);

/// @brief Set widget fixed size.
/// @param widget Widget handle.
/// @param width Width in logical units.
/// @param height Height in logical units.
void rt_widget_set_size(void *widget, int64_t width, int64_t height);

/// @brief Set widget preferred size while leaving min/max constraints unchanged.
/// @details Values are logical units and are converted exactly once using the owning app's
///          effective scale. Detached widgets use an identity scale. Invalid, negative, or
///          excessively large values are normalized to the supported layout range.
/// @param widget Widget handle.
/// @param width Preferred logical width; zero requests content-derived width.
/// @param height Preferred logical height; zero requests content-derived height.
void rt_widget_set_preferred_size(void *widget, double width, double height);

/// @brief Set widget maximum size. Pass 0 for either dimension to clear that maximum.
/// @details Values are logical units and are converted exactly once at the app boundary.
///          Invalid handles are ignored and invalid numeric input is normalized safely.
/// @param widget Widget handle.
/// @param width Maximum logical width, or zero for no maximum.
/// @param height Maximum logical height, or zero for no maximum.
void rt_widget_set_max_size(void *widget, double width, double height);

/// @brief Set the minimum layout size of a widget in logical units.
/// @details The lower toolkit stores framebuffer units; this boundary applies the owning app's
///          effective scale exactly once. Root widgets ignore explicit constraints because the
///          application owns their arranged size. Detached widgets use scale 1.
/// @param widget Widget handle; NULL, stale, and app handles are ignored.
/// @param width Non-negative minimum logical width.
/// @param height Non-negative minimum logical height.
void rt_widget_set_min_size(void *widget, double width, double height);

/// @brief Return the widget's minimum width in logical units.
/// @details Converts the stored framebuffer constraint through the current effective app scale.
///          A detached or invalid widget uses scale 1; an invalid handle returns zero.
/// @param widget Widget handle.
/// @return Non-negative minimum logical width.
double rt_widget_get_min_width(void *widget);

/// @brief Return the widget's minimum height in logical units.
/// @details Converts the stored framebuffer constraint through the current effective app scale.
///          A detached or invalid widget uses scale 1; an invalid handle returns zero.
/// @param widget Widget handle.
/// @return Non-negative minimum logical height.
double rt_widget_get_min_height(void *widget);

/// @brief Set the flex grow factor for VBox/HBox layout.
/// @param widget Widget handle.
/// @param flex Flex factor (0 = fixed size, >0 = expand proportionally).
void rt_widget_set_flex(void *widget, double flex);

/// @brief Set a uniform outer margin around a widget.
/// @param widget Widget handle.
/// @param margin Margin in logical units applied equally on all four sides.
void rt_widget_set_margin(void *widget, int64_t margin);

/// @brief Set a uniform inner padding on all four widget edges.
/// @details Padding is specified in logical units, converted once using the owning app's effective
///          scale, and causes the widget subtree to be measured again. Detached widgets use scale
///          1. Invalid handles are ignored.
/// @param widget Widget handle.
/// @param padding Non-negative logical padding.
void rt_widget_set_padding(void *widget, double padding);

/// @brief Set individual inner padding values in logical units.
/// @details Each edge is normalized independently and converted exactly once to the toolkit's
///          framebuffer-unit storage. The argument order follows CSS: left, top, right, bottom.
/// @param widget Widget handle.
/// @param left Non-negative logical left padding.
/// @param top Non-negative logical top padding.
/// @param right Non-negative logical right padding.
/// @param bottom Non-negative logical bottom padding.
void rt_widget_set_padding_edges(
    void *widget, double left, double top, double right, double bottom);

/// @brief Set individual outer margins in logical units.
/// @details Each edge is normalized independently, converted at the app boundary, and invalidates
///          the parent layout. The argument order is left, top, right, bottom.
/// @param widget Widget handle.
/// @param left Non-negative logical left margin.
/// @param top Non-negative logical top margin.
/// @param right Non-negative logical right margin.
/// @param bottom Non-negative logical bottom margin.
void rt_widget_set_margin_edges(void *widget, double left, double top, double right, double bottom);

/// @brief Set the tab-stop index for keyboard focus navigation.
/// @param widget Widget handle.
/// @param idx Tab index >= 0 for explicit ordering, -1 for natural order.
void rt_widget_set_tab_index(void *widget, int64_t idx);

// BINDING-003: GuiWidget read accessors
/// @brief Whether the widget is currently visible (1) or hidden (0).
/// @param widget Widget handle.
/// @return 1 when visible, otherwise 0, including for an invalid handle.
int64_t rt_widget_is_visible(void *widget);
/// @brief Whether the widget is currently enabled (1) or disabled (0).
/// @param widget Widget handle.
/// @return 1 when enabled, otherwise 0, including for an invalid handle.
int64_t rt_widget_is_enabled(void *widget);
/// @brief The widget's laid-out width in physical framebuffer pixels.
/// @param widget Widget handle.
/// @return Non-negative framebuffer width, or 0 for an invalid handle.
int64_t rt_widget_get_width(void *widget);
/// @brief The widget's laid-out height in physical framebuffer pixels.
/// @param widget Widget handle.
/// @return Non-negative framebuffer height, or 0 for an invalid handle.
int64_t rt_widget_get_height(void *widget);
/// @brief The widget's laid-out X position in physical framebuffer pixels.
/// @param widget Widget handle.
/// @return Parent-relative framebuffer X coordinate, or 0 for an invalid handle.
int64_t rt_widget_get_x(void *widget);
/// @brief The widget's laid-out Y position in physical framebuffer pixels.
/// @param widget Widget handle.
/// @return Parent-relative framebuffer Y coordinate, or 0 for an invalid handle.
int64_t rt_widget_get_y(void *widget);
/// @brief The widget's flex grow/shrink factor within its parent layout.
/// @param widget Widget handle.
/// @return Configured flex factor, or 0 for an invalid handle.
double rt_widget_get_flex(void *widget);

/// @brief Return the widget's parent-relative X coordinate in logical units.
/// @details The legacy integer `GetX` remains a framebuffer-unit compatibility query.
/// @param widget Widget handle.
/// @return Parent-relative logical X, or zero for an invalid handle.
double rt_widget_get_logical_x(void *widget);

/// @brief Return the widget's parent-relative Y coordinate in logical units.
/// @details The legacy integer `GetY` remains a framebuffer-unit compatibility query.
/// @param widget Widget handle.
/// @return Parent-relative logical Y, or zero for an invalid handle.
double rt_widget_get_logical_y(void *widget);

/// @brief Return the widget's arranged width in logical units.
/// @param widget Widget handle.
/// @return Logical width, or zero for an invalid handle.
double rt_widget_get_logical_width(void *widget);

/// @brief Return the widget's arranged height in logical units.
/// @param widget Widget handle.
/// @return Logical height, or zero for an invalid handle.
double rt_widget_get_logical_height(void *widget);

/// @brief Return the widget's root-relative screen X coordinate in logical units.
/// @details Parent offsets are accumulated by the lower toolkit before effective-scale conversion.
/// @param widget Widget handle.
/// @return Root-relative logical X, or zero for an invalid handle.
double rt_widget_get_screen_x(void *widget);

/// @brief Return the widget's root-relative screen Y coordinate in logical units.
/// @details Parent offsets are accumulated by the lower toolkit before effective-scale conversion.
/// @param widget Widget handle.
/// @return Root-relative logical Y, or zero for an invalid handle.
double rt_widget_get_screen_y(void *widget);

/// @brief Return the widget's screen-space width in logical units.
/// @param widget Widget handle.
/// @return Logical screen width, or zero for an invalid handle.
double rt_widget_get_screen_width(void *widget);

/// @brief Return the widget's screen-space height in logical units.
/// @param widget Widget handle.
/// @return Logical screen height, or zero for an invalid handle.
double rt_widget_get_screen_height(void *widget);

/// @brief Add a child widget to a parent.
/// @param parent Parent widget handle.
/// @param child Child widget handle.
void rt_widget_add_child(void *parent, void *child);

/// @brief Return the direct parent as an explicit optional value.
/// @details The returned `Zanna.Option` contains a borrowed live widget handle. Root and detached
///          widgets return `None`; invalid handles also return `None`. Releasing the Option does
///          not destroy the widget.
/// @param widget Widget handle.
/// @return Owned `Zanna.Option<Widget>` runtime object.
void *rt_widget_get_parent_option(void *widget);

/// @brief Return the number of direct child widgets.
/// @param widget Widget handle.
/// @return Non-negative direct-child count, or zero for an invalid handle.
int64_t rt_widget_get_child_count(void *widget);

/// @brief Return a direct child by zero-based index as an Option.
/// @details A successful Option contains a borrowed live handle. Negative, overflowing, and
///          out-of-range indices return `None` without changing the tree.
/// @param widget Parent widget handle.
/// @param index Zero-based direct-child index.
/// @return Owned `Zanna.Option<Widget>` runtime object.
void *rt_widget_get_child_at_option(void *widget, int64_t index);

/// @brief Detach one direct child without destroying it.
/// @details On success ownership transfers back to the caller and app/runtime references into the
///          detached subtree are cleared. Non-children, cross-tree handles, stale handles, and
///          attempts involving an app handle return false without mutation.
/// @param parent Parent widget handle.
/// @param child Candidate direct child handle.
/// @return 1 when the child was detached, otherwise 0.
int64_t rt_widget_remove_child(void *parent, void *child);

/// @brief Detach every direct child without destroying any child.
/// @details Ownership of each detached subtree transfers back to its existing caller-visible
///          handles. App/runtime references into those subtrees are cleared before detachment.
///          Invalid handles are ignored.
/// @param parent Parent widget handle.
void rt_widget_clear_children(void *parent);

/// @brief Assign a copied UTF-8 lookup name to a widget.
/// @details Empty text clears the name. Embedded NUL input, allocation failure, and invalid widget
///          handles preserve the previous value. Names are case-sensitive and need not be unique.
/// @param widget Widget handle.
/// @param name Runtime UTF-8 name.
void rt_widget_set_name(void *widget, rt_string name);

/// @brief Return a widget's lookup name as an owned runtime string.
/// @param widget Widget handle.
/// @return Owned UTF-8 name, or an owned empty string when unset or invalid.
rt_string rt_widget_get_name(void *widget);

/// @brief Return the widget's stable process-local identity.
/// @details IDs are never zero for live widgets and are saturated to `INT64_MAX` if the internal
///          unsigned counter ever exceeds the public integer range. Invalid handles return zero.
/// @param widget Widget handle.
/// @return Positive widget ID, or zero for an invalid handle.
int64_t rt_widget_get_id(void *widget);

/// @brief Find a widget by ID within a subtree.
/// @details Search is depth-first and includes the supplied root. Non-positive IDs and invalid
///          roots return `None`. The successful Option contains a borrowed widget handle.
/// @param root Subtree root widget handle.
/// @param id Positive ID previously returned by @ref rt_widget_get_id.
/// @return Owned `Zanna.Option<Widget>` runtime object.
void *rt_widget_find_by_id_option(void *root, int64_t id);

/// @brief Find the first case-sensitive widget name within a subtree.
/// @details Search is depth-first and includes the root. Empty, embedded-NUL, invalid, and missing
///          names return `None` without mutating the tree. The payload is a borrowed handle.
/// @param root Subtree root widget handle.
/// @param name Runtime UTF-8 lookup name.
/// @return Owned `Zanna.Option<Widget>` runtime object.
void *rt_widget_find_by_name_option(void *root, rt_string name);

/// @brief Test whether a logical screen point belongs to this widget's effective bounds.
/// @details Coordinates are converted once using the owning app scale. Hidden, disabled, clipped,
///          invalid, or ancestor-hidden widgets return false. This tests the receiver itself; it
///          does not return the deepest descendant.
/// @param widget Widget handle.
/// @param screen_x Root-relative logical X coordinate.
/// @param screen_y Root-relative logical Y coordinate.
/// @return 1 when the point is inside the effective widget bounds, otherwise 0.
int64_t rt_widget_hit_test(void *widget, double screen_x, double screen_y);

/// @brief Explicitly request repaint of a widget and its ancestor clipping chain.
/// @param widget Widget handle; invalid handles are ignored.
void rt_widget_invalidate_paint(void *widget);

/// @brief Explicitly request remeasurement, rearrangement, and repaint.
/// @details The lower toolkit propagates the dirty state through the ancestor chain so the next
///          scheduled frame observes the change.
/// @param widget Widget handle; invalid handles are ignored.
void rt_widget_invalidate_layout(void *widget);

/// @brief Check if widget is hovered.
/// @param widget Widget handle.
/// @return 1 if hovered, 0 otherwise.
int64_t rt_widget_is_hovered(void *widget);

/// @brief Check if widget is pressed.
/// @param widget Widget handle.
/// @return 1 if pressed, 0 otherwise.
int64_t rt_widget_is_pressed(void *widget);

/// @brief Check if widget is focused.
/// @param widget Widget handle.
/// @return 1 if focused, 0 otherwise.
int64_t rt_widget_is_focused(void *widget);

/// @brief Move keyboard focus to the specified widget.
/// @details Passing NULL is a no-op.
/// @param widget Widget handle.
void rt_widget_focus(void *widget);

/// @brief Check if widget was clicked this frame.
/// @param widget Widget handle.
/// @return 1 if clicked, 0 otherwise.
int64_t rt_widget_was_clicked(void *widget);

/// @brief Set widget position.
/// @details Intended for manually positioned widgets and overlay-style controls.
///          Managed layout containers may override the position on the next
///          layout pass.
/// @param widget Widget handle.
/// @param x X position in logical units.
/// @param y Y position in logical units.
void rt_widget_set_position(void *widget, int64_t x, int64_t y);

/// @brief Set a widget's cross-platform accessibility role.
/// @details Values outside `Zanna.GUI.AccessibleRole` become `None`. Invalid or destroyed handles
///          are ignored. The mutation advances the widget's non-consuming semantic revision.
/// @param widget Opaque widget handle; may be NULL or stale.
/// @param role Stable accessible-role integer.
void rt_widget_set_accessible_role(void *widget, int64_t role);

/// @brief Return a widget's cross-platform accessibility role.
/// @param widget Opaque widget handle; may be NULL or stale.
/// @return Stable role integer, or zero (`None`) for an invalid handle.
int64_t rt_widget_get_accessible_role(void *widget);

/// @brief Set an explicit accessible name.
/// @details Copies UTF-8 atomically; empty clears the override, embedded NUL input is rejected,
///          and allocation failure preserves prior state. Invalid handles are ignored.
/// @param widget Opaque widget handle.
/// @param name Runtime UTF-8 string.
void rt_widget_set_accessible_name(void *widget, rt_string name);

/// @brief Return the explicit accessible-name override.
/// @details Control text may still be used as an inferred name in accessibility snapshots when
///          this value is empty.
/// @param widget Opaque widget handle.
/// @return Owned runtime string; empty for unset or invalid handles.
rt_string rt_widget_get_accessible_name(void *widget);

/// @brief Set an accessible description copied from UTF-8 runtime text.
/// @details Empty clears the description. Embedded NUL/OOM/invalid handles preserve old state.
/// @param widget Opaque widget handle.
/// @param description Supplemental accessible description.
void rt_widget_set_accessible_description(void *widget, rt_string description);

/// @brief Return a widget's accessible description.
/// @param widget Opaque widget handle.
/// @return Owned runtime string; empty for unset or invalid handles.
rt_string rt_widget_get_accessible_description(void *widget);

/// @brief Set an explicit accessible value copied atomically from UTF-8.
/// @details Empty restores control-derived value inference. Embedded NUL and OOM are rejected.
/// @param widget Opaque widget handle.
/// @param value Semantic value text.
void rt_widget_set_accessible_value(void *widget, rt_string value);

/// @brief Return a widget's explicit accessible-value override.
/// @param widget Opaque widget handle.
/// @return Owned runtime string; empty for unset or invalid handles.
rt_string rt_widget_get_accessible_value(void *widget);

/// @brief Relate a label widget to a target in the same GUI application tree.
/// @details Cross-app, stale, self, and forged targets are rejected without changing the existing
///          relationship. Both handles remain independently owned; no retain cycle is introduced.
/// @param widget Labelling widget handle.
/// @param target Label target widget handle.
void rt_widget_set_accessible_label_for(void *widget, void *target);

/// @brief Clear a widget's non-owning accessible label relationship.
/// @param widget Labelling widget handle; invalid handles are ignored.
void rt_widget_clear_accessible_label_for(void *widget);

/// @brief Set default live-region announcement urgency.
/// @details Invalid values become `Off`; invalid widget handles are ignored.
/// @param widget Opaque widget handle.
/// @param mode `Off`, `Polite`, or `Assertive` integer.
void rt_widget_set_live_region(void *widget, int64_t mode);

/// @brief Return default live-region announcement urgency.
/// @param widget Opaque widget handle.
/// @return Stable mode integer, or `Off` for invalid handles.
int64_t rt_widget_get_live_region(void *widget);

/// @brief Return a widget's non-consuming state revision.
/// @details Unlike `Was*` edge APIs this query never clears observer state and is safe for any
///          number of independent consumers.
/// @param widget Opaque widget handle.
/// @return Monotonic revision, or zero for an invalid handle.
int64_t rt_widget_get_revision(void *widget);

/// @brief Build a deterministic, versioned accessibility snapshot for a visible widget tree.
/// @details The returned owned Map contains semantic identity, role/name/value/state,
///          logical/screen bounds, label/live-region metadata, revisions, and recursively nested
///          child Maps. Allocation is permitted; invalid roots return an empty versioned Map.
/// @param root Root widget handle to snapshot.
/// @return Owned `Zanna.Collections.Map`, or NULL only on initial allocation failure.
void *rt_accessibility_snapshot(void *root);

/// @brief Enable or disable high-contrast presentation for the active GUI app.
/// @details Rebuilds and invalidates that app's per-app theme immediately while preserving its
///          dark/light selection and explicit widget semantics. No active app is a no-op.
/// @param enabled Non-zero to enable the built-in high-contrast palette.
void rt_accessibility_set_high_contrast(int64_t enabled);

/// @brief Return whether high contrast is enabled for the active app.
/// @return 1 when enabled, otherwise 0 (including no active app).
int64_t rt_accessibility_is_high_contrast(void);

/// @brief Enable or disable reduced motion for the active GUI app.
/// @details Theme motion is rebuilt immediately. Enabling causes generic animations to snap on
///          their next scheduler pass and removes future animation-only deadlines.
/// @param enabled Non-zero to enable reduced motion.
void rt_accessibility_set_reduced_motion(int64_t enabled);

/// @brief Return whether reduced motion is enabled for the active app.
/// @return 1 when enabled, otherwise 0 (including no active app).
int64_t rt_accessibility_is_reduced_motion(void);

/// @brief Query the platform adapter's current high-contrast preference.
/// @details Returns zero when the backend cannot report the preference; explicit app state remains
///          independently available through @ref rt_accessibility_is_high_contrast.
/// @return 1 when the platform preference is active, otherwise 0.
int64_t rt_accessibility_get_system_high_contrast(void);

/// @brief Query the platform adapter's current reduced-motion preference.
/// @details Returns zero when unsupported and does not alter explicit app preferences.
/// @return 1 when the platform preference is active, otherwise 0.
int64_t rt_accessibility_get_system_reduced_motion(void);

/// @brief Emit and retain a semantic live-region announcement.
/// @details Text is copied atomically into the widget's headless semantic record. Platform bridge
///          projection is best effort and never removes the headless event. Invalid handles,
///          embedded NUL strings, and allocation failure are ignored safely.
/// @param widget Originating widget handle.
/// @param text UTF-8 announcement.
/// @param mode `Polite` or `Assertive`; invalid values use the widget default.
void rt_accessibility_announce(void *widget, rt_string text, int64_t mode);

//=========================================================================
// Label Widget
//=========================================================================

/// @brief Create a new label widget.
/// @param parent Parent widget (can be NULL).
/// @param text Label text.
/// @return Label widget handle.
void *rt_label_new(void *parent, rt_string text);

/// @brief Set label text.
/// @param label Label widget handle.
/// @param text New text.
void rt_label_set_text(void *label, rt_string text);

/// @brief Set label font.
/// @param label Label widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_label_set_font(void *label, void *font, double size);

/// @brief Set label text color.
/// @param label Label widget handle.
/// @param color ARGB color (0xAARRGGBB).
void rt_label_set_color(void *label, int64_t color);

/// @brief Enable (1) or disable (0) word wrapping on a label.
/// @param label Label widget handle.
/// @param enabled Non-zero to enable wrapping; zero to keep text on one line.
void rt_label_set_word_wrap(void *label, int64_t enabled);

/// @brief Set (or clear) a named scalable vector icon before a label's text.
/// @details Unknown names are ignored; an empty name clears the icon. Rendered
///          on non-wrapped labels only (ADR 0137).
/// @param label Label widget handle.
/// @param name Stable kebab-case icon name (e.g. "zanna-mark").
void rt_label_set_icon_name(void *label, rt_string name);

/// @brief Set a label's horizontal text alignment.
/// @details Accepted values are `0` (left), `1` (center), and `2` (right). Invalid enum values and
///          invalid handles are ignored.
/// @param label Label widget handle.
/// @param alignment Horizontal alignment enum value.
void rt_label_set_alignment(void *label, int64_t alignment);

/// @brief Return a label's horizontal text alignment.
/// @param label Label widget handle.
/// @return `0` (left), `1` (center), `2` (right), or `-1` for an invalid handle.
int64_t rt_label_get_alignment(void *label);

/// @brief Enable or disable ellipsis rendering for truncated label text.
/// @details Single-line text is fitted to its arranged width without splitting UTF-8 units.
///          Wrapped text receives U+2026 on the final visible line only when MaxLines truncates.
/// @param label Label widget handle; invalid handles are ignored.
/// @param enabled Non-zero to enable ellipsis rendering.
void rt_label_set_ellipsis(void *label, int64_t enabled);

/// @brief Set the maximum number of visible wrapped lines.
/// @details Zero and negative values mean unlimited. The setting affects wrapped labels and
///          participates in measurement; very large values clamp to `INT_MAX`.
/// @param label Label widget handle; invalid handles are ignored.
/// @param count Maximum visible line count or zero for unlimited.
void rt_label_set_max_lines(void *label, int64_t count);

/// @brief Enable or disable read-only text selection on a label.
/// @details Selectable labels accept pointer drag, Shift-click, Ctrl/Cmd+A, Ctrl/Cmd+C, and Escape.
///          Disabling selection clears endpoints and releases capture/focus.
/// @param label Label widget handle; invalid handles are ignored.
/// @param enabled Non-zero to make the label focusable and selectable.
void rt_label_set_selectable(void *label, int64_t enabled);

/// @brief Return the source text currently selected in a label.
/// @details The result is a fresh runtime string. Ctrl/Cmd+A includes source hidden by ellipsis or
///          MaxLines because selection is defined over content rather than display caches.
/// @param label Label widget handle.
/// @return Selected UTF-8 text, or an empty string when no selection exists or the handle is
/// invalid.
rt_string rt_label_get_selected_text(void *label);

//=========================================================================
// Button Widget
//=========================================================================

/// @brief Create a new button widget.
/// @param parent Parent widget (can be NULL).
/// @param text Button text.
/// @return Button widget handle.
void *rt_button_new(void *parent, rt_string text);

/// @brief Set button text.
/// @param button Button widget handle.
/// @param text New text.
void rt_button_set_text(void *button, rt_string text);

/// @brief Set button font.
/// @param button Button widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_button_set_font(void *button, void *font, double size);

/// @brief Set button style.
/// @param button Button widget handle.
/// @param style Button style (RT_BTN_DEFAULT through RT_BTN_TEXT).
void rt_button_set_style(void *button, int64_t style);

/// @brief Set button icon text (UTF-8 emoji or icon glyph).
/// @param button Button widget handle.
/// @param icon Icon text (NULL or empty to remove).
void rt_button_set_icon(void *button, rt_string icon);

/// @brief Set icon position relative to label.
/// @param button Button widget handle.
/// @param pos Icon position (RT_ICON_LEFT or RT_ICON_RIGHT).
void rt_button_set_icon_pos(void *button, int64_t pos);

/// @brief Set (or clear) a named scalable vector icon on a button.
/// @details Unknown names are ignored; an empty name clears the icon. Vector
///          icons take precedence over icon text (ADR 0137).
/// @param button Button widget handle.
/// @param name Stable kebab-case icon name (e.g. "run", "settings-gear").
void rt_button_set_icon_name(void *button, rt_string name);

//=========================================================================
// TextInput Widget
//=========================================================================

/// @brief Create a new text input widget.
/// @param parent Parent widget (can be NULL).
/// @return TextInput widget handle.
void *rt_textinput_new(void *parent);

/// @brief Set text input content.
/// @param input TextInput widget handle.
/// @param text New text.
void rt_textinput_set_text(void *input, rt_string text);

/// @brief Get text input content.
/// @param input TextInput widget handle.
/// @return Current text as runtime string.
rt_string rt_textinput_get_text(void *input);

/// @brief Set placeholder text.
/// @param input TextInput widget handle.
/// @param placeholder Placeholder text.
void rt_textinput_set_placeholder(void *input, rt_string placeholder);

/// @brief Set text input font.
/// @param input TextInput widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_textinput_set_font(void *input, void *font, double size);

/// @brief Configure the maximum committed text length in user-perceived characters.
/// @details Length is measured in Unicode extended grapheme clusters, so a combining sequence,
///          emoji ZWJ sequence, or flag counts as one character. Passing zero removes the limit;
///          negative values are clamped to zero. Lowering the limit truncates at a grapheme
///          boundary and resets the editor's undo baseline to the resulting programmatic value.
/// @param input TextInput widget handle.
/// @param max_length Maximum grapheme count, or zero for unlimited.
void rt_textinput_set_max_length(void *input, int64_t max_length);

/// @brief Return the configured maximum committed-text length.
/// @param input TextInput widget handle.
/// @return Maximum extended-grapheme count, or zero for unlimited or an invalid handle.
int64_t rt_textinput_get_max_length(void *input);

/// @brief Enable or disable password presentation for a text input.
/// @details Masking affects rendering only; committed text and undo history remain intact, and one
///          mask glyph is displayed for each Unicode extended grapheme cluster.
/// @param input TextInput widget handle.
/// @param password Non-zero to mask text, zero to render committed text normally.
void rt_textinput_set_password(void *input, int64_t password);

/// @brief Query whether password presentation is enabled.
/// @param input TextInput widget handle.
/// @return One when masking is enabled; otherwise zero.
int64_t rt_textinput_is_password(void *input);

/// @brief Enable or disable mutation while retaining navigation and copying.
/// @details Enabling read-only mode cancels active IME composition. Typing, deletion, paste,
///          undo, redo, and composition commits are rejected until the mode is disabled.
/// @param input TextInput widget handle.
/// @param read_only Non-zero to reject text mutations, zero to allow editing.
void rt_textinput_set_read_only(void *input, int64_t read_only);

/// @brief Query whether committed text is read-only.
/// @param input TextInput widget handle.
/// @return One for a valid read-only widget; otherwise zero.
int64_t rt_textinput_is_read_only(void *input);

/// @brief Select multiline editing or single-line submission behavior.
/// @details Multiline mode accepts newline insertion. Returning to single-line mode removes
///          existing carriage-return and newline code points at grapheme-safe boundaries.
/// @param input TextInput widget handle.
/// @param multiline Non-zero to enable multiple lines, zero for a single line.
void rt_textinput_set_multiline(void *input, int64_t multiline);

/// @brief Query whether multiline editing is enabled.
/// @param input TextInput widget handle.
/// @return One for a valid multiline widget; otherwise zero.
int64_t rt_textinput_is_multiline(void *input);

/// @brief Move the caret to a Unicode extended-grapheme boundary.
/// @details The requested non-negative index is clamped to the committed grapheme count. Negative
///          indices clamp to the beginning and never split combining or emoji sequences.
/// @param input TextInput widget handle.
/// @param grapheme_index Zero-based caret boundary in user-perceived characters.
void rt_textinput_set_cursor(void *input, int64_t grapheme_index);

/// @brief Return the caret boundary in Unicode extended-grapheme units.
/// @param input TextInput widget handle.
/// @return Zero-based grapheme boundary, or zero for an invalid handle.
int64_t rt_textinput_get_cursor(void *input);

/// @brief Select a half-open committed-text range in grapheme units.
/// @details Both endpoints are clamped independently. Direction is retained internally while the
///          public selection getters return ordered start and end boundaries.
/// @param input TextInput widget handle.
/// @param start Grapheme selection anchor; negative values clamp to zero.
/// @param end Grapheme selection caret; negative values clamp to zero.
void rt_textinput_select_range(void *input, int64_t start, int64_t end);

/// @brief Collapse the current selection at the caret without modifying text.
/// @param input TextInput widget handle.
void rt_textinput_clear_selection(void *input);

/// @brief Return the ordered inclusive selection start in grapheme units.
/// @param input TextInput widget handle.
/// @return Minimum selection endpoint, or zero for an invalid handle.
int64_t rt_textinput_get_selection_start(void *input);

/// @brief Return the ordered exclusive selection end in grapheme units.
/// @param input TextInput widget handle.
/// @return Maximum selection endpoint, or zero for an invalid handle.
int64_t rt_textinput_get_selection_end(void *input);

/// @brief Copy the selected committed text into a runtime string.
/// @details The selection endpoints are grapheme-safe. No selection, allocation failure, or an
///          invalid widget produces the canonical empty runtime string.
/// @param input TextInput widget handle.
/// @return Caller-owned runtime string containing the selected UTF-8 bytes.
rt_string rt_textinput_get_selected_text(void *input);

/// @brief Insert committed UTF-8 text as one undoable edit.
/// @details The current selection is replaced, malformed UTF-8 is sanitized by the GUI editor,
///          and the configured grapheme limit is enforced without splitting a cluster.
/// @param input TextInput widget handle.
/// @param text Runtime string to insert.
/// @return One only when committed text changed; otherwise zero.
int64_t rt_textinput_insert_text(void *input, rt_string text);

/// @brief Delete the selected grapheme-safe range as one undoable edit.
/// @param input TextInput widget handle.
/// @return One when a non-empty editable selection was removed; otherwise zero.
int64_t rt_textinput_delete_selection(void *input);

/// @brief Restore the preceding committed-text snapshot.
/// @param input TextInput widget handle.
/// @return One when an older snapshot was restored; otherwise zero.
int64_t rt_textinput_undo(void *input);

/// @brief Reapply the next committed-text snapshot.
/// @param input TextInput widget handle.
/// @return One when a newer snapshot was restored; otherwise zero.
int64_t rt_textinput_redo(void *input);

/// @brief Query whether an older undo snapshot is available.
/// @param input TextInput widget handle.
/// @return One when Undo can modify committed text; otherwise zero.
int64_t rt_textinput_can_undo(void *input);

/// @brief Query whether a newer redo snapshot is available.
/// @param input TextInput widget handle.
/// @return One when Redo can modify committed text; otherwise zero.
int64_t rt_textinput_can_redo(void *input);

/// @brief Consume the text-changed edge independently of submission state.
/// @param input TextInput widget handle.
/// @return One once after one or more committed changes since the previous call; otherwise zero.
int64_t rt_textinput_was_changed(void *input);

/// @brief Consume the single-line Enter-submission edge independently of text changes.
/// @param input TextInput widget handle.
/// @return One once after one or more submissions since the previous call; otherwise zero.
int64_t rt_textinput_was_submitted(void *input);

/// @brief Return the non-consuming monotonic editor state revision.
/// @details Revisions saturate rather than wrap and cover committed text and public editor-state
///          changes. IME preedit updates do not masquerade as committed-text revisions.
/// @param input TextInput widget handle.
/// @return Current revision clamped to the signed runtime integer range, or zero if invalid.
int64_t rt_textinput_get_revision(void *input);

/// @brief Query whether native IME preedit composition is active.
/// @param input TextInput widget handle.
/// @return One while composition is active; otherwise zero.
int64_t rt_textinput_is_composing(void *input);

/// @brief Copy the current uncommitted IME preedit text into a runtime string.
/// @param input TextInput widget handle.
/// @return Caller-owned preedit UTF-8 string, or the canonical empty string if unavailable.
rt_string rt_textinput_get_composition_text(void *input);

/// @brief Return the committed-text insertion boundary for active preedit.
/// @param input TextInput widget handle.
/// @return Grapheme insertion boundary, or zero when invalid or not composing.
int64_t rt_textinput_get_composition_start(void *input);

/// @brief Return current IME preedit length in user-perceived characters.
/// @param input TextInput widget handle.
/// @return Extended-grapheme count of preedit text, or zero when unavailable.
int64_t rt_textinput_get_composition_length(void *input);

//=========================================================================
// Checkbox Widget
//=========================================================================

/// @brief Create a new checkbox widget.
/// @param parent Parent widget (can be NULL).
/// @param text Checkbox label text.
/// @return Checkbox widget handle.
void *rt_checkbox_new(void *parent, rt_string text);

/// @brief Set checkbox checked state.
/// @param checkbox Checkbox widget handle.
/// @param checked 1 for checked, 0 for unchecked.
void rt_checkbox_set_checked(void *checkbox, int64_t checked);

/// @brief Get checkbox checked state.
/// @param checkbox Checkbox widget handle.
/// @return 1 if checked, 0 if not.
int64_t rt_checkbox_is_checked(void *checkbox);

/// @brief Set checkbox text.
/// @param checkbox Checkbox widget handle.
/// @param text New text.
void rt_checkbox_set_text(void *checkbox, rt_string text);

/// @brief Set checkbox indeterminate state.
/// @param checkbox Checkbox widget handle.
/// @param indeterminate Non-zero to show the mixed state; zero to clear it.
void rt_checkbox_set_indeterminate(void *checkbox, int64_t indeterminate);

/// @brief Get checkbox indeterminate state.
/// @param checkbox Checkbox widget handle.
/// @return 1 when indeterminate, otherwise 0.
int64_t rt_checkbox_is_indeterminate(void *checkbox);

/// @brief Consume the checkbox's independent value-change edge.
/// @details Checked and indeterminate transitions share this edge. Reading it does not consume the
///          non-consuming revision or any activation/submission event. Invalid handles return 0.
/// @param checkbox Checkbox widget handle.
/// @return 1 once after one or more unreported value transitions, otherwise 0.
int64_t rt_checkbox_was_changed(void *checkbox);

/// @brief Return the checkbox's non-consuming state revision.
/// @details Any number of observers may compare this monotonic value. Invalid handles return zero;
///          values above the signed runtime range saturate at `INT64_MAX`.
/// @param checkbox Checkbox widget handle.
/// @return Monotonic revision, or zero for an invalid handle.
int64_t rt_checkbox_get_revision(void *checkbox);

//=========================================================================
// ScrollView Widget
//=========================================================================

/// @brief Create a new scroll view widget.
/// @param parent Parent widget (can be NULL).
/// @return ScrollView widget handle.
void *rt_scrollview_new(void *parent);

/// @brief Set scroll position.
/// @param scroll ScrollView widget handle.
/// @param x Horizontal scroll position.
/// @param y Vertical scroll position.
void rt_scrollview_set_scroll(void *scroll, double x, double y);

/// @brief Set content size.
/// @param scroll ScrollView widget handle.
/// @param width Content width (0 = auto).
/// @param height Content height (0 = auto).
void rt_scrollview_set_content_size(void *scroll, double width, double height);

/// @brief Scroll until one descendant widget is fully visible.
/// @details Invalid handles and widgets outside this ScrollView are inert. One
///          live-ID-guarded request survives the next layout pass so a
///          collapsed ancestor may be restored by the same command.
/// @param scroll ScrollView widget handle.
/// @param widget Descendant GUI widget handle to reveal.
void rt_scrollview_scroll_to(void *scroll, void *widget);

/// @brief Get the current horizontal scroll offset.
/// @param scroll ScrollView widget handle.
/// @return Horizontal scroll position in pixels (0 at the left edge).
double rt_scrollview_get_scroll_x(void *scroll);

/// @brief Get the current vertical scroll offset.
/// @param scroll ScrollView widget handle.
/// @return Vertical scroll position in pixels (0 at the top edge).
double rt_scrollview_get_scroll_y(void *scroll);

//=========================================================================
// TreeView Widget
//=========================================================================

/// @brief Create a new tree view widget.
/// @param parent Parent widget (can be NULL).
/// @return TreeView widget handle.
void *rt_treeview_new(void *parent);

/// @brief Add a node to the tree view.
/// @param tree TreeView widget handle.
/// @param parent_node Parent node handle (NULL for root).
/// @param text Node text.
/// @return New node handle.
void *rt_treeview_add_node(void *tree, void *parent_node, rt_string text);

/// @brief Remove a node from the tree view.
/// @param tree TreeView widget handle.
/// @param node Node handle to remove.
void rt_treeview_remove_node(void *tree, void *node);

/// @brief Clear all nodes from tree view.
/// @param tree TreeView widget handle.
void rt_treeview_clear(void *tree);

/// @brief Free retired tree-node tombstones after stale node handles are discarded.
/// @param tree TreeView widget handle.
void rt_treeview_prune_retired_nodes(void *tree);

/// @brief Expand a tree node.
/// @param tree TreeView widget handle.
/// @param node Node handle.
void rt_treeview_expand(void *tree, void *node);

/// @brief Collapse a tree node.
/// @param tree TreeView widget handle.
/// @param node Node handle.
void rt_treeview_collapse(void *tree, void *node);

/// @brief Toggle a tree node between expanded and collapsed state.
/// @details The operation accepts only a live node owned by @p tree. Expanding a node that
///          advertises lazy children records a load request and enables its loading indicator.
///          Foreign, stale, or NULL handles are ignored.
/// @param tree TreeView widget handle.
/// @param node Node handle owned by @p tree.
void rt_treeview_toggle(void *tree, void *node);

/// @brief Select a tree node.
/// @details Adds @p node when retained multi-selection is enabled. Passing NULL always clears all
///          retained selection.
/// @param tree TreeView widget handle.
/// @param node Node handle.
void rt_treeview_select(void *tree, void *node);

/// @brief Enable or disable retained-node multi-selection.
/// @details Disabling preserves only the primary node. Virtual TreeViews remain single-select.
/// @param tree TreeView widget handle.
/// @param enabled Non-zero to enable additive, toggle, and visible range selection.
void rt_treeview_set_multi_select(void *tree, int64_t enabled);

/// @brief Scroll the minimum distance required to make a tree node visible.
/// @details Coordinates remain internal to the TreeView; callers provide only a live node owned by
///          @p tree. Already-visible nodes do not change scroll state or revision.
/// @param tree TreeView widget handle.
/// @param node Node handle owned by @p tree.
void rt_treeview_scroll_to(void *tree, void *node);

/// @brief Set tree view font.
/// @param tree TreeView widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_treeview_set_font(void *tree, void *font, double size);

/// @brief Get the currently selected tree node.
/// @param tree TreeView widget handle.
/// @return Selected node handle or NULL if none selected.
void *rt_treeview_get_selected(void *tree);

/// @brief Copy stable data for every selected retained node in complete preorder.
/// @details Collapsed descendants remain represented. Nodes without data contribute empty strings;
///          invalid, empty, and virtual TreeViews return a valid empty owned sequence.
/// @param tree TreeView widget handle.
/// @return Owned `Seq[String]`.
void *rt_treeview_get_selected_data(void *tree);

/// @brief Get the visible tree node under a window-space point.
/// @param tree TreeView widget handle.
/// @param x Window-space X coordinate.
/// @param y Window-space Y coordinate.
/// @return Tree node handle or NULL if the point is outside rows.
void *rt_treeview_get_node_at(void *tree, int64_t x, int64_t y);

/// @brief Enable application-directed (poll-model) drag-and-drop on the tree.
/// @details Compatibility wrapper selecting mode 1 (legacy container-only INTO) when enabled and
///          mode 0 when disabled.
/// @param tree TreeView widget handle.
/// @param enabled Non-zero to enable INTO-only latched drops.
void rt_treeview_set_drag_drop_enabled(void *tree, int64_t enabled);

/// @brief Select the application-directed drag-and-drop mode.
/// @details `0` disables; `1` preserves container-only INTO drops; `2` latches row-aware
///          BEFORE/INTO/AFTER drops. Other values are ignored.
/// @param tree TreeView widget handle.
/// @param mode Drag-and-drop mode.
void rt_treeview_set_drag_drop_mode(void *tree, int64_t mode);

/// @brief True while a completed drop is waiting to be consumed.
/// @param tree TreeView widget handle.
/// @return 1 when an unconsumed drop is latched, otherwise 0.
int64_t rt_treeview_was_drop_received(void *tree);

/// @brief Data string of the dragged node at the latched drop.
/// @param tree TreeView widget handle.
/// @return Owned source-node data, or an empty runtime string when no drop is latched.
rt_string rt_treeview_get_drop_source_data(void *tree);

/// @brief Data string of the target node at the latched drop.
/// @param tree TreeView widget handle.
/// @return Owned target-node data, or an empty runtime string when no drop is latched.
rt_string rt_treeview_get_drop_target_data(void *tree);

/// @brief Latched drop position (0=before, 1=into, 2=after).
/// @param tree TreeView widget handle.
/// @return Drop-position value, or the implementation's no-drop sentinel when unavailable.
int64_t rt_treeview_get_drop_position(void *tree);

/// @brief Consume the latched drop so the next drop can be observed.
/// @param tree TreeView widget handle.
void rt_treeview_clear_drop(void *tree);

/// @brief Begin an inline edit of one row; Enter/blur commit, Escape cancels.
/// @param tree TreeView widget handle.
/// @param node Live node handle owned by @p tree.
/// @param initial_text Editor contents at start.
/// @return 1 when the edit began, otherwise 0.
int64_t rt_treeview_begin_edit_node(void *tree, void *node, rt_string initial_text);

/// @brief Whether an inline row edit is currently in progress.
/// @param tree TreeView widget handle.
/// @return 1 while an edit is active, otherwise 0.
int64_t rt_treeview_is_editing(void *tree);

/// @brief Consume the inline-edit commit edge (1 exactly once per commit).
/// @param tree TreeView widget handle.
/// @return 1 once after an unreported commit, otherwise 0.
int64_t rt_treeview_was_edit_committed(void *tree);

/// @brief Most recently committed inline-edit text (owned; empty when none).
/// @param tree TreeView widget handle.
/// @return Owned committed text, or an empty runtime string when none is available.
rt_string rt_treeview_get_edit_text(void *tree);

/// @brief Node whose inline edit most recently committed (owned Option).
/// @param tree TreeView widget handle.
/// @return Owned `Option[TreeNode]` containing the edited node when available.
void *rt_treeview_get_edited_node_option(void *tree);

/// @brief Cancel any inline edit in progress without committing.
/// @param tree TreeView widget handle.
void rt_treeview_cancel_edit(void *tree);

/// @brief Check if selection changed since last call (polling pattern).
/// @param tree TreeView widget handle.
/// @return 1 if selection changed, 0 otherwise.
int64_t rt_treeview_was_selection_changed(void *tree);

/// @brief Consume the TreeView's common selection-change edge.
/// @details This edge is independent from `WasSelectionChanged`, allowing compatibility and new
///          observers to consume either without hiding the other. Invalid handles return zero.
/// @param tree TreeView widget handle.
/// @return 1 when an unreported selection change exists, otherwise 0.
int64_t rt_treeview_was_changed(void *tree);

/// @brief Consume a TreeView node activation edge.
/// @details Double-click and Enter on a selected node record activation before optional callbacks.
///          Reading this edge does not consume selection or general revisions.
/// @param tree TreeView widget handle.
/// @return 1 when an unreported activation exists, otherwise 0.
int64_t rt_treeview_was_activated(void *tree);

/// @brief Consume the TreeView's independent lazy-child-request edge.
/// @details Expanding an unmaterialized node whose `HasChildren` flag is set records this edge once
///          and marks the node loading. The edge is independent from selection and activation;
///          multiple unobserved requests coalesce. Invalid handles return zero.
/// @param tree TreeView widget handle.
/// @return 1 once after one or more unreported lazy-child requests, otherwise 0.
int64_t rt_treeview_was_load_children_requested(void *tree);

/// @brief Return the most recent lazy-child request target as `Zanna.Option`.
/// @details Returns `Some(TreeView.Node)` while the latched target remains live, otherwise `None`.
///          Reading the payload does not consume `WasLoadChildrenRequested`; removing the target
///          clears the latch. The returned Option is a fresh managed object containing a borrowed,
///          owner-validated node handle.
/// @param tree TreeView widget handle.
/// @return Owned `Zanna.Option` object.
void *rt_treeview_get_load_requested_node_option(void *tree);

/// @brief Return the most recently activated tree node as `Zanna.Option`.
/// @details Double-click and Enter update the payload before recording `WasActivated`. Reading the
///          Option does not consume that edge. A never-activated, removed, or invalid node yields
///          `None`; otherwise the returned managed Option contains a borrowed node handle.
/// @param tree TreeView widget handle.
/// @return Owned `Zanna.Option` object.
void *rt_treeview_get_activated_node_option(void *tree);

/// @brief Return the TreeView's non-consuming state revision.
/// @param tree TreeView widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_treeview_get_revision(void *tree);

/// @brief Get the text label of a tree node.
/// @param node Tree node handle.
/// @return Node text as runtime string.
rt_string rt_treeview_node_get_text(void *node);

/// @brief Replace a tree node's display text.
/// @details Visible text accepts arbitrary runtime bytes; embedded NUL bytes are rendered as
///          U+FFFD rather than truncating the suffix. Conversion and lower-layer replacement are
///          allocation-atomic, so failure preserves the prior text. Stale handles are ignored.
/// @param node Tree node handle.
/// @param text New display text.
void rt_treeview_node_set_text(void *node, rt_string text);

/// @brief Set UTF-8 icon text rendered before a tree node label.
/// @details The complete string is copied and rendered through the TreeView font; empty text
///          clears the icon. Embedded NUL bytes become U+FFFD. The previous icon remains installed
///          if conversion or allocation fails, and stale handles are ignored.
/// @param node Tree node handle.
/// @param icon UTF-8 icon glyph or short icon text.
void rt_treeview_node_set_icon(void *node, rt_string icon);

/// @brief Return a tree node's UTF-8 icon text.
/// @param node Tree node handle.
/// @return Fresh runtime string, or the empty string for no icon or an invalid handle.
rt_string rt_treeview_node_get_icon(void *node);

/// @brief Advertise whether a node has materialized or lazily supplied children.
/// @details Setting false cannot hide children that already exist. A true value displays the
///          expansion affordance even before children are added. Stale handles are ignored.
/// @param node Tree node handle.
/// @param has_children Non-zero to advertise children.
void rt_treeview_node_set_has_children(void *node, int64_t has_children);

/// @brief Return whether a tree node has real or advertised lazy children.
/// @param node Tree node handle.
/// @return 1 when children are available or advertised, otherwise 0.
int64_t rt_treeview_node_has_children(void *node);

/// @brief Set or clear a tree node's loading indicator.
/// @details Clearing loading after an asynchronous population allows a future expansion to request
///          children again if none were added. Stale handles are ignored.
/// @param node Tree node handle.
/// @param loading Non-zero to show the loading indicator.
void rt_treeview_node_set_loading(void *node, int64_t loading);

/// @brief Return whether a tree node is displaying its loading indicator.
/// @param node Tree node handle.
/// @return 1 while loading, otherwise 0.
int64_t rt_treeview_node_is_loading(void *node);

/// @brief Assign an application-stable identifier to a tree node.
/// @details Identifiers reject embedded NUL bytes and are copied atomically. Empty IDs are allowed;
///          uniqueness is enforced by virtual-tree model adapters rather than the base TreeView.
///          Invalid input or stale handles leave the previous ID unchanged.
/// @param node Tree node handle.
/// @param stable_id Identifier to copy.
void rt_treeview_node_set_stable_id(void *node, rt_string stable_id);

/// @brief Return a tree node's application-stable identifier.
/// @param node Tree node handle.
/// @return Fresh runtime string, or empty for an unset ID or invalid handle.
rt_string rt_treeview_node_get_stable_id(void *node);

/// @brief Store user data (file path) in a tree node.
/// @details Runtime strings are stored with their explicit length, so embedded
///          NUL bytes round-trip through GetData.
/// @param node Tree node handle.
/// @param data String data to store.
void rt_treeview_node_set_data(void *node, rt_string data);

/// @brief Get user data stored in a tree node.
/// @param node Tree node handle.
/// @return Stored string data.
rt_string rt_treeview_node_get_data(void *node);

/// @brief Check if a tree node is expanded.
/// @param node Tree node handle.
/// @return 1 if expanded, 0 otherwise.
int64_t rt_treeview_node_is_expanded(void *node);

//=========================================================================
// TabBar Widget
//=========================================================================

/// @brief Create a new tab bar widget.
/// @param parent Parent widget (can be NULL).
/// @return TabBar widget handle.
void *rt_tabbar_new(void *parent);

/// @brief Add a tab to the tab bar.
/// @param tabbar TabBar widget handle.
/// @param title Tab title. Embedded NUL bytes are rendered as U+FFFD.
/// @param closable 1 if tab can be closed, 0 otherwise.
/// @return Tab handle.
void *rt_tabbar_add_tab(void *tabbar, rt_string title, int64_t closable);

/// @brief Remove a tab from the tab bar.
/// @param tabbar TabBar widget handle.
/// @param tab Tab handle.
void rt_tabbar_remove_tab(void *tabbar, void *tab);

/// @brief Free retired tab tombstones after stale tab handles are discarded.
/// @param tabbar TabBar widget handle.
void rt_tabbar_prune_retired_tabs(void *tabbar);

/// @brief Set the active tab.
/// @param tabbar TabBar widget handle.
/// @param tab Tab handle.
void rt_tabbar_set_active(void *tabbar, void *tab);

/// @brief Set tab title.
/// @param tab Tab handle.
/// @param title New title. Embedded NUL bytes are rendered as U+FFFD.
void rt_tab_set_title(void *tab, rt_string title);

/// @brief Return a tab's display title.
/// @param tab Managed Tab handle.
/// @return Fresh runtime string, or empty for an invalid or retired tab.
rt_string rt_tab_get_title(void *tab);

/// @brief Attach byte-exact runtime string data to a tab.
/// @details The value is copied before the old payload is released, so allocation failure preserves
///          prior data. Embedded NUL bytes round-trip through `GetData`; NULL clears the value.
/// @param tab Managed Tab handle.
/// @param data Runtime string to copy.
void rt_tab_set_data(void *tab, rt_string data);

/// @brief Return byte-exact runtime string data attached to a tab.
/// @param tab Managed Tab handle.
/// @return Fresh runtime string, or empty when unset, invalid, or retired.
rt_string rt_tab_get_data(void *tab);

/// @brief Set whether a tab displays and accepts its close affordance.
/// @param tab Managed Tab handle.
/// @param closable Non-zero to make the tab closable.
void rt_tab_set_closable(void *tab, int64_t closable);

/// @brief Return whether a tab is closable.
/// @param tab Managed Tab handle.
/// @return 1 when closable, otherwise 0.
int64_t rt_tab_is_closable(void *tab);

/// @brief Set an application-stable tab identifier.
/// @details IDs are copied atomically and reject embedded NUL bytes without changing the previous
///          value. Empty strings clear the ID; uniqueness is an application/model responsibility.
/// @param tab Managed Tab handle.
/// @param stable_id Identifier to copy.
void rt_tab_set_stable_id(void *tab, rt_string stable_id);

/// @brief Return a tab's application-stable identifier.
/// @param tab Managed Tab handle.
/// @return Fresh runtime string, or empty when unset, invalid, or retired.
rt_string rt_tab_get_stable_id(void *tab);

/// @brief Set tab tooltip.
/// @param tab Tab handle.
/// @param tooltip New tooltip text. Embedded NUL bytes are rendered as U+FFFD.
void rt_tab_set_tooltip(void *tab, rt_string tooltip);

/// @brief Set tab modified state.
/// @param tab Tab handle.
/// @param modified 1 for modified, 0 for not modified.
void rt_tab_set_modified(void *tab, int64_t modified);

/// @brief Attach or clear a leading built-in vector icon on a tab (ADR 0220).
/// @param tab Tab handle.
/// @param icon_name Stable vg_icon_vector name; empty or unknown clears it.
void rt_tab_set_named_icon(void *tab, rt_string icon_name);

/// @brief Get the active tab.
/// @param tabbar TabBar widget handle.
/// @return Active tab handle, or NULL if none.
void *rt_tabbar_get_active(void *tabbar);

/// @brief Get the index of the active tab.
/// @param tabbar TabBar widget handle.
/// @return 0-based index of active tab, or -1 if none.
int64_t rt_tabbar_get_active_index(void *tabbar);

/// @brief Check if the active tab changed since last call.
/// @param tabbar TabBar widget handle.
/// @return 1 if changed, 0 otherwise. Consumes the change flag.
int64_t rt_tabbar_was_changed(void *tabbar);

/// @brief Return the TabBar's non-consuming state revision.
/// @details Active-tab changes and structural tab mutations advance this value. Reading legacy
///          `WasChanged` or close edges never changes it.
/// @param tabbar TabBar widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_tabbar_get_revision(void *tabbar);

/// @brief Set the font used to render all tab titles.
/// @details @p size is a logical point size and is converted exactly once through the TabBar's
///          effective app scale. Invalid/stale font and TabBar handles leave state unchanged.
/// @param tabbar TabBar widget handle.
/// @param font Live Font handle.
/// @param size Logical point size, clamped to the supported font range.
void rt_tabbar_set_font(void *tabbar, void *font, double size);

/// @brief Consume the TabBar's independent successful-reorder edge.
/// @details The edge is separate from active-tab and close events. Reading it does not clear the
///          last source/destination payload or the non-consuming revision; multiple unobserved
///          reorders coalesce.
/// @param tabbar TabBar widget handle.
/// @return 1 once after one or more unreported reorders, otherwise 0.
int64_t rt_tabbar_was_reordered(void *tabbar);

/// @brief Return the source index from the most recent successful tab reorder.
/// @param tabbar TabBar widget handle.
/// @return Zero-based source index, or -1 when none/invalid.
int64_t rt_tabbar_get_reordered_from(void *tabbar);

/// @brief Return the destination index from the most recent successful tab reorder.
/// @param tabbar TabBar widget handle.
/// @return Zero-based destination index, or -1 when none/invalid.
int64_t rt_tabbar_get_reordered_to(void *tabbar);

/// @brief Move a tab between zero-based indices.
/// @details Both indices must exist and differ. Success commits linked-list order, advances the
///          TabBar revision, records `WasReordered` and its payload, keeps the moved tab visible,
///          and invokes the lower callback after state is committed.
/// @param tabbar TabBar widget handle.
/// @param from_index Current zero-based index.
/// @param to_index Destination zero-based index in the final order.
/// @return 1 only when the order changed, otherwise 0.
int64_t rt_tabbar_move_tab(void *tabbar, int64_t from_index, int64_t to_index);

/// @brief Get the number of tabs.
/// @param tabbar TabBar widget handle.
/// @return Number of tabs.
int64_t rt_tabbar_get_tab_count(void *tabbar);

/// @brief Check if a tab close button was clicked since the last close check.
/// @param tabbar TabBar widget handle.
/// @return 1 if a new close event occurred, 0 otherwise. Consumes the edge flag.
int64_t rt_tabbar_was_close_clicked(void *tabbar);

/// @brief Get the index of the tab whose close button was clicked.
/// @param tabbar TabBar widget handle.
/// @return 0-based index, or -1 if none. Consumes the close event.
int64_t rt_tabbar_get_close_clicked_index(void *tabbar);

/// @brief Get a tab by index.
/// @param tabbar TabBar widget handle.
/// @param index 0-based tab index.
/// @return Tab handle, or NULL if out of bounds.
void *rt_tabbar_get_tab_at(void *tabbar, int64_t index);

/// @brief Get the index of the tab under canvas coordinates (x, y).
/// @param tabbar TabBar widget handle.
/// @param x Canvas-pixel X.
/// @param y Canvas-pixel Y.
/// @return 0-based tab index, or -1 if no tab is at the point.
int64_t rt_tabbar_get_tab_index_at(void *tabbar, int64_t x, int64_t y);

/// @brief Set whether tabs auto-close when close button is clicked.
/// @param tabbar TabBar widget handle.
/// @param auto_close 1 for auto-close (default), 0 to let Zia code handle removal.
void rt_tabbar_set_auto_close(void *tabbar, int64_t auto_close);

//=========================================================================
// SplitPane Widget
//=========================================================================

/// @brief Create a new split pane widget.
/// @param parent Parent widget (can be NULL).
/// @param horizontal 1 for horizontal split (left/right), 0 for vertical (top/bottom).
/// @return SplitPane widget handle.
void *rt_splitpane_new(void *parent, int64_t horizontal);

/// @brief Set split position.
/// @param split SplitPane widget handle.
/// @param position Split position (0.0 to 1.0).
void rt_splitpane_set_position(void *split, double position);

/// @brief Get the current divider position of a split pane.
/// @param split SplitPane widget handle.
/// @return Split position as a fraction in the range 0.0 to 1.0.
double rt_splitpane_get_position(void *split);

/// @brief Set the first pane's minimum size in logical UI units.
/// @details The value is DPI-scaled exactly once; negative and non-finite values become zero.
///          An explicit first-pane collapse temporarily overrides this constraint.
/// @param split SplitPane widget handle; invalid handles are ignored.
/// @param size Minimum logical width (horizontal) or height (vertical).
void rt_splitpane_set_min_first(void *split, double size);

/// @brief Set the second pane's minimum size in logical UI units.
/// @details The value is DPI-scaled exactly once; negative and non-finite values become zero.
///          An explicit second-pane collapse temporarily overrides this constraint.
/// @param split SplitPane widget handle; invalid handles are ignored.
/// @param size Minimum logical width (horizontal) or height (vertical).
void rt_splitpane_set_min_second(void *split, double size);

/// @brief Return the first pane's configured minimum in logical UI units.
/// @param split SplitPane widget handle.
/// @return Non-negative logical size, or zero for an invalid handle.
double rt_splitpane_get_min_first(void *split);

/// @brief Return the second pane's configured minimum in logical UI units.
/// @param split SplitPane widget handle.
/// @return Non-negative logical size, or zero for an invalid handle.
double rt_splitpane_get_min_second(void *split);

/// @brief Return the split pane orientation.
/// @param split SplitPane widget handle.
/// @return `0` for horizontal left/right, `1` for vertical top/bottom, or `-1` if invalid.
int64_t rt_splitpane_get_orientation(void *split);

/// @brief Collapse the first (left or top) pane.
/// @details The divider fraction before the first collapse is retained for Restore. Repeating the
///          same collapse is a no-op.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_collapse_first(void *split);

/// @brief Collapse the second (right or bottom) pane.
/// @details The divider fraction before the first collapse is retained for Restore. Repeating the
///          same collapse is a no-op.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_collapse_second(void *split);

/// @brief Restore both panes after an explicit collapse.
/// @details Restores the divider fraction captured before the current collapse sequence. Calling
///          Restore while neither pane is collapsed is a no-op.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_restore(void *split);

/// @brief Return the explicitly collapsed side.
/// @param split SplitPane widget handle.
/// @return `0` for neither, `1` for first, `2` for second, or `-1` if invalid.
int64_t rt_splitpane_get_collapsed_side(void *split);

/// @brief Get the first pane.
/// @param split SplitPane widget handle.
/// @return First pane widget handle.
void *rt_splitpane_get_first(void *split);

/// @brief Get the second pane.
/// @param split SplitPane widget handle.
/// @return Second pane widget handle.
void *rt_splitpane_get_second(void *split);

//=========================================================================
// CodeEditor Widget
//=========================================================================

/// @brief Create a new code editor widget.
/// @param parent Parent widget (can be NULL).
/// @return CodeEditor widget handle.
void *rt_codeeditor_new(void *parent);

/// @brief Set code editor text content.
/// @param editor CodeEditor widget handle.
/// @param text New text content. Trailing newlines are preserved; embedded NUL
///             bytes are rendered as U+FFFD.
void rt_codeeditor_set_text(void *editor, rt_string text);

/// @brief Replace complete text as one undoable edit while retaining editor state.
/// @param editor CodeEditor widget handle.
/// @param text New complete text.
/// @return 1 when applied or already equal; otherwise 0.
int64_t rt_codeeditor_replace_all_text(void *editor, rt_string text);

/// @brief Get code editor text content.
/// @param editor CodeEditor widget handle.
/// @return Text content as runtime string, including trailing newlines.
rt_string rt_codeeditor_get_text(void *editor);

/// @brief Get the current content revision. Cursor and scroll changes do not change it.
/// @param editor CodeEditor widget handle.
/// @return Monotonic content revision, or 0 when unavailable.
int64_t rt_codeeditor_get_revision(void *editor);

/// @brief Serialize buffered edit deltas after @p since_revision as compact JSON
///        for incremental language-service sync; "overflow" means full-sync.
/// @param editor CodeEditor widget handle.
/// @param since_revision Last content revision already held by the consumer.
/// @return Owned compact JSON describing subsequent deltas or an overflow marker.
rt_string rt_codeeditor_take_deltas(void *editor, int64_t since_revision);

/// @brief Get the currently selected text.
/// @param editor CodeEditor widget handle.
/// @return Selected text, or empty string if no selection.
rt_string rt_codeeditor_get_selected_text(void *editor);

/// @brief Set cursor position.
/// @param editor CodeEditor widget handle.
/// @param line Line number (0-based).
/// @param col Column number (0-based).
void rt_codeeditor_set_cursor(void *editor, int64_t line, int64_t col);

/// @brief Scroll to a specific line.
/// @param editor CodeEditor widget handle.
/// @param line Line number (0-based).
void rt_codeeditor_scroll_to_line(void *editor, int64_t line);

/// @brief Get the zero-based source line nearest the top of the editor viewport.
/// @param editor CodeEditor widget handle.
/// @return Zero-based line index, or 0 when unavailable.
int64_t rt_codeeditor_get_scroll_top_line(void *editor);

/// @brief Set the source line nearest the top of the editor viewport.
/// @param editor CodeEditor widget handle.
/// @param line Zero-based line index.
void rt_codeeditor_set_scroll_top_line(void *editor, int64_t line);

/// @brief Get line count.
/// @param editor CodeEditor widget handle.
/// @return Number of lines.
int64_t rt_codeeditor_get_line_count(void *editor);

/// @brief Check if editor content is modified.
/// @param editor CodeEditor widget handle.
/// @return 1 if modified, 0 if not.
int64_t rt_codeeditor_is_modified(void *editor);

/// @brief Clear modified flag.
/// @param editor CodeEditor widget handle.
void rt_codeeditor_clear_modified(void *editor);

/// @brief Set code editor font.
/// @param editor CodeEditor widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_codeeditor_set_font(void *editor, void *font, double size);

/// @brief Get the current font size of a code editor.
/// @param editor CodeEditor widget handle.
/// @return Current font size in pixels, or 0 when unavailable.
double rt_codeeditor_get_font_size(void *editor);

/// @brief Set only the font size of a code editor (font unchanged).
/// @param editor CodeEditor widget handle.
/// @param size Font size in pixels.
void rt_codeeditor_set_font_size(void *editor, double size);

//=========================================================================
// Dropdown Widget
//=========================================================================

/// @brief Create a new dropdown widget.
/// @param parent Parent widget (can be NULL).
/// @return Dropdown widget handle.
void *rt_dropdown_new(void *parent);

/// @brief Add an item to the dropdown.
/// @param dropdown Dropdown widget handle.
/// @param text Item text. Embedded NUL bytes are rendered as U+FFFD.
/// @return Index of the added item, or -1 on allocation/capacity failure.
int64_t rt_dropdown_add_item(void *dropdown, rt_string text);

/// @brief Remove an item from the dropdown.
/// @param dropdown Dropdown widget handle.
/// @param index Item index.
void rt_dropdown_remove_item(void *dropdown, int64_t index);

/// @brief Clear all items from the dropdown.
/// @param dropdown Dropdown widget handle.
void rt_dropdown_clear(void *dropdown);

/// @brief Set selected item.
/// @param dropdown Dropdown widget handle.
/// @param index Item index (-1 for none).
void rt_dropdown_set_selected(void *dropdown, int64_t index);

/// @brief Get selected item index.
/// @param dropdown Dropdown widget handle.
/// @return Selected index, or -1 if none.
int64_t rt_dropdown_get_selected(void *dropdown);

/// @brief Get selected item text.
/// @param dropdown Dropdown widget handle.
/// @return Selected text, or empty string if none.
rt_string rt_dropdown_get_selected_text(void *dropdown);

/// @brief Set dropdown placeholder text.
/// @param dropdown Dropdown widget handle.
/// @param placeholder Placeholder text. Embedded NUL bytes are rendered as U+FFFD.
void rt_dropdown_set_placeholder(void *dropdown, rt_string placeholder);

/// @brief Consume the dropdown's independent selected-item change edge.
/// @details Programmatic, pointer, and keyboard selection changes use one path. Structural item
///          edits that preserve selection only advance the general revision.
/// @param dropdown Dropdown widget handle.
/// @return 1 when an unreported selection change exists, otherwise 0.
int64_t rt_dropdown_was_changed(void *dropdown);

/// @brief Return the dropdown's non-consuming state revision.
/// @param dropdown Dropdown widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_dropdown_get_revision(void *dropdown);

//=========================================================================
// Slider Widget
//=========================================================================

/// @brief Create a new slider widget.
/// @param parent Parent widget (can be NULL).
/// @param horizontal 1 for horizontal, 0 for vertical.
/// @return Slider widget handle.
void *rt_slider_new(void *parent, int64_t horizontal);

/// @brief Set slider value.
/// @param slider Slider widget handle.
/// @param value Slider value.
void rt_slider_set_value(void *slider, double value);

/// @brief Get slider value.
/// @param slider Slider widget handle.
/// @return Current value.
double rt_slider_get_value(void *slider);

/// @brief Set slider range.
/// @param slider Slider widget handle.
/// @param min_val Minimum value.
/// @param max_val Maximum value.
void rt_slider_set_range(void *slider, double min_val, double max_val);

/// @brief Set slider step.
/// @param slider Slider widget handle.
/// @param step Step value (0 for continuous).
void rt_slider_set_step(void *slider, double step);

/// @brief Consume the slider's independent numeric-value change edge.
/// @details Value changes produced by pointer, keyboard, range clamping, and programmatic setters
///          share the edge. Range/step-only configuration changes affect revision but not this
///          edge.
/// @param slider Slider widget handle.
/// @return 1 when an unreported value change exists, otherwise 0.
int64_t rt_slider_was_changed(void *slider);

/// @brief Return the slider's non-consuming state revision.
/// @param slider Slider widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_slider_get_revision(void *slider);

//=========================================================================
// ProgressBar Widget
//=========================================================================

/// @brief Create a new progress bar widget.
/// @param parent Parent widget (can be NULL).
/// @return ProgressBar widget handle.
void *rt_progressbar_new(void *parent);

/// @brief Set progress bar value.
/// @param progress ProgressBar widget handle.
/// @param value Progress value (0.0 to 1.0).
void rt_progressbar_set_value(void *progress, double value);

/// @brief Get progress bar value.
/// @param progress ProgressBar widget handle.
/// @return Current value (0.0 to 1.0).
double rt_progressbar_get_value(void *progress);

/// @brief Set progress bar style: 0=bar, 1=circular, 2=indeterminate.
/// @param progress ProgressBar widget handle.
/// @param style Style value: 0 for bar, 1 for circular, or 2 for indeterminate.
void rt_progressbar_set_style(void *progress, int64_t style);

/// @brief Enable or disable percentage text.
/// @param progress ProgressBar widget handle.
/// @param show Non-zero to display percentage text; zero to hide it.
void rt_progressbar_show_percentage(void *progress, int64_t show);

//=========================================================================
// ListBox Widget
//=========================================================================

/// @brief Create a new list box widget.
/// @param parent Parent widget (can be NULL).
/// @return ListBox widget handle.
void *rt_listbox_new(void *parent);

/// @brief Add an item to the list box.
/// @param listbox ListBox widget handle.
/// @param text Item text. Embedded NUL bytes are rendered as U+FFFD.
/// @return Item handle.
void *rt_listbox_add_item(void *listbox, rt_string text);

/// @brief Remove an item from the list box.
/// @param listbox ListBox widget handle.
/// @param item Item handle.
void rt_listbox_remove_item(void *listbox, void *item);

/// @brief Clear all items from the list box.
/// @param listbox ListBox widget handle.
void rt_listbox_clear(void *listbox);

/// @brief Select an item.
/// @param listbox ListBox widget handle.
/// @param item Item handle (NULL to deselect).
void rt_listbox_select(void *listbox, void *item);

/// @brief Get selected item.
/// @param listbox ListBox widget handle.
/// @return Selected item handle, or NULL if none.
void *rt_listbox_get_selected(void *listbox);

/// @brief Get the number of items in the list box.
/// @param listbox ListBox widget handle.
/// @return Number of items.
int64_t rt_listbox_get_count(void *listbox);

/// @brief Get the selected item index.
/// @param listbox ListBox widget handle.
/// @return Selected index, or -1 if none selected.
int64_t rt_listbox_get_selected_index(void *listbox);

/// @brief Select an item by index.
/// @param listbox ListBox widget handle.
/// @param index Item index (0-based).
void rt_listbox_select_index(void *listbox, int64_t index);

/// @brief Scroll to the first row without changing selection.
/// @param listbox ListBox widget handle.
void rt_listbox_scroll_to_top(void *listbox);

/// @brief Scroll to the last row without changing selection.
/// @param listbox ListBox widget handle.
void rt_listbox_scroll_to_bottom(void *listbox);

/// @brief Enable or disable multi-row selection for a list box.
/// @param listbox ListBox widget handle.
/// @param enabled Non-zero to allow Ctrl/Shift range selection.
void rt_listbox_set_multi_select(void *listbox, int64_t enabled);

/// @brief Enable or disable application-directed retained-row reordering.
/// @details The ListBox latches source/final-target requests but never mutates item linkage.
/// @param listbox ListBox widget handle.
/// @param enabled Non-zero to enable pointer and Alt+Arrow reorder requests.
void rt_listbox_set_reorderable(void *listbox, int64_t enabled);

/// @brief Consume one pending retained-row reorder request edge.
/// @param listbox ListBox widget handle.
/// @return 1 once after a valid non-no-op request, otherwise 0.
int64_t rt_listbox_was_reorder_requested(void *listbox);

/// @brief Return the source index from the most recently latched reorder request.
/// @param listbox ListBox widget handle.
/// @return Zero-based source index, or -1 when unavailable.
int64_t rt_listbox_get_reorder_source_index(void *listbox);

/// @brief Return the final target index from the most recently latched reorder request.
/// @param listbox ListBox widget handle.
/// @return Zero-based target index after source removal, or -1 when unavailable.
int64_t rt_listbox_get_reorder_target_index(void *listbox);

/// @brief Return selected row text joined by newlines.
/// @param listbox ListBox widget handle.
/// @return Newline-delimited selected row text, or empty when nothing is selected.
rt_string rt_listbox_get_selected_text(void *listbox);

/// @brief Return data attached to all selected retained rows.
/// @details Values are byte-exact copies in current row order. A selected row without data
///          contributes an empty string. Virtual-mode lists return an empty sequence because
///          their rows are owned by the external model.
/// @param listbox ListBox widget handle.
/// @return Newly allocated owned Seq of strings; empty for no selection or an invalid handle.
void *rt_listbox_get_selected_data(void *listbox);

/// @brief Check if selection changed since last check (polling pattern).
/// @param listbox ListBox widget handle.
/// @return 1 if selection changed, 0 otherwise.
int64_t rt_listbox_was_selection_changed(void *listbox);

/// @brief Consume the ListBox's common selection-change edge.
/// @details This edge is independent of legacy `WasSelectionChanged`; consuming either leaves the
///          other and the non-consuming revision intact.
/// @param listbox ListBox widget handle.
/// @return 1 when an unreported selection change exists, otherwise 0.
int64_t rt_listbox_was_changed(void *listbox);

/// @brief Consume a ListBox row activation edge.
/// @details Double-click and Enter on a selected row record activation even when no callback is
///          installed. The edge is independent from selection changes.
/// @param listbox ListBox widget handle.
/// @return 1 when an unreported activation exists, otherwise 0.
int64_t rt_listbox_was_activated(void *listbox);

/// @brief Return the ListBox's non-consuming state revision.
/// @param listbox ListBox widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_listbox_get_revision(void *listbox);

/// @brief Get the text of a list box item.
/// @param item ListBox item handle.
/// @return Item text as runtime string using the stored GUI text length.
rt_string rt_listbox_item_get_text(void *item);

/// @brief Set the text of a list box item.
/// @param item ListBox item handle.
/// @param text New text. Embedded NUL bytes are rendered as U+FFFD.
void rt_listbox_item_set_text(void *item, rt_string text);

/// @brief Store user data in a list box item.
/// @details Runtime strings are stored with their explicit length, so embedded
///          NUL bytes round-trip through ItemGetData.
/// @param item ListBox item handle.
/// @param data String data to store.
void rt_listbox_item_set_data(void *item, rt_string data);

/// @brief Attach or clear a leading built-in vector icon on a listbox item (ADR 0220).
/// @param item ListBox item subhandle.
/// @param icon_name Stable vg_icon_vector name; empty or unknown clears it.
void rt_listbox_item_set_named_icon(void *item, rt_string icon_name);

/// @brief Set a custom text color for a list box item.
/// @param item ListBox item handle.
/// @param color RGB/RGBA color value interpreted by the active backend.
void rt_listbox_item_set_text_color(void *item, int64_t color);

/// @brief Get user data stored in a list box item.
/// @param item ListBox item handle.
/// @return Stored string data.
rt_string rt_listbox_item_get_data(void *item);

/// @brief Set list box font.
/// @param listbox ListBox widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_listbox_set_font(void *listbox, void *font, double size);

//=========================================================================
// Color Controls
//=========================================================================

/// @brief Create a focusable swatch that displays one opaque RGB color.
/// @details Public colors use `0x00RRGGBB`; any upper bits in @p color are ignored and the lower
///          toolkit stores the swatch as fully opaque. The returned widget is owned by @p parent
///          when a parent is supplied, otherwise by the caller. Construction is restricted to the
///          GUI main thread.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @param color Initial public RGB color in the low 24 bits.
/// @return Live ColorSwatch handle, or NULL on invalid parent, allocation failure, or a
///         graphics-disabled build.
void *rt_colorswatch_new(void *parent, int64_t color);

/// @brief Set the opaque RGB color displayed by a ColorSwatch.
/// @details Upper bits are ignored. An effective color change records the swatch change edge and
///          advances its non-consuming revision; assigning the current value is a no-op.
/// @param swatch Live ColorSwatch handle; invalid or stale handles are ignored.
/// @param color Public `0x00RRGGBB` color in the low 24 bits.
void rt_colorswatch_set_color(void *swatch, int64_t color);

/// @brief Return the public RGB color displayed by a ColorSwatch.
/// @param swatch Live ColorSwatch handle.
/// @return `0x00RRGGBB`, or zero for an invalid/stale handle.
int64_t rt_colorswatch_get_color(void *swatch);

/// @brief Set whether a ColorSwatch is visually and semantically selected.
/// @details An effective selection transition shares the swatch change edge with color changes and
///          advances the revision. Repeating the current state is a no-op.
/// @param swatch Live ColorSwatch handle; invalid or stale handles are ignored.
/// @param selected Non-zero selects the swatch; zero clears selection.
void rt_colorswatch_set_selected(void *swatch, int64_t selected);

/// @brief Query whether a ColorSwatch is selected.
/// @param swatch Live ColorSwatch handle.
/// @return 1 when selected, otherwise 0, including for invalid/stale handles.
int64_t rt_colorswatch_is_selected(void *swatch);

/// @brief Consume the ColorSwatch color-or-selection change edge.
/// @details This edge is independent from the non-consuming revision and coalesces multiple
///          unreported effective transitions into one result.
/// @param swatch Live ColorSwatch handle.
/// @return 1 once after one or more unreported changes, otherwise 0.
int64_t rt_colorswatch_was_changed(void *swatch);

/// @brief Return the ColorSwatch non-consuming state revision.
/// @param swatch Live ColorSwatch handle.
/// @return Monotonic signed revision saturated at `INT64_MAX`, or zero for an invalid handle.
int64_t rt_colorswatch_get_revision(void *swatch);

/// @brief Create an empty keyboard-navigable RGB color palette.
/// @details Added colors are copied as opaque `0x00RRGGBB` values. The returned widget is owned by
///          @p parent when supplied, otherwise by the caller. Construction is restricted to the
///          GUI main thread.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @return Live ColorPalette handle, or NULL on invalid parent, allocation failure, or a
///         graphics-disabled build.
void *rt_colorpalette_new(void *parent);

/// @brief Append one opaque RGB entry to a ColorPalette.
/// @details Upper bits are ignored. Allocation failure preserves all existing entries. A
///          successful append records the palette change edge and advances its revision.
/// @param palette Live ColorPalette handle; invalid or stale handles are ignored.
/// @param color Public `0x00RRGGBB` color in the low 24 bits.
void rt_colorpalette_add_color(void *palette, int64_t color);

/// @brief Remove a ColorPalette entry by zero-based index.
/// @details Later entries shift left. A selected surviving color follows its new index; removing
///          the selected color clears selection. Invalid indices preserve the palette.
/// @param palette Live ColorPalette handle.
/// @param index Zero-based entry index.
/// @return 1 when an entry was removed, otherwise 0 for invalid input or handles.
int64_t rt_colorpalette_remove_color(void *palette, int64_t index);

/// @brief Remove every ColorPalette entry and clear its selection.
/// @details Clearing an already empty unselected palette is a no-op. A real clear records the
///          palette change edge and advances its revision.
/// @param palette Live ColorPalette handle; invalid or stale handles are ignored.
void rt_colorpalette_clear(void *palette);

/// @brief Return the number of RGB entries in a ColorPalette.
/// @param palette Live ColorPalette handle.
/// @return Non-negative entry count, or zero for an invalid/stale handle.
int64_t rt_colorpalette_get_color_count(void *palette);

/// @brief Read one ColorPalette RGB entry by zero-based index.
/// @param palette Live ColorPalette handle.
/// @param index Zero-based entry index.
/// @return Public `0x00RRGGBB` color, or zero when the handle/index is invalid. Because black is
///         also zero, call GetColorCount before indexing when absence must be distinguished.
int64_t rt_colorpalette_get_color_at(void *palette, int64_t index);

/// @brief Select a ColorPalette entry by zero-based index.
/// @details Index -1 clears selection. Any other out-of-range value also clears selection for
///          compatibility with the lower toolkit. Effective transitions record the change edge
///          and advance the revision.
/// @param palette Live ColorPalette handle; invalid or stale handles are ignored.
/// @param index Zero-based index, or -1 for no selection.
void rt_colorpalette_set_selected_index(void *palette, int64_t index);

/// @brief Return the selected ColorPalette entry index.
/// @param palette Live ColorPalette handle.
/// @return Zero-based selected index, or -1 when absent or invalid.
int64_t rt_colorpalette_get_selected_index(void *palette);

/// @brief Consume the ColorPalette structural-or-selection change edge.
/// @details The edge is independent from the non-consuming revision and coalesces multiple
///          unreported effective transitions into one result.
/// @param palette Live ColorPalette handle.
/// @return 1 once after one or more unreported changes, otherwise 0.
int64_t rt_colorpalette_was_changed(void *palette);

/// @brief Return the ColorPalette non-consuming state revision.
/// @param palette Live ColorPalette handle.
/// @return Monotonic signed revision saturated at `INT64_MAX`, or zero for an invalid handle.
int64_t rt_colorpalette_get_revision(void *palette);

/// @brief Create an RGB picker with separate optional alpha-channel editing.
/// @details The picker starts at black with alpha 255, includes keyboard-editable R/G/B sliders,
///          and inherits the owning app's default font. The returned widget is owned by @p parent
///          when supplied, otherwise by the caller.
/// @param parent Live parent-container handle, or NULL for a detached widget.
/// @return Live ColorPicker handle, or NULL on invalid parent, allocation failure, or a
///         graphics-disabled build.
void *rt_colorpicker_new(void *parent);

/// @brief Set a ColorPicker RGB value without changing its separate alpha component.
/// @details Upper bits are ignored. An effective color change synchronizes sliders and preview,
///          records the change edge, and advances the revision.
/// @param picker Live ColorPicker handle; invalid or stale handles are ignored.
/// @param color Public `0x00RRGGBB` color in the low 24 bits.
void rt_colorpicker_set_color(void *picker, int64_t color);

/// @brief Return a ColorPicker public RGB value.
/// @param picker Live ColorPicker handle.
/// @return `0x00RRGGBB`, or zero for an invalid/stale handle.
int64_t rt_colorpicker_get_color(void *picker);

/// @brief Enable or disable ColorPicker alpha-channel editing.
/// @details This changes layout and revision without changing RGB, alpha, or the color-change edge.
/// @param picker Live ColorPicker handle; invalid or stale handles are ignored.
/// @param enabled Non-zero shows/enables alpha editing; zero hides it.
void rt_colorpicker_set_alpha_enabled(void *picker, int64_t enabled);

/// @brief Query whether ColorPicker alpha-channel editing is enabled.
/// @param picker Live ColorPicker handle.
/// @return 1 when enabled, otherwise 0, including for invalid/stale handles.
int64_t rt_colorpicker_is_alpha_enabled(void *picker);

/// @brief Return the ColorPicker red component.
/// @param picker Live ColorPicker handle.
/// @return Component in `[0,255]`, or zero for an invalid/stale handle.
int64_t rt_colorpicker_get_red(void *picker);

/// @brief Return the ColorPicker green component.
/// @param picker Live ColorPicker handle.
/// @return Component in `[0,255]`, or zero for an invalid/stale handle.
int64_t rt_colorpicker_get_green(void *picker);

/// @brief Return the ColorPicker blue component.
/// @param picker Live ColorPicker handle.
/// @return Component in `[0,255]`, or zero for an invalid/stale handle.
int64_t rt_colorpicker_get_blue(void *picker);

/// @brief Return the ColorPicker alpha component.
/// @param picker Live ColorPicker handle.
/// @return Component in `[0,255]`, or zero for an invalid/stale handle.
int64_t rt_colorpicker_get_alpha(void *picker);

/// @brief Consume the ColorPicker RGB-or-alpha value change edge.
/// @details Enabling alpha editing is configuration and does not set this edge. The edge is
///          independent from the non-consuming revision.
/// @param picker Live ColorPicker handle.
/// @return 1 once after one or more unreported component changes, otherwise 0.
int64_t rt_colorpicker_was_changed(void *picker);

/// @brief Return the ColorPicker non-consuming state revision.
/// @param picker Live ColorPicker handle.
/// @return Monotonic signed revision saturated at `INT64_MAX`, or zero for an invalid handle.
int64_t rt_colorpicker_get_revision(void *picker);

//=========================================================================
// OutputPane Widget
//=========================================================================

/// @brief Create a new append-only styled output pane.
/// @param parent Parent widget handle (can be NULL).
/// @return OutputPane widget handle.
void *rt_outputpane_new(void *parent);

/// @brief Append text, parsing ANSI SGR escape sequences.
/// @param pane OutputPane widget handle.
/// @param text Text to append.
void rt_outputpane_append(void *pane, rt_string text);

/// @brief Append text as a complete line.
/// @param pane OutputPane widget handle.
/// @param text Line text to append before the line terminator.
void rt_outputpane_append_line(void *pane, rt_string text);

/// @brief Append a single explicitly styled segment.
/// @param pane OutputPane widget handle.
/// @param text Segment text.
/// @param fg Foreground color value.
/// @param bg Background color value.
/// @param bold Non-zero to render the segment in bold.
void rt_outputpane_append_styled(void *pane, rt_string text, int64_t fg, int64_t bg, int64_t bold);

/// @brief Clear all retained output.
/// @param pane OutputPane widget handle.
void rt_outputpane_clear(void *pane);

/// @brief Scroll to the first output line and lock auto-scroll.
/// @param pane OutputPane widget handle.
void rt_outputpane_scroll_to_top(void *pane);

/// @brief Scroll to the latest output line and unlock auto-scroll.
/// @param pane OutputPane widget handle.
void rt_outputpane_scroll_to_bottom(void *pane);

/// @brief Enable or disable auto-scroll on append.
/// @param pane OutputPane widget handle.
/// @param enabled Non-zero to follow newly appended output.
void rt_outputpane_set_auto_scroll(void *pane, int64_t enabled);

/// @brief Return selected output text.
/// @param pane OutputPane widget handle.
/// @return Owned selected text, or an empty runtime string when no selection exists.
rt_string rt_outputpane_get_selection(void *pane);

/// @brief Select all retained output text.
/// @param pane OutputPane widget handle.
void rt_outputpane_select_all(void *pane);

/// @brief Set the retained line cap.
/// @param pane OutputPane widget handle.
/// @param max_lines Maximum retained line count.
void rt_outputpane_set_max_lines(void *pane, int64_t max_lines);

/// @brief Return the retained output line count.
/// @param pane OutputPane widget handle.
/// @return Number of currently retained lines.
int64_t rt_outputpane_get_line_count(void *pane);

/// @brief Set output pane font.
/// @param pane OutputPane widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_outputpane_set_font(void *pane, void *font, double size);

/// @brief Pixel advance of one monospace character cell ("M"); 0 when no font/invalid.
/// @param pane OutputPane widget handle.
/// @return Character-cell advance in pixels, or 0 when unavailable.
int64_t rt_outputpane_get_cell_width(void *pane);

/// @brief Pixel height of one line; 0 when no font/invalid.
/// @param pane OutputPane widget handle.
/// @return Line height in pixels, or 0 when unavailable.
int64_t rt_outputpane_get_cell_height(void *pane);

/// @brief Pixel width of @p text in the pane's font; 0 when no font/empty/invalid.
/// @param pane OutputPane widget handle.
/// @param text Text to measure.
/// @return Measured width in pixels, or 0 when unavailable.
int64_t rt_outputpane_measure_text(void *pane, rt_string text);

/// @brief Whole character columns that fit across the pane's width; 0 when no font/invalid.
/// @param pane OutputPane widget handle.
/// @return Number of complete character columns that fit, or 0 when unavailable.
int64_t rt_outputpane_columns_for_width(void *pane);

/// @brief Whole rows that fit down the pane's height; 0 when no font/invalid.
/// @param pane OutputPane widget handle.
/// @return Number of complete rows that fit, or 0 when unavailable.
int64_t rt_outputpane_rows_for_height(void *pane);

/// @brief Enable/disable interactive terminal mode (cursor model + keyboard capture).
/// @param pane OutputPane widget handle.
/// @param enabled Non-zero to enable terminal input handling.
void rt_outputpane_set_terminal_mode(void *pane, int64_t enabled);

/// @brief Drain queued terminal keystroke bytes (terminal mode).
/// @param pane OutputPane widget handle.
/// @return Owned string containing queued input bytes, or an empty string when none are queued.
rt_string rt_outputpane_take_input(void *pane);

//=========================================================================
// RadioButton Widget
//=========================================================================

/// @brief Create a new radio group.
/// @return RadioGroup handle.
void *rt_radiogroup_new(void);

/// @brief Destroy a radio group.
/// @param group RadioGroup handle.
void rt_radiogroup_destroy(void *group);

/// @brief Return the selected radio-group member index.
/// @param group RadioGroup handle.
/// @return Zero-based selected index, or `-1` when none is selected or the handle is invalid.
int64_t rt_radiogroup_get_selected_index(void *group);

/// @brief Attempt to update a radio group's selected member.
/// @details Index `-1` clears the selection. Other out-of-range values and invalid handles are
///          rejected without state changes. Selecting the current index succeeds as a no-op.
/// @param group RadioGroup handle.
/// @param index Zero-based member index or `-1`.
/// @return `1` for a valid request, otherwise `0`.
int64_t rt_radiogroup_set_selected_index(void *group, int64_t index);

/// @brief Return the number of live buttons registered with a radio group.
/// @param group RadioGroup handle.
/// @return Non-negative member count, or zero for an invalid handle.
int64_t rt_radiogroup_get_count(void *group);

/// @brief Consume the radio group's independent selected-index change edge.
/// @details Multiple unreported transitions coalesce. Reading the edge does not consume or change
///          GetRevision, and membership-only changes do not set the edge.
/// @param group RadioGroup handle.
/// @return `1` once after one or more unreported selected-index transitions, otherwise `0`.
int64_t rt_radiogroup_was_changed(void *group);

/// @brief Return the radio group's non-consuming state revision.
/// @details The revision advances for member registration/removal and selected-index changes and
///          saturates at `INT64_MAX`.
/// @param group RadioGroup handle.
/// @return Monotonic revision, or zero for an invalid handle.
int64_t rt_radiogroup_get_revision(void *group);

/// @brief Create a new radio button widget.
/// @param parent Parent widget (can be NULL).
/// @param text Radio button text.
/// @param group RadioGroup handle.
/// @return RadioButton widget handle.
void *rt_radiobutton_new(void *parent, rt_string text, void *group);

/// @brief Check if radio button is selected.
/// @param radio RadioButton widget handle.
/// @return 1 if selected, 0 otherwise.
int64_t rt_radiobutton_is_selected(void *radio);

/// @brief Set radio button selected state.
/// @param radio RadioButton widget handle.
/// @param selected 1 for selected, 0 for not selected.
void rt_radiobutton_set_selected(void *radio, int64_t selected);

/// @brief Replace a radio button's visible label text.
/// @details The runtime string is converted under the GUI UTF-8 policy and copied atomically;
///          allocation failure preserves the prior value.
/// @param radio RadioButton widget handle; invalid handles are ignored.
/// @param text New visible text.
void rt_radiobutton_set_text(void *radio, rt_string text);

/// @brief Return a radio button's visible label text.
/// @param radio RadioButton widget handle.
/// @return Fresh runtime string, or empty for an invalid handle.
rt_string rt_radiobutton_get_text(void *radio);

/// @brief Store byte-exact application data on a radio button.
/// @details The runtime copies and owns the payload. Embedded NUL bytes round-trip, NULL clears the
///          value, and allocation failure preserves the prior value.
/// @param radio RadioButton widget handle; invalid handles are ignored.
/// @param data Runtime string payload or NULL.
void rt_radiobutton_set_data(void *radio, rt_string data);

/// @brief Return byte-exact application data stored on a radio button.
/// @param radio RadioButton widget handle.
/// @return Fresh runtime string, or empty when absent or invalid.
rt_string rt_radiobutton_get_data(void *radio);

/// @brief Consume the radio button's independent selected-state change edge.
/// @details Group-driven deselection and direct selection both record changes before callbacks.
/// @param radio RadioButton widget handle.
/// @return 1 when an unreported selected-state transition exists, otherwise 0.
int64_t rt_radiobutton_was_changed(void *radio);

/// @brief Return the radio button's non-consuming state revision.
/// @param radio RadioButton widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_radiobutton_get_revision(void *radio);

//=========================================================================
// Spinner Widget
//=========================================================================

/// @brief Create a new spinner widget.
/// @param parent Parent widget (can be NULL).
/// @return Spinner widget handle.
void *rt_spinner_new(void *parent);

/// @brief Set spinner value.
/// @param spinner Spinner widget handle.
/// @param value Spinner value.
void rt_spinner_set_value(void *spinner, double value);

/// @brief Get spinner value.
/// @param spinner Spinner widget handle.
/// @return Current value.
double rt_spinner_get_value(void *spinner);

/// @brief Set whether a spinner represents a mixed group value.
/// @details The retained numeric value remains available as the editing seed.
///          Assigning a concrete value or beginning user input clears mixed.
/// @param spinner Spinner widget handle.
/// @param indeterminate Non-zero to present mixed; zero to reveal the value.
void rt_spinner_set_indeterminate(void *spinner, int64_t indeterminate);

/// @brief Query whether a spinner represents a mixed group value.
/// @param spinner Spinner widget handle.
/// @return 1 when mixed, otherwise zero.
int64_t rt_spinner_is_indeterminate(void *spinner);

/// @brief Set spinner range.
/// @param spinner Spinner widget handle.
/// @param min_val Minimum value.
/// @param max_val Maximum value.
void rt_spinner_set_range(void *spinner, double min_val, double max_val);

/// @brief Set spinner step.
/// @param spinner Spinner widget handle.
/// @param step Step value.
void rt_spinner_set_step(void *spinner, double step);

/// @brief Set spinner decimal places.
/// @param spinner Spinner widget handle.
/// @param decimals Number of decimal places.
void rt_spinner_set_decimals(void *spinner, int64_t decimals);

/// @brief Consume the spinner's independent committed-value change edge.
/// @details Arrow, wheel, keyboard, text-commit, range clamping, and programmatic value changes use
///          one edge. Formatting/range changes that preserve the value only advance revision.
/// @param spinner Spinner widget handle.
/// @return 1 when an unreported numeric-value change exists, otherwise 0.
int64_t rt_spinner_was_changed(void *spinner);

/// @brief Consume the spinner's independent text submission edge.
/// @details A valid Enter commit records submission whether or not the parsed value differs. The
///          change edge remains independently observable.
/// @param spinner Spinner widget handle.
/// @return 1 when an unreported valid Enter submission exists, otherwise 0.
int64_t rt_spinner_was_submitted(void *spinner);

/// @brief Consume the spinner's value-scrub completion edge (1 once per gesture).
/// @param spinner Spinner widget handle.
/// @return 1 once after an unreported scrub gesture completes, otherwise 0.
int64_t rt_spinner_was_scrub_finished(void *spinner);

/// @brief Return the spinner's non-consuming state revision.
/// @param spinner Spinner widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_spinner_get_revision(void *spinner);

//=========================================================================
// Grid Widget (interactive viewport-aware data table) — Zanna.GUI.Grid
//=========================================================================

/// @brief Create a tabular data grid attached to an optional parent.
/// @param parent Parent widget handle, or NULL for no parent.
/// @return DataGrid widget handle.
void *rt_datagrid_new(void *parent);
/// @brief Set the column count (clears existing headers and cells).
/// @param grid DataGrid widget handle.
/// @param count Non-negative column count.
void rt_datagrid_set_columns(void *grid, int64_t count);
/// @brief Set a column header.
/// @param grid DataGrid widget handle.
/// @param col Zero-based column index.
/// @param text Header text.
void rt_datagrid_set_header(void *grid, int64_t col, rt_string text);
/// @brief Set a cell's text, growing the row count as needed.
/// @param grid DataGrid widget handle.
/// @param row Zero-based row index.
/// @param col Zero-based column index.
/// @param text Cell text.
void rt_datagrid_set_cell(void *grid, int64_t row, int64_t col, rt_string text);
/// @brief Return a cell's text (empty string when out of range).
/// @param grid DataGrid widget handle.
/// @param row Zero-based row index.
/// @param col Zero-based column index.
/// @return Owned cell text, or an empty runtime string when out of range.
rt_string rt_datagrid_get_cell(void *grid, int64_t row, int64_t col);
/// @brief Remove all rows (columns and headers are kept).
/// @param grid DataGrid widget handle.
void rt_datagrid_clear(void *grid);
/// @brief Set the header/cell font.
/// @param grid DataGrid widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_datagrid_set_font(void *grid, void *font, double size);
/// @brief Auto-sized pixel width of a column; 0 when no font/out of range.
/// @param grid DataGrid widget handle.
/// @param col Zero-based column index.
/// @return Column width in pixels, or 0 when unavailable.
int64_t rt_datagrid_get_column_width(void *grid, int64_t col);
/// @brief Number of populated rows.
/// @param grid DataGrid widget handle.
/// @return Number of populated rows, or 0 for an invalid handle.
int64_t rt_datagrid_get_row_count(void *grid);
/// @brief Number of columns.
/// @param grid DataGrid widget handle.
/// @return Number of configured columns, or 0 for an invalid handle.
int64_t rt_datagrid_get_column_count(void *grid);

/// @brief Set the first and maximum number of logical rows in the Grid viewport.
/// @details A zero count derives capacity from arranged height. Invalid negative/non-representable
///          values preserve existing state, and no row-proportional storage is allocated.
/// @param grid Grid widget handle.
/// @param first Zero-based first logical row.
/// @param count Maximum visible rows, or zero for automatic capacity.
void rt_datagrid_set_viewport_rows(void *grid, int64_t first, int64_t count);

/// @brief Switch to sparse virtual mode and set the logical Grid row count.
/// @details Dense rows are cleared after validation; the count itself causes no per-row allocation.
/// @param grid Grid widget handle.
/// @param count Non-negative logical row count.
void rt_datagrid_set_virtual_row_count(void *grid, int64_t count);

/// @brief Materialize, replace, or clear one copied sparse virtual Grid cell.
/// @details Empty text removes materialization. Invalid indices and allocation failure are atomic.
/// @param grid Virtual Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @param text Visible UTF-8 text; embedded NUL bytes become U+FFFD.
void rt_datagrid_set_virtual_cell(void *grid, int64_t row, int64_t col, rt_string text);

/// @brief Enable or disable pointer/keyboard Grid selection.
/// @param grid Grid widget handle.
/// @param enabled Non-zero to enable; zero disables and clears selection.
void rt_datagrid_set_selectable(void *grid, int64_t enabled);

/// @brief Return the selected Grid row.
/// @param grid Grid widget handle.
/// @return Zero-based logical row, or -1 when absent/invalid.
int64_t rt_datagrid_get_selected_row(void *grid);

/// @brief Return the selected Grid column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when absent/invalid.
int64_t rt_datagrid_get_selected_column(void *grid);

/// @brief Select one valid Grid cell.
/// @param grid Selectable Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return 1 when valid and selected/already selected, otherwise 0.
int64_t rt_datagrid_select_cell(void *grid, int64_t row, int64_t col);

/// @brief Clear the current Grid selection.
/// @param grid Grid widget handle; invalid/already-clear handles are no-ops.
void rt_datagrid_clear_selection(void *grid);

/// @brief Consume the independent Grid selection transition edge.
/// @param grid Grid widget handle.
/// @return 1 once after unreported selection changes, otherwise 0.
int64_t rt_datagrid_was_selection_changed(void *grid);

/// @brief Consume the independent Grid cell-activation edge.
/// @param grid Grid widget handle.
/// @return 1 once after unreported double-click/Enter activation, otherwise 0.
int64_t rt_datagrid_was_activated(void *grid);

/// @brief Enable or disable sort requests for one Grid column.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param enabled Non-zero to enable sorting.
void rt_datagrid_set_sortable(void *grid, int64_t col, int64_t enabled);

/// @brief Set normalized Grid sort state without reordering caller-owned data.
/// @param grid Grid widget handle.
/// @param col Zero-based sortable column for nonzero direction.
/// @param direction Negative descending, zero none, positive ascending.
void rt_datagrid_set_sort(void *grid, int64_t col, int64_t direction);

/// @brief Return the active Grid sort column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when unsorted/invalid.
int64_t rt_datagrid_get_sort_column(void *grid);

/// @brief Return the active normalized Grid sort direction.
/// @param grid Grid widget handle.
/// @return -1 descending, 0 none/invalid, or 1 ascending.
int64_t rt_datagrid_get_sort_direction(void *grid);

/// @brief Consume the independent Grid sort transition edge.
/// @param grid Grid widget handle.
/// @return 1 once after unreported sort changes, otherwise 0.
int64_t rt_datagrid_was_sort_changed(void *grid);

/// @brief Set an explicit logical Grid column width or reset it to automatic.
/// @details Width is converted through effective UI scale exactly once. Zero selects auto sizing;
///          negative/non-finite values preserve existing state.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param width Public logical width, or zero for automatic.
void rt_datagrid_set_column_width(void *grid, int64_t col, double width);

/// @brief Enable or disable pointer resizing for one Grid column boundary.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param enabled Non-zero to enable resizing.
void rt_datagrid_set_column_resizable(void *grid, int64_t col, int64_t enabled);

/// @brief Consume the independent effective Grid column-resize edge.
/// @param grid Grid widget handle.
/// @return 1 once after unreported effective width changes, otherwise 0.
int64_t rt_datagrid_was_column_resized(void *grid);

/// @brief Return the most recently resized Grid column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when absent/invalid.
int64_t rt_datagrid_get_resized_column(void *grid);

/// @brief Enable or disable externally-driven Grid cell editing.
/// @param grid Grid widget handle.
/// @param enabled Non-zero to enable BeginEdit/CommitEdit; zero also cancels an active edit.
void rt_datagrid_set_editable(void *grid, int64_t enabled);

/// @brief Begin editing one valid dense or sparse Grid cell.
/// @param grid Editable Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return 1 when editing began/already targets the cell, otherwise 0.
int64_t rt_datagrid_begin_edit(void *grid, int64_t row, int64_t col);

/// @brief Commit copied visible UTF-8 text to the active Grid edit cell.
/// @param grid Grid widget handle with an active edit.
/// @param text Replacement text; empty clears the cell and embedded NUL becomes U+FFFD.
/// @return 1 when the active edit committed successfully, otherwise 0.
int64_t rt_datagrid_commit_edit(void *grid, rt_string text);

/// @brief Cancel an active Grid edit without changing cell content.
/// @param grid Grid widget handle; invalid/already-idle handles are no-ops.
void rt_datagrid_cancel_edit(void *grid);

/// @brief Query whether a Grid edit controller is active.
/// @param grid Grid widget handle.
/// @return 1 while editing, otherwise 0.
int64_t rt_datagrid_is_editing(void *grid);

/// @brief Consume the independent committed Grid cell-edit edge.
/// @param grid Grid widget handle.
/// @return 1 once after unreported effective edits, otherwise 0.
int64_t rt_datagrid_was_cell_edited(void *grid);

/// @brief Scroll so one logical Grid row is first in the viewport.
/// @param grid Grid widget handle.
/// @param row Non-negative requested row, clamped to the final logical row.
void rt_datagrid_scroll_to_row(void *grid, int64_t row);

/// @brief Return the Grid's first viewport row.
/// @param grid Grid widget handle.
/// @return Zero-based logical row, or zero when invalid.
int64_t rt_datagrid_get_scroll_row(void *grid);

/// @brief Consume the Grid's independent content-change edge.
/// @details Successful column/header/cell/clear mutations record a change after atomic allocation.
///          No-op, invalid, and failed-allocation mutations preserve the edge and revision.
/// @param grid Grid widget handle.
/// @return 1 when an unreported content change exists, otherwise 0.
int64_t rt_datagrid_was_changed(void *grid);

/// @brief Return the Grid's non-consuming state revision.
/// @param grid Grid widget handle.
/// @return Monotonic signed revision, saturated at `INT64_MAX`, or zero if invalid.
int64_t rt_datagrid_get_revision(void *grid);

//=========================================================================
// PopupList Widget (caret-anchored filtered selection list) — Zanna.GUI.PopupList
//=========================================================================

/// @brief Create a caret-anchored filtered popup list attached to an optional parent.
/// @param parent Parent widget handle, or NULL for no parent.
/// @return PopupList widget handle.
void *rt_popuplist_new(void *parent);
/// @brief Append an item (host adds items in its preferred rank order).
/// @param list PopupList widget handle.
/// @param text Item text.
void rt_popuplist_add_item(void *list, rt_string text);
/// @brief Remove all items and reset filter/selection.
/// @param list PopupList widget handle.
void rt_popuplist_clear(void *list);
/// @brief Set the case-insensitive substring filter.
/// @param list PopupList widget handle.
/// @param filter Filter text.
void rt_popuplist_set_filter(void *list, rt_string filter);
/// @brief Number of items currently visible (matching the filter).
/// @param list PopupList widget handle.
/// @return Number of items matching the current filter.
int64_t rt_popuplist_visible_count(void *list);
/// @brief Move the selection up one visible item.
/// @param list PopupList widget handle.
void rt_popuplist_navigate_up(void *list);
/// @brief Move the selection down one visible item.
/// @param list PopupList widget handle.
void rt_popuplist_navigate_down(void *list);
/// @brief Set the selection index within the visible items.
/// @param list PopupList widget handle.
/// @param index Zero-based visible-item index, or -1 to clear selection.
void rt_popuplist_set_selected_index(void *list, int64_t index);
/// @brief Selection index within the visible items, or -1 when none are visible.
/// @param list PopupList widget handle.
/// @return Zero-based visible-item index, or -1 when no item is selected.
int64_t rt_popuplist_get_selected_index(void *list);
/// @brief Text of the selected visible item (empty when none).
/// @param list PopupList widget handle.
/// @return Owned selected text, or an empty runtime string when none is selected.
rt_string rt_popuplist_get_selected(void *list);
/// @brief Mark the current selection accepted.
/// @param list PopupList widget handle.
void rt_popuplist_accept_selected(void *list);
/// @brief Whether AcceptSelected was called since the last query (consume-on-read).
/// @param list PopupList widget handle.
/// @return 1 once after an acceptance, otherwise 0.
int8_t rt_popuplist_was_accepted(void *list);
/// @brief Set the popup's anchor (top-left) position.
/// @param list PopupList widget handle.
/// @param x Root-relative anchor X coordinate.
/// @param y Root-relative anchor Y coordinate.
void rt_popuplist_anchor_at(void *list, double x, double y);
/// @brief Set the popup width.
/// @param list PopupList widget handle.
/// @param width Popup width in logical units.
void rt_popuplist_set_width(void *list, double width);
/// @brief Set the maximum number of visible rows.
/// @param list PopupList widget handle.
/// @param max_rows Maximum number of rows to display.
void rt_popuplist_set_max_rows(void *list, int64_t max_rows);
/// @brief Set the item font.
/// @param list PopupList widget handle.
/// @param font Font handle.
/// @param size Font size in pixels.
void rt_popuplist_set_font(void *list, void *font, double size);
/// @brief Show or hide the popup.
/// @param list PopupList widget handle.
/// @param visible Non-zero to show the popup; zero to hide it.
void rt_popuplist_set_visible(void *list, int64_t visible);
/// @brief Whether the popup is currently visible.
/// @param list PopupList widget handle.
/// @return 1 when visible, otherwise 0.
int8_t rt_popuplist_is_visible(void *list);

//=========================================================================
// Image Widget
//=========================================================================

/// @brief Create a new image widget.
/// @param parent Parent widget (can be NULL).
/// @return Image widget handle.
void *rt_image_new(void *parent);

/// @brief Set image pixels.
/// @details Pixels must be a Zanna.Graphics.Pixels object whose elements are
///          packed as 0xRRGGBBAA. Values are converted to byte RGBA for the
///          GUI image. Width or height <= 0 uses the source dimensions; larger
///          requested dimensions are clamped to the source bounds. NULL pixels
///          clears the image.
/// @param image Image widget handle.
/// @param pixels Zanna.Graphics.Pixels object.
/// @param width Image width.
/// @param height Image height.
void rt_image_set_pixels(void *image, void *pixels, int64_t width, int64_t height);

/// @brief Atomically upload a Zanna.Graphics.Pixels object into an image widget.
/// @details Width or height equal to zero selects the corresponding source dimension. Requested
///          dimensions larger than the source are clamped for compatibility with SetPixels.
///          Validation or allocation failure preserves the prior image byte-for-byte.
/// @param image Live image widget handle.
/// @param pixels Zanna.Graphics.Pixels object; NULL is rejected without clearing the image.
/// @param width Requested copied width, or zero for the source width.
/// @param height Requested copied height, or zero for the source height.
/// @return 1 after a complete upload, otherwise 0.
int64_t rt_image_try_set_pixels(void *image, void *pixels, int64_t width, int64_t height);
int64_t rt_image_try_set_from_render_target(void *image, void *target);
uint8_t *rt_gui_image_borrow_rgba(void *image, int64_t width, int64_t height);
void rt_gui_image_commit_rgba(void *image, int64_t width, int64_t height);

/// @brief Copy a rectangular Pixels region into an existing image.
/// @details Source and destination rectangles must fit completely. Conversion and validation
///          finish before destination mutation, so failure cannot produce a partial update.
/// @param image Live image widget handle with existing pixel storage.
/// @param pixels Source Zanna.Graphics.Pixels object.
/// @param source_x Zero-based source left coordinate.
/// @param source_y Zero-based source top coordinate.
/// @param width Positive region width.
/// @param height Positive region height.
/// @param dest_x Zero-based destination left coordinate.
/// @param dest_y Zero-based destination top coordinate.
/// @return 1 when the entire region was copied, otherwise 0.
int64_t rt_image_update_region(void *image,
                               void *pixels,
                               int64_t source_x,
                               int64_t source_y,
                               int64_t width,
                               int64_t height,
                               int64_t dest_x,
                               int64_t dest_y);

/// @brief Clear image.
/// @param image Image widget handle.
void rt_image_clear(void *image);

/// @brief Set image scale mode.
/// @param image Image widget handle.
/// @param mode Scale mode (RT_SCALE_NONE, _FIT, _FILL, or _STRETCH).
void rt_image_set_scale_mode(void *image, int64_t mode);

/// @brief Select nearest or bilinear filtering for resized image output.
/// @details Values other than RT_IMAGE_FILTER_BILINEAR normalize to nearest filtering.
/// @param image Image widget handle; invalid handles are ignored.
/// @param filter Filter constant from @ref rt_image_filter_t.
void rt_image_set_filter(void *image, int64_t filter);

/// @brief Return an image widget's resize filter.
/// @param image Image widget handle.
/// @return Filter constant, or RT_IMAGE_FILTER_NEAREST for an invalid handle.
int64_t rt_image_get_filter(void *image);

/// @brief Set image opacity.
/// @param image Image widget handle.
/// @param opacity Opacity (0.0 to 1.0).
void rt_image_set_opacity(void *image, double opacity);

/// @brief Opt the image into keyboard focus: click-to-focus plus a theme
///        focus ring, for interactive canvases (scene viewports).
/// @param image Image widget handle.
/// @param focusable 1 to accept focus; 0 restores presentation-only.
void rt_image_set_focusable(void *image, int8_t focusable);

/// @brief Load an image file (PNG, BMP, JPEG, or GIF) into the image widget.
/// @param image Image widget handle.
/// @param path File path (runtime string).
/// @return 1 on success, 0 on failure.
int64_t rt_image_load_file(void *image, rt_string path);

//=========================================================================
// Theme Functions
//=========================================================================

/// @brief Set the current theme to dark.
void rt_theme_set_dark(void);

/// @brief Set the current theme to light.
void rt_theme_set_light(void);

/// @brief Get the current theme name (`dark`, `light`, `system`, or `custom`).
/// @details This compatibility string reflects the selected mode rather than the effective
///          light/dark appearance resolved underneath System mode.
/// @return Caller-owned runtime string containing one stable lowercase mode name.
rt_string rt_theme_get_name(void);

/// @brief Select a public GUI theme mode for the active app.
/// @details Stable values are Dark=0, Light=1, System=2, and Custom=3. Custom is ignored until
///          @ref rt_theme_set_palette successfully installs an app-owned custom palette. System
///          tracks the host appearance on later GUI frames. Invalid values and missing apps use
///          safe no-op/fallback behavior; legacy SetDark/SetLight remain available.
/// @param mode Stable ThemeMode integer.
void rt_theme_set_mode(int64_t mode);

/// @brief Read the active app's selected public GUI theme mode.
/// @details System remains 2 even when its current effective appearance is light or dark. Without
///          an app, the process fallback selection is returned.
/// @return Dark=0, Light=1, System=2, or Custom=3.
int64_t rt_theme_get_mode(void);

/// @brief Select live platform-following theme behavior for the active app.
/// @details Equivalent to calling @ref rt_theme_set_mode with System=2. The host preference is
///          sampled immediately and then synchronized during GUI frames.
void rt_theme_follow_system(void);

/// @brief Install an independent custom logical palette into the active app.
/// @details The supplied ThemePalette is validated and deep-cloned before selection, so callers
///          may continue mutating or release it. Palette spatial metrics are logical values and
///          are DPI/user-scale transformed once during installation. Missing apps, invalid/stale
///          handles, invalid values, stale font roles, insufficient primary/secondary contrast,
///          or allocation failure return zero without modifying the prior palette.
/// @param palette Live opaque ThemePalette handle borrowed for the call.
/// @return One when the custom palette was cloned and selected, otherwise zero.
int64_t rt_theme_set_palette(void *palette);

/// @brief Snapshot the selected logical palette before DPI and accessibility transforms.
/// @details Returns a deep, independently mutable ThemePalette. System mode snapshots the current
///          resolved base. Without an app, the selected built-in fallback is cloned.
/// @return Caller-owned ThemePalette, or NULL on allocation failure/unavailable graphics.
void *rt_theme_get_palette(void);

/// @brief Discard the active app's stored custom palette.
/// @details An app currently in Custom mode switches safely to Dark before the palette is freed.
///          Built-in palettes and the caller's independent ThemePalette objects are unaffected.
///          Missing apps and apps without a custom palette are no-ops.
void rt_theme_reset_custom(void);

/// @brief Consume the active theme's changed edge.
/// @details Mode, palette, effective scale, accessibility appearance, and followed System changes
///          each produce an installed-theme revision. This method reports a newly observed
///          revision once without changing @ref rt_theme_get_revision.
/// @return One once after a new theme revision, otherwise zero.
int64_t rt_theme_was_changed(void);

/// @brief Read the active theme's non-consuming monotonic revision.
/// @details Multiple observers may compare this value independently. Values saturate at the
///          public signed 64-bit maximum rather than wrapping through a negative number.
/// @return Current theme revision, or zero before any no-app fallback change.
int64_t rt_theme_get_revision(void);

/// @brief Create an independently mutable logical palette from the built-in dark theme.
/// @details The managed opaque object owns its theme clone and is reclaimed by the runtime. No
///          active app is required in a graphics-enabled build.
/// @return Caller-owned ThemePalette, or NULL on allocation failure/unavailable graphics.
void *rt_theme_palette_new(void);

/// @brief Create an independently mutable logical palette from the built-in dark theme.
/// @return Caller-owned ThemePalette, or NULL on allocation failure/unavailable graphics.
void *rt_theme_palette_from_dark(void);

/// @brief Create an independently mutable logical palette from the built-in light theme.
/// @return Caller-owned ThemePalette, or NULL on allocation failure/unavailable graphics.
void *rt_theme_palette_from_light(void);

/// @brief Deep-clone a managed logical theme palette.
/// @details The clone owns independent theme storage and copies any pending validation state.
///          Invalid or stale handles return NULL without dereferencing lower theme state.
/// @param palette Live ThemePalette borrowed for the call.
/// @return Caller-owned independent ThemePalette, or NULL on invalid input/allocation failure.
void *rt_theme_palette_clone(void *palette);

/// @brief Set one named 24-bit RGB token in a ThemePalette.
/// @details Canonical names flatten `vg_theme.h` fields into lower camel case, such as
///          `bgPrimary`, `accentPrimary`, `syntaxKeyword`, `elevationShadowColor`, and
///          `focusGlowColor`. Unknown or embedded-NUL names return zero. Recognized values outside
///          [0,0xFFFFFF] return one but leave the prior field intact and make Validate fail until
///          repaired by a valid write.
/// @param palette Live ThemePalette borrowed for mutation.
/// @param token Runtime string naming the color token.
/// @param rgb Packed 0xRRGGBB integer.
/// @return One when the name is recognized, otherwise zero.
int64_t rt_theme_palette_set_color(void *palette, rt_string token, int64_t rgb);

/// @brief Read one named 24-bit RGB token from a ThemePalette.
/// @param palette Live ThemePalette borrowed for the call.
/// @param token Canonical lower-camel color-token name.
/// @return Packed 0xRRGGBB, or zero for invalid/stale handles and unknown/invalid names.
int64_t rt_theme_palette_get_color(void *palette, rt_string token);

/// @brief Set one named logical numeric token in a ThemePalette.
/// @details Supported fields cover typography, spacing, control metrics, radii, elevation,
///          gradient, focus glow, and motion. Spatial values are logical units; motion values are
///          milliseconds and opacity fields use [0,255]. Unknown names return zero. A recognized
///          non-finite, out-of-range, or fractional integer input returns one, preserves the prior
///          value, and makes Validate report that canonical token until repaired.
/// @param palette Live ThemePalette borrowed for mutation.
/// @param token Canonical lower-camel token or documented direct-field alias.
/// @param value Candidate numeric value in the token's documented units.
/// @return One when the name is recognized, otherwise zero.
int64_t rt_theme_palette_set_metric(void *palette, rt_string token, double value);

/// @brief Read one named logical numeric token from a ThemePalette.
/// @param palette Live ThemePalette borrowed for the call.
/// @param token Canonical lower-camel token or documented direct-field alias.
/// @return Metric value as f64, or zero for invalid/stale handles and unknown/invalid names.
double rt_theme_palette_get_metric(void *palette, rt_string token);

/// @brief Enable or disable palette-defined state-transition motion.
/// @details This strongly typed operation repairs any pending invalid `motionEnabled` metric.
///          Reduced-motion accessibility mode may still disable motion in the installed app copy.
/// @param palette Live ThemePalette borrowed for mutation; invalid handles are no-ops.
/// @param enabled Non-zero enables theme motion; zero disables it.
void rt_theme_palette_set_motion_enabled(void *palette, int64_t enabled);

/// @brief Assign proportional regular, proportional bold, and monospace font roles atomically.
/// @details NULL clears a role. Every non-NULL value must be a live Zanna.GUI.Font; otherwise none
///          of the three roles changes and Validate reports the first invalid role. Palette roles
///          are borrowed; after installation the app's font-retirement mechanism keeps referenced
///          fonts safe until the theme no longer uses them.
/// @param palette Live ThemePalette borrowed for mutation.
/// @param regular Nullable live regular font handle.
/// @param bold Nullable live bold font handle.
/// @param mono Nullable live monospace font handle.
void rt_theme_palette_set_font_roles(void *palette, void *regular, void *bold, void *mono);

/// @brief Validate a complete ThemePalette and report its first invalid token/value.
/// @details Validation is non-mutating and deterministic. It covers all numeric ranges, failed
///          recognized setter attempts, font liveness, and 4.5:1 primary/secondary normal-text
///          contrast. Invalid handles return an error. The error format is
///          `GUI theme token <name> has an invalid value`.
/// @param palette Live ThemePalette borrowed for validation.
/// @return Caller-owned `Zanna.Result`: Ok(1) when valid, otherwise ErrStr.
void *rt_theme_palette_validate(void *palette);

//=========================================================================
// Layout Functions
//=========================================================================

/// @brief Create a container with vertical box layout.
/// @return VBox container widget handle.
void *rt_vbox_new(void);

/// @brief Create a container with horizontal box layout.
/// @return HBox container widget handle.
void *rt_hbox_new(void);

/// @brief Set spacing for a layout container.
/// @param container Container widget handle.
/// @param spacing Spacing in pixels.
void rt_container_set_spacing(void *container, double spacing);

/// @brief Set padding for a layout container.
/// @param container Container widget handle.
/// @param padding Padding in pixels.
void rt_container_set_padding(void *container, double padding);

/// @brief Set the VBox cross-axis alignment.
/// @details Accepted values are `0=Start`, `1=Center`, `2=End`, and `3=Stretch`;
///          invalid values normalize to Start. Wrong-type handles are ignored.
/// @param vbox VBox container handle.
/// @param align Public alignment constant.
void rt_vbox_set_align(void *vbox, int64_t align);

/// @brief Read the VBox cross-axis alignment.
/// @param vbox VBox container handle.
/// @return Public alignment constant, or zero for an invalid handle.
int64_t rt_vbox_get_align(void *vbox);

/// @brief Set the VBox main-axis distribution strategy.
/// @details Accepted values are the six `Zanna.GUI.Justify` constants; invalid
///          values normalize to Start.
/// @param vbox VBox container handle.
/// @param justify Public justification constant.
void rt_vbox_set_justify(void *vbox, int64_t justify);

/// @brief Read the VBox main-axis distribution strategy.
/// @param vbox VBox container handle.
/// @return Public justification constant, or zero for an invalid handle.
int64_t rt_vbox_get_justify(void *vbox);

/// @brief Set the HBox cross-axis alignment.
/// @param hbox HBox container handle.
/// @param align Public alignment constant; invalid values normalize to Start.
void rt_hbox_set_align(void *hbox, int64_t align);

/// @brief Read the HBox cross-axis alignment.
/// @param hbox HBox container handle.
/// @return Public alignment constant, or zero for an invalid handle.
int64_t rt_hbox_get_align(void *hbox);

/// @brief Set the HBox main-axis distribution strategy.
/// @param hbox HBox container handle.
/// @param justify Public justification constant; invalid values normalize to Start.
void rt_hbox_set_justify(void *hbox, int64_t justify);

/// @brief Read the HBox main-axis distribution strategy.
/// @param hbox HBox container handle.
/// @return Public justification constant, or zero for an invalid handle.
int64_t rt_hbox_get_justify(void *hbox);

/// @brief Create a detached Flex layout container.
/// @return Flex widget handle, or NULL on allocation failure/disabled graphics.
void *rt_flex_new(void);

/// @brief Set Flex main-axis direction using the stable public constant order.
/// @details Values are `0=Row`, `1=Column`, `2=RowReverse`, and
///          `3=ColumnReverse`; invalid values normalize to Row.
/// @param flex Flex container handle.
/// @param direction Public `FlexDirection` value.
void rt_flex_set_direction(void *flex, int64_t direction);

/// @brief Set Flex line wrapping.
/// @details Values are `0=NoWrap`, `1=Wrap`, and `2=WrapReverse`; invalid
///          values normalize to NoWrap.
/// @param flex Flex container handle.
/// @param wrap Public `FlexWrap` value.
void rt_flex_set_wrap(void *flex, int64_t wrap);

/// @brief Set Flex cross-axis item alignment.
/// @param flex Flex container handle.
/// @param align Public alignment value.
void rt_flex_set_align(void *flex, int64_t align);

/// @brief Set Flex main-axis item distribution.
/// @param flex Flex container handle.
/// @param justify Public justification value.
void rt_flex_set_justify(void *flex, int64_t justify);

/// @brief Set the logical gap between Flex items and wrapped lines.
/// @param flex Flex container handle.
/// @param gap Non-negative logical gap; invalid values become zero.
void rt_flex_set_gap(void *flex, double gap);

/// @brief Set equal logical padding on all Flex edges.
/// @param flex Flex container handle.
/// @param padding Non-negative logical padding; invalid values become zero.
void rt_flex_set_padding(void *flex, double padding);

/// @brief Create a detached one-row, one-column CSS-style layout grid.
/// @return LayoutGrid widget handle, or NULL on allocation failure/disabled graphics.
void *rt_layoutgrid_new(void);

/// @brief Set the declared LayoutGrid row count.
/// @param grid LayoutGrid handle.
/// @param count Count clamped to the supported range `[1,4096]`.
void rt_layoutgrid_set_rows(void *grid, int64_t count);

/// @brief Set the declared LayoutGrid column count.
/// @param grid LayoutGrid handle.
/// @param count Count clamped to the supported range `[1,4096]`.
void rt_layoutgrid_set_columns(void *grid, int64_t count);

/// @brief Define one LayoutGrid row track.
/// @details Positive values are fixed logical units, zero is auto/content, and
///          negative values are dimensionless fractional weights.
/// @param grid LayoutGrid handle.
/// @param row Zero-based declared row index.
/// @param size Track definition; non-finite values are ignored.
void rt_layoutgrid_set_row_size(void *grid, int64_t row, double size);

/// @brief Define one LayoutGrid column track.
/// @details Positive values are fixed logical units, zero is auto/content, and
///          negative values are dimensionless fractional weights.
/// @param grid LayoutGrid handle.
/// @param column Zero-based declared column index.
/// @param size Track definition; non-finite values are ignored.
void rt_layoutgrid_set_column_size(void *grid, int64_t column, double size);

/// @brief Set logical horizontal and vertical LayoutGrid gaps.
/// @param grid LayoutGrid handle.
/// @param horizontal Non-negative logical column gap.
/// @param vertical Non-negative logical row gap.
void rt_layoutgrid_set_gap(void *grid, double horizontal, double vertical);

/// @brief Set equal logical padding on every LayoutGrid edge.
/// @param grid LayoutGrid handle.
/// @param padding Non-negative logical padding.
void rt_layoutgrid_set_padding(void *grid, double padding);

/// @brief Place an existing direct child in declared LayoutGrid cells.
/// @details Validation is atomic: detached/foreign children, negative or
///          out-of-range indices, invalid spans, and allocation failure preserve
///          previous placement metadata.
/// @param grid LayoutGrid handle.
/// @param child Existing direct child widget.
/// @param row Zero-based starting row.
/// @param column Zero-based starting column.
/// @param row_span Positive number of rows to span.
/// @param column_span Positive number of columns to span.
/// @return 1 when placement is committed, otherwise 0.
int64_t rt_layoutgrid_place(
    void *grid, void *child, int64_t row, int64_t column, int64_t row_span, int64_t column_span);

/// @brief Create a detached DockPanel layout container.
/// @return DockPanel widget handle, or NULL on allocation failure/disabled graphics.
void *rt_dockpanel_new(void);

/// @brief Set equal logical padding on every DockPanel edge.
/// @param dock DockPanel handle.
/// @param padding Non-negative logical padding.
void rt_dockpanel_set_padding(void *dock, double padding);

/// @brief Set the logical gap between claimed dock regions.
/// @param dock DockPanel handle.
/// @param gap Non-negative logical gap.
void rt_dockpanel_set_gap(void *dock, double gap);

/// @brief Attach or update a child at one DockPanel edge.
/// @details Public values are `0=Left`, `1=Top`, `2=Right`, `3=Bottom`, and
///          `4=Fill`. Detached children are attached; children owned by another
///          parent and invalid values are rejected atomically.
/// @param dock DockPanel handle.
/// @param child Detached widget or existing direct child.
/// @param position Public `Dock` value.
/// @return 1 when the dock assignment is committed, otherwise 0.
int64_t rt_dockpanel_dock_child(void *dock, void *child, int64_t position);

//=========================================================================
// Clipboard Functions (Phase 1)
//=========================================================================

/// @brief Set text to the system clipboard.
/// @param text Text to copy to clipboard.
void rt_clipboard_set_text(rt_string text);

/// @brief Get text from the system clipboard.
/// @return Text from clipboard, or empty string if not available.
rt_string rt_clipboard_get_text(void);

/// @brief Check if clipboard contains text.
/// @return 1 if text is available, 0 otherwise.
int64_t rt_clipboard_has_text(void);

/// @brief Clear all clipboard contents.
void rt_clipboard_clear(void);

/// @brief Set text to the system clipboard through Zanna.System.Clipboard.
/// @param text Text to copy to clipboard.
void rt_system_clipboard_set(rt_string text);

/// @brief Get text from the system clipboard through Zanna.System.Clipboard.
/// @return Text from clipboard, or empty string if not available.
rt_string rt_system_clipboard_get(void);

/// @brief Check if clipboard contains text through Zanna.System.Clipboard.
/// @return 1 if text is available, 0 otherwise.
int64_t rt_system_clipboard_has_text(void);

//=========================================================================
// Keyboard Shortcuts (Phase 1)
//=========================================================================

/// @brief Register a keyboard shortcut.
/// @param id Unique identifier for the shortcut.
/// @param keys Key combination string (e.g., "Ctrl+S", "Ctrl+Shift+P").
/// @param description Human-readable description.
void rt_shortcuts_register(rt_string id, rt_string keys, rt_string description);

/// @brief Unregister a keyboard shortcut.
/// @param id Shortcut identifier to remove.
void rt_shortcuts_unregister(rt_string id);

/// @brief Clear all registered shortcuts.
void rt_shortcuts_clear(void);

/// @brief Check if a specific shortcut was triggered this frame.
/// @param id Shortcut identifier to check.
/// @return 1 if triggered, 0 otherwise.
int64_t rt_shortcuts_was_triggered(rt_string id);

/// @brief Get the ID of the shortcut triggered this frame.
/// @return Shortcut ID, or empty string if none triggered.
rt_string rt_shortcuts_get_triggered(void);

/// @brief Enable or disable a specific shortcut.
/// @param id Shortcut identifier.
/// @param enabled 1 to enable, 0 to disable.
void rt_shortcuts_set_enabled(rt_string id, int64_t enabled);

/// @brief Check if a specific shortcut is enabled.
/// @param id Shortcut identifier.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_shortcuts_is_enabled(rt_string id);

/// @brief Enable or disable all shortcuts globally.
/// @param enabled 1 to enable, 0 to disable.
void rt_shortcuts_set_global_enabled(int64_t enabled);

/// @brief Check if shortcuts are globally enabled.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_shortcuts_get_global_enabled(void);

//=========================================================================
// Window Management (Phase 1)
//=========================================================================

/// @brief Set the window title.
/// @param app GUI application handle.
/// @param title New window title.
void rt_app_set_title(void *app, rt_string title);

/// @brief Get the window title.
/// @param app GUI application handle.
/// @return Current window title.
rt_string rt_app_get_title(void *app);

/// @brief Set the window size.
/// @param app GUI application handle.
/// @param width New width in pixels.
/// @param height New height in pixels.
void rt_app_set_size(void *app, int64_t width, int64_t height);

/// @brief Get the window width.
/// @param app GUI application handle.
/// @return Window width in pixels.
int64_t rt_app_get_width(void *app);

/// @brief Get the window height.
/// @param app GUI application handle.
/// @return Window height in pixels.
int64_t rt_app_get_height(void *app);

/// @brief Get the window's HiDPI backing scale factor.
/// @param app GUI application handle.
/// @return Scale factor (>= 1.0): 1.0 on standard displays, 2.0 on Retina/2x.
///         GetWidth/GetHeight report physical pixels; divide by this to recover
///         logical (point) dimensions.
double rt_app_get_scale(void *app);

/// @brief Convert a physical-pixel measurement to logical (point) units for a given scale.
/// @details Non-positive values and scales <= 1.0 (including NaN) pass through unchanged;
///          otherwise rounds physical / scale to the nearest integer. Shared by the App
///          logical-unit methods and reusable for any HiDPI-aware layout math.
static inline int64_t rt_gui_dpi_to_logical(int64_t physical, double scale) {
    if (physical <= 0)
        return physical;
    if (!(scale > 1.0)) // also rejects NaN
        return physical;
    return (int64_t)((double)physical / scale + 0.5);
}

/// @brief Convert a logical (point) measurement to physical pixels for a given scale.
/// @details Inverse of rt_gui_dpi_to_logical: non-positive values and scales <= 1.0 (including
///          NaN) pass through unchanged; otherwise rounds logical * scale to the nearest integer.
static inline int64_t rt_gui_dpi_to_physical(int64_t logical, double scale) {
    if (logical <= 0)
        return logical;
    if (!(scale > 1.0)) // also rejects NaN
        return logical;
    if (scale >= (double)INT64_MAX / (double)logical)
        return INT64_MAX;
    return (int64_t)((double)logical * scale + 0.5);
}

/// @brief Get the window width in logical (point) units (physical width / scale).
/// @param app GUI application handle.
/// @return Logical width; 0 if there is no window.
int64_t rt_app_get_logical_width(void *app);

/// @brief Get the window height in logical (point) units (physical height / scale).
/// @param app GUI application handle.
/// @return Logical height; 0 if there is no window.
int64_t rt_app_get_logical_height(void *app);

/// @brief Convert a physical-pixel value to logical units using the app's current scale.
/// @param app      GUI application handle.
/// @param physical Physical-pixel measurement.
/// @return Logical value; @p physical unchanged when there is no window (scale 1.0).
int64_t rt_app_to_logical(void *app, int64_t physical);

/// @brief Convert a logical value to physical pixels using the app's current scale.
/// @param app     GUI application handle.
/// @param logical Logical (point) measurement.
/// @return Physical value; @p logical unchanged when there is no window (scale 1.0).
int64_t rt_app_to_physical(void *app, int64_t logical);

/// @brief Set a user UI zoom multiplier applied on top of the HiDPI scale.
/// @param app   GUI application handle.
/// @param scale Zoom factor; clamped to [0.5, 3.0]. 1.0 is the default.
void rt_app_set_ui_scale(void *app, double scale);

/// @brief Enable or disable inertial smooth scrolling process-wide (ADR 0137).
/// @details Reduced-motion themes suppress easing regardless of this flag.
/// @param app App handle (accepted for symmetry; the flag is process-wide).
/// @param enabled Non-zero eases wheel scrolling; zero jumps instantly.
void rt_app_set_smooth_scroll(void *app, int64_t enabled);

/// @brief Return whether inertial smooth scrolling is requested.
/// @param app App handle (unused).
/// @return One when smooth scrolling is requested, otherwise zero.
int64_t rt_app_get_smooth_scroll(void *app);

/// @brief Get the current user UI zoom multiplier.
/// @param app GUI application handle.
/// @return Zoom factor (1.0 = default).
double rt_app_get_ui_scale(void *app);

/// @brief Get the effective logical-to-framebuffer scale for a GUI application.
/// @details The effective scale is the platform window scale multiplied by the app's user UI zoom.
///          Theme spatial metrics and logical font points are multiplied by this value exactly
///          once. Invalid handles and graphics-disabled builds return the identity scale.
/// @param app GUI application handle; may be NULL or stale.
/// @return Positive finite effective scale, or 1.0 when @p app is invalid/unavailable.
double rt_app_get_effective_scale(void *app);

/// @brief Set the global mouse-wheel scroll sensitivity (clamped in the gui lib).
/// @param app GUI application handle (accepted for symmetry; setting is global).
/// @param speed Sensitivity multiplier; 1.0 is the default.
void rt_app_set_wheel_speed(void *app, double speed);

/// @brief Get the global mouse-wheel scroll sensitivity.
/// @param app GUI application handle.
/// @return Sensitivity multiplier (1.0 = default).
double rt_app_get_wheel_speed(void *app);

/// @brief Set the window position.
/// @param app GUI application handle.
/// @param x X position in screen coordinates.
/// @param y Y position in screen coordinates.
void rt_app_set_position(void *app, int64_t x, int64_t y);

/// @brief Get the window X position.
/// @param app GUI application handle.
/// @return X position in screen coordinates.
int64_t rt_app_get_x(void *app);

/// @brief Get the window Y position.
/// @param app GUI application handle.
/// @return Y position in screen coordinates.
int64_t rt_app_get_y(void *app);

/// @brief Minimize the window.
/// @param app GUI application handle.
void rt_app_minimize(void *app);

/// @brief Maximize the window.
/// @param app GUI application handle.
void rt_app_maximize(void *app);

/// @brief Restore the window from minimized/maximized state.
/// @param app GUI application handle.
void rt_app_restore(void *app);

/// @brief Check if the window is minimized.
/// @param app GUI application handle.
/// @return 1 if minimized, 0 otherwise.
int64_t rt_app_is_minimized(void *app);

/// @brief Check if the window is maximized.
/// @param app GUI application handle.
/// @return 1 if maximized, 0 otherwise.
int64_t rt_app_is_maximized(void *app);

/// @brief Set the window fullscreen state.
/// @param app GUI application handle.
/// @param fullscreen 1 for fullscreen, 0 for windowed.
void rt_app_set_fullscreen(void *app, int64_t fullscreen);

/// @brief Check if the window is fullscreen.
/// @param app GUI application handle.
/// @return 1 if fullscreen, 0 otherwise.
int64_t rt_app_is_fullscreen(void *app);

/// @brief Bring the window to the front and give it focus.
/// @param app GUI application handle.
void rt_app_focus(void *app);

/// @brief Activate the app as the foreground OS application/window.
/// @param app GUI application handle.
void rt_app_activate(void *app);

/// @brief Check if the window has keyboard focus.
/// @param app GUI application handle.
/// @return 1 if focused, 0 otherwise.
int64_t rt_app_is_focused(void *app);

/// @brief Count of frames that took the full-window repaint path (plan 07).
/// @param app GUI application handle.
/// @return Number of full-window paint frames recorded.
int64_t rt_app_get_paint_frames_full(void *app);

/// @brief Count of frames that took the damage-region (partial) repaint path.
/// @param app GUI application handle.
/// @return Number of partial paint frames recorded.
int64_t rt_app_get_paint_frames_partial(void *app);

/// @brief Enable/disable damage-region rendering at runtime (0 forces full repaint).
/// @param app GUI application handle.
/// @param enabled Non-zero to allow partial painting; zero to force full repaint.
void rt_app_set_partial_paint(void *app, int64_t enabled);

/// @brief Enable or disable close prevention.
/// @param app GUI application handle.
/// @param prevent 1 to prevent close, 0 to allow.
void rt_app_set_prevent_close(void *app, int64_t prevent);

/// @brief Check if close was requested.
/// @param app GUI application handle.
/// @return 1 if close was requested, 0 otherwise.
int64_t rt_app_was_close_requested(void *app);

/// @brief Get the width of the monitor containing the window.
/// @param app GUI application handle.
/// @return Monitor width in pixels, or 0 when unavailable.
int64_t rt_app_get_monitor_width(void *app);

/// @brief Get the height of the monitor containing the window.
/// @param app GUI application handle.
/// @return Monitor height in pixels, or 0 when unavailable.
int64_t rt_app_get_monitor_height(void *app);

/// @brief Resize the OS window to the given dimensions.
/// @param app GUI application handle.
/// @param w Requested logical window width.
/// @param h Requested logical window height.
void rt_app_set_window_size(void *app, int64_t w, int64_t h);

/// @brief Set the minimum native client/content size of the app window.
/// @details Dimensions are logical pixels, matching SetWindowSize. Requests
///          below one are clamped to one. The constraint applies to both future
///          programmatic resizes and interactive desktop-window resizing.
/// @param app GUI application handle.
/// @param w Minimum logical window width.
/// @param h Minimum logical window height.
void rt_app_set_minimum_size(void *app, int64_t w, int64_t h);

/// @brief Get the current default font size for the application.
/// @details Compatibility alias for @ref rt_app_get_logical_font_size; the returned value is in
///          logical points and does not include window DPI or user UI zoom.
/// @param app GUI application handle; may be NULL or stale.
/// @return Stored logical font size, or 14.0 when @p app is invalid.
double rt_app_get_font_size(void *app);

/// @brief Get the application's default font size explicitly in logical points.
/// @details The runtime retains this unscaled value and derives effective framebuffer pixels each
///          time the monitor scale or user UI zoom changes. No ownership is transferred and the
///          call has no side effects.
/// @param app GUI application handle; may be NULL or stale.
/// @return Stored logical font size, or 14.0 when @p app is invalid.
double rt_app_get_logical_font_size(void *app);

/// @brief Set the default font size for the application.
/// @details Stores a logical-point value clamped to [6, 72], converts it through the app's current
///          effective scale, and invalidates every surface that inherits the default font. Invalid
///          app handles are ignored.
/// @param app GUI application handle; may be NULL or stale.
/// @param size Requested size in logical points; invalid values preserve a sanitized fallback.
void rt_app_set_font_size(void *app, double size);

//=========================================================================
// Cursor Styles (Phase 1)
//=========================================================================

/// Cursor type constants.
typedef enum {
    RT_CURSOR_ARROW = 0,
    RT_CURSOR_IBEAM = 1,
    RT_CURSOR_WAIT = 2,
    RT_CURSOR_CROSSHAIR = 3,
    RT_CURSOR_HAND = 4,
    RT_CURSOR_RESIZE_H = 5,
    RT_CURSOR_RESIZE_V = 6,
    RT_CURSOR_RESIZE_NE = 7,
    RT_CURSOR_RESIZE_NW = 8,
    RT_CURSOR_MOVE = 9,
    RT_CURSOR_NOT_ALLOWED = 10,
} rt_cursor_type_t;

/// @brief Set the global cursor style.
/// @param type Cursor type constant (RT_CURSOR_*).
void rt_cursor_set(int64_t type);

/// @brief Reset cursor to default (arrow).
void rt_cursor_reset(void);

/// @brief Set cursor visibility.
/// @param visible 1 for visible, 0 for hidden.
void rt_cursor_set_visible(int64_t visible);

/// @brief Set cursor for a specific widget.
/// @param widget Widget handle.
/// @param type Cursor type constant.
void rt_widget_set_cursor(void *widget, int64_t type);

/// @brief Reset widget cursor to default.
/// @param widget Widget handle.
void rt_widget_reset_cursor(void *widget);

//=========================================================================
// MenuBar Widget (Phase 2)
//=========================================================================

/// @brief Create a new menu bar widget.
/// @param parent Parent widget (can be NULL).
/// @return MenuBar widget handle.
void *rt_menubar_new(void *parent);

/// @brief Destroy a menu bar widget.
/// @param menubar MenuBar widget handle.
void rt_menubar_destroy(void *menubar);

/// @brief Add a menu to the menu bar.
/// @param menubar MenuBar widget handle.
/// @param title Menu title.
/// @return Menu handle.
void *rt_menubar_add_menu(void *menubar, rt_string title);

/// @brief Remove a menu from the menu bar.
/// @param menubar MenuBar widget handle.
/// @param menu Menu handle to remove.
void rt_menubar_remove_menu(void *menubar, void *menu);

/// @brief Get the number of menus in the menu bar.
/// @param menubar MenuBar widget handle.
/// @return Number of menus.
int64_t rt_menubar_get_menu_count(void *menubar);

/// @brief Get a menu by index.
/// @param menubar MenuBar widget handle.
/// @param index Menu index.
/// @return Menu handle, or NULL if out of bounds.
void *rt_menubar_get_menu(void *menubar, int64_t index);

/// @brief Set menu bar visibility.
/// @param menubar MenuBar widget handle.
/// @param visible 1 for visible, 0 for hidden.
void rt_menubar_set_visible(void *menubar, int64_t visible);

/// @brief Check if menu bar is visible.
/// @param menubar MenuBar widget handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_menubar_is_visible(void *menubar);

//=========================================================================
// Menu Widget (Phase 2)
//=========================================================================

/// @brief Add a menu item.
/// @param menu Menu handle.
/// @param text Item text.
/// @return MenuItem handle.
void *rt_menu_add_item(void *menu, rt_string text);

/// @brief Add a menu item with keyboard shortcut.
/// @param menu Menu handle.
/// @param text Item text.
/// @param shortcut Shortcut string (e.g., "Ctrl+S").
/// @return MenuItem handle.
void *rt_menu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut);

/// @brief Add a separator to the menu.
/// @param menu Menu handle.
/// @return MenuItem handle (separator).
void *rt_menu_add_separator(void *menu);

/// @brief Add a submenu.
/// @param menu Parent menu handle.
/// @param title Submenu title.
/// @return Menu handle for the submenu.
void *rt_menu_add_submenu(void *menu, rt_string title);

/// @brief Remove an item from the menu.
/// @param menu Menu handle.
/// @param item MenuItem handle to remove.
void rt_menu_remove_item(void *menu, void *item);

/// @brief Clear all items from the menu.
/// @param menu Menu handle.
void rt_menu_clear(void *menu);

/// @brief Set menu title.
/// @param menu Menu handle.
/// @param title New title.
void rt_menu_set_title(void *menu, rt_string title);

/// @brief Get menu title.
/// @param menu Menu handle.
/// @return Menu title.
rt_string rt_menu_get_title(void *menu);

/// @brief Get number of items in the menu.
/// @param menu Menu handle.
/// @return Number of items.
int64_t rt_menu_get_item_count(void *menu);

/// @brief Get a menu item by index.
/// @param menu Menu handle.
/// @param index Item index.
/// @return MenuItem handle, or NULL if out of bounds.
void *rt_menu_get_item(void *menu, int64_t index);

/// @brief Enable or disable the menu.
/// @param menu Menu handle.
/// @param enabled 1 to enable, 0 to disable.
void rt_menu_set_enabled(void *menu, int64_t enabled);

/// @brief Check if menu is enabled.
/// @param menu Menu handle.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_menu_is_enabled(void *menu);

//=========================================================================
// MenuItem Widget (Phase 2)
//=========================================================================

/// @brief Set menu item text.
/// @param item MenuItem handle.
/// @param text New text.
void rt_menuitem_set_text(void *item, rt_string text);

/// @brief Get menu item text.
/// @param item MenuItem handle.
/// @return Item text.
rt_string rt_menuitem_get_text(void *item);

/// @brief Set menu item keyboard shortcut.
/// @param item MenuItem handle.
/// @param shortcut Shortcut string.
void rt_menuitem_set_shortcut(void *item, rt_string shortcut);

/// @brief Get menu item keyboard shortcut.
/// @param item MenuItem handle.
/// @return Shortcut string.
rt_string rt_menuitem_get_shortcut(void *item);

/// @brief Set menu item icon.
/// @param item MenuItem handle.
/// @param pixels Pixel data handle rendered as an image icon.
void rt_menuitem_set_icon(void *item, void *pixels);

/// @brief Set whether menu item is checkable.
/// @param item MenuItem handle.
/// @param checkable 1 for checkable, 0 otherwise.
void rt_menuitem_set_checkable(void *item, int64_t checkable);

/// @brief Check if menu item is checkable.
/// @param item MenuItem handle.
/// @return 1 if checkable, 0 otherwise.
int64_t rt_menuitem_is_checkable(void *item);

/// @brief Set menu item checked state.
/// @param item MenuItem handle.
/// @param checked 1 for checked, 0 for unchecked.
void rt_menuitem_set_checked(void *item, int64_t checked);

/// @brief Check if menu item is checked.
/// @param item MenuItem handle.
/// @return 1 if checked, 0 otherwise.
int64_t rt_menuitem_is_checked(void *item);

/// @brief Enable or disable the menu item.
/// @param item MenuItem handle.
/// @param enabled 1 to enable, 0 to disable.
void rt_menuitem_set_enabled(void *item, int64_t enabled);

/// @brief Check if menu item is enabled.
/// @param item MenuItem handle.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_menuitem_is_enabled(void *item);

/// @brief Check if menu item is a separator.
/// @param item MenuItem handle.
/// @return 1 if separator, 0 otherwise.
int64_t rt_menuitem_is_separator(void *item);

/// @brief Check if menu item was clicked this frame.
/// @param item MenuItem handle.
/// @return 1 if clicked, 0 otherwise.
int64_t rt_menuitem_was_clicked(void *item);

/// @brief On-screen left edge of a visible context-menu row (ADR 0219).
/// @param item MenuItem handle.
/// @return Logical window X, or 0 when the row has no on-screen geometry.
double rt_menuitem_get_screen_x(void *item);

/// @brief On-screen top edge of a visible context-menu row (ADR 0219).
/// @param item MenuItem handle.
/// @return Logical window Y, or 0 when the row has no on-screen geometry.
double rt_menuitem_get_screen_y(void *item);

/// @brief On-screen width of a visible context-menu row (ADR 0219).
/// @param item MenuItem handle.
/// @return Row width, or 0 when the row has no on-screen geometry.
double rt_menuitem_get_screen_width(void *item);

/// @brief On-screen height of a visible context-menu row (ADR 0219).
/// @param item MenuItem handle.
/// @return Row height, or 0 when the row has no on-screen geometry.
double rt_menuitem_get_screen_height(void *item);

//=========================================================================
// ContextMenu Widget (Phase 2)
//=========================================================================

/// @brief Create a new context menu.
/// @return ContextMenu handle.
void *rt_contextmenu_new(void);

/// @brief Destroy a context menu.
/// @param menu ContextMenu handle.
void rt_contextmenu_destroy(void *menu);

/// @brief Add an item to the context menu.
/// @param menu ContextMenu handle.
/// @param text Item text.
/// @return MenuItem handle.
void *rt_contextmenu_add_item(void *menu, rt_string text);

/// @brief Add an item with shortcut to the context menu.
/// @param menu ContextMenu handle.
/// @param text Item text.
/// @param shortcut Shortcut string.
/// @return MenuItem handle.
void *rt_contextmenu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut);

/// @brief Add a separator to the context menu.
/// @param menu ContextMenu handle.
/// @return MenuItem handle (separator).
void *rt_contextmenu_add_separator(void *menu);

/// @brief Add a submenu to the context menu.
/// @param menu ContextMenu handle.
/// @param title Submenu title.
/// @return Menu handle for the submenu.
void *rt_contextmenu_add_submenu(void *menu, rt_string title);

/// @brief Clear all items from the context menu.
/// @param menu ContextMenu handle.
void rt_contextmenu_clear(void *menu);

/// @brief Show the context menu at a specific position.
/// @param menu ContextMenu handle.
/// @param x X position in screen coordinates.
/// @param y Y position in screen coordinates.
void rt_contextmenu_show(void *menu, int64_t x, int64_t y);

/// @brief Hide the context menu.
/// @param menu ContextMenu handle.
void rt_contextmenu_hide(void *menu);

/// @brief Check if context menu is visible.
/// @param menu ContextMenu handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_contextmenu_is_visible(void *menu);

/// @brief Get the clicked menu item.
/// @param menu ContextMenu handle.
/// @return MenuItem handle that was clicked, or NULL if none.
void *rt_contextmenu_get_clicked_item(void *menu);

//=========================================================================
// StatusBar Widget (Phase 3)
//=========================================================================

/// StatusBar zone constants.
typedef enum {
    RT_STATUSBAR_ZONE_LEFT = 0,
    RT_STATUSBAR_ZONE_CENTER = 1,
    RT_STATUSBAR_ZONE_RIGHT = 2,
} rt_statusbar_zone_t;

/// @brief Create a new status bar widget.
/// @param parent Parent widget.
/// @return StatusBar widget handle.
void *rt_statusbar_new(void *parent);

/// @brief Destroy a status bar widget.
/// @param bar StatusBar widget handle.
void rt_statusbar_destroy(void *bar);

/// @brief Set left zone text.
/// @param bar StatusBar widget handle.
/// @param text Text to display.
void rt_statusbar_set_left_text(void *bar, rt_string text);

/// @brief Set center zone text.
/// @param bar StatusBar widget handle.
/// @param text Text to display.
void rt_statusbar_set_center_text(void *bar, rt_string text);

/// @brief Set right zone text.
/// @param bar StatusBar widget handle.
/// @param text Text to display.
void rt_statusbar_set_right_text(void *bar, rt_string text);

/// @brief Get left zone text.
/// @param bar StatusBar widget handle.
/// @return Left zone text.
rt_string rt_statusbar_get_left_text(void *bar);

/// @brief Get center zone text.
/// @param bar StatusBar widget handle.
/// @return Center zone text.
rt_string rt_statusbar_get_center_text(void *bar);

/// @brief Get right zone text.
/// @param bar StatusBar widget handle.
/// @return Right zone text.
rt_string rt_statusbar_get_right_text(void *bar);

/// @brief Add a text item to the status bar.
/// @param bar StatusBar widget handle.
/// @param text Text to display.
/// @param zone Zone (RT_STATUSBAR_ZONE_*).
/// @return StatusBarItem handle.
void *rt_statusbar_add_text(void *bar, rt_string text, int64_t zone);

/// @brief Add a button item to the status bar.
/// @param bar StatusBar widget handle.
/// @param text Button text.
/// @param zone Zone (RT_STATUSBAR_ZONE_*).
/// @return StatusBarItem handle.
void *rt_statusbar_add_button(void *bar, rt_string text, int64_t zone);

/// @brief Add a progress item to the status bar.
/// @param bar StatusBar widget handle.
/// @param zone Zone (RT_STATUSBAR_ZONE_*).
/// @return StatusBarItem handle.
void *rt_statusbar_add_progress(void *bar, int64_t zone);

/// @brief Add a separator to the status bar.
/// @param bar StatusBar widget handle.
/// @param zone Zone (RT_STATUSBAR_ZONE_*).
/// @return StatusBarItem handle.
void *rt_statusbar_add_separator(void *bar, int64_t zone);

/// @brief Add a spacer to the status bar.
/// @param bar StatusBar widget handle.
/// @param zone Zone (RT_STATUSBAR_ZONE_*).
/// @return StatusBarItem handle.
void *rt_statusbar_add_spacer(void *bar, int64_t zone);

/// @brief Remove an item from the status bar.
/// @param bar StatusBar widget handle.
/// @param item StatusBarItem handle.
void rt_statusbar_remove_item(void *bar, void *item);

/// @brief Clear all items from the status bar.
/// @param bar StatusBar widget handle.
void rt_statusbar_clear(void *bar);

/// @brief Set status bar visibility.
/// @param bar StatusBar widget handle.
/// @param visible 1 for visible, 0 for hidden.
void rt_statusbar_set_visible(void *bar, int64_t visible);

/// @brief Check if status bar is visible.
/// @param bar StatusBar widget handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_statusbar_is_visible(void *bar);

//=========================================================================
// StatusBarItem Widget (Phase 3)
//=========================================================================

/// @brief Set status bar item text.
/// @param item StatusBarItem handle.
/// @param text New text.
void rt_statusbaritem_set_text(void *item, rt_string text);

/// @brief Set status bar item text color.
/// @param item StatusBarItem handle.
/// @param color Text color as 0xRRGGBB.
void rt_statusbaritem_set_text_color(void *item, int64_t color);

/// @brief Set (or clear) a named scalable vector icon on a status bar item (ADR 0137).
/// @param item StatusBarItem handle.
/// @param name Icon name from the vg_icon_vector library; empty clears.
void rt_statusbaritem_set_icon_name(void *item, rt_string name);

/// @brief Get status bar item text.
/// @param item StatusBarItem handle.
/// @return Item text.
rt_string rt_statusbaritem_get_text(void *item);

/// @brief Set status bar item tooltip.
/// @param item StatusBarItem handle.
/// @param tooltip Tooltip text.
void rt_statusbaritem_set_tooltip(void *item, rt_string tooltip);

/// @brief Set status bar item progress value.
/// @param item StatusBarItem handle.
/// @param value Progress value (0.0-1.0).
void rt_statusbaritem_set_progress(void *item, double value);

/// @brief Get status bar item progress value.
/// @param item StatusBarItem handle.
/// @return Progress value (0.0-1.0).
double rt_statusbaritem_get_progress(void *item);

/// @brief Set status bar item visibility.
/// @param item StatusBarItem handle.
/// @param visible 1 for visible, 0 for hidden.
void rt_statusbaritem_set_visible(void *item, int64_t visible);

/// @brief Check if status bar item was clicked.
/// @param item StatusBarItem handle.
/// @return 1 if clicked, 0 otherwise.
int64_t rt_statusbaritem_was_clicked(void *item);

//=========================================================================
// Toolbar Widget (Phase 3)
//=========================================================================

/// Toolbar style constants.
typedef enum {
    RT_TOOLBAR_STYLE_ICON_ONLY = 0,
    RT_TOOLBAR_STYLE_TEXT_ONLY = 1,
    RT_TOOLBAR_STYLE_ICON_TEXT = 2,
} rt_toolbar_style_t;

/// Toolbar icon size constants.
typedef enum {
    RT_TOOLBAR_ICON_SMALL = 0,
    RT_TOOLBAR_ICON_MEDIUM = 1,
    RT_TOOLBAR_ICON_LARGE = 2,
} rt_toolbar_icon_size_t;

/// @brief Create a new horizontal toolbar widget.
/// @param parent Parent widget.
/// @return Toolbar widget handle.
void *rt_toolbar_new(void *parent);

/// @brief Create a new vertical toolbar widget.
/// @param parent Parent widget.
/// @return Toolbar widget handle.
void *rt_toolbar_new_vertical(void *parent);

/// @brief Destroy a toolbar widget.
/// @param toolbar Toolbar widget handle.
void rt_toolbar_destroy(void *toolbar);

/// @brief Add a button to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param icon_path Path to icon image.
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle.
void *rt_toolbar_add_button(void *toolbar, rt_string icon_path, rt_string tooltip);

/// @brief Add a button with text to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param icon_path Path to icon image.
/// @param text Button text.
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle.
void *rt_toolbar_add_button_with_text(void *toolbar,
                                      rt_string icon_path,
                                      rt_string text,
                                      rt_string tooltip);

/// @brief Add a button using one of the built-in semantic toolbar icons.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Built-in icon name such as "save", "run", or "debug".
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle, or NULL when the toolbar is invalid.
/// @details Unknown icon names produce a textless button with no icon rather
///          than trapping, so callers can keep using semantic names while older
///          runtimes degrade safely.
void *rt_toolbar_add_named_button(void *toolbar, rt_string icon_name, rt_string tooltip);

/// @brief Add a text button with one of the built-in semantic toolbar icons.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Built-in icon name such as "save", "run", or "debug".
/// @param text Button text.
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle, or NULL when the toolbar is invalid.
void *rt_toolbar_add_named_button_with_text(void *toolbar,
                                            rt_string icon_name,
                                            rt_string text,
                                            rt_string tooltip);

/// @brief Add a toggle button to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param icon_path Path to icon image.
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle.
void *rt_toolbar_add_toggle(void *toolbar, rt_string icon_path, rt_string tooltip);

/// @brief Add a toggle button using one of the built-in semantic toolbar icons.
/// @param toolbar Toolbar widget handle.
/// @param icon_name Built-in icon name such as "explorer" or "source-control".
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle, or NULL when the toolbar is invalid.
void *rt_toolbar_add_named_toggle(void *toolbar, rt_string icon_name, rt_string tooltip);

/// @brief Add a separator to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @return ToolbarItem handle.
void *rt_toolbar_add_separator(void *toolbar);

/// @brief Add a spacer to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @return ToolbarItem handle.
void *rt_toolbar_add_spacer(void *toolbar);

/// @brief Add a dropdown button to the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param tooltip Tooltip text.
/// @return ToolbarItem handle.
void *rt_toolbar_add_dropdown(void *toolbar, rt_string tooltip);

/// @brief Remove an item from the toolbar.
/// @param toolbar Toolbar widget handle.
/// @param item ToolbarItem handle.
void rt_toolbar_remove_item(void *toolbar, void *item);

/// @brief Set toolbar icon size.
/// @param toolbar Toolbar widget handle.
/// @param size Icon size (RT_TOOLBAR_ICON_*).
void rt_toolbar_set_icon_size(void *toolbar, int64_t size);

/// @brief Get toolbar icon size.
/// @param toolbar Toolbar widget handle.
/// @return Icon size (RT_TOOLBAR_ICON_*).
int64_t rt_toolbar_get_icon_size(void *toolbar);

/// @brief Set toolbar style.
/// @param toolbar Toolbar widget handle.
/// @param style Style (RT_TOOLBAR_STYLE_*).
void rt_toolbar_set_style(void *toolbar, int64_t style);

/// @brief Get number of items in the toolbar.
/// @param toolbar Toolbar widget handle.
/// @return Number of items.
int64_t rt_toolbar_get_item_count(void *toolbar);

/// @brief Get a toolbar item by index.
/// @param toolbar Toolbar widget handle.
/// @param index Item index.
/// @return ToolbarItem handle.
void *rt_toolbar_get_item(void *toolbar, int64_t index);

/// @brief Set toolbar visibility.
/// @param toolbar Toolbar widget handle.
/// @param visible 1 for visible, 0 for hidden.
void rt_toolbar_set_visible(void *toolbar, int64_t visible);

/// @brief Check if toolbar is visible.
/// @param toolbar Toolbar widget handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_toolbar_is_visible(void *toolbar);

//=========================================================================
// ToolbarItem Widget (Phase 3)
//=========================================================================

/// @brief Set toolbar item icon from path.
/// @param item ToolbarItem handle.
/// @param icon_path Path to icon image.
void rt_toolbaritem_set_icon(void *item, rt_string icon_path);

/// @brief Set toolbar item icon from pixels.
/// @param item ToolbarItem handle.
/// @param pixels Pixels handle rendered as an image icon.
void rt_toolbaritem_set_icon_pixels(void *item, void *pixels);

/// @brief Set toolbar item icon from a built-in semantic icon name.
/// @param item ToolbarItem handle.
/// @param icon_name Built-in icon name such as "save", "run", or "find".
/// @details Unknown icon names clear the icon to VG_ICON_NONE.
void rt_toolbaritem_set_named_icon(void *item, rt_string icon_name);

/// @brief Set toolbar item text.
/// @param item ToolbarItem handle.
/// @param text Item text.
void rt_toolbaritem_set_text(void *item, rt_string text);

/// @brief Set toolbar item tooltip.
/// @param item ToolbarItem handle.
/// @param tooltip Tooltip text.
void rt_toolbaritem_set_tooltip(void *item, rt_string tooltip);

/// @brief Set toolbar item enabled state.
/// @param item ToolbarItem handle.
/// @param enabled 1 for enabled, 0 for disabled.
void rt_toolbaritem_set_enabled(void *item, int64_t enabled);

/// @brief Check if toolbar item is enabled.
/// @param item ToolbarItem handle.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_toolbaritem_is_enabled(void *item);

/// @brief Show or hide a toolbar item without removing it (ADR 0220).
/// @param item ToolbarItem handle.
/// @param visible Non-zero shows the item; zero hides it and its spacing.
void rt_toolbaritem_set_visible(void *item, int64_t visible);

/// @brief Check whether a toolbar item is currently shown.
/// @param item ToolbarItem handle.
/// @return 1 when visible, otherwise 0.
int64_t rt_toolbaritem_is_visible(void *item);

/// @brief On-screen left edge of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Logical X, or 0 when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_x(void *item);

/// @brief On-screen top edge of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Logical Y, or 0 when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_y(void *item);

/// @brief On-screen width of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Item width, or 0 when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_width(void *item);

/// @brief On-screen height of a directly visible toolbar item (ADR 0220).
/// @param item ToolbarItem handle.
/// @return Item height, or 0 when the item has no on-screen geometry.
double rt_toolbaritem_get_screen_height(void *item);

/// @brief Set toolbar item toggle state.
/// @param item ToolbarItem handle.
/// @param toggled 1 for toggled, 0 for not toggled.
void rt_toolbaritem_set_toggled(void *item, int64_t toggled);

/// @brief Check if toolbar item is toggled.
/// @param item ToolbarItem handle.
/// @return 1 if toggled, 0 otherwise.
int64_t rt_toolbaritem_is_toggled(void *item);

/// @brief Check if toolbar item was clicked.
/// @param item ToolbarItem handle.
/// @return 1 if clicked, 0 otherwise.
int64_t rt_toolbaritem_was_clicked(void *item);

//=========================================================================
// CodeEditor Enhancements - Syntax Highlighting (Phase 4)
//=========================================================================

/// Token type constants for syntax highlighting.
typedef enum {
    RT_TOKEN_NONE = 0,
    RT_TOKEN_KEYWORD = 1,
    RT_TOKEN_TYPE = 2,
    RT_TOKEN_STRING = 3,
    RT_TOKEN_NUMBER = 4,
    RT_TOKEN_COMMENT = 5,
    RT_TOKEN_OPERATOR = 6,
    RT_TOKEN_FUNCTION = 7,
    RT_TOKEN_VARIABLE = 8,
    RT_TOKEN_CONSTANT = 9,
    RT_TOKEN_ERROR = 10,
} rt_token_type_t;

/// @brief Set syntax highlighting language.
/// @param editor CodeEditor handle.
/// @param language Language identifier ("zia", "basic", "il").
void rt_codeeditor_set_language(void *editor, rt_string language);

/// @brief Set color for a token type.
/// @param editor CodeEditor handle.
/// @param token_type Token type (RT_TOKEN_*).
/// @param color Color value (0xAARRGGBB).
void rt_codeeditor_set_token_color(void *editor, int64_t token_type, int64_t color);

/// @brief Set custom keywords for highlighting.
/// @param editor CodeEditor handle.
/// @param keywords Comma-separated list of keywords.
void rt_codeeditor_set_custom_keywords(void *editor, rt_string keywords);

/// @brief Clear all syntax highlights.
/// @param editor CodeEditor handle.
void rt_codeeditor_clear_highlights(void *editor);

/// @brief Add a syntax highlight region.
/// @param editor CodeEditor handle.
/// @param start_line Starting line (0-based).
/// @param start_col Starting column (0-based).
/// @param end_line Ending line (0-based).
/// @param end_col Ending column (0-based).
/// @param token_type Token type (RT_TOKEN_*).
void rt_codeeditor_add_highlight(void *editor,
                                 int64_t start_line,
                                 int64_t start_col,
                                 int64_t end_line,
                                 int64_t end_col,
                                 int64_t token_type);

/// @brief Refresh syntax highlights.
/// @param editor CodeEditor handle.
void rt_codeeditor_refresh_highlights(void *editor);

/// @brief Overlay a compiler-classified color on an identifier range.
/// @param editor CodeEditor handle.
/// @param line 0-based line.
/// @param start 0-based start column (inclusive).
/// @param end 0-based end column (exclusive).
/// @param token_type vg_syntax_token_type value.
void rt_codeeditor_add_semantic_token(
    void *editor, int64_t line, int64_t start, int64_t end, int64_t token_type);

/// @brief Drop the semantic-token overlay and restore lexical colors.
/// @param editor CodeEditor handle.
void rt_codeeditor_clear_semantic_tokens(void *editor);

/// @brief Add display-only inlay hint text anchored to a source position.
/// @param editor CodeEditor handle.
/// @param line Zero-based source line.
/// @param col Zero-based source column.
/// @param text Hint text.
/// @param color ARGB text color.
void rt_codeeditor_add_inlay_hint(
    void *editor, int64_t line, int64_t col, rt_string text, int64_t color);

/// @brief Clear all inlay hints.
/// @param editor CodeEditor handle.
void rt_codeeditor_clear_inlay_hints(void *editor);

/// @brief Return active inlay hint count.
/// @param editor CodeEditor handle.
/// @return Number of active inlay hints.
int64_t rt_codeeditor_get_inlay_hint_count(void *editor);

//=========================================================================
// CodeEditor Enhancements - Gutter & Line Numbers (Phase 4)
//=========================================================================

/// @brief Set whether to show line numbers.
/// @param editor CodeEditor handle.
/// @param show 1 to show, 0 to hide.
void rt_codeeditor_set_show_line_numbers(void *editor, int64_t show);

/// @brief Check if line numbers are shown.
/// @param editor CodeEditor handle.
/// @return 1 if shown, 0 otherwise.
int64_t rt_codeeditor_get_show_line_numbers(void *editor);

/// @brief Set line number width.
/// @param editor CodeEditor handle.
/// @param width Width in characters.
void rt_codeeditor_set_line_number_width(void *editor, int64_t width);

/// @brief Set a gutter icon for a specific line.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
/// @param pixels Pixels handle for icon.
/// @param slot Gutter slot (0=primary, 1=secondary).
void rt_codeeditor_set_gutter_icon(void *editor, int64_t line, void *pixels, int64_t slot);

/// @brief Add/update a change bar (SCM diff marker) on a gutter line.
/// @param editor CodeEditor handle.
/// @param line 0-based line.
/// @param color Bar color (0xAARRGGBB).
/// @param slot Gutter slot (distinct from breakpoint/diagnostic slots).
void rt_codeeditor_set_gutter_bar(void *editor, int64_t line, int64_t color, int64_t slot);

/// @brief Clear a gutter icon for a specific line.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
/// @param slot Gutter slot (0=primary, 1=secondary).
void rt_codeeditor_clear_gutter_icon(void *editor, int64_t line, int64_t slot);

/// @brief Clear all gutter icons for a slot.
/// @param editor CodeEditor handle.
/// @param slot Gutter slot (0=primary, 1=secondary).
void rt_codeeditor_clear_all_gutter_icons(void *editor, int64_t slot);

/// @brief Check if gutter was clicked this frame.
/// @param editor CodeEditor handle.
/// @return 1 if clicked, 0 otherwise.
int64_t rt_codeeditor_was_gutter_clicked(void *editor);

/// @brief Atomically consume a pending gutter click.
/// @param editor CodeEditor handle.
/// @return Map with `clicked`, `line`, and `slot` fields.
void *rt_codeeditor_take_gutter_click(void *editor);

/// @brief Get the line where gutter was clicked.
/// @param editor CodeEditor handle.
/// @return Line number (0-based), or -1 if no click.
int64_t rt_codeeditor_get_gutter_clicked_line(void *editor);

/// @brief Get the slot where gutter was clicked.
/// @param editor CodeEditor handle.
/// @return Slot number, or -1 if no click.
int64_t rt_codeeditor_get_gutter_clicked_slot(void *editor);

/// @brief Set whether to show fold gutter.
/// @param editor CodeEditor handle.
/// @param show 1 to show, 0 to hide.
void rt_codeeditor_set_show_fold_gutter(void *editor, int64_t show);

//=========================================================================
// CodeEditor Enhancements - Code Folding (Phase 4)
//=========================================================================

/// @brief Add a foldable region.
/// @param editor CodeEditor handle.
/// @param start_line Starting line (0-based).
/// @param end_line Ending line (0-based).
void rt_codeeditor_add_fold_region(void *editor, int64_t start_line, int64_t end_line);

/// @brief Remove a foldable region.
/// @param editor CodeEditor handle.
/// @param start_line Starting line of region to remove.
void rt_codeeditor_remove_fold_region(void *editor, int64_t start_line);

/// @brief Clear all fold regions.
/// @param editor CodeEditor handle.
void rt_codeeditor_clear_fold_regions(void *editor);

/// @brief Fold a region at the specified line.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
void rt_codeeditor_fold(void *editor, int64_t line);

/// @brief Unfold a region at the specified line.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
void rt_codeeditor_unfold(void *editor, int64_t line);

/// @brief Toggle fold state at the specified line.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
void rt_codeeditor_toggle_fold(void *editor, int64_t line);

/// @brief Check if a line is folded.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
/// @return 1 if folded, 0 otherwise.
int64_t rt_codeeditor_is_folded(void *editor, int64_t line);

/// @brief Fold all regions.
/// @param editor CodeEditor handle.
void rt_codeeditor_fold_all(void *editor);

/// @brief Unfold all regions.
/// @param editor CodeEditor handle.
void rt_codeeditor_unfold_all(void *editor);

/// @brief Enable/disable automatic fold region detection.
/// @param editor CodeEditor handle.
/// @param enable 1 to enable, 0 to disable.
void rt_codeeditor_set_auto_fold_detection(void *editor, int64_t enable);

//=========================================================================
// CodeEditor Enhancements - Multiple Cursors (Phase 4)
//=========================================================================

/// @brief Get number of cursors.
/// @param editor CodeEditor handle.
/// @return Number of cursors (always >= 1).
int64_t rt_codeeditor_get_cursor_count(void *editor);

/// @brief Add a new cursor at the specified position.
/// @param editor CodeEditor handle.
/// @param line Line number (0-based).
/// @param col Column number (0-based).
void rt_codeeditor_add_cursor(void *editor, int64_t line, int64_t col);

/// @brief Remove a cursor by index.
/// @param editor CodeEditor handle.
/// @param index Cursor index (0 is primary, cannot be removed).
void rt_codeeditor_remove_cursor(void *editor, int64_t index);

/// @brief Clear all extra cursors, keeping only the primary.
/// @param editor CodeEditor handle.
void rt_codeeditor_clear_extra_cursors(void *editor);

/// @brief Get cursor line by index.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Line number (0-based).
int64_t rt_codeeditor_get_cursor_line_at(void *editor, int64_t index);

/// @brief Get cursor column by index.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Column number (0-based).
int64_t rt_codeeditor_get_cursor_col_at(void *editor, int64_t index);

/// @brief Get primary cursor line (0-based).
/// @param editor CodeEditor handle.
/// @return Line number.
int64_t rt_codeeditor_get_cursor_line(void *editor);

/// @brief Get primary cursor column (0-based).
/// @param editor CodeEditor handle.
/// @return Column number.
int64_t rt_codeeditor_get_cursor_col(void *editor);

/// @brief Set cursor position by index.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @param line Line number (0-based).
/// @param col Column number (0-based).
void rt_codeeditor_set_cursor_position_at(void *editor, int64_t index, int64_t line, int64_t col);

/// @brief Set selection for a specific cursor.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @param start_line Selection start line.
/// @param start_col Selection start column.
/// @param end_line Selection end line.
/// @param end_col Selection end column.
void rt_codeeditor_set_cursor_selection(void *editor,
                                        int64_t index,
                                        int64_t start_line,
                                        int64_t start_col,
                                        int64_t end_line,
                                        int64_t end_col);

/// @brief Check if cursor has a selection.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return 1 if has selection, 0 otherwise.
int64_t rt_codeeditor_cursor_has_selection(void *editor, int64_t index);

/// @brief Get normalized selection start line for a cursor.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Zero-based start line, or 0 if the cursor has no selection.
int64_t rt_codeeditor_get_selection_start_line_at(void *editor, int64_t index);

/// @brief Get normalized selection start column for a cursor.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Zero-based start column, or 0 if the cursor has no selection.
int64_t rt_codeeditor_get_selection_start_col_at(void *editor, int64_t index);

/// @brief Get normalized selection end line for a cursor.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Zero-based end line, or 0 if the cursor has no selection.
int64_t rt_codeeditor_get_selection_end_line_at(void *editor, int64_t index);

/// @brief Get normalized selection end column for a cursor.
/// @param editor CodeEditor handle.
/// @param index Cursor index.
/// @return Zero-based end column, or 0 if the cursor has no selection.
int64_t rt_codeeditor_get_selection_end_col_at(void *editor, int64_t index);

/// @brief Undo the last edit operation.
/// @param editor CodeEditor handle.
void rt_codeeditor_undo(void *editor);

/// @brief Redo the last undone operation.
/// @param editor CodeEditor handle.
void rt_codeeditor_redo(void *editor);

/// @brief Check whether undo is currently available.
/// @param editor CodeEditor handle.
/// @return 1 if undo is available, 0 otherwise.
int64_t rt_codeeditor_can_undo(void *editor);

/// @brief Check whether redo is currently available.
/// @param editor CodeEditor handle.
/// @return 1 if redo is available, 0 otherwise.
int64_t rt_codeeditor_can_redo(void *editor);

/// @brief Copy selected text to clipboard.
/// @param editor CodeEditor handle.
/// @return 1 if text was copied, 0 otherwise.
int64_t rt_codeeditor_copy(void *editor);

/// @brief Cut selected text to clipboard.
/// @param editor CodeEditor handle.
/// @return 1 if text was cut, 0 otherwise.
int64_t rt_codeeditor_cut(void *editor);

/// @brief Paste text from clipboard.
/// @param editor CodeEditor handle.
/// @return 1 if text was pasted, 0 otherwise.
int64_t rt_codeeditor_paste(void *editor);

/// @brief Select all text in the editor.
/// @param editor CodeEditor handle.
void rt_codeeditor_select_all(void *editor);

/// @brief Set tab width in spaces.
/// @param editor CodeEditor handle.
/// @param size Tab width, clamped to a sane editor range.
void rt_codeeditor_set_tab_size(void *editor, int64_t size);

/// @brief Get tab width in spaces.
/// @param editor CodeEditor handle.
/// @return Tab width in spaces.
int64_t rt_codeeditor_get_tab_size(void *editor);

/// @brief Set whether Tab inserts spaces or a hard tab.
/// @param editor CodeEditor handle.
/// @param enabled Non-zero to insert spaces.
void rt_codeeditor_set_insert_spaces(void *editor, int64_t enabled);

/// @brief Check whether Tab inserts spaces.
/// @param editor CodeEditor handle.
/// @return 1 if spaces are inserted, 0 for hard tabs.
int64_t rt_codeeditor_get_insert_spaces(void *editor);

/// @brief Enable or disable display-only word wrapping.
/// @param editor CodeEditor handle.
/// @param enabled Non-zero to enable.
void rt_codeeditor_set_word_wrap(void *editor, int64_t enabled);

/// @brief Enable or disable ligature shaping for one editor (ADR 0137).
/// @param editor CodeEditor widget handle.
/// @param enabled Non-zero renders liga/calt ligatures (default); zero shows
///        plain per-character glyphs.
void rt_codeeditor_set_ligatures_enabled(void *editor, int64_t enabled);

/// @brief Return whether one editor renders ligatures.
/// @param editor CodeEditor widget handle.
/// @return One when ligatures render, otherwise zero.
int64_t rt_codeeditor_get_ligatures_enabled(void *editor);

/// @brief Check whether display-only word wrapping is enabled.
/// @param editor CodeEditor handle.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_codeeditor_get_word_wrap(void *editor);

/// @brief Set whitespace-marker rendering mode (0=none, 1=boundary, 2=all).
/// @param editor CodeEditor handle.
/// @param mode Whitespace mode; out-of-range values clamp to none.
void rt_codeeditor_set_whitespace_mode(void *editor, int64_t mode);

/// @brief Return the current whitespace-marker rendering mode.
/// @param editor CodeEditor handle.
/// @return 0=none, 1=boundary, 2=all.
int64_t rt_codeeditor_get_whitespace_mode(void *editor);

/// @brief `EditorBuffer.New` — detached per-document editor state from text.
/// @param text Initial document text.
/// @return Detached EditorBuffer handle.
void *rt_editorbuffer_new(rt_string text);
/// @brief `EditorBuffer.get_Text` — full document text.
/// @param handle EditorBuffer handle.
/// @return Owned full document text.
rt_string rt_editorbuffer_get_text(void *handle);
/// @brief `EditorBuffer.get_Revision` — content revision.
/// @param handle EditorBuffer handle.
/// @return Current content revision, or 0 when unavailable.
int64_t rt_editorbuffer_get_revision(void *handle);
/// @brief `EditorBuffer.IsModified`.
/// @param handle EditorBuffer handle.
/// @return 1 when content has changed since the modified flag was cleared, otherwise 0.
int64_t rt_editorbuffer_is_modified(void *handle);
/// @brief `EditorBuffer.ClearModified`.
/// @param handle EditorBuffer handle.
void rt_editorbuffer_clear_modified(void *handle);
/// @brief Apply an undoable complete-text replacement to a detached buffer.
/// @param handle EditorBuffer handle.
/// @param text New complete text.
/// @return 1 when applied or already equal; otherwise 0.
int64_t rt_editorbuffer_replace_all_text(void *handle, rt_string text);
/// @brief `CodeEditor.AttachBuffer` — swap the editor's document for a buffer,
///        returning the previous document as a new EditorBuffer (buffer consumed).
/// @param editor CodeEditor widget handle.
/// @param bufHandle EditorBuffer handle consumed by the editor.
/// @return Detached EditorBuffer handle containing the editor's previous document.
void *rt_codeeditor_attach_buffer(void *editor, void *bufHandle);

/// @brief Toggle faint vertical indentation guides.
/// @param editor CodeEditor handle.
/// @param enabled Non-zero to draw indent guides.
void rt_codeeditor_set_show_indent_guides(void *editor, int64_t enabled);

/// @brief Check whether indentation guides are drawn.
/// @param editor CodeEditor handle.
/// @return 1 if enabled, 0 otherwise.
int64_t rt_codeeditor_get_show_indent_guides(void *editor);

/// @brief Enable or disable read-only editing mode.
/// @param editor CodeEditor handle.
/// @param enabled Non-zero to reject text mutations while preserving navigation.
void rt_codeeditor_set_read_only(void *editor, int64_t enabled);

/// @brief Check whether read-only editing mode is enabled.
/// @param editor CodeEditor handle.
/// @return 1 if read-only, 0 otherwise.
int64_t rt_codeeditor_get_read_only(void *editor);

/// @brief Reset low-level CodeEditor performance counters.
/// @param editor CodeEditor handle.
void rt_codeeditor_reset_perf_stats(void *editor);

/// @brief Snapshot every low-level CodeEditor performance counter in one Map.
/// @details The schema-versioned result contains the stable keys `totalHeightLinearScans`,
///          `totalVisualRowLinearScans`, `visualRowLinearScans`,
///          `locateVisualRowLinearScans`, `lineHighlightCalls`, `syntaxStateLineScans`,
///          `highlightSpanChecks`, `fullTextCopies`, and `fullTextCopyBytes`. Values saturate at
///          `INT64_MAX`; invalid editors and graphics-disabled builds return the complete schema
///          with zero counters. The caller owns the returned managed Map.
/// @param editor CodeEditor handle to inspect; invalid handles produce zero-valued statistics.
/// @return New `Zanna.Collections.Map` with `schemaVersion=1`, or NULL on allocation failure.
void *rt_codeeditor_get_perf_stats(void *editor);

/// @brief Return how many times full editor text has been materialized.
/// @param editor CodeEditor handle.
/// @return Full-text copy count.
int64_t rt_codeeditor_get_full_text_copy_count(void *editor);

/// @brief Return aggregate line visits from CodeEditor layout/scroll scans.
/// @param editor CodeEditor handle.
/// @return Linear scan count, clamped to INT64_MAX.
int64_t rt_codeeditor_get_layout_linear_scan_count(void *editor);

/// @brief Return syntax-highlighter invocations from editor paint.
/// @param editor CodeEditor handle.
/// @return Highlight call count.
int64_t rt_codeeditor_get_syntax_highlight_call_count(void *editor);

/// @brief Return syntax-state cache line scans.
/// @param editor CodeEditor handle.
/// @return Syntax-state line scan count.
int64_t rt_codeeditor_get_syntax_state_line_scan_count(void *editor);

/// @brief Return highlight spans inspected during editor paint.
/// @param editor CodeEditor handle.
/// @return Highlight span check count.
int64_t rt_codeeditor_get_highlight_span_check_count(void *editor);

/// @brief Return bytes copied by full-editor text materializations.
/// @param editor CodeEditor handle.
/// @return Full-text copied byte count.
int64_t rt_codeeditor_get_full_text_copy_byte_count(void *editor);

//=========================================================================
// Phase 5: MessageBox Dialog
//=========================================================================

/// @brief Shared state-machine values for asynchronous file and message dialogs.
/// @details Values are stable public constants surfaced through `Zanna.GUI.DialogStatus`. Idle is
///          the initial reusable state; Open lasts from successful ShowAsync until one close
///          callback; Accepted and Cancelled are terminal user outcomes; Failed carries a
///          diagnostic through the dialog's GetError operation.
typedef enum {
    RT_GUI_DIALOG_STATUS_IDLE = 0,      ///< Constructed but not currently presented.
    RT_GUI_DIALOG_STATUS_OPEN = 1,      ///< Presented and awaiting a semantic result.
    RT_GUI_DIALOG_STATUS_ACCEPTED = 2,  ///< User accepted or chose a non-cancel action.
    RT_GUI_DIALOG_STATUS_CANCELLED = 3, ///< User cancelled, rejected, escaped, or closed.
    RT_GUI_DIALOG_STATUS_FAILED = 4,    ///< Presentation/result handling failed; inspect GetError.
} rt_gui_dialog_status_t;

/// @brief Semantic roles for localizable custom message-box buttons.
/// @details Roles, rather than translated labels, determine Enter/Escape behavior and outcome
///          classification. Values are surfaced by `Zanna.GUI.DialogButtonRole` and remain ABI
///          stable for persisted application configuration.
typedef enum {
    RT_GUI_DIALOG_BUTTON_NORMAL = 0,      ///< Ordinary action with no implicit keyboard binding.
    RT_GUI_DIALOG_BUTTON_DEFAULT = 1,     ///< Primary Enter-key action.
    RT_GUI_DIALOG_BUTTON_CANCEL = 2,      ///< Escape-key cancellation action.
    RT_GUI_DIALOG_BUTTON_DESTRUCTIVE = 3, ///< Irreversible action styled semantically.
    RT_GUI_DIALOG_BUTTON_ACCEPT = 4,      ///< Explicit acceptance action.
    RT_GUI_DIALOG_BUTTON_REJECT = 5,      ///< Explicit rejection/cancellation action.
    RT_GUI_DIALOG_BUTTON_HELP = 6,        ///< Help action that is neither accept nor cancel.
} rt_gui_dialog_button_role_t;

/// MessageBox type constants.
typedef enum {
    RT_MESSAGEBOX_INFO = 0,
    RT_MESSAGEBOX_WARNING = 1,
    RT_MESSAGEBOX_ERROR = 2,
    RT_MESSAGEBOX_QUESTION = 3,
} rt_messagebox_type_t;

/// @brief Show an info message box.
/// @param title Dialog title.
/// @param message Dialog message.
/// @return Button index clicked (0 = OK).
int64_t rt_messagebox_info(rt_string title, rt_string message);

/// @brief Show a warning message box.
/// @param title Dialog title.
/// @param message Dialog message.
/// @return Button index clicked (0 = OK).
int64_t rt_messagebox_warning(rt_string title, rt_string message);

/// @brief Show an error message box.
/// @param title Dialog title.
/// @param message Dialog message.
/// @return Button index clicked (0 = OK).
int64_t rt_messagebox_error(rt_string title, rt_string message);

/// @brief Show a yes/no question dialog.
/// @param title Dialog title.
/// @param message Dialog message.
/// @return 1 = Yes, 0 = No.
int64_t rt_messagebox_question(rt_string title, rt_string message);

/// @brief Show an OK/Cancel confirmation dialog.
/// @param title Dialog title.
/// @param message Dialog message.
/// @return 1 = OK, 0 = Cancel.
int64_t rt_messagebox_confirm(rt_string title, rt_string message);

/// @brief Show a prompt dialog with a text input field.
/// @param title Dialog title.
/// @param message Prompt label shown above the input.
/// @return Text entered by the user, or empty string if cancelled.
rt_string rt_messagebox_prompt(rt_string title, rt_string message);

/// @brief Show a prompt and preserve the distinction between acceptance of empty text and cancel.
/// @details This synchronous compatibility companion returns `Some(text)` for every accepted
///          value, including `Some("")`, and `None` for cancel, missing app, graphics-disabled
///          operation, or allocation failure.
/// @param title Dialog title copied into the lower retained dialog.
/// @param message Prompt label shown above the editable field.
/// @return Managed `Zanna.Option[str]` owned by the caller's runtime graph.
void *rt_messagebox_prompt_option(rt_string title, rt_string message);

/// @brief Create a custom message box.
/// @param title Dialog title.
/// @param message Dialog message.
/// @param type Message type (RT_MESSAGEBOX_INFO, etc.).
/// @return MessageBox handle.
void *rt_messagebox_new(rt_string title, rt_string message, int64_t type);

/// @brief Create a custom info message box.
/// @param title Dialog title.
/// @param message Informational message.
/// @return MessageBox handle.
void *rt_messagebox_new_info(rt_string title, rt_string message);

/// @brief Create a custom warning message box.
/// @param title Dialog title.
/// @param message Warning message.
/// @return MessageBox handle.
void *rt_messagebox_new_warning(rt_string title, rt_string message);

/// @brief Create a custom error message box.
/// @param title Dialog title.
/// @param message Error message.
/// @return MessageBox handle.
void *rt_messagebox_new_error(rt_string title, rt_string message);

/// @brief Create a custom question message box.
/// @param title Dialog title.
/// @param message Question text.
/// @return MessageBox handle.
void *rt_messagebox_new_question(rt_string title, rt_string message);

/// @brief Add a button to a custom message box.
/// @param box MessageBox handle.
/// @param text Button text.
/// @param id Button ID (returned when clicked).
void rt_messagebox_add_button(void *box, rt_string text, int64_t id);

/// @brief Add a localizable custom button with an explicit semantic role.
/// @details IDs must be unique within @p box. A duplicate traps with
///          `Message box button ID must be unique: <id>` and leaves all button state unchanged.
///          The label is copied and may be translated independently of its role.
/// @param box Live MessageBox wrapper; invalid/destroyed handles are ignored.
/// @param text UTF-8 visible label; embedded NUL bytes are replaced at the GUI boundary.
/// @param id Stable application-defined result identifier.
/// @param role One of @ref rt_gui_dialog_button_role_t; invalid values use Normal.
void rt_messagebox_add_button_with_role(void *box, rt_string text, int64_t id, int64_t role);

/// @brief Change the semantic role of an existing custom button.
/// @details Default and cancel bindings are recomputed immediately without inspecting the label.
///          Invalid roles and missing IDs preserve the previous configuration.
/// @param box Live MessageBox wrapper.
/// @param id Stable button identifier.
/// @param role One of @ref rt_gui_dialog_button_role_t.
/// @return 1 when the button was found and updated, otherwise 0.
int64_t rt_messagebox_set_button_role(void *box, int64_t id, int64_t role);

/// @brief Select the explicit Escape/cancel button by stable ID.
/// @details Any previous cancel role remains semantically recorded but only the selected button
///          receives the lower dialog's Escape binding. Missing IDs preserve the prior binding.
/// @param box Live MessageBox wrapper.
/// @param id Existing button identifier.
/// @return 1 when the cancel binding was set, otherwise 0.
int64_t rt_messagebox_set_cancel_button(void *box, int64_t id);

/// @brief Set the default button for a message box.
/// @param box MessageBox handle.
/// @param id Button ID to make default.
/// @return 1 when @p id names an existing button, otherwise 0.
int64_t rt_messagebox_set_default_button(void *box, int64_t id);

/// @brief Present a MessageBox through ordinary app frames without nested polling.
/// @details The owning app routes events and rendering until completion. Reopening the same live
///          object traps with `GUI dialog is already open`; setup failure records Failed status and
///          one completion edge.
/// @param box Live MessageBox wrapper.
/// @return 1 when presentation started, otherwise 0.
int64_t rt_messagebox_show_async(void *box);

/// @brief Query whether a stateful MessageBox is currently presented.
/// @param box MessageBox wrapper; invalid/destroyed handles are treated as closed.
/// @return 1 only in the Open state, otherwise 0.
int64_t rt_messagebox_is_open(void *box);

/// @brief Consume one exactly-once MessageBox completion edge.
/// @details Accepted, cancelled, and failed completions each produce one edge. Status and result
///          remain non-consuming and independently readable.
/// @param box Registered MessageBox wrapper.
/// @return 1 when an unread completion exists, otherwise 0.
int64_t rt_messagebox_was_completed(void *box);

/// @brief Return the current asynchronous MessageBox status.
/// @param box Registered MessageBox wrapper.
/// @return One of @ref rt_gui_dialog_status_t; invalid handles report Failed.
int64_t rt_messagebox_get_status(void *box);

/// @brief Return the selected custom button ID from the most recent completion.
/// @param box Registered MessageBox wrapper.
/// @return Application-defined ID, legacy preset code, or -1 before selection/failure/invalid
///         handle. A preset Cancel retains compatibility code 2.
int64_t rt_messagebox_get_result(void *box);

/// @brief Return the stable diagnostic for the most recent MessageBox failure.
/// @param box Registered MessageBox wrapper.
/// @return Owned runtime string; empty when no error is recorded.
rt_string rt_messagebox_get_error(void *box);

/// @brief Show the message box and wait for user response.
/// @param box MessageBox handle.
/// @return ID of clicked button.
int64_t rt_messagebox_show(void *box);

/// @brief Destroy a message box.
/// @param box MessageBox handle.
void rt_messagebox_destroy(void *box);

//=========================================================================
// Phase 5: FileDialog
//=========================================================================

/// FileDialog mode constants.
typedef enum {
    RT_FILEDIALOG_OPEN = 0,
    RT_FILEDIALOG_SAVE = 1,
    RT_FILEDIALOG_FOLDER = 2,
} rt_filedialog_mode_t;

/// @brief Show a file open dialog (quick version).
/// @param title Dialog title.
/// @param default_path Default directory path.
/// @param filter File filter (e.g., "*.txt;*.md").
/// @return Selected file path, or empty string if cancelled.
rt_string rt_filedialog_open(rt_string title, rt_string default_path, rt_string filter);

/// @brief Show the single-file picker and return an explicit optional result.
/// @details Uses the same platform implementation as @ref rt_filedialog_open but maps cancel,
///          unavailable graphics, and setup failure to `None` instead of an empty-string sentinel.
/// @param title Dialog title.
/// @param default_path Initial directory path.
/// @param filter Semicolon-delimited glob filter.
/// @return Managed `Zanna.Option[str]` containing the selected non-empty path, or `None`.
void *rt_filedialog_open_option(rt_string title, rt_string default_path, rt_string filter);

/// @brief Show a file open dialog for multiple files.
/// @param title Dialog title.
/// @param default_path Default directory path.
/// @param filter File filter.
/// @return Escaped semicolon-separated list of paths, or empty string if cancelled.
rt_string rt_filedialog_open_multiple(rt_string title, rt_string default_path, rt_string filter);

/// @brief Show the multi-file picker and return decoded paths as an owned sequence.
/// @details Literal semicolons and backslashes in path names are decoded from the legacy escaped
///          representation. Cancellation/failure returns an empty sequence.
/// @param title Dialog title.
/// @param default_path Initial directory path.
/// @param filter Semicolon-delimited glob filter.
/// @return Managed owned `Seq[str]` independent of dialog lifetime.
void *rt_filedialog_open_multiple_seq(rt_string title, rt_string default_path, rt_string filter);

/// @brief Count entries in the escaped list returned by rt_filedialog_open_multiple.
/// @param escaped Escaped path list.
/// @return Number of paths in the list.
int64_t rt_filedialog_path_list_count(rt_string escaped);

/// @brief Decode one entry from the escaped list returned by rt_filedialog_open_multiple.
/// @param escaped Escaped path list.
/// @param index Zero-based path index.
/// @return Decoded path, or empty string when out of range.
rt_string rt_filedialog_path_list_get(rt_string escaped, int64_t index);

/// @brief Show a file save dialog (quick version).
/// @param title Dialog title.
/// @param default_path Default directory path.
/// @param filter File filter.
/// @param default_name Default file name.
/// @return Selected file path, or empty string if cancelled.
rt_string rt_filedialog_save(rt_string title,
                             rt_string default_path,
                             rt_string filter,
                             rt_string default_name);

/// @brief Show the save picker and return an explicit optional path.
/// @param title Dialog title.
/// @param default_path Initial directory path.
/// @param filter Semicolon-delimited glob filter.
/// @param default_name Initial save filename.
/// @return `Some(path)` after acceptance, otherwise `None`.
void *rt_filedialog_save_option(rt_string title,
                                rt_string default_path,
                                rt_string filter,
                                rt_string default_name);

/// @brief Show a folder selection dialog (quick version).
/// @param title Dialog title.
/// @param default_path Default directory path.
/// @return Selected folder path, or empty string if cancelled.
rt_string rt_filedialog_select_folder(rt_string title, rt_string default_path);

/// @brief Show the folder picker and return an explicit optional path.
/// @param title Dialog title.
/// @param default_path Initial directory path.
/// @return `Some(path)` after acceptance, otherwise `None`.
void *rt_filedialog_select_folder_option(rt_string title, rt_string default_path);

/// @brief Create a custom file dialog.
/// @param type Dialog type (RT_FILEDIALOG_OPEN, SAVE, or FOLDER).
/// @return FileDialog handle.
void *rt_filedialog_new(int64_t type);

/// @brief Create a custom open-file dialog.
/// @return FileDialog handle configured to open a file.
void *rt_filedialog_new_open(void);

/// @brief Create a custom save-file dialog.
/// @return FileDialog handle configured to save a file.
void *rt_filedialog_new_save(void);

/// @brief Create a custom select-folder dialog.
/// @return FileDialog handle configured to select a folder.
void *rt_filedialog_new_folder(void);

/// @brief Set the title of a file dialog.
/// @param dialog FileDialog handle.
/// @param title Dialog title.
void rt_filedialog_set_title(void *dialog, rt_string title);

/// @brief Set the initial directory path.
/// @param dialog FileDialog handle.
/// @param path Directory path.
void rt_filedialog_set_path(void *dialog, rt_string path);

/// @brief Set the file filter (replaces existing).
/// @param dialog FileDialog handle.
/// @param name Filter name (e.g., "Text Files").
/// @param pattern Filter pattern (e.g., "*.txt").
void rt_filedialog_set_filter(void *dialog, rt_string name, rt_string pattern);

/// @brief Add an additional file filter.
/// @param dialog FileDialog handle.
/// @param name Filter name.
/// @param pattern Filter pattern.
void rt_filedialog_add_filter(void *dialog, rt_string name, rt_string pattern);

/// @brief Set the default file name (for save dialogs).
/// @param dialog FileDialog handle.
/// @param name Default file name.
void rt_filedialog_set_default_name(void *dialog, rt_string name);

/// @brief Enable/disable multiple file selection.
/// @param dialog FileDialog handle.
/// @param multiple 1 to enable, 0 to disable.
void rt_filedialog_set_multiple(void *dialog, int64_t multiple);

/// @brief Control whether platform-hidden entries appear in the retained picker.
/// @details The setting applies consistently on macOS, Windows, and POSIX adapters and reloads the
///          current directory when changed. Invalid handles are ignored.
/// @param dialog Live FileDialog wrapper.
/// @param show_hidden Non-zero to show hidden files/directories.
void rt_filedialog_set_show_hidden(void *dialog, int64_t show_hidden);

/// @brief Enable or disable overwrite confirmation for save dialogs.
/// @param dialog Live FileDialog wrapper.
/// @param confirm Non-zero to request confirmation before replacing an existing file.
void rt_filedialog_set_confirm_overwrite(void *dialog, int64_t confirm);

/// @brief Configure the extension appended to extensionless save names.
/// @details The extension may include a leading period; empty disables appending. Embedded NUL
///          input and invalid handles preserve the prior setting.
/// @param dialog Live FileDialog wrapper.
/// @param extension UTF-8 extension such as `.zia`.
void rt_filedialog_set_default_extension(void *dialog, rt_string extension);

/// @brief Add an application-specific quick-access directory bookmark.
/// @details The path is copied by the lower dialog and used as both label and destination. Invalid
///          or embedded-NUL input is ignored atomically.
/// @param dialog Live FileDialog wrapper.
/// @param path Absolute UTF-8 directory path.
void rt_filedialog_add_bookmark(void *dialog, rt_string path);

/// @brief Remove default and application-specific bookmarks from a picker.
/// @param dialog Live FileDialog wrapper; invalid handles are ignored.
void rt_filedialog_clear_bookmarks(void *dialog);

/// @brief Present a FileDialog through ordinary app frames without nested polling.
/// @details Reopening an already-open object traps with the stable dialog-open diagnostic. Setup
///          failure records Failed status, an error string, and one completion edge.
/// @param dialog Live FileDialog wrapper.
/// @return 1 when presentation started, otherwise 0.
int64_t rt_filedialog_show_async(void *dialog);

/// @brief Show the file dialog and wait for user response.
/// @param dialog FileDialog handle.
/// @return 1 = OK, 0 = Cancel.
int64_t rt_filedialog_show(void *dialog);

/// @brief Query whether a stateful FileDialog is currently presented.
/// @param dialog FileDialog wrapper; invalid/destroyed handles are treated as closed.
/// @return 1 only while status and lower presentation are both Open.
int64_t rt_filedialog_is_open(void *dialog);

/// @brief Consume one file-dialog completion edge.
/// @details Acceptance, cancellation, and failure each enqueue exactly one edge; status and paths
///          are non-consuming and remain available afterward.
/// @param dialog Registered FileDialog wrapper.
/// @return 1 when an unread completion exists, otherwise 0.
int64_t rt_filedialog_was_completed(void *dialog);

/// @brief Return the current asynchronous file-dialog status.
/// @param dialog Registered FileDialog wrapper.
/// @return One of @ref rt_gui_dialog_status_t; invalid handles report Failed.
int64_t rt_filedialog_get_status(void *dialog);

/// @brief Return the stable diagnostic for the latest file-dialog failure.
/// @param dialog Registered FileDialog wrapper.
/// @return Owned runtime string, empty when no error is recorded.
rt_string rt_filedialog_get_error(void *dialog);

/// @brief Snapshot every accepted path into an independently owned sequence.
/// @param dialog Registered FileDialog wrapper.
/// @return Managed owned `Seq[str]`, empty before acceptance or for invalid handles.
void *rt_filedialog_get_paths(void *dialog);

/// @brief Get the selected path (single selection).
/// @param dialog FileDialog handle.
/// @return Selected path.
rt_string rt_filedialog_get_path(void *dialog);

/// @brief Get the number of selected paths (multiple selection).
/// @param dialog FileDialog handle.
/// @return Number of selected paths.
int64_t rt_filedialog_get_path_count(void *dialog);

/// @brief Get a selected path by index.
/// @param dialog FileDialog handle.
/// @param index Path index.
/// @return Selected path at index.
rt_string rt_filedialog_get_path_at(void *dialog, int64_t index);

/// @brief Destroy a file dialog.
/// @param dialog FileDialog handle.
void rt_filedialog_destroy(void *dialog);

//=========================================================================
// Phase 6: FindBar (Search & Replace)
//=========================================================================

/// @brief Create a new find/replace bar.
/// @param parent Parent widget.
/// @return FindBar handle.
void *rt_findbar_new(void *parent);

/// @brief Destroy a find bar.
/// @param bar FindBar handle.
void rt_findbar_destroy(void *bar);

/// @brief Bind the find bar to a code editor.
/// @param bar FindBar handle.
/// @param editor CodeEditor handle.
void rt_findbar_bind_editor(void *bar, void *editor);

/// @brief Unbind the find bar from the current editor.
/// @param bar FindBar handle.
void rt_findbar_unbind_editor(void *bar);

/// @brief Set find/replace mode.
/// @param bar FindBar handle.
/// @param replace 1 for replace mode, 0 for find only.
void rt_findbar_set_replace_mode(void *bar, int64_t replace);

/// @brief Check if in replace mode.
/// @param bar FindBar handle.
/// @return 1 if replace mode, 0 if find only.
int64_t rt_findbar_is_replace_mode(void *bar);

/// @brief Set the search text.
/// @param bar FindBar handle.
/// @param text Text to search for.
void rt_findbar_set_find_text(void *bar, rt_string text);

/// @brief Get the current search text.
/// @param bar FindBar handle.
/// @return Current live search text from the input widget.
rt_string rt_findbar_get_find_text(void *bar);

/// @brief Set the replacement text.
/// @param bar FindBar handle.
/// @param text Replacement text.
void rt_findbar_set_replace_text(void *bar, rt_string text);

/// @brief Get the current replacement text.
/// @param bar FindBar handle.
/// @return Current live replacement text from the input widget.
rt_string rt_findbar_get_replace_text(void *bar);

/// @brief Enable/disable case-sensitive search.
/// @param bar FindBar handle.
/// @param sensitive 1 for case-sensitive, 0 for case-insensitive.
void rt_findbar_set_case_sensitive(void *bar, int64_t sensitive);

/// @brief Check if case-sensitive search is enabled.
/// @param bar FindBar handle.
/// @return 1 if case-sensitive, 0 otherwise.
int64_t rt_findbar_is_case_sensitive(void *bar);

/// @brief Enable/disable whole word matching.
/// @param bar FindBar handle.
/// @param whole 1 for whole word, 0 for partial.
void rt_findbar_set_whole_word(void *bar, int64_t whole);

/// @brief Check if whole word matching is enabled.
/// @param bar FindBar handle.
/// @return 1 if whole word, 0 otherwise.
int64_t rt_findbar_is_whole_word(void *bar);

/// @brief Enable/disable regex search.
/// @param bar FindBar handle.
/// @param regex 1 to enable regex, 0 for literal.
void rt_findbar_set_regex(void *bar, int64_t regex);

/// @brief Check if regex search is enabled.
/// @param bar FindBar handle.
/// @return 1 if regex enabled, 0 otherwise.
int64_t rt_findbar_is_regex(void *bar);

/// @brief Find next match.
/// @param bar FindBar handle.
/// @return 1 if found, 0 if not found.
int64_t rt_findbar_find_next(void *bar);

/// @brief Find next match and return the current match index as an Option.
/// @details This is the sentinel-free variant of rt_findbar_find_next().
///          Successful searches return SomeI64(current 1-based match index);
///          misses, unbound bars, and disabled-graphics stubs return None.
/// @param bar FindBar handle.
/// @return Opaque Zanna.Option containing an i64 match index, or None.
void *rt_findbar_find_next_option(void *bar);

/// @brief Find previous match.
/// @param bar FindBar handle.
/// @return 1 if found, 0 if not found.
int64_t rt_findbar_find_previous(void *bar);

/// @brief Find previous match and return the current match index as an Option.
/// @details This is the sentinel-free variant of rt_findbar_find_previous().
/// @param bar FindBar handle.
/// @return Opaque Zanna.Option containing an i64 match index, or None.
void *rt_findbar_find_previous_option(void *bar);

/// @brief Replace current match.
/// @param bar FindBar handle.
/// @return 1 if replaced, 0 if no current match or no bound editor.
int64_t rt_findbar_replace(void *bar);

/// @brief Replace all matches.
/// @param bar FindBar handle.
/// @return Number of replacements made.
int64_t rt_findbar_replace_all(void *bar);

/// @brief Get total match count.
/// @param bar FindBar handle.
/// @return Number of matches found.
int64_t rt_findbar_get_match_count(void *bar);

/// @brief Get current match index.
/// @param bar FindBar handle.
/// @return Current match index (1-based), 0 if no matches.
int64_t rt_findbar_get_current_match(void *bar);

/// @brief Set find bar visibility.
/// @param bar FindBar handle.
/// @param visible 1 to show, 0 to hide.
void rt_findbar_set_visible(void *bar, int64_t visible);

/// @brief Check if find bar is visible.
/// @param bar FindBar handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_findbar_is_visible(void *bar);

/// @brief Focus the find bar input.
/// @param bar FindBar handle.
void rt_findbar_focus(void *bar);

//=========================================================================
// Phase 6: CommandPalette
//=========================================================================

/// @brief Create a new command palette.
/// @param parent Parent widget.
/// @return CommandPalette handle.
void *rt_commandpalette_new(void *parent);

/// @brief Destroy a command palette.
/// @param palette CommandPalette handle.
void rt_commandpalette_destroy(void *palette);

/// @brief Add a command to the palette.
/// @param palette CommandPalette handle.
/// @param id Command identifier.
/// @param label Display label.
/// @param category Command category.
void rt_commandpalette_add_command(void *palette,
                                   rt_string id,
                                   rt_string label,
                                   rt_string category);

/// @brief Add a command with a keyboard shortcut.
/// @param palette CommandPalette handle.
/// @param id Command identifier.
/// @param label Display label.
/// @param category Command category.
/// @param shortcut Keyboard shortcut (e.g., "Ctrl+S").
void rt_commandpalette_add_command_with_shortcut(
    void *palette, rt_string id, rt_string label, rt_string category, rt_string shortcut);

/// @brief Remove a command from the palette.
/// @param palette CommandPalette handle.
/// @param id Command identifier to remove.
void rt_commandpalette_remove_command(void *palette, rt_string id);

/// @brief Clear all commands from the palette.
/// @param palette CommandPalette handle.
void rt_commandpalette_clear(void *palette);

/// @brief Show the command palette.
/// @param palette CommandPalette handle.
void rt_commandpalette_show(void *palette);

/// @brief Hide the command palette.
/// @param palette CommandPalette handle.
void rt_commandpalette_hide(void *palette);

/// @brief Check if the command palette is visible.
/// @param palette CommandPalette handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_commandpalette_is_visible(void *palette);

/// @brief Set the placeholder text.
/// @param palette CommandPalette handle.
/// @param text Placeholder text.
void rt_commandpalette_set_placeholder(void *palette, rt_string text);

/// @brief Get the selected command ID.
/// @param palette CommandPalette handle.
/// @return Selected command ID, or empty string if none.
rt_string rt_commandpalette_get_selected_command(void *palette);

/// @brief Check if a command was selected since last check.
/// @param palette CommandPalette handle.
/// @return 1 if command was selected, 0 otherwise.
int64_t rt_commandpalette_was_command_selected(void *palette);

/// @brief Current live query text (never NULL; "" when empty).
/// @param palette CommandPalette handle.
/// @return Owned current query text.
rt_string rt_commandpalette_get_query(void *palette);

/// @brief Query generation counter (bumped on every query change).
/// @param palette CommandPalette handle.
/// @return Monotonic query generation.
int64_t rt_commandpalette_get_query_generation(void *palette);

/// @brief Programmatically set the query text and re-filter.
/// @param palette CommandPalette handle.
/// @param text Replacement query text.
void rt_commandpalette_set_query(void *palette, rt_string text);

/// @brief Enable client-filtered mode (application filters/ranks; widget shows order).
/// @param palette CommandPalette handle.
/// @param enabled Non-zero when the application supplies filtered ordering.
void rt_commandpalette_set_client_filtered(void *palette, int64_t enabled);

//=========================================================================
// Phase 7: Tooltip
//=========================================================================

/// @brief Show a tooltip at the specified position.
/// @param text Tooltip text.
/// @param x X position.
/// @param y Y position.
void rt_tooltip_show(rt_string text, int64_t x, int64_t y);

/// @brief Show a rich tooltip with title and body.
/// @param title Tooltip title.
/// @param body Tooltip body text.
/// @param x X position.
/// @param y Y position.
void rt_tooltip_show_rich(rt_string title, rt_string body, int64_t x, int64_t y);

/// @brief Hide the current tooltip.
void rt_tooltip_hide(void);

/// @brief Set the tooltip show delay.
/// @param delay_ms Delay in milliseconds before showing tooltip.
void rt_tooltip_set_delay(int64_t delay_ms);

/// @brief Set tooltip text for a widget.
/// @param widget Widget handle.
/// @param text Tooltip text.
void rt_widget_set_tooltip(void *widget, rt_string text);

/// @brief Set rich tooltip for a widget.
/// @param widget Widget handle.
/// @param title Tooltip title.
/// @param body Tooltip body text.
void rt_widget_set_tooltip_rich(void *widget, rt_string title, rt_string body);

/// @brief Clear tooltip from a widget.
/// @param widget Widget handle.
void rt_widget_clear_tooltip(void *widget);

//=========================================================================
// Phase 7: Toast/Notifications
//=========================================================================

/// Toast type constants.
typedef enum {
    RT_TOAST_INFO = 0,
    RT_TOAST_SUCCESS = 1,
    RT_TOAST_WARNING = 2,
    RT_TOAST_ERROR = 3,
} rt_toast_type_t;

/// Toast position constants.
typedef enum {
    RT_TOAST_POSITION_TOP_RIGHT = 0,
    RT_TOAST_POSITION_TOP_LEFT = 1,
    RT_TOAST_POSITION_BOTTOM_RIGHT = 2,
    RT_TOAST_POSITION_BOTTOM_LEFT = 3,
    RT_TOAST_POSITION_TOP_CENTER = 4,
    RT_TOAST_POSITION_BOTTOM_CENTER = 5,
} rt_toast_position_t;

/// @brief Show an info toast notification.
/// @param message Toast message.
void rt_toast_info(rt_string message);

/// @brief Show a success toast notification.
/// @param message Toast message.
void rt_toast_success(rt_string message);

/// @brief Show a warning toast notification.
/// @param message Toast message.
void rt_toast_warning(rt_string message);

/// @brief Show an error toast notification.
/// @param message Toast message.
void rt_toast_error(rt_string message);

/// @brief Create a custom toast notification.
/// @param message Toast message.
/// @param type Toast type (RT_TOAST_INFO, etc.).
/// @param duration_ms Duration in milliseconds (0 for sticky).
/// @return Toast handle.
void *rt_toast_new(rt_string message, int64_t type, int64_t duration_ms);

/// @brief Set an action button on a toast.
/// @param toast Toast handle.
/// @param label Action button label.
void rt_toast_set_action(void *toast, rt_string label);

/// @brief Check if the toast action was clicked.
/// @param toast Toast handle.
/// @return 1 if action was clicked, 0 otherwise.
int64_t rt_toast_was_action_clicked(void *toast);

/// @brief Check if the toast was dismissed.
/// @param toast Toast handle.
/// @return 1 if dismissed, 0 otherwise.
int64_t rt_toast_was_dismissed(void *toast);

/// @brief Dismiss a toast notification.
/// @param toast Toast handle.
void rt_toast_dismiss(void *toast);

/// @brief Set the position for toast notifications.
/// @param position Position constant (RT_TOAST_POSITION_*).
void rt_toast_set_position(int64_t position);

/// @brief Set the maximum number of visible toasts.
/// @param count Maximum visible toast count.
void rt_toast_set_max_visible(int64_t count);

/// @brief Dismiss all toast notifications.
void rt_toast_dismiss_all(void);

//=========================================================================
// Phase 8: Breadcrumb
//=========================================================================

/// @brief Create a new breadcrumb widget.
/// @param parent Parent widget.
/// @return Breadcrumb handle.
void *rt_breadcrumb_new(void *parent);

/// @brief Destroy a breadcrumb widget.
/// @param crumb Breadcrumb handle.
void rt_breadcrumb_destroy(void *crumb);

/// @brief Set breadcrumb path from a path string.
/// @param crumb Breadcrumb handle.
/// @param path Path string (e.g., "src/lib/gui").
/// @param separator Path separator (e.g., "/").
void rt_breadcrumb_set_path(void *crumb, rt_string path, rt_string separator);

/// @brief Set breadcrumb items from a comma-separated string.
/// @param crumb Breadcrumb handle.
/// @param items Comma-separated items.
void rt_breadcrumb_set_items(void *crumb, rt_string items);

/// @brief Add an item to the breadcrumb.
/// @param crumb Breadcrumb handle.
/// @param text Item text.
/// @param data User data string.
void rt_breadcrumb_add_item(void *crumb, rt_string text, rt_string data);

/// @brief Clear all breadcrumb items.
/// @param crumb Breadcrumb handle.
void rt_breadcrumb_clear(void *crumb);

/// @brief Check if a breadcrumb item was clicked.
/// @param crumb Breadcrumb handle.
/// @return 1 if item was clicked, 0 otherwise.
int64_t rt_breadcrumb_was_item_clicked(void *crumb);

/// @brief Get the index of the clicked breadcrumb item.
/// @param crumb Breadcrumb handle.
/// @return Clicked item index, or -1 if none.
int64_t rt_breadcrumb_get_clicked_index(void *crumb);

/// @brief Get the data of the clicked breadcrumb item.
/// @param crumb Breadcrumb handle.
/// @return Clicked item data, or empty string if none.
rt_string rt_breadcrumb_get_clicked_data(void *crumb);

/// @brief Set the breadcrumb separator string.
/// @param crumb Breadcrumb handle.
/// @param sep Separator string.
void rt_breadcrumb_set_separator(void *crumb, rt_string sep);

/// @brief Set the maximum number of visible items.
/// @param crumb Breadcrumb handle.
/// @param max Maximum visible items.
void rt_breadcrumb_set_max_items(void *crumb, int64_t max);

/// @brief Show or hide the breadcrumb widget.
/// @param crumb Breadcrumb handle.
/// @param visible Non-zero to show, zero to hide.
void rt_breadcrumb_set_visible(void *crumb, int64_t visible);

/// @brief Check whether the breadcrumb widget is visible.
/// @param crumb Breadcrumb handle.
/// @return 1 if visible, 0 otherwise.
int64_t rt_breadcrumb_is_visible(void *crumb);

//=========================================================================
// Phase 8: Minimap
//=========================================================================

/// @brief Create a new minimap widget.
/// @param parent Parent widget.
/// @return Minimap handle.
void *rt_minimap_new(void *parent);

/// @brief Destroy a minimap widget.
/// @param minimap Minimap handle.
void rt_minimap_destroy(void *minimap);

/// @brief Bind the minimap to a code editor.
/// @param minimap Minimap handle.
/// @param editor CodeEditor handle.
void rt_minimap_bind_editor(void *minimap, void *editor);

/// @brief Unbind the minimap from its editor.
/// @param minimap Minimap handle.
void rt_minimap_unbind_editor(void *minimap);

/// @brief Set the minimap width.
/// @param minimap Minimap handle.
/// @param width Width in pixels.
void rt_minimap_set_width(void *minimap, int64_t width);

/// @brief Show or hide the minimap widget.
/// @param minimap Minimap handle.
/// @param visible 1 to show, 0 to hide.
void rt_minimap_set_visible(void *minimap, int64_t visible);

/// @brief Check whether the minimap widget is visible.
/// @param minimap Minimap handle.
/// @return 1 when visible, 0 when hidden.
int64_t rt_minimap_is_visible(void *minimap);

/// @brief Get the minimap width.
/// @param minimap Minimap handle.
/// @return Width in pixels.
int64_t rt_minimap_get_width(void *minimap);

/// @brief Set the minimap scale factor.
/// @param minimap Minimap handle.
/// @param scale Scale factor (0.05 - 0.2 typical).
void rt_minimap_set_scale(void *minimap, double scale);

/// @brief Show or hide the viewport slider on the minimap.
/// @param minimap Minimap handle.
/// @param show 1 to show, 0 to hide.
void rt_minimap_set_show_slider(void *minimap, int64_t show);

/// @brief Return the minimap's combined observed editor-source revision.
/// @details The revision covers binding, content, visual layout, and viewport transitions and is
///          non-consuming. It saturates at INT64_MAX at the runtime boundary.
/// @param minimap Minimap handle.
/// @return Monotonic source revision, or zero for an invalid handle.
int64_t rt_minimap_get_source_revision(void *minimap);

/// @brief Invalidate a contiguous range of cached source-line summaries.
/// @details Non-positive counts and negative first-line values are ignored. Valid ranges preserve
///          cache entries for all unaffected lines and schedule a repaint.
/// @param minimap Minimap handle.
/// @param first Zero-based first affected source line.
/// @param count Number of affected lines.
void rt_minimap_invalidate_lines(void *minimap, int64_t first, int64_t count);

/// @brief Configure the bounded source-line summary cache.
/// @details Zero disables caching. Values are clamped to one million entries; allocation failure
///          preserves the previous cache and limit.
/// @param minimap Minimap handle.
/// @param count Maximum cached source lines.
void rt_minimap_set_maximum_cached_lines(void *minimap, int64_t count);

/// @brief Return the number of currently valid source-line cache entries.
/// @param minimap Minimap handle.
/// @return Valid entry count, or zero for an invalid handle.
int64_t rt_minimap_get_cached_line_count(void *minimap);

/// Minimap marker type constants.
typedef enum {
    RT_MINIMAP_MARKER_ERROR = 0,
    RT_MINIMAP_MARKER_WARNING = 1,
    RT_MINIMAP_MARKER_INFO = 2,
    RT_MINIMAP_MARKER_SEARCH = 3,
} rt_minimap_marker_t;

/// @brief Add a marker to the minimap.
/// @param minimap Minimap handle.
/// @param line Line number.
/// @param color Marker color (RGBA).
/// @param type Marker type (RT_MINIMAP_MARKER_*).
void rt_minimap_add_marker(void *minimap, int64_t line, int64_t color, int64_t type);

/// @brief Remove markers at a specific line.
/// @param minimap Minimap handle.
/// @param line Line number.
void rt_minimap_remove_markers(void *minimap, int64_t line);

/// @brief Clear all markers from the minimap.
/// @param minimap Minimap handle.
void rt_minimap_clear_markers(void *minimap);

//=========================================================================
// Phase 8: Drag and Drop
//=========================================================================

/// @brief Set whether a widget is draggable.
/// @param widget Widget handle.
/// @param draggable 1 to enable dragging, 0 to disable.
void rt_widget_set_draggable(void *widget, int64_t draggable);

/// @brief Set the drag data for a widget.
/// @param widget Widget handle.
/// @param type Data type identifier.
/// @param data Data string.
void rt_widget_set_drag_data(void *widget, rt_string type, rt_string data);

/// @brief Check if a widget is currently being dragged.
/// @param widget Widget handle.
/// @return 1 if being dragged, 0 otherwise.
int64_t rt_widget_is_being_dragged(void *widget);

/// @brief Set whether a widget is a drop target.
/// @param widget Widget handle.
/// @param target 1 to enable drop target, 0 to disable.
void rt_widget_set_drop_target(void *widget, int64_t target);

/// @brief Set accepted drop types for a widget.
/// @param widget Widget handle.
/// @param types Comma-separated list of accepted types.
void rt_widget_set_accepted_drop_types(void *widget, rt_string types);

/// @brief Check if a drag is currently over a widget.
/// @param widget Widget handle.
/// @return 1 if drag is over widget, 0 otherwise.
int64_t rt_widget_is_drag_over(void *widget);

/// @brief Check if a drop occurred on a widget.
/// @param widget Widget handle.
/// @return 1 if drop occurred, 0 otherwise.
int64_t rt_widget_was_dropped(void *widget);

/// @brief Get the type of the dropped data.
/// @param widget Widget handle.
/// @return Drop type string.
rt_string rt_widget_get_drop_type(void *widget);

/// @brief Get the dropped data.
/// @param widget Widget handle.
/// @return Drop data string.
rt_string rt_widget_get_drop_data(void *widget);

/// @brief Pointer x recorded when the last drop landed on this widget.
/// @param widget Widget handle.
/// @return Root-relative drop X coordinate, or 0 when unavailable.
double rt_widget_get_drop_x(void *widget);

/// @brief Pointer y recorded when the last drop landed on this widget.
/// @param widget Widget handle.
/// @return Root-relative drop Y coordinate, or 0 when unavailable.
double rt_widget_get_drop_y(void *widget);

/// @brief Check if a file was dropped on the application.
/// @param app Application handle.
/// @return 1 if file was dropped, 0 otherwise.
int64_t rt_app_was_file_dropped(void *app);

/// @brief Get the number of dropped files.
/// @param app Application handle.
/// @return Number of dropped files.
int64_t rt_app_get_dropped_file_count(void *app);

/// @brief Get a dropped file path by index.
/// @param app Application handle.
/// @param index File index.
/// @return File path string.
rt_string rt_app_get_dropped_file(void *app, int64_t index);

//=========================================================================
// CodeEditor Completion Helpers
//=========================================================================

/// @brief Get the screen-absolute X pixel coordinate of the primary cursor.
/// @param editor CodeEditor handle.
/// @return X pixel coordinate (top-left of caret).
int64_t rt_codeeditor_get_cursor_pixel_x(void *editor);

/// @brief Get the screen-absolute Y pixel coordinate of the primary cursor.
/// @param editor CodeEditor handle.
/// @return Y pixel coordinate (top-left of caret).
int64_t rt_codeeditor_get_cursor_pixel_y(void *editor);

/// @brief Return the 0-based editor line at a screen-absolute Y coordinate.
/// @param editor CodeEditor handle.
/// @param y Screen-absolute Y coordinate.
/// @return 0-based line index, or -1 when unavailable.
int64_t rt_codeeditor_get_line_at_pixel(void *editor, int64_t y);

/// @brief Return the 0-based editor column at a screen-absolute X/Y coordinate.
/// @param editor CodeEditor handle.
/// @param x Screen-absolute X coordinate.
/// @param y Screen-absolute Y coordinate, used to clamp to the hovered line length.
/// @return 0-based column index, or -1 when unavailable.
int64_t rt_codeeditor_get_col_at_pixel(void *editor, int64_t x, int64_t y);

/// @brief Insert text at the primary cursor position.
/// @param editor CodeEditor handle.
/// @param text Text to insert.
void rt_codeeditor_insert_at_cursor(void *editor, rt_string text);

/// @brief Advance an editor cursor (@p line, @p col) by @p offset characters through @p text,
///        counting newlines (UTF-8 aware: continuation bytes share their leading byte's column).
/// @details Shared by CodeEditor.InsertAndPlaceCursor and unit-testable on its own.
static inline void rt_codeeditor_advance_position(const char *text,
                                                  int64_t offset,
                                                  int64_t *line,
                                                  int64_t *col) {
    if (!text || offset <= 0 || !line || !col)
        return;
    int64_t consumed = 0;
    for (int64_t i = 0; text[i] && consumed < offset; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c & 0xC0) == 0x80)
            continue; // UTF-8 continuation byte: same codepoint as its leading byte
        if (c == '\n') {
            (*line)++;
            *col = 0;
        } else {
            (*col)++;
        }
        consumed++;
    }
}

/// @brief Insert @p text at the primary cursor, then place the caret @p caret_offset characters
///        into the inserted text (counting newlines) — replacing the hand-rolled insert-then-walk
///        idiom for placing the caret inside a multi-line insertion (e.g. a completion snippet).
/// @param editor CodeEditor widget handle.
/// @param text Text to insert.
/// @param caret_offset Number of inserted Unicode characters preceding the final caret.
void rt_codeeditor_insert_and_place_cursor(void *editor, rt_string text, int64_t caret_offset);

/// @brief Return the identifier word under the primary cursor.
/// @param editor CodeEditor handle.
/// @return Word string (may be empty if cursor is not on an identifier).
rt_string rt_codeeditor_get_word_at_cursor(void *editor);

/// @brief Replace the identifier word under the primary cursor.
/// @param editor CodeEditor handle.
/// @param new_text Replacement text.
void rt_codeeditor_replace_word_at_cursor(void *editor, rt_string new_text);

/// @brief Return the text of a single line by 0-based index.
/// @param editor CodeEditor handle.
/// @param line_index 0-based line index.
/// @return Line text string (empty if index out of range).
rt_string rt_codeeditor_get_line(void *editor, int64_t line_index);

//=========================================================================
// FloatingPanel bridge functions
//=========================================================================

/// @brief Create a floating overlay panel attached to @p root.
/// @param root Root widget that owns the overlay.
/// @return FloatingPanel handle.
void *rt_floatingpanel_new(void *root);

/// @brief Destroy a floating panel and its overlay children.
/// @param panel FloatingPanel handle.
void rt_floatingpanel_destroy(void *panel);

/// @brief Set absolute root-relative position in logical units.
/// @details Coordinates cross the owning app's effective UI scale exactly once.
/// @param panel FloatingPanel handle.
/// @param x Root-relative logical X coordinate.
/// @param y Root-relative logical Y coordinate.
void rt_floatingpanel_set_position(void *panel, double x, double y);

/// @brief Center the panel within its parent (root) bounds, clamped to the top-left.
/// @param panel FloatingPanel handle.
void rt_floatingpanel_center_in_parent(void *panel);

/// @brief Set panel dimensions in logical units.
/// @details Lengths cross the owning app's effective UI scale exactly once.
/// @param panel FloatingPanel handle.
/// @param w Logical width.
/// @param h Logical height.
void rt_floatingpanel_set_size(void *panel, double w, double h);

/// @brief Show or hide the panel (1 = show, 0 = hide).
/// @param panel FloatingPanel handle.
/// @param visible Non-zero to show the panel; zero to hide it.
void rt_floatingpanel_set_visible(void *panel, int64_t visible);

/// @brief Add a widget as a private (overlay-only) child.
/// @param panel FloatingPanel handle.
/// @param child Widget to attach to the overlay.
void rt_floatingpanel_add_child(void *panel, void *child);

// GroupBox — titled "card" container for grouping related controls.
/// @brief Create a titled card group box attached to a parent container.
/// @param parent Parent widget handle, or NULL for no parent.
/// @param title Visible group title.
/// @return GroupBox widget handle.
void *rt_groupbox_new(void *parent, rt_string title);
/// @brief Destroy a group box and its children.
/// @param gb GroupBox widget handle.
void rt_groupbox_destroy(void *gb);
/// @brief Replace the group box title text.
/// @param gb GroupBox widget handle.
/// @param title Replacement title text.
void rt_groupbox_set_title(void *gb, rt_string title);
/// @brief Add a control as a child of the group box.
/// @param gb GroupBox widget handle.
/// @param child Widget to attach.
void rt_groupbox_add_child(void *gb, void *child);

#ifdef __cplusplus
}
#endif
