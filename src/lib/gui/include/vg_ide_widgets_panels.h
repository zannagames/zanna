//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_panels.h
// Purpose: SplitPane, TabBar, OutputPane, and Breadcrumb widget declarations —
//          layout primitives for multi-pane IDE-style shells.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
//   - SplitPane divider positions are stored as fractions in [0.0, 1.0].
// Ownership/Lifetime:
//   - Widgets are owned by their parent in the widget tree.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_ide_widgets_common.h"

/// @file
/// @brief Declares tab bars, split panes, output panes, and breadcrumb navigation.
/// @details These widgets form the retained layout and document-navigation primitives used by
/// IDE shells, including stable tab identities, draggable split ratios, categorized output
/// streams, and path segments with callback-driven activation.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// TabBar Widget
//=============================================================================

/// @brief Tab structure
typedef struct vg_tab {
    uint64_t magic;          ///< Live-tab sentinel for stale handle detection
    struct vg_tabbar *owner; ///< Owning tab bar for invalidation/reorder
    char *title;             ///< Tab title (owned)
    char *tooltip;           ///< Tab tooltip (owned)
    int32_t icon_vector_id;  ///< Vector icon before the title; negative = none.
    char *stable_id;         ///< Optional application-stable identifier (owned)
    size_t stable_id_len;    ///< Stable identifier length in bytes
    void *user_data;         ///< User data
    bool owns_user_data;     ///< True when user_data must be freed with the tab
    bool closable;           ///< Can tab be closed
    bool modified;           ///< Show modified indicator
    struct vg_tab *next;
    struct vg_tab *prev;
    struct vg_tab *retired_next; ///< Retired-tab list link
} vg_tab_t;

/// @brief Notify a client that a tab became selected.
/// @param tabbar TabBar widget that owns the tab.
/// @param tab Borrowed selected tab.
/// @param user_data Opaque pointer registered with the TabBar.
typedef void (*vg_tab_select_callback_t)(vg_widget_t *tabbar, vg_tab_t *tab, void *user_data);
/// @brief Ask a client whether a requested tab close may proceed.
/// @param tabbar TabBar widget that owns the tab.
/// @param tab Borrowed tab requested for closure.
/// @param user_data Opaque pointer registered with the TabBar.
/// @return True to permit closure, or false to veto it.
typedef bool (*vg_tab_close_callback_t)(vg_widget_t *tabbar, vg_tab_t *tab, void *user_data);
/// @brief Notify a client after a tab is reordered.
/// @param tabbar TabBar widget that owns the tab.
/// @param tab Borrowed reordered tab.
/// @param new_index New zero-based index.
/// @param user_data Opaque pointer registered with the TabBar.
typedef void (*vg_tab_reorder_callback_t)(vg_widget_t *tabbar,
                                          vg_tab_t *tab,
                                          int new_index,
                                          void *user_data);

/// @brief TabBar widget structure
typedef struct vg_tabbar {
    vg_widget_t base;

    vg_tab_t *first_tab;    ///< First tab
    vg_tab_t *last_tab;     ///< Last tab
    vg_tab_t *active_tab;   ///< Currently active tab
    vg_tab_t *retired_tabs; ///< Removed tabs kept until tabbar destroy for stale handle checks
    int tab_count;          ///< Number of tabs

    vg_font_t *font; ///< Font for rendering
    float font_size; ///< Font size

    // Appearance
    float tab_height;        ///< Tab height
    float tab_padding;       ///< Tab horizontal padding
    float close_button_size; ///< Close button size
    float max_tab_width;     ///< Maximum tab width
    uint32_t active_bg;      ///< Active tab background
    uint32_t inactive_bg;    ///< Inactive tab background
    uint32_t text_color;     ///< Text color
    uint32_t close_color;    ///< Close button color

    // Scrolling (for many tabs)
    float scroll_x;    ///< Horizontal scroll offset
    float total_width; ///< Total width of all tabs

    // Callbacks
    vg_tab_select_callback_t on_select;
    void *on_select_data;
    vg_tab_close_callback_t on_close;
    void *on_close_data;
    vg_tab_reorder_callback_t on_reorder;
    void *on_reorder_data;

    // State
    vg_tab_t *hovered_tab;       ///< Currently hovered tab
    bool close_button_hovered;   ///< Is close button hovered
    bool dragging;               ///< Is dragging a tab
    bool drag_pending;           ///< Pointer is captured and may become a drag after threshold
    vg_tab_t *drag_tab;          ///< Tab being dragged
    float drag_origin_x;         ///< Mouse-down X position for drag-threshold checks
    float drag_x;                ///< Drag position
    vg_tab_t *pressed_tab;       ///< Tab pressed on mouse-down, committed on mouse-up
    vg_tab_t *pressed_close_tab; ///< Close button pressed on mouse-down, committed on mouse-up

    // Per-frame tracking for Zia runtime
    vg_tab_t *prev_active_tab;    ///< Previous active tab (for change detection)
    int close_clicked_index;      ///< Last close-click index (cleared on index read, -1 = none)
    uint64_t close_click_version; ///< Monotonic counter for close-click events
    uint64_t reported_close_click_version;   ///< Last close-click version observed by runtime
    bool auto_close;                         ///< Auto-remove tab on close click (default true)
    uint64_t active_change_version;          ///< Monotonic counter for active-tab changes
    uint64_t reported_active_change_version; ///< Last active-change version observed by runtime
    uint64_t reorder_version;                ///< Monotonic counter for successful reorder events
    uint64_t reported_reorder_version;       ///< Last reorder version consumed by a poll observer
    int reordered_from;                      ///< Source index of the most recent reorder
    int reordered_to;                        ///< Destination index of the most recent reorder
    char *saved_tooltip_text;  ///< Preserved widget tooltip while a tab tooltip is active
    bool hover_tooltip_active; ///< True while widget tooltip is overridden by hovered tab
} vg_tabbar_t;

/// @brief Create a new tab bar widget.
/// @param parent Parent widget (can be NULL).
/// @return New tab bar or NULL on failure.
vg_tabbar_t *vg_tabbar_create(vg_widget_t *parent);

/// @brief Add a new tab to the end of the tab bar.
/// @param tabbar   Tab bar widget.
/// @param title    Tab label text (copied internally).
/// @param closable true if the tab should show a close button.
/// @return Newly added tab, or NULL on failure.
vg_tab_t *vg_tabbar_add_tab(vg_tabbar_t *tabbar, const char *title, bool closable);

/// @brief Return true if a tab handle is still live and owned by a tab bar.
/// @param tab Tab handle to test.
/// @return true when the handle points at a currently live tab.
bool vg_tab_is_live(const vg_tab_t *tab);

/// @brief Remove a tab, retiring the stale handle until destroy or prune.
/// @param tabbar Tab bar widget.
/// @param tab    Tab to remove (must belong to @p tabbar).
void vg_tabbar_remove_tab(vg_tabbar_t *tabbar, vg_tab_t *tab);

