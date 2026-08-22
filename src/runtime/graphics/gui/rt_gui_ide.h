//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_ide.h
// Purpose: GUI automation, virtualization, and command/accessibility helpers
//          for IDE-style applications.
// Key invariants:
//   - Virtual-model stable IDs are unique and non-empty.
//   - Model/control bindings are non-owning and self-clear when either endpoint
//     is destroyed.
//   - New bound virtualization APIs are present in graphics-enabled and
//     graphics-disabled runtime builds.
// Ownership/Lifetime:
//   - Constructors return managed runtime objects owned by the caller.
//   - Returned strings, maps, sequences, and options are managed values.
//   - Bind operations never transfer ownership of a model or GUI control.
// Links: src/runtime/graphics/gui/rt_gui_ide.cpp,
//        src/runtime/graphics/gui/rt_gui.h,
//        src/il/runtime/defs/api/gui_layout.def,
//        docs/adr/0290-virtual-tree-bulk-updates.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_ide.h
/// @brief Declares IDE automation, virtual-model, accessibility, and command runtime APIs.
///
/// @details
/// Constructors return managed runtime objects, while bindings to GUI controls
/// are non-owning and self-invalidating. All returned strings, maps, sequences,
/// and options follow ordinary runtime ownership rules.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_GUI_TEST_HARNESS_CLASS_ID INT64_C(-0x4755495445535401)
#define RT_GUI_VIRTUAL_LIST_CLASS_ID INT64_C(-0x475549564c495354)
#define RT_GUI_VIRTUAL_TREE_CLASS_ID INT64_C(-0x4755495654524545)
#define RT_GUI_COMMAND_STATE_CLASS_ID INT64_C(-0x475549434d445354)
#define RT_GUI_COMMAND_CLASS_ID INT64_C(-0x475549434f4d4e44)          // tag "GUICOMND"
#define RT_GUI_COMMAND_REGISTRY_CLASS_ID INT64_C(-0x475549434d445247) // tag "GUICMDRG"

