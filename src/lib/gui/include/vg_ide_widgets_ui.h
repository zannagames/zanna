//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_ui.h
// Purpose: StatusBar, Toolbar, CommandPalette, Notification, Tooltip, and
//          FloatingPanel widget declarations — chrome and overlay UI for IDE shells.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
//   - StatusBar items are positioned in left/center/right zones.
// Ownership/Lifetime:
//   - Widgets are owned by their parent in the widget tree.
//   - CommandPalette and Tooltip may be parentless and must be explicitly
//     destroyed.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_ide_widgets_common.h"

/// @file
/// @brief Declares IDE chrome, command, notification, tooltip, and floating-panel widgets.
/// @details The API covers zoned status items, configurable toolbars, searchable command
/// palettes, timed notifications, anchored tooltips, and movable overlay panels, with explicit
/// callback and ownership contracts for retained application state.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// StatusBar Widget
//=============================================================================

/// @brief StatusBar item types
typedef enum vg_statusbar_item_type {
    VG_STATUSBAR_ITEM_TEXT,      ///< Static text label
    VG_STATUSBAR_ITEM_BUTTON,    ///< Clickable button
    VG_STATUSBAR_ITEM_PROGRESS,  ///< Progress indicator
    VG_STATUSBAR_ITEM_SEPARATOR, ///< Vertical separator line
    VG_STATUSBAR_ITEM_SPACER     ///< Flexible spacer
} vg_statusbar_item_type_t;

/// @brief StatusBar zone for item placement
typedef enum vg_statusbar_zone {
    VG_STATUSBAR_ZONE_LEFT,   ///< Left-aligned zone
    VG_STATUSBAR_ZONE_CENTER, ///< Center-aligned zone
    VG_STATUSBAR_ZONE_RIGHT   ///< Right-aligned zone
} vg_statusbar_zone_t;

/// @brief StatusBar item structure
typedef struct vg_statusbar_item {
    uint64_t magic;                ///< Live-item sentinel for stale handle detection
    vg_statusbar_item_type_t type; ///< Item type
    struct vg_statusbar *owner;    ///< Owning status bar for invalidation
    char *text;                    ///< Item text (owned)
    char *tooltip;                 ///< Tooltip text (owned)
    float min_width;               ///< Minimum width (0 = auto)
    float max_width;               ///< Maximum width (0 = unlimited)
    uint32_t text_color;           ///< Optional per-item text color.
    bool has_text_color;           ///< True when text_color overrides the status bar default.
    bool visible;                  ///< Is item visible
    int32_t icon_vector_id;        ///< Vector icon id (vg_icon_vector); negative = none.
    float progress;                ///< Progress value (0-1) for progress items
    void *user_data;               ///< User data
    void (*on_click)(struct vg_statusbar_item *, void *); ///< Click callback for buttons
    struct vg_statusbar_item *retired_next;               ///< Retired-item list link
} vg_statusbar_item_t;

/// @brief StatusBar widget structure
typedef struct vg_statusbar {
    vg_widget_t base;

    // Items by zone
    vg_statusbar_item_t **left_items;
    size_t left_count;
    size_t left_capacity;

    vg_statusbar_item_t **center_items;
    size_t center_count;
    size_t center_capacity;

    vg_statusbar_item_t **right_items;
    size_t right_count;
    size_t right_capacity;
    vg_statusbar_item_t *retired_items; ///< Removed items kept until status bar destroy

    // Styling
    int height;            ///< StatusBar height
    float item_padding;    ///< Padding between items
    float separator_width; ///< Separator line width

    // Font
    vg_font_t *font; ///< Font for text
    float font_size; ///< Font size

    // Colors
    uint32_t bg_color;     ///< Background color
    uint32_t text_color;   ///< Text color
    uint32_t hover_color;  ///< Hover background
    uint32_t border_color; ///< Top border color

    // State
    vg_statusbar_item_t *hovered_item; ///< Currently hovered item
} vg_statusbar_t;

/// @brief Create a new status bar widget.
/// @param parent Parent widget (can be NULL).
/// @return New status bar or NULL on failure.
vg_statusbar_t *vg_statusbar_create(vg_widget_t *parent);

/// @brief Add a static text label to a zone.
/// @param sb   Status bar widget.
/// @param zone Placement zone (LEFT, CENTER, or RIGHT).
/// @param text Label text (copied internally).
/// @return New status bar item, or NULL on failure.
vg_statusbar_item_t *vg_statusbar_add_text(vg_statusbar_t *sb,
                                           vg_statusbar_zone_t zone,
                                           const char *text);

/// @brief Add a clickable button to a zone.
/// @param sb        Status bar widget.
/// @param zone      Placement zone.
/// @param text      Button label text (copied internally).
/// @param on_click  Click handler function.
/// @param user_data User data passed to @p on_click.
/// @return New status bar item, or NULL on failure.
vg_statusbar_item_t *vg_statusbar_add_button(vg_statusbar_t *sb,
                                             vg_statusbar_zone_t zone,
                                             const char *text,
                                             void (*on_click)(vg_statusbar_item_t *, void *),
                                             void *user_data);

/// @brief Add an inline progress indicator to a zone.
/// @param sb   Status bar widget.
/// @param zone Placement zone.
/// @return New status bar item, or NULL on failure.
vg_statusbar_item_t *vg_statusbar_add_progress(vg_statusbar_t *sb, vg_statusbar_zone_t zone);

/// @brief Add a vertical separator line between items.
/// @param sb   Status bar widget.
/// @param zone Placement zone.
/// @return New separator item, or NULL on failure.
vg_statusbar_item_t *vg_statusbar_add_separator(vg_statusbar_t *sb, vg_statusbar_zone_t zone);

/// @brief Add a flexible spacer that pushes adjacent items apart.
/// @param sb   Status bar widget.
/// @param zone Placement zone.
/// @return New spacer item, or NULL on failure.
vg_statusbar_item_t *vg_statusbar_add_spacer(vg_statusbar_t *sb, vg_statusbar_zone_t zone);

/// @brief Return true if a status bar item handle is currently live.
/// @param item Item handle to test.
/// @return true when item belongs to a live status bar.
bool vg_statusbar_item_is_live(const vg_statusbar_item_t *item);