/// @brief Free retired tab tombstones after all stale tab handles are discarded.
/// @param tabbar Tab bar widget.
void vg_tabbar_prune_retired_tabs(vg_tabbar_t *tabbar);

/// @brief Reclaim one specific retired tab tombstone from a live TabBar.
/// @details Unlinks and frees @p tab only when it is present in `tabbar->retired_tabs`. The caller
///          must first discard or invalidate every managed wrapper for that tab. Live tabs,
///          foreign addresses, NULL inputs, and already reclaimed tabs return false without
///          mutation. No allocation is performed.
/// @param tabbar Live owner whose retired chain is searched.
/// @param tab Candidate retained tab record.
/// @return true when the exact tombstone was unlinked and freed, otherwise false.
bool vg_tabbar_reclaim_retired_tab(vg_tabbar_t *tabbar, vg_tab_t *tab);

/// @brief Make a tab the active (foreground) tab.
/// @param tabbar Tab bar widget.
/// @param tab    Tab to activate (must belong to @p tabbar).
void vg_tabbar_set_active(vg_tabbar_t *tabbar, vg_tab_t *tab);

/// @brief Get the currently active tab.
/// @param tabbar Tab bar widget.
/// @return Active tab pointer, or NULL if the bar is empty.
vg_tab_t *vg_tabbar_get_active(vg_tabbar_t *tabbar);

/// @brief Get the zero-based index of a tab within the bar.
/// @param tabbar Tab bar widget.
/// @param tab    Tab to locate.
/// @return Zero-based index, or -1 if the tab does not belong to @p tabbar.
int vg_tabbar_get_tab_index(vg_tabbar_t *tabbar, vg_tab_t *tab);

/// @brief Get the tab at a zero-based index.
/// @param tabbar Tab bar widget.
/// @param index  Zero-based index.
/// @return Tab at @p index, or NULL if the index is out of range.
vg_tab_t *vg_tabbar_get_tab_at(vg_tabbar_t *tabbar, int index);

/// @brief Get the zero-based index of the tab under canvas coordinates (x, y).
/// @param tabbar Tab bar widget.
/// @param x      Canvas-pixel X (input-layer coordinate space).
/// @param y      Canvas-pixel Y.
/// @return Tab index at the point, or -1 if none.
int vg_tabbar_index_at(vg_tabbar_t *tabbar, int x, int y);

/// @brief Set the label text on an existing tab.
/// @param tab   Tab to modify.
/// @param title New title string (copied internally).
void vg_tab_set_title(vg_tab_t *tab, const char *title);

/// @brief Return a tab's borrowed title text.
/// @param tab Live tab to inspect.
/// @return Borrowed NUL-terminated title, or an empty string for a stale/NULL tab.
const char *vg_tab_get_title(const vg_tab_t *tab);

/// @brief Set the modified indicator on a tab.
/// @param tab      Tab to modify.
/// @param modified true to show the unsaved-changes dot.
void vg_tab_set_modified(vg_tab_t *tab, bool modified);

/// @brief Attach or clear a leading vector icon on one live tab (ADR 0220).
/// @param tab Live tab to modify; invalid or retired records are ignored.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_tab_set_icon_vector(vg_tab_t *tab, int32_t vector_id);

/// @brief Set the tooltip text shown when hovering over a tab.
/// @param tab     Tab to modify.
/// @param tooltip Tooltip string (copied internally); NULL clears it.
void vg_tab_set_tooltip(vg_tab_t *tab, const char *tooltip);

/// @brief Set arbitrary caller data associated with a tab.
/// @param tab  Tab to modify.
/// @param data Caller-owned pointer stored without copying.
void vg_tab_set_data(vg_tab_t *tab, void *data);

/// @brief Return the opaque caller data stored on a tab.
/// @param tab Live tab to inspect.
/// @return Borrowed data pointer, or NULL when absent or stale.
void *vg_tab_get_data(const vg_tab_t *tab);

/// @brief Set whether a tab exposes its close affordance.
/// @details The value affects layout immediately and advances the owning TabBar revision only when
///          it changes. Stale tabs are ignored.
/// @param tab Tab to modify.
/// @param closable true to display and enable the close button.
void vg_tab_set_closable(vg_tab_t *tab, bool closable);

/// @brief Return whether a live tab is closable.
/// @param tab Tab to inspect.
/// @return true when closable, otherwise false.
bool vg_tab_is_closable(const vg_tab_t *tab);

/// @brief Assign an application-stable identifier to a tab.
/// @details The identifier is copied before the old allocation is released, preserving the prior
///          value on allocation failure. Empty values clear the ID. Uniqueness is an application
///          or virtual-model responsibility.
/// @param tab Tab to modify.
/// @param stable_id NUL-terminated identifier; NULL is treated as empty.
/// @return true on success, including an unchanged value; false for stale tabs or allocation
///         failure.
bool vg_tab_set_stable_id(vg_tab_t *tab, const char *stable_id);

/// @brief Return a tab's borrowed application-stable identifier.
/// @param tab Tab to inspect.
/// @return Borrowed identifier, or an empty string when absent or stale.
const char *vg_tab_get_stable_id(const vg_tab_t *tab);

/// @brief Set the font used to render tab labels.
/// @param tabbar Tab bar widget.
/// @param font   Font handle.
/// @param size   Font size in pixels.
void vg_tabbar_set_font(vg_tabbar_t *tabbar, vg_font_t *font, float size);

/// @brief Move a tab from one zero-based index to another.
/// @details Invalid indices and unchanged moves return false without mutation. A successful move
///          updates linked-list order, records the independent reorder edge and source/destination
///          payload, advances the TabBar revision, and invokes the optional reorder callback after
///          the committed state is visible.
/// @param tabbar Tab bar to reorder.
/// @param from_index Current zero-based tab index.
/// @param to_index Destination zero-based tab index in the final order.
/// @return true only when the order changed.
bool vg_tabbar_move_tab(vg_tabbar_t *tabbar, int from_index, int to_index);

/// @brief Consume the TabBar's independent reorder edge.
/// @details Does not clear the last source/destination payload, active-change edge, close edge, or
///          non-consuming revision. Multiple unreported reorders coalesce.
/// @param tabbar Tab bar to inspect.
/// @return true once after one or more unreported successful reorders.
bool vg_tabbar_was_reordered(vg_tabbar_t *tabbar);

/// @brief Return the source index from the most recent successful reorder.
/// @param tabbar Tab bar to inspect.
/// @return Zero-based source index, or -1 when no reorder has occurred.
int vg_tabbar_get_reordered_from(const vg_tabbar_t *tabbar);

