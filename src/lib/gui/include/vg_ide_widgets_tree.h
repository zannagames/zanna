//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_tree.h
// Purpose: TreeView, MenuBar, and ContextMenu widget declarations — hierarchical
//          navigation controls and popup/pull-down menu systems.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
//   - TreeView node depth is unlimited; rendering uses iterative traversal.
//   - Retained TreeView selection flags may contain several nodes only when
//     multi-select is enabled; selected names the primary selected node.
//   - Public TreeView drop positions are stable: BEFORE=0, INTO=1, AFTER=2.
// Ownership/Lifetime:
//   - TreeView and MenuBar are owned by their parent in the widget tree.
//   - ContextMenu may be created without a parent and must be explicitly
//     destroyed.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_widget.h,
//        docs/adr/0163-stable-multiselect-and-row-aware-treeview-editing.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_ide_widgets_common.h"

/// @file
/// @brief Declares tree views, menu bars, pull-down menus, and context menus.
/// @details The hierarchy APIs support stable retained nodes, single or multiple selection,
/// row-aware drag-and-drop, expansion, inline editing, and callback-driven activation. Menu
/// APIs provide nested commands, keyboard navigation, stale-handle detection, and popup state.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// ContextMenu Widget
//=============================================================================

/// @brief ContextMenu widget structure
typedef struct vg_contextmenu {
    vg_widget_t base;

    // Menu items (reuses vg_menu_item_t structure)
    vg_menu_item_t **items;        ///< Array of items
    size_t item_count;             ///< Number of items
    size_t item_capacity;          ///< Allocated capacity
    vg_menu_item_t *retired_items; ///< Removed items kept until menu destroy for stale handles

    // Positioning
    int anchor_x; ///< Screen X where menu appears
    int anchor_y; ///< Screen Y where menu appears

    // State
    bool is_visible;                       ///< Is menu visible
    int hovered_index;                     ///< Hovered item index (-1 if none)
    int clicked_index;                     ///< Last clicked item index (-1 if none, edge-triggered)
    struct vg_contextmenu *active_submenu; ///< Open submenu
    struct vg_contextmenu *parent_menu;    ///< Parent menu (for submenus)
    struct vg_menu_item *parent_item;      ///< Parent item that owns this submenu, if any

    // Styling
    uint32_t min_width;  ///< Minimum menu width (default: 150)
    uint32_t max_height; ///< Maximum height before scrolling

    // Font
    vg_font_t *font; ///< Font for text
    float font_size; ///< Font size

    // Colors
    uint32_t bg_color;        ///< Background color
    uint32_t hover_color;     ///< Hover color
    uint32_t text_color;      ///< Text color
    uint32_t disabled_color;  ///< Disabled text color
    uint32_t border_color;    ///< Border color
    uint32_t separator_color; ///< Separator color

    // Callbacks
    void *user_data;       ///< Legacy selection user data.
    void *on_select_data;  ///< User data for selection callback.
    void *on_dismiss_data; ///< User data for dismiss callback.
    void (*on_select)(struct vg_contextmenu *menu,
                      vg_menu_item_t *item,
                      void *user_data);                               ///< Selection callback
    void (*on_dismiss)(struct vg_contextmenu *menu, void *user_data); ///< Dismiss callback
} vg_contextmenu_t;

/// @brief Create a new context menu (not attached to a widget or parent).
/// @return New context menu or NULL on failure.
vg_contextmenu_t *vg_contextmenu_create(void);

/// @brief Destroy a context menu and free all items.
/// @param menu Context menu to destroy (may be NULL).
void vg_contextmenu_destroy(vg_contextmenu_t *menu);

/// @brief Add a clickable item to the context menu.
/// @param menu      Context menu.
/// @param label     Item label text (copied internally).
/// @param shortcut  Keyboard shortcut display string, e.g. "Ctrl+C" (copied; may be NULL).
/// @param action    Callback invoked when the item is selected (may be NULL).
/// @param user_data User data passed to @p action.
/// @return Newly added menu item, or NULL on failure.
vg_menu_item_t *vg_contextmenu_add_item(vg_contextmenu_t *menu,
                                        const char *label,
                                        const char *shortcut,
                                        void (*action)(void *),
                                        void *user_data);

/// @brief Add a nested submenu item to the context menu.
/// @param menu    Context menu.
/// @param label   Label for the submenu item (copied internally).
/// @param submenu Submenu shown when this item is hovered.
/// @return Menu item representing the submenu entry, or NULL on failure.
vg_menu_item_t *vg_contextmenu_add_submenu(vg_contextmenu_t *menu,
                                           const char *label,
                                           vg_contextmenu_t *submenu);

/// @brief Add a horizontal separator line to the context menu.
/// @param menu Context menu.
/// @return The separator menu item, or NULL on failure.
vg_menu_item_t *vg_contextmenu_add_separator(vg_contextmenu_t *menu);

/// @brief Remove and free all items from the context menu.
/// @param menu Context menu.
void vg_contextmenu_clear(vg_contextmenu_t *menu);

/// @brief Reclaim one specific retired context-menu item tombstone.
/// @details Unlinks and frees @p item only when it is present in the live menu's retired-item
///          chain. Managed runtimes call this after the last wrapper disappears. Live items,
///          foreign addresses, NULL inputs, and already reclaimed items return false without
///          mutation. Any submenu owned by the item was already detached during retirement.
/// @param menu Live context menu that owns the tombstone.
/// @param item Candidate retired item record.
/// @return true when the exact tombstone was reclaimed, otherwise false.
bool vg_contextmenu_reclaim_retired_item(vg_contextmenu_t *menu, vg_menu_item_t *item);

/// @brief Enable or disable a menu item (greyed out when disabled).
/// @param item    Menu item.
/// @param enabled true to make the item interactive.
void vg_contextmenu_item_set_enabled(vg_menu_item_t *item, bool enabled);