/// @brief Remove and free an item from the status bar.
/// @param sb   Status bar widget.
/// @param item Item to remove.
void vg_statusbar_remove_item(vg_statusbar_t *sb, vg_statusbar_item_t *item);

/// @brief Remove and free all items in a zone.
/// @param sb   Status bar widget.
/// @param zone Zone to clear.
void vg_statusbar_clear_zone(vg_statusbar_t *sb, vg_statusbar_zone_t zone);

/// @brief Reclaim one specific retired status-bar item tombstone.
/// @details Searches the owner's retired chain, unlinks the exact item, and frees its cleared
///          record. The caller must invalidate or finalize every external wrapper first. Live,
///          foreign, NULL, and already reclaimed items return false without side effects.
/// @param sb Live StatusBar that owns the retirement chain.
/// @param item Candidate retained item record.
/// @return true when the tombstone was found and freed, otherwise false.
bool vg_statusbar_reclaim_retired_item(vg_statusbar_t *sb, vg_statusbar_item_t *item);

/// @brief Update the text of a text or button item.
/// @param item Item to modify.
/// @param text New text (copied internally).
void vg_statusbar_item_set_text(vg_statusbar_item_t *item, const char *text);

/// @brief Set a per-item text color override.
/// @param item  Item to modify.
/// @param color Text color as 0xRRGGBB.
void vg_statusbar_item_set_text_color(vg_statusbar_item_t *item, uint32_t color);

/// @brief Set (or clear) a leading scalable vector icon on a text/button item.
/// @param item      Item to modify.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_statusbar_item_set_icon_vector(vg_statusbar_item_t *item, int32_t vector_id);

/// @brief Set the hover tooltip for a status bar item.
/// @param item    Item to modify.
/// @param tooltip Tooltip text (copied internally; NULL clears it).
void vg_statusbar_item_set_tooltip(vg_statusbar_item_t *item, const char *tooltip);

/// @brief Set the progress value on a progress-type item.
/// @param item     Status bar item (must be VG_STATUSBAR_ITEM_PROGRESS).
/// @param progress Normalised progress in [0.0, 1.0].
void vg_statusbar_item_set_progress(vg_statusbar_item_t *item, float progress);

/// @brief Show or hide a status bar item.
/// @param item    Item to modify.
/// @param visible true to show the item.
void vg_statusbar_item_set_visible(vg_statusbar_item_t *item, bool visible);

/// @brief Set the font used for status bar text.
/// @param sb   Status bar widget.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_statusbar_set_font(vg_statusbar_t *sb, vg_font_t *font, float size);

/// @brief Update the cursor-position indicator (shows "Ln X, Col Y").
/// @param sb   Status bar widget.
/// @param line One-based line number.
/// @param col  One-based column number.
void vg_statusbar_set_cursor_position(vg_statusbar_t *sb, int line, int col);

//=============================================================================
// Toolbar Widget
//=============================================================================

/// @brief Toolbar item types
typedef enum vg_toolbar_item_type {
    VG_TOOLBAR_ITEM_BUTTON,    ///< Standard button
    VG_TOOLBAR_ITEM_TOGGLE,    ///< Toggle button (checkable)
    VG_TOOLBAR_ITEM_DROPDOWN,  ///< Button with dropdown menu
    VG_TOOLBAR_ITEM_SEPARATOR, ///< Vertical line separator
    VG_TOOLBAR_ITEM_SPACER,    ///< Flexible spacer
    VG_TOOLBAR_ITEM_WIDGET     ///< Custom embedded widget
} vg_toolbar_item_type_t;

/// @brief Toolbar orientation
typedef enum vg_toolbar_orientation {
    VG_TOOLBAR_HORIZONTAL, ///< Horizontal toolbar
    VG_TOOLBAR_VERTICAL    ///< Vertical toolbar
} vg_toolbar_orientation_t;

/// @brief Toolbar icon size presets
typedef enum vg_toolbar_icon_size {
    VG_TOOLBAR_ICONS_SMALL,  ///< 16x16 icons
    VG_TOOLBAR_ICONS_MEDIUM, ///< 24x24 icons
    VG_TOOLBAR_ICONS_LARGE   ///< 32x32 icons
} vg_toolbar_icon_size_t;

/// @brief Toolbar item structure
typedef struct vg_toolbar_item {
    uint64_t magic;              ///< Live-item sentinel for stale handle detection
    vg_toolbar_item_type_t type; ///< Item type
    struct vg_toolbar *owner;    ///< Owning toolbar for invalidation/popup handling
    char *id;                    ///< Unique identifier
    char *label;                 ///< Text label (optional)
    char *tooltip;               ///< Hover tooltip
    vg_icon_t icon;              ///< Icon specification
    bool enabled;                ///< Enabled state
    bool checked;                ///< For toggle items
    bool show_label;             ///< Show text label
    bool was_clicked;            ///< Set true when item is clicked (cleared on read)

    struct vg_menu *dropdown_menu; ///< Dropdown menu (for DROPDOWN type)
    vg_widget_t *custom_widget;    ///< Custom widget (for WIDGET type)

    void *user_data;                                           ///< User data
    void (*on_click)(struct vg_toolbar_item *, void *);        ///< Click callback
    void (*on_toggle)(struct vg_toolbar_item *, bool, void *); ///< Toggle callback
    struct vg_toolbar_item *retired_next;                      ///< Retired-item list link
} vg_toolbar_item_t;

/// @brief Toolbar widget structure
typedef struct vg_toolbar {
    vg_widget_t base;

    vg_toolbar_item_t **items;        ///< Array of items
    size_t item_count;                ///< Number of items
    size_t item_capacity;             ///< Allocated capacity
    vg_toolbar_item_t *retired_items; ///< Removed items kept until toolbar destroy

    // Configuration
    vg_toolbar_orientation_t orientation; ///< Orientation
    vg_toolbar_icon_size_t icon_size;     ///< Icon size preset
    uint32_t item_padding;                ///< Padding around items
    uint32_t item_spacing;                ///< Space between items
    bool show_labels;                     ///< Global label visibility
    bool overflow_menu;                   ///< Show overflow items in dropdown

    // Font
    vg_font_t *font; ///< Font for text
    float font_size; ///< Font size

    // Colors
    uint32_t bg_color;       ///< Background color
    uint32_t hover_color;    ///< Hover color
    uint32_t active_color;   ///< Active/pressed color
    uint32_t text_color;     ///< Text color
    uint32_t disabled_color; ///< Disabled text color

    // State
    vg_toolbar_item_t *hovered_item;  ///< Currently hovered item
    vg_toolbar_item_t *pressed_item;  ///< Currently pressed item
    int overflow_start_index;         ///< First item in overflow (-1 if none)
    bool overflow_button_hovered;     ///< Hover state for overflow button
    vg_contextmenu_t *overflow_popup; ///< Popup for overflowed items
    bool overflow_popup_dirty;        ///< Rebuild popup contents before next show
    vg_contextmenu_t *dropdown_popup; ///< Popup sourced from a dropdown menu item
    vg_toolbar_item_t *dropdown_item; ///< Dropdown item currently showing a popup
    int focused_index;                ///< Keyboard-focused visible item index (-1 if none)
    char *saved_tooltip_text;         ///< Preserved widget tooltip while an item tooltip is active
    bool hover_tooltip_active;        ///< True while widget tooltip is overridden by hovered item
} vg_toolbar_t;