/// @brief Return the destination index from the most recent successful reorder.
/// @param tabbar Tab bar to inspect.
/// @return Zero-based destination index, or -1 when no reorder has occurred.
int vg_tabbar_get_reordered_to(const vg_tabbar_t *tabbar);

/// @brief Set the callback fired when the active tab changes.
/// @param tabbar    Tab bar widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_tabbar_set_on_select(vg_tabbar_t *tabbar,
                             vg_tab_select_callback_t callback,
                             void *user_data);

/// @brief Set the callback fired when a close button is clicked.
/// @param tabbar    Tab bar widget.
/// @param callback  Handler; return true to allow removal, false to cancel.
/// @param user_data User data passed to the handler.
void vg_tabbar_set_on_close(vg_tabbar_t *tabbar, vg_tab_close_callback_t callback, void *user_data);

/// @brief Set the callback fired when a tab is dragged to a new position.
/// @param tabbar    Tab bar widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_tabbar_set_on_reorder(vg_tabbar_t *tabbar,
                              vg_tab_reorder_callback_t callback,
                              void *user_data);

//=============================================================================
// SplitPane Widget
//=============================================================================

/// @brief Split direction
typedef enum vg_split_direction {
    VG_SPLIT_HORIZONTAL, ///< Left/Right split
    VG_SPLIT_VERTICAL    ///< Top/Bottom split
} vg_split_direction_t;

/// @brief Identifies which split-pane side is collapsed.
typedef enum vg_split_collapsed_side {
    VG_SPLIT_COLLAPSED_NONE = 0,  ///< Both panes are visible.
    VG_SPLIT_COLLAPSED_FIRST = 1, ///< The left/top pane is collapsed.
    VG_SPLIT_COLLAPSED_SECOND = 2 ///< The right/bottom pane is collapsed.
} vg_split_collapsed_side_t;

/// @brief SplitPane widget structure
typedef struct vg_splitpane {
    vg_widget_t base;

    vg_split_direction_t direction;           ///< Split direction
    float split_position;                     ///< Splitter position (0-1 ratio)
    float min_first_size;                     ///< Minimum size for first pane
    float min_second_size;                    ///< Minimum size for second pane
    float splitter_size;                      ///< Splitter bar thickness
    float restore_position;                   ///< Divider position restored after collapse
    vg_split_collapsed_side_t collapsed_side; ///< Currently collapsed pane

    uint32_t splitter_color;       ///< Splitter bar color
    uint32_t splitter_hover_color; ///< Splitter hover color

    // State
    bool splitter_hovered;  ///< Is splitter hovered
    bool dragging;          ///< Is dragging splitter
    float drag_start;       ///< Drag start position
    float drag_start_split; ///< Split position at drag start
} vg_splitpane_t;

/// @brief Create a new split pane widget.
/// @param parent    Parent widget (can be NULL).
/// @param direction VG_SPLIT_HORIZONTAL (left/right) or VG_SPLIT_VERTICAL (top/bottom).
/// @return New split pane or NULL on failure.
vg_splitpane_t *vg_splitpane_create(vg_widget_t *parent, vg_split_direction_t direction);

/// @brief Set the divider position as a normalised fraction.
/// @param split    Split pane widget.
/// @param position Fraction in [0.0, 1.0] where 0.5 places the divider at the midpoint.
void vg_splitpane_set_position(vg_splitpane_t *split, float position);

/// @brief Get the current divider position.
/// @param split Split pane widget.
/// @return Current position fraction in [0.0, 1.0].
float vg_splitpane_get_position(vg_splitpane_t *split);

/// @brief Set the minimum pixel sizes for each pane.
/// @param split      Split pane widget.
/// @param min_first  Minimum size of the first (left/top) pane in pixels.
/// @param min_second Minimum size of the second (right/bottom) pane in pixels.
void vg_splitpane_set_min_sizes(vg_splitpane_t *split, float min_first, float min_second);

/// @brief Set the minimum pixel size of the first (left or top) pane.
/// @details Negative and non-finite values are treated as zero. The value is ignored while the
///          first pane is explicitly collapsed and becomes effective again after restore.
/// @param split Split pane widget to configure; NULL is ignored.
/// @param size Minimum first-pane size in physical pixels.
void vg_splitpane_set_min_first(vg_splitpane_t *split, float size);

/// @brief Set the minimum pixel size of the second (right or bottom) pane.
/// @details Negative and non-finite values are treated as zero. The value is ignored while the
///          second pane is explicitly collapsed and becomes effective again after restore.
/// @param split Split pane widget to configure; NULL is ignored.
/// @param size Minimum second-pane size in physical pixels.
void vg_splitpane_set_min_second(vg_splitpane_t *split, float size);

/// @brief Return the configured minimum size of the first pane.
/// @param split Split pane widget to inspect.
/// @return Minimum first-pane size in physical pixels, or 0 when @p split is NULL.
float vg_splitpane_get_min_first(const vg_splitpane_t *split);

/// @brief Return the configured minimum size of the second pane.
/// @param split Split pane widget to inspect.
/// @return Minimum second-pane size in physical pixels, or 0 when @p split is NULL.
float vg_splitpane_get_min_second(const vg_splitpane_t *split);

/// @brief Return the split pane's immutable orientation.
/// @param split Split pane widget to inspect.
/// @return The creation-time orientation, or VG_SPLIT_HORIZONTAL when @p split is NULL.
vg_split_direction_t vg_splitpane_get_direction(const vg_splitpane_t *split);

/// @brief Collapse the first (left or top) pane.
/// @details The current non-collapsed divider position is retained for a later call to
///          vg_splitpane_restore(). Repeating the operation is a no-op.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_collapse_first(vg_splitpane_t *split);

/// @brief Collapse the second (right or bottom) pane.
/// @details The current non-collapsed divider position is retained for a later call to
///          vg_splitpane_restore(). Repeating the operation is a no-op.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_collapse_second(vg_splitpane_t *split);

/// @brief Restore both panes after an explicit collapse.
/// @details Restores the divider fraction saved immediately before the first collapse in the
///          current collapse sequence. Calling this while neither side is collapsed is a no-op.
/// @param split Split pane widget to update; NULL is ignored.
void vg_splitpane_restore(vg_splitpane_t *split);

/// @brief Return which pane is explicitly collapsed.
/// @param split Split pane widget to inspect.
/// @return VG_SPLIT_COLLAPSED_NONE, VG_SPLIT_COLLAPSED_FIRST, or
///         VG_SPLIT_COLLAPSED_SECOND; NULL returns VG_SPLIT_COLLAPSED_NONE.
vg_split_collapsed_side_t vg_splitpane_get_collapsed_side(const vg_splitpane_t *split);

/// @brief Get the container widget for the first (left or top) pane.
/// @param split Split pane widget.
/// @return First pane container widget.
vg_widget_t *vg_splitpane_get_first(vg_splitpane_t *split);