/// @brief Set the checked (tick mark) state of a menu item.
/// @param item    Menu item.
/// @param checked true to show a check mark beside the item.
void vg_contextmenu_item_set_checked(vg_menu_item_t *item, bool checked);

/// @brief Set the icon displayed beside a menu item.
/// @details Takes ownership of the icon payload (deep copy not made).
/// @param item Menu item.
/// @param icon Icon to display.
void vg_contextmenu_item_set_icon(vg_menu_item_t *item, vg_icon_t icon);

/// @brief Show the context menu at specific screen coordinates.
/// @param menu Context menu.
/// @param x    Horizontal position in screen pixels.
/// @param y    Vertical position in screen pixels.
void vg_contextmenu_show_at(vg_contextmenu_t *menu, int x, int y);

/// @brief Show the context menu offset from a widget's origin.
/// @param menu     Context menu.
/// @param widget   Reference widget.
/// @param offset_x Horizontal offset from the widget's left edge in pixels.
/// @param offset_y Vertical offset from the widget's top edge in pixels.
void vg_contextmenu_show_for_widget(vg_contextmenu_t *menu,
                                    vg_widget_t *widget,
                                    int offset_x,
                                    int offset_y);

/// @brief Hide the context menu.
/// @param menu Context menu.
void vg_contextmenu_dismiss(vg_contextmenu_t *menu);

/// @brief Set the callback fired when an item is selected.
/// @param menu      Context menu.
/// @param callback  Handler called with the menu, selected item, and user_data.
/// @param user_data User data passed to the handler.
void vg_contextmenu_set_on_select(vg_contextmenu_t *menu,
                                  void (*callback)(vg_contextmenu_t *, vg_menu_item_t *, void *),
                                  void *user_data);

/// @brief Set the callback fired when the menu is dismissed without a selection.
/// @param menu      Context menu.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_contextmenu_set_on_dismiss(vg_contextmenu_t *menu,
                                   void (*callback)(vg_contextmenu_t *, void *),
                                   void *user_data);

/// @brief Attach a context menu to a widget so it opens on right-click.
/// @param widget Widget that should trigger the menu.
/// @param menu   Context menu to show on right-click.
void vg_contextmenu_register_for_widget(vg_widget_t *widget, vg_contextmenu_t *menu);

/// @brief Detach any registered context menu from a widget.
/// @param widget Widget to detach the menu from.
void vg_contextmenu_unregister_for_widget(vg_widget_t *widget);

/// @brief Forward an event to a widget's registered context menu.
/// @param widget Widget whose context menu should receive the event.
/// @param event  Event to forward.
/// @return true if the context menu handled the event.
bool vg_contextmenu_process_event(vg_widget_t *widget, vg_event_t *event);

/// @brief Set the font used to render menu item labels.
/// @param menu Context menu.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_contextmenu_set_font(vg_contextmenu_t *menu, vg_font_t *font, float size);

/// @brief Apply theme colors to a context menu and all nested submenus.
/// @details Copies popup background, hover, text, disabled, border, and
///          separator colors from @p theme. Font fields are intentionally left
///          untouched so app-level font inheritance and theme refresh can be
///          applied independently.
/// @param menu Context menu root to update; may be NULL.
/// @param theme Theme to copy colors from; NULL resolves the current theme.
void vg_contextmenu_apply_theme(vg_contextmenu_t *menu, const vg_theme_t *theme);

//=============================================================================
// TreeView Widget
//=============================================================================

/// @brief Tree node structure
typedef struct vg_tree_node {
    uint64_t magic;              ///< Live-node sentinel for stale handle detection
    struct vg_treeview *owner;   ///< Owning tree while node is live
    char *text;                  ///< Node text (owned)
    size_t text_len;             ///< Node text length in bytes
    char *icon_text;             ///< Optional UTF-8 icon text rendered before the label (owned)
    size_t icon_text_len;        ///< Icon-text length in bytes
    char *stable_id;             ///< Optional application-stable identifier (owned)
    size_t stable_id_len;        ///< Stable-identifier length in bytes
    void *user_data;             ///< User data associated with node
    bool owns_user_data;         ///< True when user_data is owned and should be freed
    bool expanded;               ///< Is node expanded
    bool selected;               ///< Is node selected
    bool has_children;           ///< Does node have children (for lazy loading)
    bool loading;                ///< Is node loading children (lazy loading)
    struct vg_tree_node *parent; ///< Parent node
    struct vg_tree_node *first_child;
    struct vg_tree_node *last_child;
    struct vg_tree_node *next_sibling;
    struct vg_tree_node *prev_sibling;
    struct vg_tree_node *retired_next; ///< Retired-subtree list link
    int child_count;
    int depth; ///< Depth in tree (0 = root)

    // Icon support
    vg_icon_t icon;          ///< Node icon
    vg_icon_t expanded_icon; ///< Icon when expanded (optional, for folders)
} vg_tree_node_t;

/// @brief TreeView callback types
typedef void (*vg_tree_select_callback_t)(vg_widget_t *tree, vg_tree_node_t *node, void *user_data);
typedef void (*vg_tree_expand_callback_t)(vg_widget_t *tree,
                                          vg_tree_node_t *node,
                                          bool expanded,
                                          void *user_data);
typedef void (*vg_tree_activate_callback_t)(vg_widget_t *tree,
                                            vg_tree_node_t *node,
                                            void *user_data);

/// @brief Drop position for drag-and-drop
typedef enum vg_tree_drop_position {
    VG_TREE_DROP_BEFORE = 0, ///< Drop before target node
    VG_TREE_DROP_INTO = 1,   ///< Drop as child of target node
    VG_TREE_DROP_AFTER = 2   ///< Drop after target node
} vg_tree_drop_position_t;