/// @brief Create a new toolbar widget.
/// @param parent      Parent widget (can be NULL).
/// @param orientation VG_TOOLBAR_HORIZONTAL or VG_TOOLBAR_VERTICAL.
/// @return New toolbar or NULL on failure.
vg_toolbar_t *vg_toolbar_create(vg_widget_t *parent, vg_toolbar_orientation_t orientation);

/// @brief Add a standard push button to the toolbar.
/// @param tb        Toolbar widget.
/// @param id        Unique string identifier for the item.
/// @param label     Text label (copied; may be NULL when icon-only).
/// @param icon      Icon specification.
/// @param on_click  Click handler.
/// @param user_data User data passed to @p on_click.
/// @return New toolbar item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_button(vg_toolbar_t *tb,
                                         const char *id,
                                         const char *label,
                                         vg_icon_t icon,
                                         void (*on_click)(vg_toolbar_item_t *, void *),
                                         void *user_data);

/// @brief Add a checkable toggle button to the toolbar.
/// @param tb              Toolbar widget.
/// @param id              Unique string identifier.
/// @param label           Text label (copied; may be NULL).
/// @param icon            Icon specification.
/// @param initial_checked Initial checked state.
/// @param on_toggle       Toggle callback receiving the new checked state.
/// @param user_data       User data passed to @p on_toggle.
/// @return New toolbar item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_toggle(vg_toolbar_t *tb,
                                         const char *id,
                                         const char *label,
                                         vg_icon_t icon,
                                         bool initial_checked,
                                         void (*on_toggle)(vg_toolbar_item_t *, bool, void *),
                                         void *user_data);

/// @brief Add a button that opens a drop-down menu when clicked.
/// @param tb    Toolbar widget.
/// @param id    Unique string identifier.
/// @param label Text label (copied; may be NULL).
/// @param icon  Icon specification.
/// @param menu  Menu to open on click.
/// @return New toolbar item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_dropdown(
    vg_toolbar_t *tb, const char *id, const char *label, vg_icon_t icon, struct vg_menu *menu);

/// @brief Add a vertical separator between toolbar items.
/// @param tb Toolbar widget.
/// @return New separator item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_separator(vg_toolbar_t *tb);

/// @brief Add a flexible spacer that pushes subsequent items to the far end.
/// @param tb Toolbar widget.
/// @return New spacer item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_spacer(vg_toolbar_t *tb);

/// @brief Embed an arbitrary widget in the toolbar.
/// @param tb     Toolbar widget.
/// @param id     Unique string identifier.
/// @param widget Widget to embed (toolbar takes ownership).
/// @return New toolbar item, or NULL on failure.
vg_toolbar_item_t *vg_toolbar_add_widget(vg_toolbar_t *tb, const char *id, vg_widget_t *widget);

/// @brief Remove and free the toolbar item with the given ID.
/// @param tb Toolbar widget.
/// @param id Identifier of the item to remove.
void vg_toolbar_remove_item(vg_toolbar_t *tb, const char *id);

/// @brief Remove and free the exact toolbar item pointer.
/// @param tb Toolbar widget.
/// @param item Item handle previously returned by a toolbar add function.
void vg_toolbar_remove_item_ptr(vg_toolbar_t *tb, vg_toolbar_item_t *item);

/// @brief Reclaim one specific retired toolbar item tombstone.
/// @details Searches `tb->retired_items`, unlinks @p item, and releases its cleared record only
///          after the embedding runtime has discarded all wrappers. Live, foreign, NULL, and
///          already reclaimed items return false without mutation. No allocation is performed.
/// @param tb Live Toolbar that owns the retirement chain.
/// @param item Candidate retained item record.
/// @return true when the exact tombstone was reclaimed, otherwise false.
bool vg_toolbar_reclaim_retired_item(vg_toolbar_t *tb, vg_toolbar_item_t *item);

/// @brief Look up a toolbar item by its unique ID.
/// @param tb Toolbar widget.
/// @param id Identifier to search for.
/// @return Matching toolbar item, or NULL if not found.
vg_toolbar_item_t *vg_toolbar_get_item(vg_toolbar_t *tb, const char *id);

/// @brief Return true if a toolbar item handle is currently live.
/// @param item Item handle to test.
/// @return true when item belongs to a live toolbar.
bool vg_toolbar_item_is_live(const vg_toolbar_item_t *item);

/// @brief Enable or disable a toolbar item.
/// @param item    Toolbar item.
/// @param enabled true to make the item interactive.
void vg_toolbar_item_set_enabled(vg_toolbar_item_t *item, bool enabled);

/// @brief Set the checked state of a toggle item.
/// @param item    Toolbar item (must be VG_TOOLBAR_ITEM_TOGGLE).
/// @param checked New checked state.
void vg_toolbar_item_set_checked(vg_toolbar_item_t *item, bool checked);

/// @brief Set the hover tooltip for a toolbar item.
/// @param item    Toolbar item.
/// @param tooltip Tooltip text (copied internally; NULL clears it).
void vg_toolbar_item_set_tooltip(vg_toolbar_item_t *item, const char *tooltip);

/// @brief Update the text label on a toolbar item.
/// @param item Toolbar item.
/// @param text New label text (copied internally).
void vg_toolbar_item_set_text(vg_toolbar_item_t *item, const char *text);