/// @brief Get the container widget for the second (right or bottom) pane.
/// @param split Split pane widget.
/// @return Second pane container widget.
vg_widget_t *vg_splitpane_get_second(vg_splitpane_t *split);

//=============================================================================
// OutputPane Widget (Terminal-like output)
//=============================================================================

/// @brief ANSI color codes
typedef enum vg_ansi_color {
    VG_ANSI_DEFAULT = 0,
    VG_ANSI_BLACK = 30,
    VG_ANSI_RED,
    VG_ANSI_GREEN,
    VG_ANSI_YELLOW,
    VG_ANSI_BLUE,
    VG_ANSI_MAGENTA,
    VG_ANSI_CYAN,
    VG_ANSI_WHITE,
    VG_ANSI_BRIGHT_BLACK = 90,
    VG_ANSI_BRIGHT_RED,
    VG_ANSI_BRIGHT_GREEN,
    VG_ANSI_BRIGHT_YELLOW,
    VG_ANSI_BRIGHT_BLUE,
    VG_ANSI_BRIGHT_MAGENTA,
    VG_ANSI_BRIGHT_CYAN,
    VG_ANSI_BRIGHT_WHITE
} vg_ansi_color_t;

/// @brief Styled text segment
typedef struct vg_styled_segment {
    char *text;        ///< Segment text
    uint32_t fg_color; ///< Foreground color
    uint32_t bg_color; ///< Background color
    bool bold;         ///< Bold text
    bool italic;       ///< Italic text
    bool underline;    ///< Underlined text
} vg_styled_segment_t;

/// @brief Output line
typedef struct vg_output_line {
    vg_styled_segment_t *segments; ///< Styled segments
    size_t segment_count;          ///< Number of segments
    size_t segment_capacity;       ///< Capacity
    uint64_t timestamp;            ///< When line was added
} vg_output_line_t;

/// @brief One terminal grid cell (interactive terminal mode): a UTF-8 glyph + style.
typedef struct vg_term_cell {
    char utf8[5]; ///< UTF-8 bytes of the glyph (nul-terminated); empty = space
    uint32_t fg;  ///< Foreground color
    uint32_t bg;  ///< Background color
    bool bold;    ///< Bold attribute
} vg_term_cell_t;

/// @brief OutputPane widget structure
///
/// @details `max_lines` is clamped to at least one line. `append_line` writes
///          exactly one logical line and reuses the trailing empty line left by
///          previous appends, so repeated calls do not introduce blank spacer
///          lines. Clearing the pane also clears any active selection.
typedef struct vg_outputpane {
    vg_widget_t base;

    // Lines
    vg_output_line_t *lines; ///< Array of output lines
    size_t line_start;       ///< Physical index of logical line 0 in the ring
    size_t line_count;
    size_t line_capacity;
    size_t max_lines; ///< Ring buffer limit (default: 10000)

    // Scrolling
    float scroll_y;     ///< Vertical scroll position
    bool auto_scroll;   ///< Scroll to bottom on new output
    bool scroll_locked; ///< User scrolled up

    // Selection
    bool has_selection;
    uint32_t sel_start_line, sel_start_col;
    uint32_t sel_end_line, sel_end_col;

    // Styling
    float line_height; ///< Height per line
    vg_font_t *font;   ///< Monospace font
    float font_size;
    uint32_t bg_color;
    uint32_t default_fg;

    // ANSI parser state
    uint32_t current_fg;
    uint32_t current_bg;
    bool ansi_bold;
    bool in_escape;
    char escape_buf[32];
    int escape_len;

    // Interactive terminal mode (vg_outputpane_set_terminal_mode). When enabled the
    // pane uses a cursor-position overwrite model (so \r, \b, ESC[K, cursor moves render),
    // a full escape state machine (OSC / ESC7-8 / charset are swallowed, not leaked),
    // and captures keystrokes into pending_input for the controller to drain to a PTY.
    bool terminal_mode;        ///< Cursor-position overwrite + keyboard capture active
    bool has_focus;            ///< Pane currently holds keyboard focus (terminal caret)
    int esc_state;             ///< Terminal escape parser state
                               ///< (0=normal,1=esc,2=csi,3=osc,4=charset,5=osc-esc)
    size_t term_cursor_line;   ///< Logical output line holding the terminal cursor
    size_t term_origin_line;   ///< Logical top row used for CSI row/column addressing
    size_t saved_cursor_line;  ///< Saved terminal cursor line for ESC7/ESC8 and CSI s/u
    uint32_t saved_cursor_col; ///< Saved terminal cursor column for ESC7/ESC8 and CSI s/u
    uint32_t cursor_col;       ///< Cursor column on term_cursor_line (terminal mode)
    vg_term_cell_t *cells;     ///< Cells for term_cursor_line (terminal mode)
    size_t cell_count;         ///< Cells on term_cursor_line
    size_t cell_capacity;      ///< Allocated cell capacity
    char *pending_input;       ///< Keystroke bytes awaiting drain to the PTY
    size_t pending_len;        ///< Bytes queued in pending_input
    size_t pending_capacity;   ///< Allocated pending_input capacity
    float caret_blink_time;    ///< Accumulated time toward the next caret blink toggle
    bool caret_visible;        ///< Current caret on/off phase (terminal mode)

    // Alternate-screen preservation. CSI ?1047/1049 h swaps the active terminal
    // buffer into an empty full-screen buffer; CSI ?1047/1049 l restores it.
    bool alternate_screen;             ///< True when the pane is showing the alternate buffer.
    vg_output_line_t *primary_lines;   ///< Saved primary scrollback while alternate_screen is true.
    size_t primary_line_start;         ///< Saved primary ring start.
    size_t primary_line_count;         ///< Saved primary logical line count.
    size_t primary_line_capacity;      ///< Saved primary ring capacity.
    float primary_scroll_y;            ///< Saved primary vertical scroll.
    bool primary_scroll_locked;        ///< Saved primary scroll-lock flag.
    size_t primary_term_cursor_line;   ///< Saved primary terminal cursor line.
    size_t primary_term_origin_line;   ///< Saved primary cursor-addressing origin line.
    size_t primary_saved_cursor_line;  ///< Saved primary saved-cursor line.
    uint32_t primary_saved_cursor_col; ///< Saved primary saved-cursor column.
    uint32_t primary_cursor_col;       ///< Saved primary terminal cursor column.

    // Extended VT state (plan 11): scroll margins, DEC private modes, reverse
    // video, and tab stops — the pinned vim/less/htop sequence table.
    int term_scroll_top;            ///< DECSTBM top row (0-based, screen-relative; -1 unset).
    int term_scroll_bottom;         ///< DECSTBM bottom row (0-based, inclusive; -1 unset).
    int primary_term_scroll_top;    ///< Saved primary DECSTBM top while alternate_screen.
    int primary_term_scroll_bottom; ///< Saved primary DECSTBM bottom while alternate_screen.
    bool term_cursor_hidden;        ///< DECRST ?25: terminal caret suppressed.
    bool bracketed_paste;           ///< DECSET ?2004: wrap pastes in ESC[200~ / ESC[201~.
    bool app_cursor_keys;           ///< DECSET ?1: unmodified arrows send SS3 (ESC O A..D).
    bool ansi_reverse;              ///< SGR 7: swap fg/bg for newly written cells.
    uint8_t term_tab_stops[512];    ///< Tab-stop bitset, one bit per column (4096 columns).

    // Callbacks
    void (*on_line_click)(struct vg_outputpane *pane, int line, int col, void *user_data);
    void *user_data;
} vg_outputpane_t;