/// @brief Application-directed TreeView drag-and-drop behavior.
typedef enum vg_treeview_app_dnd_mode {
    VG_TREEVIEW_APP_DND_DISABLED = 0,    ///< Do not enable poll-model dragging.
    VG_TREEVIEW_APP_DND_LEGACY_INTO = 1, ///< Accept container-only INTO drops.
    VG_TREEVIEW_APP_DND_ROW_AWARE = 2    ///< Classify BEFORE/INTO/AFTER on any row.
} vg_treeview_app_dnd_mode_t;

/// @brief Drag-and-drop callback types
typedef bool (*vg_tree_can_drag_callback_t)(vg_tree_node_t *node, void *user_data);
typedef bool (*vg_tree_can_drop_callback_t)(vg_tree_node_t *source,
                                            vg_tree_node_t *target,
                                            vg_tree_drop_position_t position,
                                            void *user_data);
typedef void (*vg_tree_on_drop_callback_t)(vg_tree_node_t *source,
                                           vg_tree_node_t *target,
                                           vg_tree_drop_position_t position,
                                           void *user_data);

/// @brief Lazy loading callback type
typedef void (*vg_tree_load_children_callback_t)(struct vg_treeview *tree,
                                                 vg_tree_node_t *node,
                                                 void *user_data);

/// @brief Borrowed display data for one row supplied by an external virtual-tree model.
/// @details String storage remains owned by the provider and only needs to stay valid until the
///          provider callback returns. Depth is expressed in logical tree levels, not pixels.
typedef struct vg_treeview_virtual_row {
    const char *text;  ///< Borrowed UTF-8 row label; NULL renders as an empty label.
    size_t depth;      ///< Zero-based indentation level.
    bool expanded;     ///< Whether descendants are currently visible.
    bool has_children; ///< Whether the row offers an expand/collapse affordance.
    bool loading;      ///< Whether lazy children are currently being populated.
} vg_treeview_virtual_row_t;

/// @brief Populate one viewport row from an external virtual-tree model.
/// @details The callback is invoked only for rows intersecting the viewport plus at most two
///          safety rows. It must not mutate or destroy the TreeView during the call.
/// @param tree TreeView requesting data.
/// @param index Zero-based index in the model's current flattened visible order.
/// @param out_row Descriptor to initialize with borrowed row data.
/// @param user_data Opaque model-owned pointer supplied when binding.
/// @return true when @p out_row was populated; false renders an empty placeholder row.
typedef bool (*vg_treeview_virtual_provider_t)(struct vg_treeview *tree,
                                               size_t index,
                                               vg_treeview_virtual_row_t *out_row,
                                               void *user_data);

/// @brief User action emitted by a virtual TreeView to its external model.
typedef enum vg_treeview_virtual_action {
    VG_TREEVIEW_VIRTUAL_SELECT,   ///< Make the indexed row the model selection.
    VG_TREEVIEW_VIRTUAL_TOGGLE,   ///< Toggle the indexed row's expanded state.
    VG_TREEVIEW_VIRTUAL_ACTIVATE, ///< Activate the indexed row.
    VG_TREEVIEW_VIRTUAL_PARENT    ///< Select the indexed row's visible parent when available.
} vg_treeview_virtual_action_t;

/// @brief Deliver pointer or keyboard interaction from a virtual TreeView to its model.
/// @param tree TreeView that received the interaction.
/// @param index Current visible-row index targeted by the action.
/// @param action Semantic action requested by the user.
/// @param user_data Opaque model-owned pointer supplied when binding.
typedef void (*vg_treeview_virtual_action_callback_t)(struct vg_treeview *tree,
                                                      size_t index,
                                                      vg_treeview_virtual_action_t action,
                                                      void *user_data);

/// @brief Notify an external model that its non-owning TreeView binding is being detached.
/// @details Called at most once for each successful binding, before explicit clearing,
///          replacement, legacy structural mutation, or TreeView destruction.
/// @param tree TreeView being detached; valid only for the callback duration.
/// @param user_data Opaque model-owned pointer supplied when binding.
typedef void (*vg_treeview_virtual_unbind_callback_t)(struct vg_treeview *tree, void *user_data);