/// @brief Replace the icon on a toolbar item.
/// @param item Toolbar item.
/// @param icon New icon specification (toolbar takes ownership).
void vg_toolbar_item_set_icon(vg_toolbar_item_t *item, vg_icon_t icon);

/// @brief Set the icon size preset for all toolbar items.
/// @param tb   Toolbar widget.
/// @param size VG_TOOLBAR_ICONS_SMALL (16px), MEDIUM (24px), or LARGE (32px).
void vg_toolbar_set_icon_size(vg_toolbar_t *tb, vg_toolbar_icon_size_t size);

/// @brief Show or hide text labels on all toolbar buttons.
/// @param tb   Toolbar widget.
/// @param show true to display text labels alongside icons.
void vg_toolbar_set_show_labels(vg_toolbar_t *tb, bool show);

/// @brief Set the font used to render toolbar labels.
/// @param tb   Toolbar widget.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_toolbar_set_font(vg_toolbar_t *tb, vg_font_t *font, float size);

//=============================================================================
// Tooltip Widget
//=============================================================================

/// @brief Tooltip position mode
typedef enum vg_tooltip_position {
    VG_TOOLTIP_FOLLOW_CURSOR, ///< Follow mouse cursor
    VG_TOOLTIP_ANCHOR_WIDGET  ///< Anchor to specific widget
} vg_tooltip_position_t;

/// @brief Tooltip widget structure
typedef struct vg_tooltip {
    vg_widget_t base;

    // Content
    char *text;           ///< Plain text content
    vg_widget_t *content; ///< Rich content (alternative)

    // Timing
    uint32_t show_delay_ms; ///< Delay before showing (default: 500)
    uint32_t hide_delay_ms; ///< Delay before hiding on leave (default: 100)
    uint32_t duration_ms;   ///< Auto-hide after (0 = stay until leave)

    // Positioning
    vg_tooltip_position_t position_mode;
    int offset_x;               ///< X offset from anchor
    int offset_y;               ///< Y offset from anchor
    vg_widget_t *anchor_widget; ///< Widget to anchor to

    // Styling
    uint32_t max_width;     ///< Max width before wrapping (default: 300)
    uint32_t padding;       ///< Internal padding
    uint32_t corner_radius; ///< Corner radius
    uint32_t bg_color;      ///< Background color
    uint32_t text_color;    ///< Text color
    uint32_t border_color;  ///< Border color

    // Font
    vg_font_t *font;
    float font_size;

    // State
    bool is_visible;     ///< Currently visible
    uint64_t show_timer; ///< Timer for show delay
    uint64_t hide_timer; ///< Timer for hide delay
} vg_tooltip_t;

/// @brief Tooltip manager (singleton pattern)
typedef struct vg_tooltip_manager {
    vg_tooltip_t *active_tooltip; ///< Currently showing tooltip
    vg_widget_t *hovered_widget;  ///< Widget mouse is over
    uint64_t hover_start_time;    ///< When hover started
    bool pending_show;            ///< Tooltip pending display
    int cursor_x;                 ///< Cursor position
    int cursor_y;
} vg_tooltip_manager_t;

/// @brief Create a new tooltip widget (not attached to any parent).
/// @return New tooltip or NULL on failure.
vg_tooltip_t *vg_tooltip_create(void);

/// @brief Destroy a tooltip widget and free its resources.
/// @param tooltip Tooltip to destroy (may be NULL).
void vg_tooltip_destroy(vg_tooltip_t *tooltip);

/// @brief Set the plain-text content of the tooltip.
/// @param tooltip Tooltip widget.
/// @param text    Tooltip text (copied internally).
void vg_tooltip_set_text(vg_tooltip_t *tooltip, const char *text);

/// @brief Position and show the tooltip at specific screen coordinates.
/// @param tooltip Tooltip widget.
/// @param x       Horizontal position in screen pixels.
/// @param y       Vertical position in screen pixels.
void vg_tooltip_show_at(vg_tooltip_t *tooltip, int x, int y);

/// @brief Hide the tooltip.
/// @param tooltip Tooltip widget.
void vg_tooltip_hide(vg_tooltip_t *tooltip);

/// @brief Anchor the tooltip to appear beside a specific widget.
/// @param tooltip Tooltip widget.
/// @param anchor  Widget to anchor to; the tooltip appears at the widget's edge.
void vg_tooltip_set_anchor(vg_tooltip_t *tooltip, vg_widget_t *anchor);

/// @brief Configure show/hide timing and auto-dismiss duration.
/// @param tooltip       Tooltip widget.
/// @param show_delay_ms Milliseconds to wait before showing after hover.
/// @param hide_delay_ms Milliseconds to wait before hiding after mouse leaves.
/// @param duration_ms   Milliseconds before auto-dismissal (0 = never auto-dismiss).
void vg_tooltip_set_timing(vg_tooltip_t *tooltip,
                           uint32_t show_delay_ms,
                           uint32_t hide_delay_ms,
                           uint32_t duration_ms);

// --- Tooltip Manager ---

/// @brief Get the process-wide tooltip manager singleton.
/// @return Pointer to the global tooltip manager (never NULL after initialization).
vg_tooltip_manager_t *vg_tooltip_manager_get(void);

/// @brief Advance tooltip show/hide timers; call once per frame.
/// @param mgr    Tooltip manager.
/// @param now_ms Current wall-clock time in milliseconds.
void vg_tooltip_manager_update(vg_tooltip_manager_t *mgr, uint64_t now_ms);

/// @brief Return the nearest pending tooltip timer relative to @p now_ms.
/// @details The query covers delayed show, delayed hide, and finite visible duration without
///          mutating manager state. Expired work returns zero, an active future timer returns its
///          integer delay, and a manager with no timer returns -1. The function performs no
///          allocation and accepts NULL.
/// @param mgr Tooltip manager to inspect; NULL has no deadline.
/// @param now_ms Scheduler time in milliseconds using the same clock passed to
///               @ref vg_tooltip_manager_update.
/// @return Milliseconds until the next tooltip transition, zero when due, or -1 for none.
int64_t vg_tooltip_manager_next_deadline_ms(const vg_tooltip_manager_t *mgr, uint64_t now_ms);

/// @brief Notify the manager that the cursor entered a widget.
/// @param mgr    Tooltip manager.
/// @param widget Widget that is now hovered.
/// @param x      Cursor X in screen pixels.
/// @param y      Cursor Y in screen pixels.
void vg_tooltip_manager_on_hover(vg_tooltip_manager_t *mgr, vg_widget_t *widget, int x, int y);