/// @brief Create a new output pane (not yet attached to a parent).
/// @return New output pane or NULL on failure.
vg_outputpane_t *vg_outputpane_create(void);

/// @brief Destroy an output pane and free all line/segment data.
/// @param pane Output pane to destroy (may be NULL).
void vg_outputpane_destroy(vg_outputpane_t *pane);

/// @brief Append text to the output pane, parsing embedded ANSI escape codes.
/// @param pane Output pane.
/// @param text Null-terminated text to append (may contain ANSI sequences).
void vg_outputpane_append(vg_outputpane_t *pane, const char *text);

/// @brief Append a complete line (a newline is appended automatically).
/// @param pane Output pane.
/// @param text Null-terminated line text (ANSI codes are parsed).
void vg_outputpane_append_line(vg_outputpane_t *pane, const char *text);

/// @brief Append text with explicit colour and style attributes.
/// @param pane Output pane.
/// @param text Null-terminated text to append.
/// @param fg   Foreground ARGB colour (0 = use current foreground).
/// @param bg   Background ARGB colour (0 = use current background).
/// @param bold true to render the segment in bold.
void vg_outputpane_append_styled(
    vg_outputpane_t *pane, const char *text, uint32_t fg, uint32_t bg, bool bold);

/// @brief Remove all output lines and reset the ANSI parser state.
/// @param pane Output pane.
void vg_outputpane_clear(vg_outputpane_t *pane);

/// @brief Scroll to the last line of output.
/// @param pane Output pane.
void vg_outputpane_scroll_to_bottom(vg_outputpane_t *pane);

/// @brief Scroll to the first line of output.
/// @param pane Output pane.
void vg_outputpane_scroll_to_top(vg_outputpane_t *pane);

/// @brief Control whether the pane scrolls to the bottom on each new line.
/// @param pane        Output pane.
/// @param auto_scroll true to keep the view pinned to the newest output.
void vg_outputpane_set_auto_scroll(vg_outputpane_t *pane, bool auto_scroll);

/// @brief Return the currently selected text as a newly allocated string.
/// @param pane Output pane.
/// @return Caller-owned null-terminated string, or NULL if nothing is selected.
char *vg_outputpane_get_selection(vg_outputpane_t *pane);

/// @brief Select all text in the output pane.
/// @param pane Output pane.
void vg_outputpane_select_all(vg_outputpane_t *pane);

/// @brief Set the ring-buffer line limit; oldest lines are discarded when exceeded.
/// @param pane Output pane.
/// @param max  Maximum number of lines to retain (0 = unlimited).
void vg_outputpane_set_max_lines(vg_outputpane_t *pane, size_t max);

/// @brief Set the monospace font used for output text.
/// @param pane Output pane.
/// @param font Font handle (should be a monospace face).
/// @param size Font size in pixels.
void vg_outputpane_set_font(vg_outputpane_t *pane, vg_font_t *font, float size);

/// @brief Pixel advance of one monospace character cell (the width of "M" in the pane's font).
/// @param pane Output pane whose configured font is measured.
/// @return The cell width in pixels, or 0 when no font is set.
int vg_outputpane_cell_width(const vg_outputpane_t *pane);

/// @brief Pixel height of one line in the pane's font.
/// @param pane Output pane whose configured font is measured.
/// @return The line height in pixels, or 0 when no font is set.
int vg_outputpane_cell_height(const vg_outputpane_t *pane);

/// @brief Pixel width of @p text rendered in the pane's font (sums glyph advances).
/// @param pane Output pane supplying the configured font and size.
/// @param text NUL-terminated text to measure.
/// @return The text width in pixels, or 0 when no font is set or @p text is NULL/empty.
int vg_outputpane_measure_text(const vg_outputpane_t *pane, const char *text);

/// @brief Whole character columns that fit across the pane's arranged width.
/// @param pane Arranged output pane to measure.
/// @return floor(width / cellWidth), or 0 when no font is set.
int vg_outputpane_columns_for_width(const vg_outputpane_t *pane);

/// @brief Whole rows that fit down the pane's arranged height.
/// @param pane Arranged output pane to measure.
/// @return floor(height / cellHeight), or 0 when no font is set.
int vg_outputpane_rows_for_height(const vg_outputpane_t *pane);

/// @brief Enable/disable interactive terminal mode. When enabled the pane renders with
///        a cursor-position overwrite model (handles \r, \b, ESC[K, CSI H/f, cursor
///        moves), parses and swallows non-SGR escape sequences, and captures keyboard
///        focus, queueing keystrokes for vg_outputpane_take_input. Off = append-only log.
/// @param pane Output pane whose interpretation and input policy is changed.
/// @param enabled True for interactive terminal behavior, or false for append-only logging.
void vg_outputpane_set_terminal_mode(vg_outputpane_t *pane, bool enabled);

/// @brief Drain queued keystroke bytes (terminal mode). Returns a heap string the caller
///        must free, or NULL when nothing is queued.
///
/// @details This compatibility helper NUL-terminates the returned buffer. Call
///          vg_outputpane_take_input_bytes when the exact byte length matters,
///          because terminal input can contain embedded NUL control bytes.
/// @param pane Terminal-mode output pane whose pending input should be drained.
/// @return NUL-terminated heap buffer owned by the caller, or NULL when empty.
char *vg_outputpane_take_input(vg_outputpane_t *pane);

/// @brief Drain queued terminal-input bytes with an explicit byte count.
///
/// @details The returned buffer is still NUL-terminated for debugging and legacy
///          callers, but @p len_out is authoritative. Use this for PTY bridges
///          and other raw terminal consumers so control bytes such as Ctrl+Space
///          are not truncated by C string functions.
///
/// @param pane Terminal-mode output pane whose pending input should be drained.
/// @param len_out Optional output receiving the number of queued bytes copied.
/// @return Heap buffer owned by the caller, or NULL when no bytes are queued.
char *vg_outputpane_take_input_bytes(vg_outputpane_t *pane, size_t *len_out);