/// @brief TreeView widget structure
typedef struct vg_treeview {
    vg_widget_t base;

    vg_tree_node_t *root;                 ///< Root node (hidden, children are top-level)
    vg_tree_node_t *selected;             ///< Primary selected retained node
    vg_tree_node_t *prev_selected;        ///< Previous selection (for change detection)
    vg_tree_node_t *anchor_selected;      ///< Visible range-selection anchor
    bool multi_select;                    ///< Allow multiple retained nodes to be selected
    vg_tree_node_t *retired_nodes;        ///< Detached stale node subtrees freed on tree destroy
    uint64_t selection_revision;          ///< Incremented whenever logical selection changes
    uint64_t reported_selection_revision; ///< Last selection revision reported to runtime callers
    vg_tree_node_t *last_activated;       ///< Most recently activated live node, if any
    vg_tree_node_t *last_load_requested;  ///< Most recent lazy-child request target, if any
    uint64_t load_request_revision;       ///< Incremented for each lazy-child request
    uint64_t reported_load_request_revision; ///< Last consumed lazy-child request revision
    vg_font_t *font;                         ///< Font for rendering
    float font_size;                         ///< Font size

    // Appearance
    float row_height;     ///< Height of each row
    float indent_size;    ///< Indentation per level
    float icon_size;      ///< Icon size
    float icon_gap;       ///< Gap between icon and text
    uint32_t text_color;  ///< Text color
    uint32_t selected_bg; ///< Selected item background
    uint32_t hover_bg;    ///< Hover background

    // Scrolling
    float scroll_y;                ///< Vertical scroll position
    int visible_start;             ///< First visible row index
    int visible_count;             ///< Number of visible rows
    bool scrollbar_hovered;        ///< Pointer is over the vertical scrollbar gutter
    bool scrollbar_dragging;       ///< Vertical scrollbar thumb owns input capture
    bool scrollbar_suppress_click; ///< Swallow the synthetic click after scrollbar input
    float scrollbar_drag_offset;   ///< Pointer offset inside the thumb while dragging

    // External virtual model. The model and TreeView hold non-owning pointers to each other.
    bool virtual_mode;        ///< Render flattened rows through the external provider when true.
    size_t virtual_row_count; ///< Total currently visible rows in the external model.
    size_t virtual_selected_index; ///< Selected virtual row, or SIZE_MAX when none.
    size_t virtual_hovered_index;  ///< Hovered virtual row, or SIZE_MAX when none.
    vg_treeview_virtual_provider_t virtual_provider;      ///< Viewport row provider.
    vg_treeview_virtual_action_callback_t virtual_action; ///< Interaction callback.
    vg_treeview_virtual_unbind_callback_t virtual_unbind; ///< Lifetime callback.
    void *virtual_model_user_data; ///< Opaque pointer shared by virtual callbacks.

    // Callbacks
    vg_tree_select_callback_t on_select;
    void *on_select_data;
    vg_tree_expand_callback_t on_expand;
    void *on_expand_data;
    vg_tree_activate_callback_t on_activate;
    void *on_activate_data;

    // Lazy loading
    vg_tree_load_children_callback_t on_load_children;
    void *on_load_children_data;

    // Drag and drop
    bool drag_enabled;                     ///< Enable drag-and-drop
    vg_tree_node_t *drag_node;             ///< Node being dragged
    int drag_start_x;                      ///< Drag start X position
    int drag_start_y;                      ///< Drag start Y position
    bool is_dragging;                      ///< Currently dragging
    vg_tree_node_t *drop_target;           ///< Current drop target
    vg_tree_drop_position_t drop_position; ///< Current drop position

    // Drag callbacks
    vg_tree_can_drag_callback_t can_drag;
    vg_tree_can_drop_callback_t can_drop;
    vg_tree_on_drop_callback_t on_drop;
    void *drag_user_data;

    // Application-directed drag-and-drop (poll model for the Zia runtime).
    // A completed drop is latched rather than self-reordered or sent through
    // on_drop. Legacy mode is container-only/INTO; row-aware mode exposes all
    // three drop regions and leaves semantic validation to the application.
    vg_treeview_app_dnd_mode_t app_directed_dnd_mode; ///< Poll-model drop behavior.
    bool drop_latched;                                ///< Completed drop awaits consumption.
    vg_tree_node_t *latched_src;                      ///< Dragged node at latched drop.
    vg_tree_node_t *latched_tgt;                      ///< Target node at latched drop.
    vg_tree_drop_position_t latched_pos;              ///< Position at latched drop.

    // State
    vg_tree_node_t *hovered; ///< Currently hovered node
    bool suppress_click;     ///< Swallow the synthetic click that follows a drag
} vg_treeview_t;

/// @brief Create a new tree view widget.
/// @param parent Parent widget (can be NULL).
/// @return New tree view or NULL on failure.
vg_treeview_t *vg_treeview_create(vg_widget_t *parent);

/// @brief Atomically bind a flattened external model to a TreeView.
/// @details The TreeView stores no per-row strings or nodes in virtual mode and invokes @p provider
///          only for its viewport slice. A successful binding disables drag-and-drop of concrete
///          node handles but preserves the ordinary node tree for use after unbinding. The model
///          and control do not retain or own one another.
/// @param tree TreeView to bind; NULL is rejected.
/// @param row_count Number of rows in the model's current flattened visible order.
/// @param provider Callback supplying borrowed viewport row descriptors; NULL is rejected.
/// @param action Callback receiving semantic selection, toggle, parent, and activation actions;
///               may be NULL for a read-only virtual view.
/// @param user_data Opaque model-owned pointer passed to every callback.
/// @param on_unbind Lifetime callback that clears the model's raw TreeView pointer; may be NULL.
/// @return true when the binding was installed; false on invalid input, leaving any existing
///         binding unchanged.
bool vg_treeview_bind_virtual_model(vg_treeview_t *tree,
                                    size_t row_count,
                                    vg_treeview_virtual_provider_t provider,
                                    vg_treeview_virtual_action_callback_t action,
                                    void *user_data,
                                    vg_treeview_virtual_unbind_callback_t on_unbind);

/// @brief Detach an external model and resume rendering the ordinary retained node tree.
/// @details Invokes the binding's unbind callback exactly once. The concrete nodes that existed
///          before binding are preserved. NULL is ignored.
/// @param tree TreeView whose virtual model should be detached.
void vg_treeview_clear_virtual_model(vg_treeview_t *tree);

/// @brief Update the external model's flattened visible-row count in O(1).
/// @details Selection and scroll are clamped to the new range. This does not call the provider.
/// @param tree Bound virtual TreeView; NULL or non-virtual controls are ignored.
/// @param row_count New flattened visible-row count.
void vg_treeview_set_virtual_row_count(vg_treeview_t *tree, size_t row_count);

/// @brief Select a virtual row without sending an action back to the external model.
/// @details Used by model-driven selection synchronization. Passing SIZE_MAX clears selection;
///          other out-of-range indices are ignored.
/// @param tree Bound virtual TreeView.
/// @param index Row to select, or SIZE_MAX to clear.
void vg_treeview_select_virtual_index(vg_treeview_t *tree, size_t index);

/// @brief Return the selected virtual row index.
/// @param tree TreeView to inspect.
/// @return Selected index, or SIZE_MAX when unbound or unselected.
size_t vg_treeview_get_virtual_selected_index(const vg_treeview_t *tree);