/// @brief Notify the manager that the cursor left the previously hovered widget.
/// @param mgr Tooltip manager.
void vg_tooltip_manager_on_leave(vg_tooltip_manager_t *mgr);

/// @brief Notify manager that a widget is being destroyed.
///
/// Clears any references the manager holds to the widget so subsequent
/// hover/leave callbacks do not dereference freed memory. Safe to call from
/// vg_widget_destroy. Operates on the global manager singleton.
/// @param widget Widget leaving the live registry.
void vg_tooltip_manager_widget_destroyed(vg_widget_t *widget);

/// @brief Notify manager that a widget (or one of its descendants) became hidden/disabled.
///
/// Hides the active tooltip when its hovered widget or anchor lives inside the
/// subtree being hidden. Safe to call from visibility/enabled-state setters.
/// @param widget Root of the subtree becoming hidden or disabled.
void vg_tooltip_manager_widget_hidden(vg_widget_t *widget);

//=============================================================================
// CommandPalette Widget
//=============================================================================

/// @brief Command structure
typedef struct vg_command {
    char *id;                                                ///< Unique ID
    char *label;                                             ///< Display text
    char *description;                                       ///< Optional description
    char *shortcut;                                          ///< Keyboard shortcut display
    char *category;                                          ///< Category for grouping
    vg_icon_t icon;                                          ///< Command icon
    bool enabled;                                            ///< Is command enabled
    void *user_data;                                         ///< User data
    void (*action)(struct vg_command *cmd, void *user_data); ///< Action callback
} vg_command_t;

/// @brief CommandPalette widget structure
typedef struct vg_commandpalette {
    vg_widget_t base;

    // Commands
    vg_command_t **commands; ///< All registered commands
    size_t command_count;
    size_t command_capacity;

    // Filtered results
    vg_command_t **filtered; ///< Filtered command list
    size_t filtered_count;
    size_t filtered_capacity;

    // Search state
    char *placeholder_text; ///< Placeholder shown when query is empty
    char *current_query;    ///< Current search query (UTF-8)
    uint64_t
        query_generation; ///< Bumped whenever current_query changes (poll key for the runtime).
    bool client_filtered; ///< When true, the widget skips its own fuzzy filter and shows
                          ///< commands[] in insertion order (the app ranks/repopulates per
                          ///< keystroke).

    // State
    bool is_visible;         ///< Is palette visible
    int selected_index;      ///< Selected result index
    int hovered_index;       ///< Hovered result index
    int first_visible_index; ///< First filtered result currently visible

    // Appearance
    uint32_t item_height; ///< Logical height of each result item
    uint32_t max_visible; ///< Max visible results
    float width;          ///< Logical palette width before viewport clamping
    uint32_t bg_color;
    uint32_t selected_bg;
    uint32_t text_color;
    uint32_t shortcut_color;

    // Font
    vg_font_t *font;
    float font_size;

    // Callbacks
    void (*on_execute)(struct vg_commandpalette *palette, vg_command_t *cmd, void *user_data);
    void (*on_dismiss)(struct vg_commandpalette *palette, void *user_data);
    void *user_data;
} vg_commandpalette_t;

/// @brief Create a new command palette (not attached to any parent).
/// @return New command palette or NULL on failure.
vg_commandpalette_t *vg_commandpalette_create(void);

/// @brief Destroy a command palette and free all registered commands.
/// @param palette Command palette to destroy (may be NULL).
void vg_commandpalette_destroy(vg_commandpalette_t *palette);

/// @brief Register a command in the palette.
/// @param palette   Command palette.
/// @param id        Unique string identifier (copied internally).
/// @param label     Display label shown in search results (copied internally).
/// @param shortcut  Keyboard shortcut display string (copied; may be NULL).
/// @param action    Callback invoked when the command is selected.
/// @param user_data User data passed to @p action.
/// @return Pointer to the new command, or NULL on failure.
vg_command_t *vg_commandpalette_add_command(vg_commandpalette_t *palette,
                                            const char *id,
                                            const char *label,
                                            const char *shortcut,
                                            void (*action)(vg_command_t *, void *),
                                            void *user_data);

/// @brief Remove and free the command with the given ID.
/// @param palette Command palette.
/// @param id      Identifier of the command to remove.
void vg_commandpalette_remove_command(vg_commandpalette_t *palette, const char *id);

/// @brief Look up a command by its unique ID.
/// @param palette Command palette.
/// @param id      Identifier to search for.
/// @return Matching command, or NULL if not found.
vg_command_t *vg_commandpalette_get_command(vg_commandpalette_t *palette, const char *id);

/// @brief Remove and free all registered commands.
/// @param palette Command palette.
void vg_commandpalette_clear(vg_commandpalette_t *palette);

/// @brief Show the command palette overlay.
/// @param palette Command palette.
void vg_commandpalette_show(vg_commandpalette_t *palette);

/// @brief Hide the command palette overlay.
/// @param palette Command palette.
void vg_commandpalette_hide(vg_commandpalette_t *palette);

/// @brief Toggle the command palette between visible and hidden.
/// @param palette Command palette.
void vg_commandpalette_toggle(vg_commandpalette_t *palette);

/// @brief Execute the currently highlighted command and close the palette.
/// @param palette Command palette.
void vg_commandpalette_execute_selected(vg_commandpalette_t *palette);

/// @brief Set the execute and dismiss callbacks at once.
/// @param palette    Command palette.
/// @param on_execute Called when a command is executed.
/// @param on_dismiss Called when the palette is closed without executing a command.
/// @param user_data  User data passed to both callbacks.
void vg_commandpalette_set_callbacks(vg_commandpalette_t *palette,
                                     void (*on_execute)(vg_commandpalette_t *,
                                                        vg_command_t *,
                                                        void *),
                                     void (*on_dismiss)(vg_commandpalette_t *, void *),
                                     void *user_data);

/// @brief Set the font used in the search input and result list.
/// @param palette Command palette.
/// @param font    Font handle.
/// @param size    Font size in pixels.
void vg_commandpalette_set_font(vg_commandpalette_t *palette, vg_font_t *font, float size);