/// @brief Advance the terminal caret blink timer by @p dt seconds (terminal mode + focused);
///        toggles caret visibility and marks the pane for repaint on each phase change.
/// @param pane Output pane whose caret animation is advanced.
/// @param dt Positive elapsed time in seconds.
void vg_outputpane_tick(vg_outputpane_t *pane, float dt);

//=============================================================================
// Breadcrumb Widget
//=============================================================================

/// @brief Breadcrumb dropdown item
typedef struct vg_breadcrumb_dropdown {
    char *label; ///< Dropdown item label
    void *data;  ///< User data
} vg_breadcrumb_dropdown_t;

/// @brief Breadcrumb item
typedef struct vg_breadcrumb_item {
    char *label;         ///< Item label
    char *tooltip;       ///< Tooltip text
    vg_icon_t icon;      ///< Optional icon
    void *user_data;     ///< User data
    bool owns_user_data; ///< Free user_data when the item is destroyed

    // Dropdown items
    vg_breadcrumb_dropdown_t *dropdown_items;
    size_t dropdown_count;
    size_t dropdown_capacity;
} vg_breadcrumb_item_t;

/// @brief Breadcrumb widget structure
typedef struct vg_breadcrumb {
    vg_widget_t base;

    // Items
    vg_breadcrumb_item_t *items; ///< Breadcrumb items
    size_t item_count;
    size_t item_capacity;

    // Styling
    char *separator;            ///< Separator string (default: ">")
    uint32_t item_padding;      ///< Padding around items
    uint32_t separator_padding; ///< Padding around separator
    uint32_t bg_color;
    uint32_t text_color;
    uint32_t hover_bg;
    uint32_t separator_color;

    // Font
    vg_font_t *font;
    float font_size;

    // State
    int hovered_index;    ///< Hovered item index
    bool dropdown_open;   ///< Is dropdown open
    int dropdown_index;   ///< Which item's dropdown is open
    int dropdown_hovered; ///< Hovered dropdown item
    int max_items; ///< Maximum items to display (0 = unlimited, oldest removed when exceeded)

    // Callbacks
    void (*on_click)(struct vg_breadcrumb *bc, int index, void *user_data);
    void (*on_dropdown_select)(struct vg_breadcrumb *bc,
                               int crumb_index,
                               int dropdown_index,
                               void *user_data);
    void *user_data;
} vg_breadcrumb_t;

/// @brief Create a new breadcrumb widget (not yet attached to a parent).
/// @return New breadcrumb or NULL on failure.
vg_breadcrumb_t *vg_breadcrumb_create(void);

/// @brief Destroy a breadcrumb widget and free all item data.
/// @param bc Breadcrumb widget to destroy (may be NULL).
void vg_breadcrumb_destroy(vg_breadcrumb_t *bc);

/// @brief Append a new crumb to the right end of the breadcrumb trail.
/// @param bc    Breadcrumb widget.
/// @param label Display label (copied internally).
/// @param data  Caller-owned data associated with this crumb.
void vg_breadcrumb_push(vg_breadcrumb_t *bc, const char *label, void *data);

/// @brief Remove and free the rightmost crumb.
/// @param bc Breadcrumb widget.
void vg_breadcrumb_pop(vg_breadcrumb_t *bc);

/// @brief Remove and free all crumbs.
/// @param bc Breadcrumb widget.
void vg_breadcrumb_clear(vg_breadcrumb_t *bc);

/// @brief Add a dropdown entry to an existing breadcrumb item.
/// @param item  The parent breadcrumb item.
/// @param label Dropdown item label (copied internally).
/// @param data  Caller-owned data for the dropdown entry.
void vg_breadcrumb_item_add_dropdown(vg_breadcrumb_item_t *item, const char *label, void *data);

/// @brief Set the separator string drawn between crumbs.
/// @param bc  Breadcrumb widget.
/// @param sep Separator string (copied internally; default is ">").
void vg_breadcrumb_set_separator(vg_breadcrumb_t *bc, const char *sep);

/// @brief Set the callback fired when a crumb is clicked.
/// @param bc        Breadcrumb widget.
/// @param callback  Handler called with the breadcrumb, zero-based crumb index, and user_data.
/// @param user_data User data passed to the handler.
void vg_breadcrumb_set_on_click(vg_breadcrumb_t *bc,
                                void (*callback)(vg_breadcrumb_t *, int, void *),
                                void *user_data);

/// @brief Set the font used to render crumb labels.
/// @param bc   Breadcrumb widget.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_breadcrumb_set_font(vg_breadcrumb_t *bc, vg_font_t *font, float size);

/// @brief Set the maximum number of crumbs to display (oldest are removed when exceeded).
/// @param bc  Breadcrumb widget.
/// @param max Maximum crumb count (0 = unlimited).
void vg_breadcrumb_set_max_items(vg_breadcrumb_t *bc, int max);

//=============================================================================
// Grid Widget (interactive viewport-aware tabular data)
//=============================================================================

/// @brief Sparse virtual-cell entry owned by a data grid.
/// @details The concrete layout is private to `vg_datagrid.c`; callers only observe the pointer in
///          `vg_datagrid_t` for diagnostics and must not dereference it.
typedef struct vg_datagrid_virtual_cell vg_datagrid_virtual_cell_t;