/// @brief Return the first virtual row intersecting the current viewport in O(1).
/// @param tree TreeView to inspect.
/// @return Zero-based first visible row, or zero when unbound/empty.
size_t vg_treeview_get_visible_first(vg_treeview_t *tree);

/// @brief Return the number of virtual rows requested for the viewport in O(1).
/// @details Includes at most two trailing safety rows and never exceeds the row count.
/// @param tree TreeView to inspect.
/// @return Viewport materialization count, or zero when unbound/empty.
size_t vg_treeview_get_visible_count(vg_treeview_t *tree);

/// @brief Invalidate virtual row appearance after a model mutation.
/// @details No row storage is retained by the TreeView, so invalidation only schedules paint and
///          advances the widget revision. NULL and non-virtual controls are ignored.
/// @param tree Bound virtual TreeView.
void vg_treeview_invalidate_virtual_rows(vg_treeview_t *tree);

/// @brief Get the hidden root node (children of root are top-level items).
/// @param tree Tree view widget.
/// @return Root node pointer.
vg_tree_node_t *vg_treeview_get_root(vg_treeview_t *tree);

/// @brief Check whether a node handle still belongs to a live tree.
/// @param node Node handle to test.
/// @return true if the node is live; false if it has been removed or the tree destroyed.
bool vg_tree_node_is_live(const vg_tree_node_t *node);

/// @brief Add a new child node to a parent.
/// @param tree   Tree view widget.
/// @param parent Parent node (pass vg_treeview_get_root() for top-level items).
/// @param text   Node label text (copied internally).
/// @return Newly created node, or NULL on failure.
vg_tree_node_t *vg_treeview_add_node(vg_treeview_t *tree, vg_tree_node_t *parent, const char *text);