/// @brief Set the placeholder text shown in the search input when empty.
/// @param palette Command palette.
/// @param text    Placeholder text (copied internally).
void vg_commandpalette_set_placeholder(vg_commandpalette_t *palette, const char *text);

/// @brief Return the live query text (never NULL; "" when empty).
/// @param palette Command palette to query.
/// @return Borrowed NUL-terminated query text.
const char *vg_commandpalette_get_query(vg_commandpalette_t *palette);

/// @brief Return the query generation counter (bumped on every query change).
/// @param palette Command palette to query.
/// @return Monotonic query-generation counter.
uint64_t vg_commandpalette_get_query_generation(vg_commandpalette_t *palette);

/// @brief Programmatically set the query text and re-filter.
/// @param palette Command palette to update.
/// @param text New query text copied by the widget; NULL is treated as empty.
void vg_commandpalette_set_query(vg_commandpalette_t *palette, const char *text);

/// @brief Enable client-filtered mode: show commands in insertion order and let
///        the application filter/rank them (repopulating per keystroke).
/// @param palette Command palette whose filtering policy is changed.
/// @param enabled True to delegate filtering and ranking to the client.
void vg_commandpalette_set_client_filtered(vg_commandpalette_t *palette, bool enabled);

//=============================================================================
// Notification Widget
//=============================================================================

/// @brief Notification type
typedef enum vg_notification_type {
    VG_NOTIFICATION_INFO,    ///< Informational
    VG_NOTIFICATION_SUCCESS, ///< Success message
    VG_NOTIFICATION_WARNING, ///< Warning message
    VG_NOTIFICATION_ERROR    ///< Error message
} vg_notification_type_t;

/// @brief Notification position
typedef enum vg_notification_position {
    VG_NOTIFICATION_TOP_RIGHT,
    VG_NOTIFICATION_TOP_LEFT,
    VG_NOTIFICATION_BOTTOM_RIGHT,
    VG_NOTIFICATION_BOTTOM_LEFT,
    VG_NOTIFICATION_TOP_CENTER,
    VG_NOTIFICATION_BOTTOM_CENTER
} vg_notification_position_t;

/// @brief Single notification
typedef struct vg_notification {
    uint32_t id;                 ///< Unique ID
    vg_notification_type_t type; ///< Notification type
    char *title;                 ///< Title text
    char *message;               ///< Message text
    uint32_t duration_ms;        ///< Auto-dismiss duration (0 = sticky)
    uint64_t created_at;         ///< Creation timestamp

    // Action
    char *action_label; ///< Action button label
    void (*action_callback)(uint32_t id, void *user_data);
    void *action_user_data;

    // State
    float opacity;               ///< Current opacity (for animation)
    float slide_progress;        ///< Entrance / exit slide interpolation (0..1)
    uint64_t dismiss_started_at; ///< When the dismissal animation began (0 = not dismissing)
    bool dismissed;              ///< Dismissal requested / in progress
} vg_notification_t;

/// @brief Notification manager widget
typedef struct vg_notification_manager {
    vg_widget_t base;

    // Notifications
    vg_notification_t **notifications;
    size_t notification_count;
    size_t notification_capacity;

    // Positioning
    vg_notification_position_t position;

    // Styling
    uint32_t max_visible; ///< Max visible notifications
    uint32_t notification_width;
    uint32_t spacing; ///< Space between notifications
    uint32_t margin;  ///< Margin from edges
    uint32_t padding; ///< Internal padding

    // Font
    vg_font_t *font;
    float font_size;
    float title_font_size;

    // Colors per type
    uint32_t info_color;
    uint32_t success_color;
    uint32_t warning_color;
    uint32_t error_color;
    uint32_t bg_color;
    uint32_t text_color;

    // Animation
    uint32_t fade_duration_ms;
    uint32_t slide_duration_ms;

    // ID counter
    uint32_t next_id;
} vg_notification_manager_t;

/// @brief Create a notification manager widget (not attached to a parent).
/// @return New notification manager or NULL on failure.
vg_notification_manager_t *vg_notification_manager_create(void);

/// @brief Destroy a notification manager and all pending notifications.
/// @param mgr Notification manager to destroy (may be NULL).
void vg_notification_manager_destroy(vg_notification_manager_t *mgr);

/// @brief Advance notification animations; call once per frame.
/// @param mgr    Notification manager.
/// @param now_ms Current wall-clock time in milliseconds.
void vg_notification_manager_update(vg_notification_manager_t *mgr, uint64_t now_ms);

/// @brief Return the nearest notification animation or expiry deadline.
/// @details New, entering, and dismissing notifications request immediate or 16 ms animation
///          ticks. Fully visible finite-duration notifications return their remaining lifetime;
///          stable sticky notifications have no deadline. The query is read-only, allocation-free,
///          and uses the same scheduler clock as @ref vg_notification_manager_update.
/// @param mgr Notification manager to inspect; NULL has no deadline.
/// @param now_ms Current scheduler time in milliseconds.
/// @return Milliseconds until scheduled notification work, zero when due, or -1 for none.
int64_t vg_notification_manager_next_deadline_ms(const vg_notification_manager_t *mgr,
                                                 uint64_t now_ms);

/// @brief Show a new notification toast.
/// @param mgr         Notification manager.
/// @param type        Notification severity (INFO, SUCCESS, WARNING, or ERROR).
/// @param title       Title line text (copied internally).
/// @param message     Body text (copied internally; may be NULL).
/// @param duration_ms Auto-dismiss delay in milliseconds (0 = sticky).
/// @return Unique notification ID that can be passed to vg_notification_dismiss.
uint32_t vg_notification_show(vg_notification_manager_t *mgr,
                              vg_notification_type_t type,
                              const char *title,
                              const char *message,
                              uint32_t duration_ms);

/// @brief Show a notification with an inline action button.
/// @param mgr             Notification manager.
/// @param type            Notification severity.
/// @param title           Title line text (copied internally).
/// @param message         Body text (copied internally; may be NULL).
/// @param duration_ms     Auto-dismiss delay (0 = sticky until button or dismiss).
/// @param action_label    Label for the action button (copied internally).
/// @param action_callback Called with the notification ID when the button is clicked.
/// @param user_data       User data passed to @p action_callback.
/// @return Unique notification ID.
uint32_t vg_notification_show_with_action(vg_notification_manager_t *mgr,
                                          vg_notification_type_t type,
                                          const char *title,
                                          const char *message,
                                          uint32_t duration_ms,
                                          const char *action_label,
                                          void (*action_callback)(uint32_t, void *),
                                          void *user_data);