/// @brief Interactive, viewport-aware table with optional headers and sparse virtual rows.
/// @details Legacy `SetCell` tables remain display-only until selection, sorting, resizing, or
///          editing is explicitly enabled. Normal cells use a dense owned array. Virtual cells use
///          a sorted sparse array so large logical row counts do not allocate per-row storage.
typedef struct vg_datagrid {
    vg_widget_t base;

    int col_count;    ///< Number of columns.
    char **headers;   ///< [col_count] header strings (entries may be NULL).
    bool has_headers; ///< True once any header is set (a header row is drawn).

    int row_count;    ///< Number of populated rows.
    int row_capacity; ///< Allocated row capacity.
    char **cells;     ///< [row_capacity * col_count] cell strings (entries may be NULL).

    bool virtual_mode;        ///< True when virtual_row_count is authoritative.
    size_t virtual_row_count; ///< Logical virtual row count without per-row allocation.
    vg_datagrid_virtual_cell_t *virtual_cells; ///< Sorted sparse materialized cells (owned).
    size_t virtual_cell_count;                 ///< Number of materialized sparse cells.
    size_t virtual_cell_capacity;              ///< Allocated sparse-cell capacity.

    size_t viewport_first_row; ///< First source row rendered by the explicit viewport.
    size_t viewport_row_count; ///< Maximum rendered rows; zero derives count from widget height.
    size_t scroll_row;         ///< First row selected by ScrollToRow/keyboard scrolling.

    bool selectable;                     ///< Enable pointer/keyboard cell selection.
    size_t selected_row;                 ///< Selected source row, or SIZE_MAX when absent.
    int selected_col;                    ///< Selected column, or -1 when absent.
    uint64_t selection_version;          ///< Monotonic selection transition counter.
    uint64_t reported_selection_version; ///< Last selection transition consumed by polling.

    bool *sortable_columns;         ///< [col_count] columns allowed to publish sort requests.
    int sort_column;                ///< Active sort column, or -1 when unsorted.
    int sort_direction;             ///< -1 descending, 0 none, 1 ascending.
    uint64_t sort_version;          ///< Monotonic sort transition counter.
    uint64_t reported_sort_version; ///< Last sort transition consumed by polling.

    float *column_widths;             ///< [col_count] explicit physical widths; zero means auto.
    int *auto_column_widths;          ///< [col_count] cached measured widths for O(visible) paint.
    bool *resizable_columns;          ///< [col_count] columns that support pointer resize.
    bool resizing_column;             ///< True while a resize drag owns input capture.
    int resize_column;                ///< Column being resized, or -1.
    float resize_start_x;             ///< Widget-local pointer X at resize start.
    float resize_start_width;         ///< Physical column width at resize start.
    bool resize_drag_changed;         ///< True when current capture changed effective width.
    bool suppress_click;              ///< Suppress synthetic header click after a resize drag.
    uint64_t resize_version;          ///< Monotonic effective width-change counter.
    uint64_t reported_resize_version; ///< Last resize transition consumed by polling.
    int resized_column;               ///< Most recently resized column, or -1.

    bool editable;                  ///< Allow BeginEdit/CommitEdit operations.
    bool editing;                   ///< True while one cell is in externally-driven edit mode.
    size_t editing_row;             ///< Source row being edited, or SIZE_MAX.
    int editing_col;                ///< Column being edited, or -1.
    uint64_t edit_version;          ///< Monotonic committed-cell edit counter.
    uint64_t reported_edit_version; ///< Last committed edit consumed by polling.

    vg_font_t *font;    ///< Font for cells and headers.
    float font_size;    ///< Font size in pixels.
    float line_height;  ///< Row height in pixels.
    float cell_padding; ///< Horizontal padding added to each side of a column.

    uint32_t bg_color;     ///< Background fill color.
    uint32_t fg_color;     ///< Cell text color.
    uint32_t header_color; ///< Header text color.
    uint32_t grid_color;   ///< Row/column separator color.
} vg_datagrid_t;

/// @brief Create a tabular grid widget attached to @p parent.
/// @details The grid begins in compatibility display-only mode with no columns or rows. Attached
///          grids are owned by @p parent; detached grids are owned by the caller.
/// @param parent Parent widget, or NULL for a detached grid.
/// @return Newly allocated grid, or NULL on allocation failure.
vg_datagrid_t *vg_datagrid_create(vg_widget_t *parent);

/// @brief Destroy a grid and all dense/sparse cell, header, and column metadata storage.
/// @param grid Grid to destroy; NULL is ignored.
void vg_datagrid_destroy(vg_datagrid_t *grid);

/// @brief Set the number of columns, atomically clearing all existing headers and rows.
/// @details Allocation failure preserves the previous table. Interactive column metadata is reset
///          to its disabled defaults. Negative values are treated as zero.
/// @param grid Grid to update.
/// @param count New column count.
void vg_datagrid_set_columns(vg_datagrid_t *grid, int count);

/// @brief Set copied header text for one column.
/// @details NULL/empty clears the header. Allocation failure preserves the old text.
/// @param grid Grid to update.
/// @param col Zero-based column index.
/// @param text UTF-8 header text, or NULL/empty to clear.
void vg_datagrid_set_header(vg_datagrid_t *grid, int col, const char *text);

/// @brief Set copied text in a dense compatibility cell, growing row count as needed.
/// @details Calling this API exits virtual mode and clears sparse virtual cells. Invalid indices or
///          allocation failure preserve the previous table.
/// @param grid Grid to update.
/// @param row Zero-based dense row index.
/// @param col Zero-based column index.
/// @param text UTF-8 text, or NULL/empty for an empty cell.
void vg_datagrid_set_cell(vg_datagrid_t *grid, int row, int col, const char *text);

/// @brief Return a dense or virtual cell's borrowed text.
/// @param grid Grid to inspect.
/// @param row Zero-based row index.
/// @param col Zero-based column index.
/// @return Borrowed NUL-terminated text, or NULL when empty/out of range.
const char *vg_datagrid_get_cell(const vg_datagrid_t *grid, size_t row, int col);

/// @brief Remove every dense or sparse row while retaining columns and headers.
/// @param grid Grid to clear; NULL is ignored.
void vg_datagrid_clear(vg_datagrid_t *grid);

/// @brief Set the font used for headers/cells and refresh cached auto widths.
/// @param grid Grid to update.
/// @param font Borrowed live font, or NULL to disable text drawing.
/// @param size Physical font size; non-positive values keep the previous positive size.
void vg_datagrid_set_font(vg_datagrid_t *grid, vg_font_t *font, float size);

/// @brief Auto-sized pixel width of a column: the widest of its header and cells, plus
///        padding, unless an explicit width is configured.
/// @param grid Grid to inspect.
/// @param col Zero-based column index.
/// @return Effective physical width, or zero when @p col is invalid.
int vg_datagrid_column_width(const vg_datagrid_t *grid, int col);

/// @brief Return the logical row count for dense or virtual mode.
/// @param grid Grid to inspect.
/// @return Row count saturated at `INT_MAX`, or zero for NULL.
int vg_datagrid_row_count(const vg_datagrid_t *grid);

/// @brief Return the full logical row count without integer saturation.
/// @details This companion to `vg_datagrid_row_count` is intended for runtime bridges and
///          virtualization code whose row domain can exceed `INT_MAX`. Dense compatibility grids
///          remain limited by their signed-int indexing API, while sparse virtual grids use the
///          complete `size_t` range.
/// @param grid Grid to inspect.
/// @return Exact dense-or-virtual logical row count, or zero for NULL.
size_t vg_datagrid_logical_row_count(const vg_datagrid_t *grid);

/// @brief Return the number of columns.
/// @param grid Grid to inspect.
/// @return Non-negative column count, or zero for NULL.
int vg_datagrid_column_count(const vg_datagrid_t *grid);

/// @brief Set the first row and maximum count painted by the grid viewport.
/// @details Only the requested slice is visited during paint. A count of zero derives capacity
///          from arranged height. Values beyond the logical row count are clamped on use.
/// @param grid Grid to update.
/// @param first Zero-based first logical row.
/// @param count Maximum visible row count, or zero for automatic height-based count.
void vg_datagrid_set_viewport_rows(vg_datagrid_t *grid, size_t first, size_t count);

/// @brief Switch to sparse virtual mode with the specified logical row count.
/// @details Dense rows are cleared when entering virtual mode. Shrinking removes materialized cells
///          beyond the new end and clears invalid selection/edit state. No per-row array is
///          allocated.
/// @param grid Grid to update.
/// @param count Logical virtual row count.
void vg_datagrid_set_virtual_row_count(vg_datagrid_t *grid, size_t count);