// --- GUI test harness: retain the legacy synthetic model for headless unit tests,
//     or bind a live App to drive its real input/render/accessibility paths. ---
/// @brief Create a new headless GUI test harness object.
/// @return New managed TestHarness object, or `NULL` on allocation failure.
void *rt_gui_test_harness_new(void);
/// @brief Remove all registered widgets and reset harness state.
/// @param harness Managed TestHarness object; invalid handles trap.
void rt_gui_test_harness_clear(void *harness);
/// @brief Bind a live GUI application for real input dispatch and framebuffer inspection.
/// @details The harness retains @p app until UnbindApp, rebinding, or harness reclamation.
///          Events already present in the legacy synthetic journal are marked dispatched so
///          binding never replays historical input unexpectedly. Invalid or explicitly destroyed
///          app handles are rejected without disturbing an existing live binding. Graphics-
///          disabled runtimes return zero and keep the harness in synthetic mode.
/// @param harness Managed TestHarness object; invalid handles trap.
/// @param app Live managed Zanna.GUI.App object to retain.
/// @return 1 when the app is bound (including an already-current binding), otherwise 0.
int8_t rt_gui_test_harness_bind_app(void *harness, void *app);
/// @brief Release the application retained by a GUI test harness.
/// @details Pending journal records remain inspectable but are marked dispatched, preventing
///          their later replay if another app is bound. Synthetic widgets, focus, and frame count
///          are preserved. Calling this on an already-unbound harness is harmless.
/// @param harness Managed TestHarness object; invalid handles trap.
void rt_gui_test_harness_unbind_app(void *harness);
/// @brief Dispatch every pending harness input record through the bound App.Poll path.
/// @details Key records expand to native key-down/key-up events and, for printable unmodified
///          ASCII, a text-input event. Mouse `click` expands to down/up. Records are attempted
///          exactly once and use physical framebuffer coordinates and ZannaGFX button ordinals
///          (left=0, right=1, middle=2). App.Poll is invoked once even when no records are pending,
///          allowing queued native events to participate in the same frame.
/// @param harness Managed TestHarness object; invalid handles trap.
/// @return Number of journal records fully queued, or zero when no live app is bound.
int64_t rt_gui_test_harness_dispatch_pending(void *harness);
/// @brief Dispatch pending input and render one deterministic real GUI frame.
/// @details Pending authored events are queued before App.RunFrameWithDelta, so the app polls once
///          and then performs normal routing, layout, animation, damage, and presentation. Invalid,
///          negative, or non-finite deltas are normalized by the App API. Synthetic mode and stale
///          app bindings return zero without modifying the journal.
/// @param harness Managed TestHarness object; invalid handles trap.
/// @param delta_ms Deterministic scheduler time to advance in milliseconds.
/// @return 1 while the bound app remains runnable after the frame, otherwise 0.
int8_t rt_gui_test_harness_render_frame(void *harness, double delta_ms);
/// @brief Copy a physical framebuffer region into a managed Pixels image.
/// @details The returned image always has the requested positive dimensions. Samples outside the
///          framebuffer are transparent black, enabling stable edge and overscan assertions.
///          Coordinates and dimensions are physical pixels with a top-left origin. The capture is
///          a deep copy and remains valid after later frames or framebuffer reallocations.
/// @param harness Managed TestHarness object with a live bound app.
/// @param x Physical X coordinate of the requested region.
/// @param y Physical Y coordinate of the requested region.
/// @param width Positive output width in pixels.
/// @param height Positive output height in pixels.
/// @return New managed Zanna.Graphics.Pixels object, or NULL for invalid/unbound input or OOM.
void *rt_gui_test_harness_capture_pixels(
    void *harness, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Hash a physical framebuffer region into a stable lowercase hexadecimal digest.
/// @details Uses FNV-1a 64 over the requested dimensions and canonical RGBA bytes. Out-of-bounds
///          pixels hash as transparent black, and the explicit byte order makes results identical
///          across host endianness. The hash is intended for deterministic regression comparisons,
///          not cryptographic authentication.
/// @param harness Managed TestHarness object with a live bound app.
/// @param x Physical X coordinate of the requested region.
/// @param y Physical Y coordinate of the requested region.
/// @param width Positive region width.
/// @param height Positive region height.
/// @return Newly referenced 16-character hash, or an empty string when capture is unavailable.
rt_string rt_gui_test_harness_capture_hash(
    void *harness, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Compare a Pixels reference against the same-sized bound framebuffer region.
/// @details Per-channel absolute differences at or below @p tolerance are accepted. The returned
///          schema-versioned Map always includes `matches`, dimensions, clamped tolerance,
///          `comparedPixels`, `differentPixels`, `maxChannelDelta`, and `meanAbsoluteError`.
///          Framebuffer samples outside the window are transparent. Invalid expected images or
///          missing bindings return a well-formed non-matching map with zero comparison counts.
/// @param harness Managed TestHarness object; invalid handles trap.
/// @param expected Managed Pixels reference image.
/// @param x Physical framebuffer X coordinate aligned with expected pixel (0,0).
/// @param y Physical framebuffer Y coordinate aligned with expected pixel (0,0).
/// @param tolerance Per-channel tolerance, clamped to the inclusive range [0,255].
/// @return New managed Zanna.Collections.Map with schemaVersion=1 comparison statistics.
void *rt_gui_test_harness_compare_region(
    void *harness, void *expected, int64_t x, int64_t y, int64_t tolerance);
/// @brief Snapshot the bound application's semantic accessibility tree.
/// @details Delegates to the same iterative, schema-versioned accessibility snapshot used by the
///          public runtime. Invisible descendants are omitted and allocation truncation is marked
///          in the root. An unbound, destroyed, or graphics-disabled app returns an empty Map with
///          schemaVersion=1 rather than NULL when allocation succeeds.
/// @param harness Managed TestHarness object; invalid handles trap.
/// @return New managed Zanna.Collections.Map accessibility snapshot, or NULL on root OOM.
void *rt_gui_test_harness_get_accessibility_snapshot(void *harness);
/// @brief Advance the harness by @p frames simulated frames (clamped to ≥1).
/// @param harness Managed TestHarness object; invalid handles trap.
/// @param frames Number of synthetic frames to add.
/// @return The harness's new accumulated frame counter.
int64_t rt_gui_test_harness_tick(void *harness, int64_t frames);
/// @brief Register (or replace by id) a synthetic widget with the given id/type/name
///        and bounds in the harness's widget table.
/// @param harness Managed TestHarness object.
/// @param id Stable widget identifier.
/// @param type Widget type name.
/// @param name Accessible or display name.
/// @param x Synthetic bounds X coordinate.
/// @param y Synthetic bounds Y coordinate.
/// @param w Synthetic bounds width.
/// @param h Synthetic bounds height.
void rt_gui_test_harness_register_widget(void *harness,
                                         rt_string id,
                                         rt_string type,
                                         rt_string name,
                                         int64_t x,
                                         int64_t y,
                                         int64_t w,
                                         int64_t h);
/// @brief Find a registered widget record by exact id (NULL if none).
/// @param harness Managed TestHarness object.
/// @param id Widget identifier to match.
/// @return New query map whose `found` field reports whether a match exists.
void *rt_gui_test_harness_find_by_id(void *harness, rt_string id);
/// @brief Find a registered widget record by exact id as an Option.
/// @details Returns Some(Map) when found and None when no matching widget exists.
/// @param harness Managed TestHarness object.
/// @param id Widget identifier to match.
/// @return Managed Option containing the result map when found.
void *rt_gui_test_harness_find_by_id_option(void *harness, rt_string id);
/// @brief Find the first registered widget record with the given name (NULL if none).
/// @param harness Managed TestHarness object.
/// @param name Widget name to match.
/// @return New query map whose `found` field reports whether a match exists.
void *rt_gui_test_harness_find_by_name(void *harness, rt_string name);
/// @brief Find the first registered widget record with the given name as an Option.
/// @details Returns Some(Map) when found and None when no matching widget exists.
/// @param harness Managed TestHarness object.
/// @param name Widget name to match.
/// @return Managed Option containing the result map when found.
void *rt_gui_test_harness_find_by_name_option(void *harness, rt_string name);
/// @brief Find the first registered widget record of the given type (NULL if none).
/// @param harness Managed TestHarness object.
/// @param type Widget type to match.
/// @return New query map whose `found` field reports whether a match exists.
void *rt_gui_test_harness_find_by_type(void *harness, rt_string type);
/// @brief Find the first registered widget record of the given type as an Option.
/// @details Returns Some(Map) when found and None when no matching widget exists.
/// @param harness Managed TestHarness object.
/// @param type Widget type to match.
/// @return Managed Option containing the result map when found.
void *rt_gui_test_harness_find_by_type_option(void *harness, rt_string type);
/// @brief Inject a key event (@p key plus a modifier bitmask) into the harness.
/// @param harness Managed TestHarness object.
/// @param key Runtime key token.
/// @param modifiers Platform modifier-bit mask.
void rt_gui_test_harness_send_key(void *harness, rt_string key, int64_t modifiers);
/// @brief Inject a mouse event of @p event_type at (@p x, @p y) for the given button.
/// @param harness Managed TestHarness object.
/// @param event_type Runtime mouse action token.
/// @param x Synthetic pointer X coordinate.
/// @param y Synthetic pointer Y coordinate.
/// @param button Platform mouse-button ordinal.
void rt_gui_test_harness_send_mouse(
    void *harness, rt_string event_type, int64_t x, int64_t y, int64_t button);
/// @brief Return the number of synthetic input events recorded by the harness.
/// @param harness Managed TestHarness object.
/// @return Event count saturated to `INT64_MAX`.
int64_t rt_gui_test_harness_event_count(void *harness);
/// @brief Return a Map snapshot for the event at @p index, or found=0 if out of range.
/// @param harness Managed TestHarness object.
/// @param index Zero-based event index.
/// @return New event-result map.
void *rt_gui_test_harness_event_at(void *harness, int64_t index);
/// @brief Remove all recorded synthetic input events without changing widgets or focus.
/// @param harness Managed TestHarness object.
void rt_gui_test_harness_clear_events(void *harness);
/// @brief Return the id of the currently focused widget (empty string if none).
/// @param harness Managed TestHarness object.
/// @return Newly referenced focus identifier.
rt_string rt_gui_test_harness_get_focus(void *harness);
/// @brief Return a sequence of widget ids in registration (focus-traversal) order.
/// @param harness Managed TestHarness object.
/// @return New owned sequence of identifiers, or `NULL` on allocation failure.
void *rt_gui_test_harness_focus_order(void *harness);
/// @brief Capture a synthetic snapshot of a region as a Map with x/y/width/height,
///        `nonBlankPixels` and a `nonBlank` flag (derived from widget coverage, not real pixels).
/// @param harness Managed TestHarness object.
/// @param x Query rectangle X coordinate.
/// @param y Query rectangle Y coordinate.
/// @param w Query rectangle width.
/// @param h Query rectangle height.
/// @return New managed snapshot map, or `NULL` on allocation failure.
void *rt_gui_test_harness_capture_region(void *harness, int64_t x, int64_t y, int64_t w, int64_t h);
/// @brief Read the `nonBlank` flag from a capture_region snapshot; 0 if @p snapshot is not one.
/// @param snapshot Runtime map returned by CaptureRegion.
/// @return `1` when the snapshot reports nonblank content; otherwise `0`.
int8_t rt_gui_test_harness_assert_nonblank(void *snapshot);

// --- Virtualized list: only the rows visible at the current scroll offset are
//     realized, so arbitrarily large row counts stay cheap. ---
/// @brief Create a virtualized list with @p row_count rows of @p row_height pixels
///        shown through a @p viewport_height window.
/// @param row_count Initial non-negative logical row count.
/// @param row_height Row height clamped to at least one pixel.
/// @param viewport_height Viewport height clamped to at least one pixel.
/// @return New managed VirtualList object, or `NULL` on allocation failure.
void *rt_virtual_list_new(int64_t row_count, int64_t row_height, int64_t viewport_height);
/// @brief Update the total row count (clamped to ≥0).
/// @param list Managed VirtualList object; invalid handles trap.
/// @param row_count New logical row count.
void rt_virtual_list_set_count(void *list, int64_t row_count);
/// @brief Assign a stable id to the row at index @p row (ignored if out of range).
/// @param list Managed VirtualList object; invalid handles trap.
/// @param row Zero-based logical row.
/// @param id Non-empty unique stable identifier.
void rt_virtual_list_set_row_id(void *list, int64_t row, rt_string id);
/// @brief Set the UTF-8 display text supplied for one virtual row.
/// @details Text is copied into sparse model storage. Embedded NUL bytes are rendered as visible
///          replacement characters so the lower C provider never truncates subsequent text. An
///          out-of-range row or allocation failure leaves the model unchanged.
/// @param list Managed `Zanna.GUI.VirtualList` object; invalid handles trap.
/// @param row Zero-based logical row index.
/// @param text Managed UTF-8 text to copy; NULL is treated as empty text.
void rt_virtual_list_set_row_text(void *list, int64_t row, rt_string text);
/// @brief Schedule repaint for one row whose application-owned backing data changed.
/// @details This is an O(1) no-op when the model is unbound or @p row is outside the logical
///          range. It does not change row identity, selection, or text stored by SetRowText.
/// @param list Managed `Zanna.GUI.VirtualList` object; invalid handles trap.
/// @param row Zero-based logical row index to invalidate.
void rt_virtual_list_invalidate_row(void *list, int64_t row);
/// @brief Bind a VirtualList to a live ListBox using a non-owning, viewport-backed provider.
/// @details Existing bindings on either endpoint are replaced only after the lower ListBox has
///          allocated its new viewport cache. Destroying either endpoint clears the other raw
///          pointer before reclamation. Graphics-disabled builds return zero.
/// @param list Managed VirtualList object; invalid model handles trap.
/// @param listbox Live `Zanna.GUI.ListBox` handle.
/// @return One on success, zero for an invalid/unavailable ListBox or allocation failure.
int8_t rt_virtual_list_bind(void *list, void *listbox);
/// @brief Detach a VirtualList from its current ListBox without destroying either endpoint.
/// @param list Managed VirtualList object; invalid handles trap.
void rt_virtual_list_unbind(void *list);
/// @brief Bind a ListBox to a VirtualList; reverse-form convenience for VirtualList.Bind.
/// @param listbox Live `Zanna.GUI.ListBox` handle.
/// @param model Managed `Zanna.GUI.VirtualList` object.
/// @return One on success, zero when graphics are unavailable or either endpoint is invalid.
int8_t rt_listbox_set_virtual_model(void *listbox, void *model);
/// @brief Remove an external model from a ListBox and restore ordinary retained-item mode.
/// @details The model is notified before the lower binding storage is released. Invalid or
///          graphics-disabled ListBox handles are harmless no-ops.
/// @param listbox ListBox handle whose model binding should be cleared.
void rt_listbox_clear_virtual_model(void *listbox);
/// @brief Return the first model row intersecting a bound ListBox viewport.
/// @details Computed in O(1) without invoking the model provider. The result is zero for an
///          invalid, unbound, empty, or graphics-disabled ListBox.
/// @param listbox ListBox handle to inspect.
/// @return Non-negative row index, saturated at the runtime signed-integer boundary.
int64_t rt_listbox_get_visible_first(void *listbox);
/// @brief Return the bounded number of model rows materialized by a ListBox viewport.
/// @details Includes the lower renderer's safety rows, does not invoke the provider, and returns
///          zero for invalid, unbound, empty, or graphics-disabled controls.
/// @param listbox ListBox handle to inspect.
/// @return Non-negative viewport row count.
int64_t rt_listbox_get_visible_count(void *listbox);
/// @brief Compute the rows to realize at scroll offset @p scroll_y.
/// @param list Managed VirtualList object; invalid handles trap.
/// @param scroll_y Vertical scroll offset, clamped to zero.
/// @return A Map with `start`, `end` and `count` (overscan rows included).
void *rt_virtual_list_visible_range(void *list, int64_t scroll_y);
/// @brief Select the row carrying the given id.
/// @param list Managed VirtualList object; invalid handles trap.
/// @param id Stable row identifier to select.
void rt_virtual_list_select_id(void *list, rt_string id);
/// @brief Return the selected row's id (empty string if none).
/// @param list Managed VirtualList object; invalid handles trap.
/// @return Newly referenced stable identifier.
rt_string rt_virtual_list_get_selected_id(void *list);
/// @brief Return the selected row's index, or -1 if nothing is selected.
/// @param list Managed VirtualList object; invalid handles trap.
/// @return Zero-based logical row, or `-1` when selection is unresolved.
int64_t rt_virtual_list_get_selected_index(void *list);

// --- Virtualized tree: lazily expanded, only visible (expanded) rows realized. ---
/// @brief Create an empty virtualized tree.
/// @return New managed VirtualTree object, or `NULL` on allocation failure.
void *rt_virtual_tree_new(void);

/// @brief Begin a nestable mutation batch that defers bound-control projection.
void rt_virtual_tree_begin_update(void *tree);

/// @brief End a mutation batch and project all accumulated changes once.
/// @details An unmatched call traps as a programming error.
void rt_virtual_tree_end_update(void *tree);
/// @brief Add a node @p id labelled @p text under @p parent_id (empty parent = root).
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param parent_id Stable parent identifier, or empty for the hidden root.
/// @param id Non-empty unique node identifier.
/// @param text Runtime display label.
void rt_virtual_tree_add_node(void *tree, rt_string parent_id, rt_string id, rt_string text);
/// @brief Move an existing virtual-tree node to a new parent while preserving its stable ID.
/// @details This additive operation preserves the pre-unique-ID model's reparenting capability
///          without overloading AddNode with duplicate-ID updates. Cycles, the hidden root,
///          missing nodes, and allocation failure are rejected atomically.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable ID of the declared node to move.
/// @param parent_id Stable ID of the new parent, or empty string for the hidden root.
/// @return One on success (including an unchanged parent), otherwise zero.
int8_t rt_virtual_tree_move_node(void *tree, rt_string id, rt_string parent_id);
/// @brief Replace a declared virtual-tree node's display text without changing identity.
/// @details The UTF-8 value is copied before the old text is released; embedded NUL bytes become
///          U+FFFD for lower C rendering. Missing/placeholder nodes and allocation failure leave
///          the model unchanged.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable ID of the declared node to update.
/// @param text New managed UTF-8 label.
/// @return One on success, including an unchanged label; otherwise zero.
int8_t rt_virtual_tree_set_node_text(void *tree, rt_string id, rt_string text);
/// @brief Mark a node expanded.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
/// @return A Map describing the result: `found`, `expanded` and `needsPopulate`
///         (set when the node has no loaded children yet).
void *rt_virtual_tree_expand(void *tree, rt_string id);
/// @brief Collapse a node, hiding its subtree.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
void rt_virtual_tree_collapse(void *tree, rt_string id);
/// @brief Select the node carrying the given id.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
void rt_virtual_tree_select_id(void *tree, rt_string id);
/// @brief Return the selected node's id (empty string if none).
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @return Newly referenced stable identifier.
rt_string rt_virtual_tree_get_selected_id(void *tree);
/// @brief Return the currently visible (expanded) rows as a sequence.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @return New owned sequence of visible-row maps.
void *rt_virtual_tree_visible_rows(void *tree);
/// @brief Return only a requested slice of the flattened visible-tree order.
/// @details The model maintains a lazy stable-ID index. After structural warm-up this operation is
///          O(count), allocates maps only for the requested slice, clamps negative arguments to
///          zero, and never materializes omitted rows.
/// @param tree Managed `Zanna.GUI.VirtualTree` object; invalid handles trap.
/// @param first Zero-based first visible row; negative values normalize to zero.
/// @param count Maximum rows to return; negative values normalize to zero.
/// @return Owned `Zanna.Collections.Seq` of row maps, possibly empty.
void *rt_virtual_tree_visible_rows_range(void *tree, int64_t first, int64_t count);
/// @brief Rebuild the visible-row cache for a node's subtree after structural changes.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable identifier of the retained subtree root.
void rt_virtual_tree_refresh_subtree(void *tree, rt_string id);
/// @brief Bind a VirtualTree to a live TreeView through its viewport provider path.
/// @details No retained TreeNode objects are created for model rows. Existing endpoint bindings
///          are detached safely, and destroying either endpoint invalidates the other raw pointer.
///          Graphics-disabled builds return zero while the headless model remains usable.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param treeview Live `Zanna.GUI.TreeView` handle.
/// @return One on success, zero when unavailable or invalid.
int8_t rt_virtual_tree_bind(void *tree, void *treeview);
/// @brief Detach a VirtualTree from its bound TreeView without destroying either object.
/// @param tree Managed VirtualTree object; invalid handles trap.
void rt_virtual_tree_unbind(void *tree);
/// @brief Bind a TreeView to a VirtualTree; reverse-form convenience for VirtualTree.Bind.
/// @param treeview Live `Zanna.GUI.TreeView` handle.
/// @param model Managed `Zanna.GUI.VirtualTree` object.
/// @return One on success, zero for invalid endpoints or graphics-disabled builds.
int8_t rt_treeview_set_virtual_model(void *treeview, void *model);
/// @brief Remove an external model from a TreeView and restore retained-node rendering.
/// @details The pre-existing retained node tree is preserved. Invalid and graphics-disabled
///          handles are harmless no-ops.
/// @param treeview TreeView handle whose external model should be cleared.
void rt_treeview_clear_virtual_model(void *treeview);
/// @brief Remove one node together with its whole subtree from a VirtualTree model.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable identifier of the subtree root to remove.
/// @return One when the node existed and was removed, zero otherwise.
int8_t rt_virtual_tree_remove_node(void *tree, rt_string id);
/// @brief Replace a VirtualTree model's ordered multi-selection.
/// @details The first identifier becomes the primary selection; empty entries are skipped.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param ids Borrowed sequence of stable identifier strings.
void rt_virtual_tree_select_ids(void *tree, void *ids);
/// @brief Snapshot a VirtualTree model's ordered multi-selection.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @return New owned sequence of stable identifiers, primary first.
void *rt_virtual_tree_get_selected_ids(void *tree);
/// @brief Set one virtual node's projected row icon from a spec string.
/// @details `vector:<name>` selects a named scalable icon; other non-empty text is a
///          literal glyph; empty clears both.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
/// @param spec Icon specification string.
/// @return One when the node existed, zero otherwise.
int8_t rt_virtual_tree_set_node_icon(void *tree, rt_string id, rt_string spec);
/// @brief Set one virtual node's row text color and dim state.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
/// @param rgb Packed 0xRRGGBB color; zero restores the theme color.
/// @param dim Non-zero blends the row toward the background.
/// @return One when the node existed, zero otherwise.
int8_t rt_virtual_tree_set_node_style(void *tree, rt_string id, int64_t rgb, int8_t dim);
/// @brief Consume the bound TreeView's latched virtual drop as stable identifiers.
/// @details The returned map is empty when nothing is latched; otherwise it carries
///          `source`, `target`, and `position` (`before`/`into`/`after`) keys.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @return New owned map describing the drop, possibly empty.
void *rt_virtual_tree_take_drop_action(void *tree);
/// @brief Scroll the bound TreeView so one node's visible row enters the viewport.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier; hidden and unknown rows are ignored.
void rt_virtual_tree_reveal_id(void *tree, rt_string id);
/// @brief Begin an inline row edit over one node's visible row in the bound TreeView.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
/// @param text Initial editor contents.
/// @return One when the editor was placed, zero for hidden rows or unbound models.
int8_t rt_virtual_tree_begin_edit(void *tree, rt_string id, rt_string text);
/// @brief Declare whether one node's children are fully populated.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param id Stable node identifier.
/// @param loaded Non-zero marks the node's child list as complete.
/// @return One when the node existed, zero otherwise.
int8_t rt_virtual_tree_set_node_loaded(void *tree, rt_string id, int8_t loaded);
/// @brief Resolve the stable id of the visible row under a window-space point.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @param x Window-space horizontal coordinate.
/// @param y Window-space vertical coordinate.
/// @return New owned id string; empty when the point misses every row.
rt_string rt_virtual_tree_row_id_at(void *tree, int64_t x, int64_t y);
/// @brief Consume the bound TreeView's latched inline-edit commit.
/// @details The returned map is empty when no commit is pending; otherwise it carries
///          `id` and `text` keys. The edge is consumed on read.
/// @param tree Managed VirtualTree object; invalid handles trap.
/// @return New owned map describing the commit, possibly empty.
void *rt_virtual_tree_take_edit_commit(void *tree);

// --- Command state: the enabled/checked/accessibility state of a UI command,
//     used to drive menu items, toolbar buttons and the command palette. ---
/// @brief Create a command-state object identified by @p id with display @p label.
/// @param id Stable command identifier.
/// @param label Display and initial accessible label.
/// @return New managed CommandState object, or `NULL` on allocation failure.
void *rt_command_state_new(rt_string id, rt_string label);
/// @brief Set whether the command is enabled (invokable).
/// @param state Managed CommandState object; invalid handles trap.
/// @param enabled Non-zero to enable invocation.
void rt_command_state_set_enabled(void *state, int8_t enabled);
/// @brief Return 1 if the command is enabled.
/// @param state Managed CommandState object; invalid handles trap.
/// @return `1` when enabled; otherwise `0`.
int8_t rt_command_state_get_enabled(void *state);
/// @brief Set the command's checked (toggled-on) state.
/// @param state Managed CommandState object; invalid handles trap.
/// @param checked Non-zero to mark the command checked.
void rt_command_state_set_checked(void *state, int8_t checked);
/// @brief Return 1 if the command is checked.
/// @param state Managed CommandState object; invalid handles trap.
/// @return `1` when checked; otherwise `0`.
int8_t rt_command_state_get_checked(void *state);
/// @brief Set the accessibility label and description announced for the command.
/// @param state Managed CommandState object; invalid handles trap.
/// @param label Accessible label.
/// @param description Accessible description.
void rt_command_state_set_accessible(void *state, rt_string label, rt_string description);
/// @brief Snapshot the command state as a Map (id, label, accessibleLabel,
///        accessibleDescription, enabled, checked).
/// @param state Managed CommandState object; invalid handles trap.
/// @return New managed state map, or `NULL` on allocation failure.
void *rt_command_state_snapshot(void *state);

// --- Command: a UI action bound to its menu item, toolbar button, keyboard
//     shortcut and command-palette entry, polled from one place. ---
/// @brief Create a command identified by @p id with display @p title (enabled by default).
/// @param id Stable command identifier.
/// @param title Display title.
/// @return New managed Command object, or `NULL` on allocation failure.
void *rt_command_new(rt_string id, rt_string title);
/// @brief Return the command's stable id.
/// @param command Managed Command object; invalid handles trap.
/// @return Newly referenced identifier.
rt_string rt_command_get_id(void *command);
/// @brief Return the command's display title.
/// @param command Managed Command object; invalid handles trap.
/// @return Newly referenced title.
rt_string rt_command_get_title(void *command);
/// @brief Set the command's keyboard shortcut chord (e.g. "Ctrl+B") and register it with the
///        global Zanna.GUI.Shortcuts registry under the command id (best-effort if no app yet).
/// @param command Managed Command object; invalid handles trap.
/// @param keys Runtime shortcut chord specification.
void rt_command_set_shortcut(void *command, rt_string keys);
/// @brief Return the command's shortcut chord (empty string if none).
/// @param command Managed Command object; invalid handles trap.
/// @return Newly referenced shortcut string.
rt_string rt_command_get_shortcut(void *command);
/// @brief Set whether the command is enabled; pushed to bound widgets.
/// @param command Managed Command object; invalid handles trap.
/// @param enabled Non-zero to enable invocation.
void rt_command_set_enabled(void *command, int8_t enabled);
/// @brief Return 1 if the command is enabled.
/// @param command Managed Command object; invalid handles trap.
/// @return `1` when enabled; otherwise `0`.
int8_t rt_command_is_enabled(void *command);
/// @brief Set whether the command is checkable (a toggle); pushed to a bound menu item.
/// @param command Managed Command object; invalid handles trap.
/// @param checkable Non-zero to expose checked state.
void rt_command_set_checkable(void *command, int8_t checkable);
/// @brief Return 1 if the command is checkable.
/// @param command Managed Command object; invalid handles trap.
/// @return `1` when checkable; otherwise `0`.
int8_t rt_command_is_checkable(void *command);
/// @brief Set the command's checked (toggled-on) state; pushed to bound widgets.
/// @param command Managed Command object; invalid handles trap.
/// @param checked Non-zero to mark the command checked.
void rt_command_set_checked(void *command, int8_t checked);
/// @brief Return 1 if the command is checked.
/// @param command Managed Command object; invalid handles trap.
/// @return `1` when checked; otherwise `0`.
int8_t rt_command_is_checked(void *command);
/// @brief Bind a Zanna.GUI.MenuItem the command should read (clicks) and drive (enabled/checked).
/// @param command Managed Command object; invalid handles trap.
/// @param item MenuItem handle, or `NULL` to unbind.
void rt_command_bind_menu_item(void *command, void *item);
/// @brief Bind a Zanna.GUI.ToolbarItem the command should read (clicks) and drive
/// (enabled/toggled).
/// @param command Managed Command object; invalid handles trap.
/// @param item ToolbarItem handle, or `NULL` to unbind.
void rt_command_bind_toolbar_item(void *command, void *item);
/// @brief Poll a standalone command: read bound menu/toolbar/shortcut, push state to bound
///        widgets, and return 1 if invoked this frame (disabled commands never report invoked).
/// @param command Managed Command object; invalid handles trap.
/// @return `1` when invoked by an enabled source; otherwise `0`.
int8_t rt_command_poll(void *command);
/// @brief Return the invoked flag computed by the most recent command- or registry-level poll.
/// @param command Managed Command object; invalid handles trap.
/// @return `1` when the latest poll observed invocation; otherwise `0`.
int8_t rt_command_was_invoked(void *command);
/// @brief Snapshot the command as a Map (id, title, shortcut, enabled, checkable, checked,
/// invoked).
/// @param command Managed Command object; invalid handles trap.
/// @return New managed command map, or `NULL` on allocation failure.
void *rt_command_snapshot(void *command);

// --- Command registry: owns a set of commands and routes menu/toolbar/shortcut/palette
//     to them in a single per-frame poll. ---
/// @brief Create an empty command registry.
/// @return New managed CommandRegistry object, or `NULL` on allocation failure.
void *rt_command_registry_new(void);
/// @brief Add (retain) a command to the registry; ignored if @p command is not a Command or already
/// present.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @param command Live Command object to retain.
void rt_command_registry_add(void *registry, void *command);
/// @brief Return the number of commands in the registry.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @return Retained command count saturated to `INT64_MAX`.
int64_t rt_command_registry_count(void *registry);
/// @brief Return the command with id @p id (a retained reference), or NULL if none.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @param id Stable command identifier to match.
/// @return Owned Command reference, or `NULL` when absent.
void *rt_command_registry_find(void *registry, rt_string id);
/// @brief Return the command with id @p id as an Option.
/// @details Returns Some(Command) when found and None when no registered command matches.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @param id Stable command identifier to match.
/// @return Managed Option containing the command when found.
void *rt_command_registry_find_option(void *registry, rt_string id);
/// @brief Bind the Zanna.GUI.CommandPalette whose selection routes to registered commands.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @param palette CommandPalette handle, or `NULL` to unbind.
void rt_command_registry_bind_palette(void *registry, void *palette);
/// @brief Poll the palette once and every command (menu/toolbar/shortcut/palette); push state.
/// @param registry Managed CommandRegistry object; invalid handles trap.
/// @return The id of a command invoked this frame, or the empty string. Each command's
///         WasInvoked() flag is updated.
rt_string rt_command_registry_poll(void *registry);
/// @brief Release all commands and empty the registry.
/// @param registry Managed CommandRegistry object; invalid handles trap.
void rt_command_registry_clear(void *registry);

// --- Accessibility: WCAG contrast math and high-contrast theme tokens. ---
/// @brief Compute the WCAG relative-luminance contrast ratio between two 0xRRGGBB colors.
/// @param fg_rgb Foreground packed RGB color.
/// @param bg_rgb Background packed RGB color.
/// @return A ratio in [1, 21] (lighter:darker, order-independent).
double rt_accessibility_contrast_ratio(int64_t fg_rgb, int64_t bg_rgb);
/// @brief Return 1 if the fg/bg contrast ratio meets @p min_ratio.
/// @details A non-finite or non-positive @p min_ratio defaults to the WCAG AA threshold of 4.5.
/// @param fg_rgb Foreground packed RGB color.
/// @param bg_rgb Background packed RGB color.
/// @param min_ratio Required contrast ratio.
/// @return `1` when the computed ratio meets the threshold; otherwise `0`.
int8_t rt_accessibility_meets_contrast(int64_t fg_rgb, int64_t bg_rgb, double min_ratio);
/// @brief Return a Map of high-contrast theme color tokens
///        (background, foreground, accent, warning, error).
/// @return New managed color-token map, or `NULL` on allocation failure.
void *rt_accessibility_high_contrast_tokens(void);

#ifdef __cplusplus
}
#endif