/// @brief Dismiss a specific notification by ID.
/// @param mgr Notification manager.
/// @param id  ID returned by vg_notification_show or vg_notification_show_with_action.
void vg_notification_dismiss(vg_notification_manager_t *mgr, uint32_t id);

/// @brief Dismiss all currently displayed notifications.
/// @param mgr Notification manager.
void vg_notification_dismiss_all(vg_notification_manager_t *mgr);

/// @brief Set the screen corner where notifications appear.
/// @param mgr      Notification manager.
/// @param position One of the VG_NOTIFICATION_* position constants.
void vg_notification_manager_set_position(vg_notification_manager_t *mgr,
                                          vg_notification_position_t position);

/// @brief Set the font used for notification titles and body text.
/// @param mgr  Notification manager.
/// @param font Font handle.
/// @param size Body font size in pixels (title is scaled up automatically).
void vg_notification_manager_set_font(vg_notification_manager_t *mgr, vg_font_t *font, float size);

/// @brief Return the screen-space union of notification pixels currently eligible to paint.
/// @details Computes animated card geometry for at most `max_visible` notifications, excludes
///          fully transparent cards, and expands the result for the active theme's level-2 shadow
///          plus anti-aliasing slack. The manager's full-window arranged base is deliberately not
///          included, because it is only a positioning surface and paints no background. This
///          allocation-free query is intended for retained damage tracking and does not mutate
///          notification animation state. Output coordinates are physical framebuffer pixels.
/// @param mgr Notification manager to inspect; NULL produces zero outputs and returns false.
/// @param x Optional destination for the union's screen-space left edge.
/// @param y Optional destination for the union's screen-space top edge.
/// @param width Optional destination for the non-negative union width.
/// @param height Optional destination for the non-negative union height.
/// @return true when at least one notification has visible pixels, otherwise false.
bool vg_notification_manager_get_visual_bounds(
    vg_notification_manager_t *mgr, float *x, float *y, float *width, float *height);

//==========================================================================
// Floating Panel — absolute-positioned overlay widget
//==========================================================================

/// @brief Floating panel widget.
/// @details A lightweight overlay that draws at an absolute screen position
///          regardless of the normal layout hierarchy. Children added via
///          vg_floatingpanel_add_child() are reparented under the panel so hit
///          testing, focus, and destruction follow the normal widget tree, but
///          the panel paints them during the overlay pass so they appear above
///          normal widget content.
typedef struct vg_floatingpanel {
    vg_widget_t base; ///< Base widget (connected to root as last child).

    float abs_x; ///< Absolute screen X position.
    float abs_y; ///< Absolute screen Y position.
    float abs_w; ///< Panel width in pixels.
    float abs_h; ///< Panel height in pixels.

    uint32_t bg_color;     ///< Background fill color (0xAARRGGBB).
    uint32_t border_color; ///< Border color (0xAARRGGBB).
    float border_width;    ///< Border width in pixels (0 = no border).

    bool dragging;       ///< True while the panel is being dragged.
    float drag_offset_x; ///< Pointer X offset from panel origin during drag.
    float drag_offset_y; ///< Pointer Y offset from panel origin during drag.
} vg_floatingpanel_t;

/// @brief Create a floating panel connected to @p root.
/// @param root Root widget; the panel is appended as root's last child so it
///             paints above all sibling content.
/// @return New floating panel or NULL on failure.
vg_floatingpanel_t *vg_floatingpanel_create(vg_widget_t *root);

/// @brief Destroy and free a floating panel and all its children.
/// @param panel Floating panel to destroy (may be NULL).
void vg_floatingpanel_destroy(vg_floatingpanel_t *panel);

/// @brief Set the absolute screen position of the panel.
/// @param panel Floating panel.
/// @param x     Horizontal position in screen pixels.
/// @param y     Vertical position in screen pixels.
void vg_floatingpanel_set_position(vg_floatingpanel_t *panel, float x, float y);

/// @brief Set the panel dimensions.
/// @param panel Floating panel.
/// @param w     Width in pixels.
/// @param h     Height in pixels.
void vg_floatingpanel_set_size(vg_floatingpanel_t *panel, float w, float h);

/// @brief Center the panel within its connected root's bounds (clamped to the top-left).
/// @param panel Floating panel; sized and attached to a root first.
void vg_floatingpanel_center_in_parent(vg_floatingpanel_t *panel);

/// @brief Show or hide the floating panel.
/// @param panel   Floating panel.
/// @param visible Non-zero to show; zero to hide.
void vg_floatingpanel_set_visible(vg_floatingpanel_t *panel, int visible);

/// @brief Add a widget as a child of the floating panel.
/// @param panel Floating panel.
/// @param child Widget to reparent under the panel (ownership transfers).
void vg_floatingpanel_add_child(vg_floatingpanel_t *panel, vg_widget_t *child);
/// @brief Returns true when @p panel is a live floating-panel widget.
/// @param panel Candidate floating-panel handle.
/// @return True when the panel remains in the live-widget registry.
bool vg_floatingpanel_is_live(const vg_floatingpanel_t *panel);

//==========================================================================
// Group Box — titled "card" container for grouping related controls
//==========================================================================

/// @brief Titled card container. Stacks its children vertically below a title
///        and paints an elevated, rounded card background through the shared
///        anti-aliased drawing core (Refined Depth).
typedef struct vg_groupbox {
    vg_widget_t base;      ///< Base widget.
    char *title;           ///< Title text (heap-owned; never NULL).
    uint32_t bg_color;     ///< Card background (0 = theme bg_secondary).
    uint32_t border_color; ///< Border colour (0 = theme border_primary).
    uint32_t title_color;  ///< Title text colour (0 = theme fg_primary).
    float corner_radius;   ///< Corner radius in pixels.
    float padding;         ///< Inner padding in pixels.
    float spacing;         ///< Gap between stacked children.
    vg_font_t *font;       ///< Title font.
    float font_size;       ///< Title font size.
} vg_groupbox_t;

/// @brief Create a titled card container attached to @p parent (may be NULL).
/// @param parent Optional parent that takes ownership of the new group box.
/// @param title Initial title copied by the widget; NULL selects an empty title.
/// @return Newly allocated group box, or NULL on allocation failure.
vg_groupbox_t *vg_groupbox_create(vg_widget_t *parent, const char *title);