/// @brief Set one copied sparse virtual cell value.
/// @details The grid must have columns and @p row must be below the virtual row count. Empty text
///          removes an existing sparse entry. Allocation failure is atomic.
/// @param grid Grid to update.
/// @param row Zero-based logical virtual row.
/// @param col Zero-based column.
/// @param text UTF-8 text, or NULL/empty to remove materialization.
/// @return true when the request was valid and storage succeeded, including unchanged values.
bool vg_datagrid_set_virtual_cell(vg_datagrid_t *grid, size_t row, int col, const char *text);

/// @brief Enable or disable cell selection.
/// @details Disabling clears any current selection and records one selection transition.
/// @param grid Grid to update.
/// @param enabled true to accept pointer/keyboard selection.
void vg_datagrid_set_selectable(vg_datagrid_t *grid, bool enabled);

/// @brief Return the selected logical row.
/// @param grid Grid to inspect.
/// @return Selected row, or `SIZE_MAX` when absent/invalid.
size_t vg_datagrid_get_selected_row(const vg_datagrid_t *grid);

/// @brief Return the selected column.
/// @param grid Grid to inspect.
/// @return Zero-based column, or -1 when absent/invalid.
int vg_datagrid_get_selected_column(const vg_datagrid_t *grid);

/// @brief Select one valid cell.
/// @param grid Grid to update; selection must be enabled.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return true when the requested cell is valid, including unchanged selection.
bool vg_datagrid_select_cell(vg_datagrid_t *grid, size_t row, int col);

/// @brief Clear the current grid selection.
/// @param grid Grid to update; NULL or already-clear grids are no-ops.
void vg_datagrid_clear_selection(vg_datagrid_t *grid);

/// @brief Consume the independent selection transition edge.
/// @param grid Grid to inspect.
/// @return true once after one or more unreported selection changes.
bool vg_datagrid_was_selection_changed(vg_datagrid_t *grid);

/// @brief Enable or disable sorting requests for one column.
/// @details Disabling the active sort column clears its sort direction and publishes a sort edge.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param enabled true to allow `SetSort` and header activation.
void vg_datagrid_set_sortable(vg_datagrid_t *grid, int col, bool enabled);

/// @brief Set the active sort request.
/// @details Direction is normalized to -1 (descending), 0 (none), or 1 (ascending). Nonzero
///          directions require a valid sortable column. This records state only; applications or
///          virtual models own data ordering.
/// @param grid Grid to update.
/// @param col Zero-based sort column; ignored for direction zero.
/// @param direction Requested direction.
/// @return true when the request is valid, including unchanged state.
bool vg_datagrid_set_sort(vg_datagrid_t *grid, int col, int direction);

/// @brief Return the active sort column.
/// @param grid Grid to inspect.
/// @return Zero-based column, or -1 when unsorted/invalid.
int vg_datagrid_get_sort_column(const vg_datagrid_t *grid);

/// @brief Return normalized sort direction.
/// @param grid Grid to inspect.
/// @return -1 descending, 0 none, or 1 ascending.
int vg_datagrid_get_sort_direction(const vg_datagrid_t *grid);

/// @brief Consume the independent sort-state transition edge.
/// @param grid Grid to inspect.
/// @return true once after one or more unreported sort changes.
bool vg_datagrid_was_sort_changed(vg_datagrid_t *grid);

/// @brief Set one column's explicit physical width, or reset it to automatic.
/// @details Width zero selects cached auto sizing. Positive widths are clamped to a safe minimum.
///          Effective changes publish the resize edge and revision.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param width Physical toolkit width; non-finite/negative values are rejected.
/// @return true when the request is valid, including unchanged width.
bool vg_datagrid_set_column_width(vg_datagrid_t *grid, int col, float width);

/// @brief Enable or disable pointer resizing for one column boundary.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param enabled true to allow drag resizing.
void vg_datagrid_set_column_resizable(vg_datagrid_t *grid, int col, bool enabled);

/// @brief Consume the independent effective-column-resize edge.
/// @param grid Grid to inspect.
/// @return true once after one or more unreported width changes.
bool vg_datagrid_was_column_resized(vg_datagrid_t *grid);

/// @brief Return the most recently resized column.
/// @param grid Grid to inspect.
/// @return Zero-based column, or -1 when no resize exists/handle is invalid.
int vg_datagrid_get_resized_column(const vg_datagrid_t *grid);

/// @brief Enable or disable externally-driven cell editing.
/// @details Disabling cancels an active edit without changing cell content.
/// @param grid Grid to update.
/// @param enabled true to permit BeginEdit/CommitEdit.
void vg_datagrid_set_editable(vg_datagrid_t *grid, bool enabled);

/// @brief Begin editing one valid cell.
/// @details Editing is controller-driven; text is committed later through
///          `vg_datagrid_commit_edit`. Beginning an edit selects the cell when selection is
///          enabled.
/// @param grid Grid to update.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return true when edit mode began or already targets the requested cell.
bool vg_datagrid_begin_edit(vg_datagrid_t *grid, size_t row, int col);

/// @brief Commit copied UTF-8 text to the active edit cell.
/// @details Dense and sparse virtual cells use their normal atomic setters. A successful effective
///          text change publishes the cell-edited edge; the edit closes even when text is
///          unchanged.
/// @param grid Grid to update.
/// @param text UTF-8 replacement, or NULL/empty to clear.
/// @return true when an active valid edit was committed, otherwise false.
bool vg_datagrid_commit_edit(vg_datagrid_t *grid, const char *text);

/// @brief Cancel active edit mode without changing cell content.
/// @param grid Grid to update; NULL/already-idle grids are no-ops.
void vg_datagrid_cancel_edit(vg_datagrid_t *grid);

/// @brief Query whether a grid has an active edit controller.
/// @param grid Grid to inspect.
/// @return true when editing a valid cell, otherwise false.
bool vg_datagrid_is_editing(const vg_datagrid_t *grid);

/// @brief Consume the independent committed-cell-edit edge.
/// @param grid Grid to inspect.
/// @return true once after one or more unreported effective edits.
bool vg_datagrid_was_cell_edited(vg_datagrid_t *grid);

/// @brief Scroll the viewport so @p row becomes its first visible row.
/// @param grid Grid to update.
/// @param row Requested logical row; values beyond the end clamp to the last row.
void vg_datagrid_scroll_to_row(vg_datagrid_t *grid, size_t row);

/// @brief Return the current first viewport row.
/// @param grid Grid to inspect.
/// @return Zero-based row, or zero for NULL/empty grids.
size_t vg_datagrid_get_scroll_row(const vg_datagrid_t *grid);

#ifdef __cplusplus
}
#endif