/// @brief Remove a node and all of its descendants from the tree.
/// @param tree Tree view widget.
/// @param node Node to remove (must belong to @p tree).
void vg_treeview_remove_node(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Remove all nodes from the tree, retiring stale handles until destroy or prune.
/// @param tree Tree view widget.
void vg_treeview_clear(vg_treeview_t *tree);

/// @brief Free retired node tombstones after all stale node handles are discarded.
/// @param tree Tree view widget.
void vg_treeview_prune_retired_nodes(vg_treeview_t *tree);

/// @brief Reclaim one retired root subtree from a live TreeView.
/// @details Removed TreeView subtrees are linked by their retired root while descendants remain
///          connected beneath that root. This function unlinks and destroys the exact root only
///          when it is present in `tree->retired_nodes`. The caller must prove that no managed
///          wrapper references the root or any descendant. Live/foreign nodes, NULL inputs, and
///          already reclaimed roots return false without side effects. No allocation is performed.
/// @param tree Live TreeView that owns the retirement chain.
/// @param retired_root Candidate root of one retained removed subtree.
/// @return true when the subtree was unlinked and freed, otherwise false.
bool vg_treeview_reclaim_retired_subtree(vg_treeview_t *tree, vg_tree_node_t *retired_root);

/// @brief Expand a node, showing its children.
/// @param tree Tree view widget.
/// @param node Node to expand.
void vg_treeview_expand(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Collapse a node, hiding its children.
/// @param tree Tree view widget.
/// @param node Node to collapse.
void vg_treeview_collapse(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Toggle a node between expanded and collapsed.
/// @param tree Tree view widget.
/// @param node Node to toggle.
void vg_treeview_toggle(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Select a node.
/// @details Replaces the selection in single-select mode and adds @p node in multi-select mode.
///          Passing NULL always clears every retained selection.
/// @param tree Tree view widget.
/// @param node Node to select, or NULL to deselect all.
void vg_treeview_select(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Enable or disable retained-node multi-selection.
/// @details Disabling keeps only the primary selected node. Virtual TreeViews remain
///          single-select regardless of this setting.
/// @param tree Tree view widget.
/// @param enabled true to allow additive, toggle, and visible range selection.
void vg_treeview_set_multi_select(vg_treeview_t *tree, bool enabled);

/// @brief Return the visible node under a window-space point.
/// @param tree Tree view widget.
/// @param x Window-space X coordinate.
/// @param y Window-space Y coordinate.
/// @return Node under the point, or NULL if the point is outside rows.
vg_tree_node_t *vg_treeview_node_at(vg_treeview_t *tree, float x, float y);

/// @brief Scroll the view so that a node is visible.
/// @param tree Tree view widget.
/// @param node Node to scroll into view.
void vg_treeview_scroll_to(vg_treeview_t *tree, vg_tree_node_t *node);

/// @brief Replace a node's display text atomically.
/// @details The UTF-8 text is copied before the previous value is released. Allocation failure
///          therefore leaves the node unchanged. A successful change invalidates layout and paint
///          and advances the owning tree's non-consuming revision.
/// @param node Node to modify; stale or NULL nodes are ignored.
/// @param text NUL-terminated UTF-8 text to copy; NULL is treated as an empty string.
/// @return true when the requested value is installed or already present; false for an invalid
///         node or allocation failure.
bool vg_tree_node_set_text(vg_tree_node_t *node, const char *text);

/// @brief Set arbitrary UTF-8 icon text rendered in the node's icon slot.
/// @details Icon text is copied atomically and may contain one or more Unicode glyphs. Installing
///          non-empty text clears any resource-backed @ref vg_icon_t previously assigned through
///          @ref vg_tree_node_set_icon. Passing NULL or an empty string clears both forms. The
///          owning tree is invalidated only when the visible value changes.
/// @param node Node to modify; stale or NULL nodes are ignored.
/// @param icon_text NUL-terminated UTF-8 icon text to copy, or NULL to clear.
/// @return true on success, including an unchanged value; false for an invalid node or allocation
///         failure.
bool vg_tree_node_set_icon_text(vg_tree_node_t *node, const char *icon_text);

/// @brief Return the node's borrowed UTF-8 icon text.
/// @details The returned pointer remains owned by the node and is invalidated by the next icon
///          mutation, node retirement, or tree destruction. Resource-backed icons do not have a
///          text representation and return NULL.
/// @param node Node to inspect.
/// @return Borrowed icon text, or NULL when absent or the node is stale.
const char *vg_tree_node_get_icon_text(const vg_tree_node_t *node);

/// @brief Set an application-stable identifier on a tree node.
/// @details The identifier is copied atomically. Empty identifiers are permitted and represent
///          "not assigned" to callers that use an empty-string sentinel. The toolkit does not
///          enforce uniqueness; virtualized model adapters enforce uniqueness at their boundary.
/// @param node Node to modify; stale or NULL nodes are ignored.
/// @param stable_id NUL-terminated identifier to copy; NULL is treated as empty.
/// @return true on success, including an unchanged value; false for an invalid node or allocation
///         failure.
bool vg_tree_node_set_stable_id(vg_tree_node_t *node, const char *stable_id);

/// @brief Return a node's borrowed stable identifier.
/// @param node Node to inspect.
/// @return Borrowed NUL-terminated identifier, or an empty string when absent or stale.
const char *vg_tree_node_get_stable_id(const vg_tree_node_t *node);

/// @brief Set arbitrary caller data on a node.
/// @param node Node to modify.
/// @param data Caller-owned pointer stored without copying.
void vg_tree_node_set_data(vg_tree_node_t *node, void *data);

/// @brief Set the font used for node text rendering.
/// @param tree Tree view widget.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_treeview_set_font(vg_treeview_t *tree, vg_font_t *font, float size);

/// @brief Set the callback fired when the selected node changes.
/// @param tree      Tree view widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_treeview_set_on_select(vg_treeview_t *tree,
                               vg_tree_select_callback_t callback,
                               void *user_data);

/// @brief Set the callback fired when a node is expanded or collapsed.
/// @param tree      Tree view widget.
/// @param callback  Handler called with the node and new expanded state.
/// @param user_data User data passed to the handler.
void vg_treeview_set_on_expand(vg_treeview_t *tree,
                               vg_tree_expand_callback_t callback,
                               void *user_data);

/// @brief Set the callback fired when a node is double-clicked (activated).
/// @param tree      Tree view widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_treeview_set_on_activate(vg_treeview_t *tree,
                                 vg_tree_activate_callback_t callback,
                                 void *user_data);

// --- Icon Support ---

/// @brief Set the icon displayed beside a node.
/// @param node Node to modify.
/// @param icon Icon specification (tree takes ownership).
void vg_tree_node_set_icon(vg_tree_node_t *node, vg_icon_t icon);

/// @brief Set the alternate icon shown when a node is in the expanded state.
/// @param node Icon shown while the node is expanded (e.g. open-folder glyph).
void vg_tree_node_set_expanded_icon(vg_tree_node_t *node, vg_icon_t icon);

// --- Drag and Drop ---

/// @brief Enable or disable drag-and-drop reordering.
/// @param tree    Tree view widget.
/// @param enabled true to allow nodes to be dragged.
void vg_treeview_set_drag_enabled(vg_treeview_t *tree, bool enabled);

/// @brief Enable application-directed (poll-model) drag-and-drop.
/// @details Turns on dragging, restricts drops to INTO an expandable node, and
///          latches a completed drop for polling instead of self-reordering.
///          This compatibility operation selects LEGACY_INTO when enabled and
///          DISABLED otherwise.
/// @param tree    Tree view widget.
/// @param enabled true to enable the poll model.
void vg_treeview_set_app_directed_dnd(vg_treeview_t *tree, bool enabled);

/// @brief Select an application-directed drag-and-drop mode.
/// @details Mode changes cancel active and latched drops. Values outside the
///          vg_treeview_app_dnd_mode_t range are ignored.
/// @param tree Tree view widget.
/// @param mode Disabled, legacy container-only INTO, or row-aware mode.
void vg_treeview_set_app_directed_dnd_mode(vg_treeview_t *tree, int mode);

/// @brief True when a completed drop is waiting to be consumed.
bool vg_treeview_has_pending_drop(const vg_treeview_t *tree);

/// @brief The dragged (source) node of the latched drop, or NULL.
vg_tree_node_t *vg_treeview_drop_source(vg_treeview_t *tree);

/// @brief The target node of the latched drop, or NULL.
vg_tree_node_t *vg_treeview_drop_target_node(vg_treeview_t *tree);

/// @brief The latched drop position (0=before, 1=into, 2=after).
int vg_treeview_drop_position_value(const vg_treeview_t *tree);

/// @brief Consume the latched drop so the next drop can be observed.
void vg_treeview_clear_drop(vg_treeview_t *tree);

/// @brief Set all three drag-and-drop callbacks at once.
/// @param tree      Tree view widget.
/// @param can_drag  Returns true if @p node may be dragged.
/// @param can_drop  Returns true if @p source may be dropped onto @p target at @p position.
/// @param on_drop   Called to perform the actual reparenting/reordering.
/// @param user_data User data passed to all three callbacks.
void vg_treeview_set_drag_callbacks(vg_treeview_t *tree,
                                    vg_tree_can_drag_callback_t can_drag,
                                    vg_tree_can_drop_callback_t can_drop,
                                    vg_tree_on_drop_callback_t on_drop,
                                    void *user_data);

// --- Lazy Loading ---

/// @brief Set the callback invoked when a node with @c has_children is first expanded.
/// @param tree      Tree view widget.
/// @param callback  Handler that should call vg_treeview_add_node to populate children.
/// @param user_data User data passed to the handler.
void vg_treeview_set_on_load_children(vg_treeview_t *tree,
                                      vg_tree_load_children_callback_t callback,
                                      void *user_data);

/// @brief Mark a node as having children without actually creating them yet (lazy loading).
/// @param node         Node to modify.
/// @param has_children true to show the expand arrow even when the child list is empty.
void vg_tree_node_set_has_children(vg_tree_node_t *node, bool has_children);

/// @brief Set or clear the "loading" spinner shown while children are being fetched.
/// @param node    Node to modify.
/// @param loading true to display a loading indicator.
void vg_tree_node_set_loading(vg_tree_node_t *node, bool loading);

/// @brief Return whether a live node advertises real or lazily supplied children.
/// @param node Node to inspect.
/// @return true when the node has children or a lazy-child placeholder; false for stale nodes.
bool vg_tree_node_has_children(const vg_tree_node_t *node);

/// @brief Return whether a live node is displaying its lazy-loading indicator.
/// @param node Node to inspect.
/// @return true while loading, otherwise false.
bool vg_tree_node_is_loading(const vg_tree_node_t *node);

/// @brief Consume the tree's independent lazy-child-request edge.
/// @details This does not clear the last requested node, the selection edge, activation edge, or
///          the tree's non-consuming revision. Multiple requests coalesce until observed.
/// @param tree Tree view to inspect.
/// @return true once after one or more unreported requests, otherwise false.
bool vg_treeview_was_load_children_requested(vg_treeview_t *tree);

/// @brief Return the most recently requested lazy-child node without consuming its edge.
/// @param tree Tree view to inspect.
/// @return Borrowed live node, or NULL if no request has occurred or the node was removed.
vg_tree_node_t *vg_treeview_get_load_requested_node(vg_treeview_t *tree);

/// @brief Return the most recently activated node without consuming the activation edge.
/// @param tree Tree view to inspect.
/// @return Borrowed live node, or NULL if no activation has occurred or the node was removed.
vg_tree_node_t *vg_treeview_get_activated_node(vg_treeview_t *tree);

//=============================================================================
// MenuBar Widget
//=============================================================================

/// @brief Parsed keyboard accelerator
typedef struct vg_accelerator {
    int key;            ///< Key code (VG_KEY_*)
    uint32_t modifiers; ///< Modifier flags (VG_MOD_*)
} vg_accelerator_t;

/// @brief Menu item structure (forward declared in vg_ide_widgets_common.h)
struct vg_menu_item {
    uint64_t magic;                           ///< Live-item sentinel for stale handle detection
    char *text;                               ///< Item text (owned, heap-allocated)
    char *shortcut;                           ///< Keyboard shortcut text (owned, heap-allocated)
    vg_accelerator_t accel;                   ///< Parsed accelerator
    void (*action)(void *data);               ///< Action callback
    void *action_data;                        ///< Action data
    bool enabled;                             ///< Is item enabled
    bool checked;                             ///< Is item checked (for toggles)
    bool checkable;                           ///< Can item display/toggle checked state
    bool separator;                           ///< Is this a separator
    bool was_clicked;                         ///< Set true when item is clicked (cleared on read)
    vg_icon_t icon;                           ///< Optional icon (VG_ICON_NONE = no icon)
    struct vg_menu *parent_menu;              ///< Owning menu for change propagation.
    struct vg_contextmenu *owner_contextmenu; ///< Owning context menu for change propagation.
    struct vg_menu *submenu;                  ///< Submenu (if any)
    struct vg_menu_item *next;
    struct vg_menu_item *prev;
    struct vg_menu_item *retired_next; ///< Retired-item list link
};

/// @brief Menu structure (forward declared in vg_ide_widgets_common.h)
struct vg_menu {
    uint64_t magic;                   ///< Live-menu sentinel for stale handle detection
    char *title;                      ///< Menu title (owned, heap-allocated)
    struct vg_menubar *owner_menubar; ///< Owning menubar for change propagation.
    vg_menu_item_t *first_item;
    vg_menu_item_t *last_item;
    vg_menu_item_t *retired_items; ///< Removed items kept until menu destroy for stale handles
    int item_count;
    struct vg_menu *next;
    struct vg_menu *prev;
    struct vg_menu *retired_next; ///< Retired-menu list link
    bool open;                    ///< Is menu currently open
    bool enabled;                 ///< Is menu enabled (default true)
};

/// @brief Accelerator table entry
typedef struct vg_accel_entry {
    vg_accelerator_t accel; ///< Accelerator key
    vg_menu_item_t *item;   ///< Menu item to trigger
    struct vg_accel_entry *next;
} vg_accel_entry_t;

/// @brief MenuBar widget structure
typedef struct vg_menubar {
    vg_widget_t base;

    vg_menu_t *first_menu;       ///< First menu
    vg_menu_t *last_menu;        ///< Last menu
    vg_menu_t *retired_menus;    ///< Removed menus kept until menubar destroy for stale handles
    int menu_count;              ///< Number of menus
    vg_menu_t *open_menu;        ///< Currently open menu
    vg_menu_item_t *highlighted; ///< Currently highlighted item

    vg_font_t *font; ///< Font for rendering
    float font_size; ///< Font size

    // Appearance
    float height;            ///< Menu bar height
    float menu_padding;      ///< Horizontal padding for menu titles
    float item_padding;      ///< Padding for menu items
    uint32_t bg_color;       ///< Background color
    uint32_t text_color;     ///< Text color
    uint32_t highlight_bg;   ///< Highlighted item background
    uint32_t disabled_color; ///< Disabled item text color

    // Keyboard accelerators
    vg_accel_entry_t *accel_table; ///< Accelerator lookup table

    // State
    bool menu_active;      ///< Is any menu active
    bool native_main_menu; ///< Mirrors to the native macOS app menubar.
} vg_menubar_t;

/// @brief Create a new menu bar widget.
/// @param parent Parent widget (can be NULL).
/// @return New menu bar or NULL on failure.
vg_menubar_t *vg_menubar_create(vg_widget_t *parent);

/// @brief Add a top-level pull-down menu to the menu bar.
/// @param menubar Menu bar widget.
/// @param title   Menu title displayed in the bar (copied internally).
/// @return Newly added menu, or NULL on failure.
vg_menu_t *vg_menubar_add_menu(vg_menubar_t *menubar, const char *title);

/// @brief Add a clickable item to a menu.
/// @param menu     Menu to add the item to.
/// @param text     Item label text (copied internally).
/// @param shortcut Keyboard shortcut display string (copied; may be NULL).
/// @param action   Callback invoked when the item is selected (may be NULL).
/// @param data     User data passed to @p action.
/// @return Newly added menu item, or NULL on failure.
vg_menu_item_t *vg_menu_add_item(
    vg_menu_t *menu, const char *text, const char *shortcut, void (*action)(void *), void *data);

/// @brief Add a horizontal separator to a menu.
/// @param menu Menu to add the separator to.
/// @return Separator menu item, or NULL on failure.
vg_menu_item_t *vg_menu_add_separator(vg_menu_t *menu);

/// @brief Add a nested submenu to an existing menu.
/// @param menu  Parent menu.
/// @param title Submenu title (copied internally).
/// @return New submenu, or NULL on failure.
vg_menu_t *vg_menu_add_submenu(vg_menu_t *menu, const char *title);

/// @brief Enable or disable a menu item.
/// @param item    Menu item.
/// @param enabled true to make the item interactive; false to grey it out.
void vg_menu_item_set_enabled(vg_menu_item_t *item, bool enabled);

/// @brief Set the checked (tick mark) state of a menu item.
/// @param item    Menu item.
/// @param checked true to show a check mark beside the label.
void vg_menu_item_set_checked(vg_menu_item_t *item, bool checked);

/// @brief Remove and free a specific item from a menu.
/// @param menu Menu that owns the item.
/// @param item Item to remove.
void vg_menu_remove_item(vg_menu_t *menu, vg_menu_item_t *item);

/// @brief Remove and free all items from a menu.
/// @param menu Menu to clear.
void vg_menu_clear(vg_menu_t *menu);

/// @brief Remove a top-level menu from the menu bar and free it.
/// @param menubar Menu bar widget.
/// @param menu    Menu to remove.
void vg_menubar_remove_menu(vg_menubar_t *menubar, vg_menu_t *menu);

/// @brief Reclaim one retired item from a live or retired menubar menu record.
/// @details Searches `menu->retired_items`, unlinks the exact tombstone, and frees it. The caller
///          must ensure no wrapper references @p item. Foreign/live/already-freed items and NULL
///          inputs return false without side effects. No allocation is performed.
/// @param menu Menu record whose retired-item chain is searched.
/// @param item Candidate retired item record.
/// @return true when the item was found and reclaimed, otherwise false.
bool vg_menu_reclaim_retired_item(vg_menu_t *menu, vg_menu_item_t *item);

/// @brief Reclaim one specific retired menu record from a live MenuBar.
/// @details Unlinks @p menu from `menubar->retired_menus` and frees the menu plus any remaining
///          retired items. The caller must first establish that neither the menu nor any contained
///          item has a managed wrapper. Live menus, foreign addresses, NULL inputs, and already
///          reclaimed menus return false without side effects.
/// @param menubar Live owner whose retired-menu chain is searched.
/// @param menu Candidate retired menu record.
/// @return true when the exact retired menu was reclaimed, otherwise false.
bool vg_menubar_reclaim_retired_menu(vg_menubar_t *menubar, vg_menu_t *menu);

/// @brief Set the font used to render menu bar and item labels.
/// @param menubar Menu bar widget.
/// @param font    Font handle.
/// @param size    Font size in pixels.
void vg_menubar_set_font(vg_menubar_t *menubar, vg_font_t *font, float size);

// --- Keyboard Accelerators ---

/// @brief Parse an accelerator string into a key + modifier combination.
/// @param shortcut Accelerator string, e.g. "Ctrl+S" or "Cmd+Shift+N".
/// @param accel    Output structure that receives the parsed key and modifiers.
/// @return true if parsing succeeded; false for an unrecognised format.
bool vg_parse_accelerator(const char *shortcut, vg_accelerator_t *accel);

/// @brief Register a keyboard shortcut that triggers a menu item.
/// @param menubar  Menu bar that owns the accelerator table.
/// @param item     Menu item to trigger.
/// @param shortcut Accelerator string (see vg_parse_accelerator).
void vg_menubar_register_accelerator(vg_menubar_t *menubar,
                                     vg_menu_item_t *item,
                                     const char *shortcut);

/// @brief Rebuild the accelerator lookup table from all items in all menus.
/// @param menubar Menu bar widget.
void vg_menubar_rebuild_accelerators(vg_menubar_t *menubar);

/// @brief Handle a key event, firing the matching accelerator if found.
/// @param menubar   Menu bar widget.
/// @param key       Virtual key code (VG_KEY_*).
/// @param modifiers Active modifier flags (VG_MOD_*).
/// @return true if an accelerator matched and its action was called.
bool vg_menubar_handle_accelerator(vg_menubar_t *menubar, int key, uint32_t modifiers);

/// @brief Return true if a menu/context-menu item handle is currently live.
/// @param item Item handle to test.
/// @return true when item belongs to a live menu or context menu.
bool vg_menu_item_is_live(const vg_menu_item_t *item);
/// @brief Returns true when @p menu is still part of a live menubar menu tree.
bool vg_menu_is_live(const vg_menu_t *menu);
/// @brief Returns true when @p menu is still a live context menu.
bool vg_contextmenu_is_live(const vg_contextmenu_t *menu);

#ifdef __cplusplus
}
#endif