/// @brief Destroy a group box and all its children.
/// @param gb Group box to destroy; NULL is ignored.
void vg_groupbox_destroy(vg_groupbox_t *gb);

/// @brief Replace the group box title text (copied internally).
/// @param gb Group box to update.
/// @param title Replacement title; NULL selects an empty title.
void vg_groupbox_set_title(vg_groupbox_t *gb, const char *title);

/// @brief Add a control as a child of the group box.
/// @param gb Group box that takes ownership of @p child.
/// @param child Widget to reparent beneath the group box.
void vg_groupbox_add_child(vg_groupbox_t *gb, vg_widget_t *child);

//=============================================================================
// PopupList Widget (caret-anchored filtered selection list)
//=============================================================================

/// @brief A caret-anchored, filterable, keyboard-navigable popup list rendered in the overlay
///        pass. The host drives keyboard (NavigateUp/Down/AcceptSelected) and visibility;
///        language-specific ranking stays in the host (it adds pre-ranked items).
typedef struct vg_popuplist {
    vg_widget_t base;

    char **items;    ///< All items (heap-owned strings).
    int item_count;
    int item_capacity;

    char *filter;        ///< Active filter (case-insensitive substring), or NULL.
    int *filtered;       ///< Indices into items matching the filter.
    int filtered_count;
    int selected;        ///< Selection index into the filtered view [0, filtered_count).

    float anchor_x; ///< Popup top-left X (overlay coordinates).
    float anchor_y; ///< Popup top-left Y.
    float width;    ///< Popup width in pixels.
    int max_rows;   ///< Maximum visible rows before clamping height.
    bool accepted;  ///< Set by AcceptSelected; consumed by WasAccepted.

    vg_font_t *font;
    float font_size;
    float line_height;

    uint32_t bg_color;
    uint32_t fg_color;
    uint32_t sel_bg_color;
    uint32_t sel_fg_color;
    uint32_t border_color;
} vg_popuplist_t;

/// @brief Create a popup list attached to @p root (rendered in the overlay pass). Hidden initially.
/// @param root Widget-tree root that owns and overlays the popup.
/// @return Newly allocated hidden popup list, or NULL on allocation failure.
vg_popuplist_t *vg_popuplist_create(vg_widget_t *root);
/// @brief Destroy the popup list and free its item/filter storage.
/// @param list Popup list to destroy; NULL is ignored.
void vg_popuplist_destroy(vg_popuplist_t *list);
/// @brief Append an item (the host adds items in its preferred rank order).
/// @param list Popup list receiving the item.
/// @param text Item text copied into list-owned storage.
void vg_popuplist_add_item(vg_popuplist_t *list, const char *text);
/// @brief Remove all items and reset the filter and selection.
/// @param list Popup list to clear.
void vg_popuplist_clear(vg_popuplist_t *list);
/// @brief Set the filter; only items containing it (case-insensitive substring) stay visible.
/// @param list Popup list whose visible projection is recomputed.
/// @param filter Filter text copied by the widget; NULL or empty matches every item.
void vg_popuplist_set_filter(vg_popuplist_t *list, const char *filter);
/// @brief Number of items currently visible (matching the filter).
/// @param list Popup list to query.
/// @return Number of filtered visible items, or zero for NULL.
int vg_popuplist_visible_count(const vg_popuplist_t *list);
/// @brief Move the selection up one visible item (clamped to the first).
/// @param list Popup list whose selection moves.
void vg_popuplist_navigate_up(vg_popuplist_t *list);
/// @brief Move the selection down one visible item (clamped to the last).
/// @param list Popup list whose selection moves.
void vg_popuplist_navigate_down(vg_popuplist_t *list);
/// @brief Set the selection index within the visible items (clamped).
/// @param list Popup list whose selection is updated.
/// @param index Requested zero-based filtered index.
void vg_popuplist_set_selected_index(vg_popuplist_t *list, int index);
/// @brief Selection index within the visible items, or -1 when none are visible.
/// @param list Popup list to query.
/// @return Zero-based filtered selection index, or -1 when no selection exists.
int vg_popuplist_selected_index(const vg_popuplist_t *list);
/// @brief Text of the selected visible item, or NULL when none. Borrowed; do not free.
/// @param list Popup list to query.
/// @return Borrowed selected item text, or NULL.
const char *vg_popuplist_selected_text(const vg_popuplist_t *list);
/// @brief Mark the current selection accepted (consumed by vg_popuplist_was_accepted).
/// @param list Popup list whose selection is latched as accepted.
void vg_popuplist_accept_selected(vg_popuplist_t *list);
/// @brief Whether AcceptSelected was called since the last query (consume-on-read).
/// @param list Popup list whose acceptance latch is consumed.
/// @return True once for each accepted selection.
bool vg_popuplist_was_accepted(vg_popuplist_t *list);
/// @brief Set the popup's anchor (top-left) position in overlay coordinates.
/// @param list Popup list to reposition.
/// @param x Overlay-space left coordinate.
/// @param y Overlay-space top coordinate.
void vg_popuplist_anchor_at(vg_popuplist_t *list, float x, float y);
/// @brief Set the popup width in pixels.
/// @param list Popup list to resize.
/// @param width Requested width in pixels.
void vg_popuplist_set_width(vg_popuplist_t *list, float width);
/// @brief Set the maximum number of visible rows.
/// @param list Popup list to configure.
/// @param max_rows Requested positive row limit.
void vg_popuplist_set_max_rows(vg_popuplist_t *list, int max_rows);
/// @brief Set the item font.
/// @param list Popup list to configure.
/// @param font Borrowed font used to measure and render item text.
/// @param size Font size in pixels.
void vg_popuplist_set_font(vg_popuplist_t *list, vg_font_t *font, float size);
/// @brief Show or hide the popup.
/// @param list Popup list whose visibility changes.
/// @param visible True to show the overlay, or false to hide it.
void vg_popuplist_set_visible(vg_popuplist_t *list, bool visible);
/// @brief Whether the popup is currently visible.
/// @param list Popup list to query.
/// @return True when the popup is visible.
bool vg_popuplist_is_visible(const vg_popuplist_t *list);

#ifdef __cplusplus
}
#endif
