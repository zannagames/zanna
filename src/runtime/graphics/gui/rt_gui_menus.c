//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_menus.c
// Purpose: Bind MenuBar, Menu, MenuItem, and standalone ContextMenu widgets to the runtime and
//   provide shared StatusBar, Toolbar, and icon-validation helpers consumed by rt_gui_bars.c.
//
// Key invariants:
//   - MenuBar owns its menu/item tree. Removal retires lower records until no managed wrapper can
//     reference their tombstones; it never transfers ownership to runtime callers.
//   - MenuItem click state is edge-triggered and cleared each frame by the vg
//     widget's internal update; callers must poll within the same frame.
//   - ContextMenu must be shown explicitly (rt_contextmenu_show) at a screen
//     coordinate; it auto-hides on any click outside its bounds.
//   - Managed Menu, MenuItem, StatusBarItem, ToolbarItem, and ContextMenu handles are authenticated
//     and resolve to NULL after their lower records retire.
//   - Standalone ContextMenus are not children of app->root and therefore must be explicitly
//     registered as the active app overlay while visible.
//
// Ownership/Lifetime:
//   - MenuBar widgets are owned by their widget tree; they own their menu hierarchy.
//   - Standalone ContextMenus own their nested item/submenu hierarchy and require explicit destroy.
//   - C strings passed to menu constructors/mutators are copied by the vg layer; the runtime frees
//     temporary conversions immediately after each call.
//   - Icons returned by the shared conversion helpers own their toolkit payload until destroyed or
//     transferred into a menu item.
//
// Links: src/runtime/graphics/gui/rt_gui_internal.h,
//        src/lib/gui/include/vg_ide_widgets_tree.h,
//        src/runtime/graphics/gui/rt_gui_app.c
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements runtime menus, context menus, and shared bar-item helpers.
 *
 * @details The bindings authenticate stable managed subhandles, maintain
 *          MenuBar-owned and standalone ContextMenu hierarchies, synchronize
 *          accelerators and native menu state, bridge retained item lifetimes,
 *          and provide icon plus StatusBar/Toolbar helpers shared with bar code.
 */

#include "rt_gui_internal.h"
#include "rt_pixels.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Record the StatusBar item activated during the current GUI frame.
/// @param item Borrowed lower-toolkit item pointer, or nullptr to clear the record.
void rt_gui_set_clicked_statusbar_item(void *item);

//=============================================================================
// MenuBar Widget (Phase 2)
//=============================================================================

/// @brief Resolve the MenuBar that owns a regular menu.
/// @param menu Borrowed candidate menu; may be NULL or a detached submenu record.
/// @return Borrowed owning MenuBar, or NULL when no MenuBar owns @p menu.
static vg_menubar_t *rt_gui_menu_owner_from_menu(const vg_menu_t *menu) {
    return menu ? menu->owner_menubar : NULL;
}

/// @brief Resolve the MenuBar that owns a regular menu item.
/// @details Follows the item's borrowed parent-menu link and then its owner link without changing
///          either hierarchy.
/// @param item Borrowed candidate item; may be NULL or belong to a ContextMenu.
/// @return Borrowed owning MenuBar, or NULL when the ownership chain is absent.
static vg_menubar_t *rt_gui_menu_owner_from_item(const vg_menu_item_t *item) {
    return item && item->parent_menu ? item->parent_menu->owner_menubar : NULL;
}

/// @brief Refresh runtime and native accelerator state after a menubar mutation.
///
/// The in-process accelerator table is used by non-native menu dispatch on every
/// platform; macOS also mirrors the same model into the native menu strip.
/// @param menubar Borrowed MenuBar whose current hierarchy should be synchronized; NULL only
///                requests the platform bridge's null-safe no-op path.
static void rt_gui_menu_sync_menubar(vg_menubar_t *menubar) {
    if (menubar)
        vg_menubar_rebuild_accelerators(menubar);
    rt_gui_macos_menu_sync_for_menubar(menubar);
}

/// @brief Validate an opaque handle as a live MenuBar widget.
/// @param menubar Candidate runtime widget handle.
/// @return Borrowed live MenuBar, or NULL for an invalid, stale, or wrong-type handle.
static vg_menubar_t *rt_menubar_checked(void *menubar) {
    return (vg_menubar_t *)rt_gui_widget_handle_checked_type(menubar, VG_WIDGET_MENUBAR);
}

/// @brief Resolve a managed Menu handle to its live lower record.
/// @param menu Candidate runtime subhandle.
/// @return Borrowed live Menu, or NULL for an invalid, retired, or wrong-kind handle.
static vg_menu_t *rt_menu_checked(void *menu) {
    return rt_gui_menu_from_handle(menu);
}

/// @brief Resolve a managed ContextMenu handle to its live lower widget.
/// @param menu Candidate runtime subhandle.
/// @return Borrowed live ContextMenu, or NULL for an invalid, retired, or wrong-kind handle.
static vg_contextmenu_t *rt_contextmenu_checked(void *menu) {
    return rt_gui_contextmenu_from_handle(menu);
}

/// @brief Apply the active app font and theme to a standalone context-menu tree.
/// @details Context menus are not parented into app->root, so the normal
///          App.SetFont widget-tree walk cannot reach them. This helper mirrors
///          the app chrome font and current theme into the menu immediately
///          before it is returned or shown, including any nested submenus.
/// @param menu Context menu root to style; may be NULL.
static void rt_contextmenu_apply_app_defaults(vg_contextmenu_t *menu) {
    if (!menu)
        return;

    rt_gui_app_t *app = rt_gui_get_active_app();
    if (!app)
        app = s_current_app;

    if (app) {
        rt_gui_activate_app(app);
        rt_gui_refresh_theme(app);
        vg_contextmenu_apply_theme(menu, app->theme);
        if (!app->default_font)
            rt_gui_ensure_default_font();
        vg_font_t *font = rt_gui_font_handle_checked(app->default_font);
        if (font)
            vg_contextmenu_set_font(menu, font, rt_gui_app_effective_font_size(app));
    } else {
        vg_contextmenu_apply_theme(menu, NULL);
    }
}

/// @brief Resolve a managed MenuItem handle to its live lower record.
/// @param item Candidate runtime subhandle.
/// @return Borrowed live MenuItem, or NULL for an invalid, retired, or wrong-kind handle.
static vg_menu_item_t *rt_menuitem_checked(void *item) {
    return rt_gui_menu_item_from_handle(item);
}

/// @brief Resolve a managed StatusBarItem handle to its live lower record.
/// @param item Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for an invalid, retired, or wrong-kind handle.
vg_statusbar_item_t *rt_statusbaritem_checked(void *item) {
    return rt_gui_statusbar_item_from_handle(item);
}

/// @brief Resolve a managed ToolbarItem handle to its live lower record.
/// @param item Candidate runtime subhandle.
/// @return Borrowed live item, or NULL for an invalid, retired, or wrong-kind handle.
vg_toolbar_item_t *rt_toolbaritem_checked(void *item) {
    return rt_gui_toolbar_item_from_handle(item);
}

/// @brief Validate an opaque handle as a live StatusBar widget.
/// @param bar Candidate runtime widget handle.
/// @return Borrowed live StatusBar, or NULL for an invalid, stale, or wrong-type handle.
vg_statusbar_t *rt_statusbar_checked(void *bar) {
    return (vg_statusbar_t *)rt_gui_widget_handle_checked_type(bar, VG_WIDGET_STATUSBAR);
}

/// @brief Validate an opaque handle as a live Toolbar widget.
/// @param toolbar Candidate runtime widget handle.
/// @return Borrowed live Toolbar, or NULL for an invalid, stale, or wrong-type handle.
vg_toolbar_t *rt_toolbar_checked(void *toolbar) {
    return (vg_toolbar_t *)rt_gui_widget_handle_checked_type(toolbar, VG_WIDGET_TOOLBAR);
}

/// @brief Range-check a status-bar zone index and narrow it to the enum.
/// @param zone Caller-supplied zone index (LEFT..RIGHT).
/// @param out_zone Receives the validated enum value when in range.
/// @return 1 if @p zone is a valid zone, 0 otherwise.
int rt_statusbar_zone_checked(int64_t zone, vg_statusbar_zone_t *out_zone) {
    if (zone < (int64_t)VG_STATUSBAR_ZONE_LEFT || zone > (int64_t)VG_STATUSBAR_ZONE_RIGHT)
        return 0;
    if (out_zone)
        *out_zone = (vg_statusbar_zone_t)zone;
    return 1;
}

/// @brief Convert a runtime Pixels object (0xRRGGBBAA) into a `vg_icon_t` (RGBA).
/// @details Repacks the pixels' 0xRRGGBBAA uint32 buffer into the byte-order
///          RGBA layout that the GUI library's icon API expects (one byte
///          each, R-G-B-A in memory). Validates `width * height` for
///          integer overflow before allocating — guards against a
///          maliciously-sized Pixels object claiming dimensions whose
///          product overflows `size_t`.
///
///          Unlike `rt_codeeditor_icon_from_pixels`, this function returns
///          an empty `vg_icon_t{0}` on any failure path (NULL pixels, bad
///          dimensions, allocation failure) instead of trapping. Menu
///          icons are decorative — the menu still works without them — so
///          the runtime trades a missing glyph for a usable menu rather
///          than aborting the program.
/// @param pixels Source Pixels object; ownership not transferred.
/// @return Populated icon on success; `{0}` (`type == VG_ICON_NONE`) on
///         failure.
vg_icon_t rt_gui_icon_from_pixels(void *pixels) {
    vg_icon_t icon = {0};
    if (!pixels)
        return icon;

    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    const uint32_t *raw = rt_pixels_raw_buffer(pixels);
    if (width <= 0 || height <= 0 || !raw)
        return icon;
    if ((uintmax_t)width > (uintmax_t)SIZE_MAX || (uintmax_t)height > (uintmax_t)SIZE_MAX)
        return icon;

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count / (size_t)width != (size_t)height)
        return icon;
    if (width > UINT32_MAX || height > UINT32_MAX || pixel_count > SIZE_MAX / 4)
        return icon;

    uint8_t *rgba = (uint8_t *)malloc(pixel_count * 4);
    if (!rgba)
        return icon;

    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t px = raw[i];
        rgba[i * 4 + 0] = (uint8_t)((px >> 24) & 0xFF);
        rgba[i * 4 + 1] = (uint8_t)((px >> 16) & 0xFF);
        rgba[i * 4 + 2] = (uint8_t)((px >> 8) & 0xFF);
        rgba[i * 4 + 3] = (uint8_t)(px & 0xFF);
    }

    icon = vg_icon_from_pixels(rgba, (uint32_t)width, (uint32_t)height);
    free(rgba);
    return icon;
}

/// @brief Load a Pixels image from a file path and convert it to a `vg_icon_t`.
/// @details Loads the file via `rt_pixels_load`, delegates conversion to
///          `rt_gui_icon_from_pixels`, then immediately releases the Pixels
///          object (the icon owns its own RGBA copy). An empty or NULL path,
///          a missing file, or a zero-dimension image all produce a
///          `VG_ICON_NONE` sentinel — callers treat a missing icon as
///          decorative fallback.
/// @param path Filesystem path to the image file (C string).
/// @return Populated icon on success; `{0}` on any failure.
vg_icon_t rt_gui_icon_from_path_cstr(const char *path) {
    vg_icon_t icon = {0};
    if (!path || path[0] == '\0')
        return icon;

    void *pixels = rt_pixels_load(rt_const_cstr(path));
    if (!pixels)
        return icon;

    icon = rt_gui_icon_from_pixels(pixels);
    if (rt_obj_release_check0(pixels))
        rt_obj_free(pixels);
    return icon;
}

/// @brief Create a new menu bar widget at the top of the window.
/// @details Creates a vg_menubar_t and applies the app's default font. On
///          macOS, also registers with the native app menu bar so menus appear
///          in the system menu strip rather than inside the window.
/// @param parent Parent container or app handle.
/// @return Opaque menu bar widget handle, or NULL on failure.
void *rt_menubar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_menubar_t *mb = vg_menubar_create(parent_widget);
    if (mb) {
        if (app)
            rt_gui_activate_app(app);
        rt_gui_ensure_default_font();
        if (app && app->default_font)
            vg_menubar_set_font(mb, app->default_font, rt_gui_app_effective_font_size(app));
        mb->native_main_menu = rt_gui_macos_menu_register_menubar(mb);
        if (mb->native_main_menu) {
            mb->base.constraints.min_height = 0.0f;
            mb->base.constraints.preferred_height = 0.0f;
            mb->base.measured_height = 0.0f;
        }
        rt_gui_menu_sync_menubar(mb);
    }
    return mb;
}

/// @brief Release resources and destroy the menubar.
/// @details Unregisters any native macOS representation, invalidates runtime references through
///          normal widget destruction, and recursively retires/frees the owned menu hierarchy.
/// @param menubar Candidate live MenuBar handle; invalid handles are ignored.
void rt_menubar_destroy(void *menubar) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    if (!mb)
        return;
    rt_gui_macos_menu_unregister_menubar(mb);
    rt_widget_destroy(mb);
}

/// @brief Add a top-level menu (e.g., "File", "Edit") to the menu bar.
/// @details The title text is copied by the vg layer. Syncs with the native
///          macOS menu bar if registered. Returns the menu handle for adding items.
/// @param menubar Menu bar widget handle.
/// @param title   Menu title text (runtime string).
/// @return Opaque menu handle, or NULL on failure.
void *rt_menubar_add_menu(void *menubar, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    if (!mb)
        return NULL;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_menu_t *menu = vg_menubar_add_menu(mb, ctitle);
    free(ctitle);
    rt_gui_menu_sync_menubar(mb);
    return rt_gui_wrap_menu(menu);
}

/// @brief Remove a menu from the menu bar.
/// @details Requires @p menu to be a live managed menu owned directly by @p menubar. The lower menu
///          and its items are retired, unreferenced tombstones are collected, and accelerator plus
///          native-menu state is rebuilt. Foreign or stale pairs are unchanged.
/// @param menubar Candidate owning MenuBar widget.
/// @param menu Candidate managed top-level Menu handle to retire.
void rt_menubar_remove_menu(void *menubar, void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    vg_menu_t *m = rt_menu_checked(menu);
    if (!mb || !m || m->owner_menubar != mb)
        return;
    vg_menubar_remove_menu(mb, m);
    rt_gui_collect_retired_subhandles(&mb->base);
    rt_gui_menu_sync_menubar(mb);
}

/// @brief Get the number of menus in the menu bar.
/// @param menubar Candidate live MenuBar handle.
/// @return Number of live top-level menus, or zero for an invalid handle.
int64_t rt_menubar_get_menu_count(void *menubar) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    return mb ? mb->menu_count : 0;
}

/// @brief Return the `index`-th top-level menu in `menubar`, or NULL if out of range.
///
/// Linear walk through the singly-linked menu list — fine because
/// the typical menubar has under 10 entries (File/Edit/View/Help/…).
/// @param menubar Candidate live MenuBar handle.
/// @param index Zero-based top-level menu index.
/// @return Stable managed Menu handle, or NULL for invalid input, an out-of-range index, or wrapper
///         allocation failure.
void *rt_menubar_get_menu(void *menubar, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    if (!mb)
        return NULL;
    if (index < 0 || index >= mb->menu_count)
        return NULL;

    vg_menu_t *menu = mb->first_menu;
    for (int64_t i = 0; i < index && menu; i++) {
        menu = menu->next;
    }
    return rt_gui_wrap_menu(menu);
}

/// @brief Show or hide the menu bar widget.
/// @details Normalizes @p visible to Boolean, updates retained widget visibility, and rebuilds both
///          in-process and native representations.
/// @param menubar Candidate live MenuBar handle.
/// @param visible Non-zero to show the bar, zero to hide it.
void rt_menubar_set_visible(void *menubar, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    if (!mb)
        return;
    vg_widget_set_visible(&mb->base, visible != 0);
    rt_gui_menu_sync_menubar(mb);
}

/// @brief Check whether the menu bar is currently visible.
/// @param menubar Candidate live MenuBar handle.
/// @return One when the live bar's visibility flag is set, otherwise zero.
int64_t rt_menubar_is_visible(void *menubar) {
    RT_ASSERT_MAIN_THREAD();
    vg_menubar_t *mb = rt_menubar_checked(menubar);
    return mb && mb->base.visible ? 1 : 0;
}

//=============================================================================
// Menu Widget (Phase 2)
//=============================================================================

/// @brief Add a clickable item to a menu.
/// @details The text is copied by the vg layer. Returns the item handle for
///          checking clicks, enabling/disabling, or setting check state. Syncs
///          with the macOS native menu bar if the menu is part of a registered menubar.
/// @param menu Menu handle.
/// @param text Item label text (runtime string).
/// @return Opaque menu item handle, or NULL on failure.
void *rt_menu_add_item(void *menu, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_menu_item_t *item = vg_menu_add_item(m, rt_gui_cstr_or_empty(ctext), NULL, NULL, NULL);
    free(ctext);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
    return rt_gui_wrap_menu_item(item);
}

/// @brief Add an item with a keyboard shortcut (e.g. `"Ctrl+S"`).
/// @details Converts both runtime strings to GUI-safe UTF-8, lets the lower menu copy them, and
///          rebuilds accelerator/native state. The shortcut string is parsed by the platform layer
///          during synchronization; malformed text remains displayable but produces no accelerator.
/// @param menu Candidate live managed Menu handle.
/// @param text Runtime display label; embedded NULs become replacement characters.
/// @param shortcut Runtime accelerator description such as `Ctrl+S`.
/// @return Stable managed MenuItem handle, or NULL for invalid input or allocation failure.
void *rt_menu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    char *cshortcut = rt_string_to_gui_cstr(shortcut);
    vg_menu_item_t *item = vg_menu_add_item(
        m, rt_gui_cstr_or_empty(ctext), rt_gui_cstr_or_empty(cshortcut), NULL, NULL);
    free(ctext);
    free(cshortcut);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
    return rt_gui_wrap_menu_item(item);
}

/// @brief Append a horizontal separator line between menu items.
/// @details Adds an owned non-interactive item and resynchronizes accelerators/native menus.
/// @param menu Candidate live managed Menu handle.
/// @return Stable managed separator-item handle, or NULL on invalid input or allocation failure.
void *rt_menu_add_separator(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return NULL;
    vg_menu_item_t *item = vg_menu_add_separator(m);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
    return rt_gui_wrap_menu_item(item);
}

/// @brief Add a nested submenu with the given title; returns the new submenu handle.
/// @details The parent menu owns the new submenu through its representing item. The lower layer
///          copies the converted title, and synchronization publishes the new hierarchy.
/// @param menu Candidate live managed parent Menu handle.
/// @param title Runtime submenu title.
/// @return Stable managed submenu handle, or NULL for invalid input or allocation failure.
void *rt_menu_add_submenu(void *menu, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return NULL;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_menu_t *submenu = vg_menu_add_submenu(m, ctitle);
    free(ctitle);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
    return rt_gui_wrap_menu(submenu);
}

/// @brief Remove a menu item and free its resources.
/// @details The lower layer accepts only an item whose parent is @p menu, retires its text,
///          shortcut, icon, and submenu payload, then the runtime reclaims only tombstones with no
///          managed wrapper. Accelerator/native state is rebuilt after the attempt.
/// @param menu Candidate live managed owner Menu.
/// @param item Candidate live managed MenuItem owned by @p menu.
void rt_menu_remove_item(void *menu, void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!m || !mi)
        return;
    vg_menu_remove_item(m, mi);
    if (m->owner_menubar)
        rt_gui_collect_retired_subhandles(&m->owner_menubar->base);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
}

/// @brief Remove all items from a menu, leaving it empty.
/// @details Retires the complete owned item/submenu hierarchy, preserves the Menu record, collects
///          unreferenced stable-handle tombstones, and rebuilds accelerator/native state.
/// @param menu Candidate live managed Menu; invalid handles are ignored.
void rt_menu_clear(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return;
    vg_menu_clear(m);
    if (m->owner_menubar)
        rt_gui_collect_retired_subhandles(&m->owner_menubar->base);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
}

/// @brief Set the title of the menu.
/// @details Converts the runtime string before mutating state, so allocation failure preserves the
///          previous title. NULL clears the title. Successful changes rebuild native menu state.
/// @param menu Candidate live managed Menu handle.
/// @param title New runtime title, or NULL to clear it.
void rt_menu_set_title(void *menu, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return;
    char *new_title = title ? rt_string_to_gui_cstr(title) : NULL;
    if (title && !new_title)
        return;
    free(m->title);
    m->title = new_title;
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
}

/// @brief Get the title of the menu.
/// @details Copies the lower NUL-terminated title into a runtime string; no lower storage is
/// exposed.
/// @param menu Candidate live managed Menu handle.
/// @return Runtime title copy, or the canonical empty string for invalid handles or no title.
rt_string rt_menu_get_title(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return rt_str_empty();
    const char *title = m->title;
    if (!title)
        return rt_str_empty();
    return rt_string_from_bytes(title, strlen(title));
}

/// @brief Get the number of items in a menu.
/// @param menu Candidate live managed Menu handle.
/// @return Number of live direct items, including separators and submenu entries, or zero if
/// invalid.
int64_t rt_menu_get_item_count(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    return m ? m->item_count : 0;
}

/// @brief Return the `index`-th item in `menu` (linear walk; safe on out-of-range).
/// @param menu Candidate live managed Menu handle.
/// @param index Zero-based direct-item index.
/// @return Stable managed MenuItem handle, or NULL for invalid input, out-of-range index, or
///         wrapper allocation failure.
void *rt_menu_get_item(void *menu, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return NULL;
    if (index < 0 || index >= m->item_count)
        return NULL;

    vg_menu_item_t *item = m->first_item;
    for (int64_t i = 0; i < index && item; i++) {
        item = item->next;
    }
    return rt_gui_wrap_menu_item(item);
}

/// @brief Enable or disable an entire menu (greys out all items when disabled).
/// @details Normalizes @p enabled to Boolean and republishes accelerator/native menu state.
/// @param menu Candidate live managed Menu handle.
/// @param enabled Non-zero to enable interaction, zero to disable it.
void rt_menu_set_enabled(void *menu, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    if (!m)
        return;
    m->enabled = enabled != 0;
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_menu(m));
}

/// @brief Check whether a menu is currently enabled.
/// @param menu Candidate live managed Menu handle.
/// @return One when the menu is live and enabled, otherwise zero.
int64_t rt_menu_is_enabled(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_t *m = rt_menu_checked(menu);
    return m && m->enabled ? 1 : 0;
}

//=============================================================================
// MenuItem Widget (Phase 2)
//=============================================================================

/// @brief Set the text of the menuitem.
/// @details Converts the entire runtime display string before replacing lower storage. Allocation
///          failure and byte-identical text preserve current state. Regular items rebuild their
///          owning MenuBar; ContextMenu items invalidate their popup layout through the lower API.
/// @param item Candidate live managed MenuItem handle.
/// @param text New runtime display label, or NULL to clear it.
void rt_menuitem_set_text(void *item, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    char *new_text = text ? rt_string_to_gui_cstr(text) : NULL;
    if (text && !new_text)
        return;
    if ((!mi->text && !new_text) || (mi->text && new_text && strcmp(mi->text, new_text) == 0)) {
        free(new_text);
        return;
    }
    free(mi->text);
    mi->text = new_text;
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
    if (mi->owner_contextmenu)
        vg_contextmenu_item_set_enabled(mi, mi->enabled);
}

/// @brief Get the text of the menuitem.
/// @param item Candidate live managed MenuItem handle.
/// @return Runtime copy of the current label, or the canonical empty string when absent or invalid.
rt_string rt_menuitem_get_text(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return rt_str_empty();
    const char *text = mi->text;
    if (!text)
        return rt_str_empty();
    return rt_string_from_bytes(text, strlen(text));
}

/// @brief Set the shortcut of the menuitem.
/// @details Converts before mutation and preserves the old shortcut on allocation failure or a
///          byte-identical value. Regular MenuBars rebuild accelerators/native state; ContextMenus
///          refresh popup measurement but do not register app-level accelerators.
/// @param item Candidate live managed MenuItem handle.
/// @param shortcut New runtime accelerator text, or NULL to clear it.
void rt_menuitem_set_shortcut(void *item, rt_string shortcut) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    char *new_shortcut = shortcut ? rt_string_to_gui_cstr(shortcut) : NULL;
    if (shortcut && !new_shortcut)
        return;
    if ((!mi->shortcut && !new_shortcut) ||
        (mi->shortcut && new_shortcut && strcmp(mi->shortcut, new_shortcut) == 0)) {
        free(new_shortcut);
        return;
    }
    free(mi->shortcut);
    mi->shortcut = new_shortcut;
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
    if (mi->owner_contextmenu)
        vg_contextmenu_item_set_enabled(mi, mi->enabled);
}

/// @brief Get the shortcut of the menuitem.
/// @param item Candidate live managed MenuItem handle.
/// @return Runtime copy of the shortcut text, or the canonical empty string when absent or invalid.
rt_string rt_menuitem_get_shortcut(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return rt_str_empty();
    const char *shortcut = mi->shortcut;
    if (!shortcut)
        return rt_str_empty();
    return rt_string_from_bytes(shortcut, strlen(shortcut));
}

/// @brief Set the icon of the menuitem.
/// @details Converts the borrowed runtime Pixels object into an independently owned toolkit icon.
///          ContextMenu and regular-menu setters take ownership of the converted payload; a
///          conversion failure installs the no-icon sentinel and releases the prior icon.
/// @param item Candidate live managed MenuItem handle.
/// @param pixels Borrowed runtime Pixels object, or NULL to clear the icon.
void rt_menuitem_set_icon(void *item, void *pixels) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    vg_icon_t icon = rt_gui_icon_from_pixels(pixels);
    if (mi->owner_contextmenu) {
        vg_contextmenu_item_set_icon(mi, icon);
        return;
    }

    vg_icon_destroy(&mi->icon);
    mi->icon = icon;
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
}

/// @brief Enable or disable the checkable toggle behavior for a menu item.
/// @details Disabling checkability also clears the checked flag. The operation updates the owning
///          ContextMenu or MenuBar representation while retaining item identity.
/// @param item Candidate live managed MenuItem handle.
/// @param checkable Non-zero to permit checked state, zero to make the item ordinary.
void rt_menuitem_set_checkable(void *item, int64_t checkable) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    mi->checkable = checkable != 0;
    if (!mi->checkable)
        mi->checked = false;
    if (mi->owner_contextmenu) {
        if (mi->checkable)
            vg_contextmenu_item_set_checked(mi, mi->checked);
        else {
            vg_contextmenu_item_set_checked(mi, false);
            mi->checkable = false;
        }
    } else if (mi->parent_menu) {
        if (mi->checkable)
            vg_menu_item_set_checked(mi, mi->checked);
        rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
    }
}

/// @brief Check whether a menu item supports the checkable toggle state.
/// @param item Candidate live managed MenuItem handle.
/// @return One when the item is live and checkable, otherwise zero.
int64_t rt_menuitem_is_checkable(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return 0;
    return mi->checkable ? 1 : 0;
}

/// @brief Set the checked state of a menu item.
/// @details Normalizes @p checked to Boolean. Lower setters enforce their own checkability
///          semantics and update the appropriate popup/native representation; unchanged valid
///          state is not republished.
/// @param item Candidate live managed MenuItem handle.
/// @param checked Non-zero to request a check mark, zero to clear it.
void rt_menuitem_set_checked(void *item, int64_t checked) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    bool new_checked = checked != 0;
    if (mi->owner_contextmenu) {
        if (mi->checkable && mi->checked == new_checked)
            return;
        vg_contextmenu_item_set_checked(mi, new_checked);
        return;
    }
    if (mi->checkable && mi->checked == new_checked)
        return;
    vg_menu_item_set_checked(mi, new_checked);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
}

/// @brief Check whether a menu item is currently in the checked state.
/// @param item Candidate live managed MenuItem handle.
/// @return One when the live item is checked, otherwise zero.
int64_t rt_menuitem_is_checked(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return 0;
    return mi->checked ? 1 : 0;
}

/// @brief Enable or disable a menu item (disabled items are greyed out).
/// @details Normalizes @p enabled to Boolean and updates the owning ContextMenu or MenuBar only
///          when the state actually changes.
/// @param item Candidate live managed MenuItem handle.
/// @param enabled Non-zero to allow interaction, zero to disable the item.
void rt_menuitem_set_enabled(void *item, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return;
    bool new_enabled = enabled != 0;
    if (mi->enabled == new_enabled)
        return;
    if (mi->owner_contextmenu) {
        vg_contextmenu_item_set_enabled(mi, new_enabled);
        return;
    }
    vg_menu_item_set_enabled(mi, new_enabled);
    rt_gui_menu_sync_menubar(rt_gui_menu_owner_from_item(mi));
}

/// @brief Check whether a menu item is currently enabled.
/// @param item Candidate live managed MenuItem handle.
/// @return One when the live item is enabled, otherwise zero.
int64_t rt_menuitem_is_enabled(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return 0;
    return mi->enabled ? 1 : 0;
}

/// @brief Check whether a menu item is a separator (horizontal dividing line).
/// @param item Candidate live managed MenuItem handle.
/// @return One for a live separator record, otherwise zero.
int64_t rt_menuitem_is_separator(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return 0;
    return mi->separator ? 1 : 0;
}

/// @brief Check whether a menu item was clicked this frame (edge-triggered, clears after read).
/// @details Consumes the lower item's click latch so one activation is reported at most once.
///          Invalid, retired, disabled, or unclicked items return zero.
/// @param item Candidate live managed MenuItem handle.
/// @return One when a pending click was consumed, otherwise zero.
int64_t rt_menuitem_was_clicked(void *item) {
    RT_ASSERT_MAIN_THREAD();
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi)
        return 0;
    if (mi->was_clicked) {
        mi->was_clicked = false;
        return 1;
    }
    return 0;
}

/// @brief Resolve a MenuItem handle to its visible context-menu row rect (ADR 0219).
/// @details Shared core for the four geometry getters. Only context-menu rows
///          have on-screen geometry; menubar-owned items report all zeros.
/// @param item Candidate live managed MenuItem handle.
/// @param out_x Receives the row's left edge in logical window coordinates.
/// @param out_y Receives the row's top edge in logical window coordinates.
/// @param out_w Receives the row's width.
/// @param out_h Receives the row's height.
static void rt_menuitem_screen_rect(
    void *item, float *out_x, float *out_y, float *out_w, float *out_h) {
    *out_x = 0.0f;
    *out_y = 0.0f;
    *out_w = 0.0f;
    *out_h = 0.0f;
    vg_menu_item_t *mi = rt_menuitem_checked(item);
    if (!mi || !mi->owner_contextmenu)
        return;
    if (!vg_contextmenu_get_item_rect(mi->owner_contextmenu, mi, out_x, out_y, out_w, out_h))
        return;
    float scale = rt_gui_app_effective_scale(rt_gui_get_active_app());
    *out_x /= scale;
    *out_y /= scale;
    *out_w /= scale;
    *out_h /= scale;
}

/// @brief Report a visible context-menu row's on-screen left edge (ADR 0219).
/// @param item Candidate live managed MenuItem handle.
/// @return Logical window X, or zero when the row has no on-screen geometry.
double rt_menuitem_get_screen_x(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_menuitem_screen_rect(item, &x, &y, &w, &h);
    return (double)x;
}

/// @brief Report a visible context-menu row's on-screen top edge (ADR 0219).
/// @param item Candidate live managed MenuItem handle.
/// @return Logical window Y, or zero when the row has no on-screen geometry.
double rt_menuitem_get_screen_y(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_menuitem_screen_rect(item, &x, &y, &w, &h);
    return (double)y;
}

/// @brief Report a visible context-menu row's on-screen width (ADR 0219).
/// @param item Candidate live managed MenuItem handle.
/// @return Row width, or zero when the row has no on-screen geometry.
double rt_menuitem_get_screen_width(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_menuitem_screen_rect(item, &x, &y, &w, &h);
    return (double)w;
}

/// @brief Report a visible context-menu row's on-screen height (ADR 0219).
/// @param item Candidate live managed MenuItem handle.
/// @return Row height, or zero when the row has no on-screen geometry.
double rt_menuitem_get_screen_height(void *item) {
    RT_ASSERT_MAIN_THREAD();
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    rt_menuitem_screen_rect(item, &x, &y, &w, &h);
    return (double)h;
}

//=============================================================================
// ContextMenu Widget (Phase 2)
//=============================================================================

/// @brief Create a new context (right-click) menu.
/// @details Context menus are standalone popup menus not attached to a menu bar.
///          Use rt_contextmenu_show to display at specific screen coordinates.
///          The menu auto-dismisses when the user clicks outside its bounds.
/// @return Opaque context menu handle, or NULL on failure.
void *rt_contextmenu_new(void) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = vg_contextmenu_create();
    rt_contextmenu_apply_app_defaults(cm);
    return rt_gui_wrap_contextmenu(cm);
}

/// @brief Release resources and destroy the contextmenu.
/// @details Clears the active app's detached-overlay pointer before invalidating every managed
/// item,
///          submenu, and root handle, then recursively destroys the standalone toolkit tree.
/// @param menu Candidate managed ContextMenu handle; invalid or retired handles are ignored.
void rt_contextmenu_destroy(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (cm) {
        // Clear the active-overlay pointer first so the main loop never dereferences a
        // freed menu.
        rt_gui_app_t *app = rt_gui_get_active_app();
        if (app && app->active_context_menu == cm)
            app->active_context_menu = NULL;
        rt_gui_invalidate_contextmenu_tree(cm);
        vg_contextmenu_destroy(cm);
    }
}

/// @brief Add a clickable item to a context (right-click) popup menu.
/// @details Converts embedded NULs to replacement characters, lets the lower menu copy the
///          temporary UTF-8 label, and returns the stable managed wrapper for the owned item.
/// @param menu Candidate live managed ContextMenu handle.
/// @param text Runtime display label.
/// @return Stable managed MenuItem handle, or NULL for invalid input or allocation failure.
void *rt_contextmenu_add_item(void *menu, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (!cm)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_menu_item_t *item =
        vg_contextmenu_add_item(cm, rt_gui_cstr_or_empty(ctext), NULL, NULL, NULL);
    free(ctext);
    return rt_gui_wrap_menu_item(item);
}

/// @brief Add a context-menu item with an associated keyboard shortcut.
/// @details Both runtime strings are converted to GUI-safe text and copied by the lower menu. The
///          shortcut is presentation metadata for this standalone popup.
/// @param menu Candidate live managed ContextMenu handle.
/// @param text Runtime display label.
/// @param shortcut Runtime shortcut description displayed beside the label.
/// @return Stable managed MenuItem handle, or NULL for invalid input or allocation failure.
void *rt_contextmenu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (!cm)
        return NULL;
    char *ctext = rt_string_to_gui_cstr(text);
    char *cshortcut = rt_string_to_gui_cstr(shortcut);
    vg_menu_item_t *item = vg_contextmenu_add_item(
        cm, rt_gui_cstr_or_empty(ctext), rt_gui_cstr_or_empty(cshortcut), NULL, NULL);
    free(ctext);
    free(cshortcut);
    return rt_gui_wrap_menu_item(item);
}

/// @brief Append a separator line to a context menu.
/// @details Returns the separator item handle so callers can retain and inspect it consistently
///          with other managed MenuItem handles.
/// @param menu Candidate live managed ContextMenu handle.
/// @return Stable managed separator handle, or NULL for invalid input or allocation failure.
void *rt_contextmenu_add_separator(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    return cm ? rt_gui_wrap_menu_item(vg_contextmenu_add_separator(cm)) : NULL;
}

/// @brief Add a nested submenu to a context menu; returns the new submenu handle.
/// @details Creates and styles a standalone child ContextMenu, then transfers its ownership to the
///          newly allocated parent item. If attachment fails, the temporary submenu is destroyed
///          and the parent remains unchanged.
/// @param menu Candidate live managed parent ContextMenu.
/// @param title Runtime label for the submenu entry.
/// @return Stable managed child ContextMenu handle, or NULL for invalid input or allocation
/// failure.
void *rt_contextmenu_add_submenu(void *menu, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (!cm)
        return NULL;
    char *ctitle = rt_string_to_gui_cstr(title);

    // Create a child context menu and attach it as a submenu
    vg_contextmenu_t *submenu = vg_contextmenu_create();
    if (!submenu) {
        free(ctitle);
        return NULL;
    }
    rt_contextmenu_apply_app_defaults(submenu);
    vg_menu_item_t *item = vg_contextmenu_add_submenu(cm, ctitle, submenu);
    free(ctitle);
    if (!item) {
        vg_contextmenu_destroy(submenu);
        return NULL;
    }
    return rt_gui_wrap_contextmenu(submenu);
}

/// @brief Remove all items from a context menu.
/// @details Invalidates stable item/submenu wrappers before the lower layer retires their payloads,
///          then reclaims only tombstones no managed wrapper still references. The root menu
///          remains live and reusable.
/// @param menu Candidate live managed ContextMenu handle.
void rt_contextmenu_clear(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (cm) {
        rt_gui_invalidate_contextmenu_contents(cm);
        vg_contextmenu_clear(cm);
        rt_gui_collect_retired_subhandles(&cm->base);
    }
}

/// @brief Show a standalone ContextMenu at a screen-space coordinate.
/// @details Reapplies the active app's theme and font, narrows each coordinate by saturation to the
///          toolkit integer domain, opens the popup, and registers it as the active app's detached
///          overlay so polling and painting can reach it.
/// @param menu Candidate live managed ContextMenu handle.
/// @param x Screen-space X coordinate, saturated to signed 32-bit range.
/// @param y Screen-space Y coordinate, saturated to signed 32-bit range.
void rt_contextmenu_show(void *menu, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (cm) {
        rt_contextmenu_apply_app_defaults(cm);
        vg_contextmenu_show_at(cm,
                               rt_gui_clamp_i64_to_i32(x, INT32_MIN, INT32_MAX),
                               rt_gui_clamp_i64_to_i32(y, INT32_MIN, INT32_MAX));
        // Register as the active app's overlay so the main loop paints it and routes
        // input to it. A standalone context menu is not parented into app->root, so
        // without this it would be marked visible but never drawn or clicked.
        rt_gui_app_t *app = rt_gui_get_active_app();
        if (app)
            app->active_context_menu = cm;
    }
}

/// @brief Dismiss a standalone ContextMenu.
/// @details Clears the active app's detached-overlay pointer only when it refers to this menu,
///          leaving other active overlays intact. The menu and its item hierarchy remain reusable.
/// @param menu Candidate live managed ContextMenu handle.
void rt_contextmenu_hide(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (cm) {
        vg_contextmenu_dismiss(cm);
        rt_gui_app_t *app = rt_gui_get_active_app();
        if (app && app->active_context_menu == cm)
            app->active_context_menu = NULL;
    }
}

/// @brief Check whether a context menu is currently visible on screen.
/// @param menu Candidate live managed ContextMenu handle.
/// @return One when the popup is live and visible, otherwise zero.
int64_t rt_contextmenu_is_visible(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    return cm && cm->is_visible ? 1 : 0;
}

/// @brief Return (and consume) the most recently clicked context-menu item.
///
/// Edge-triggered: each click is reported exactly once. Subsequent
/// calls without a fresh click return NULL. The expected polling
/// pattern is `if (item := get_clicked()) handle(item);` once per frame.
/// @param menu Candidate live managed ContextMenu handle.
/// @return Stable managed handle for the consumed clicked item, or NULL when no valid click is
///         pending or wrapper allocation fails.
void *rt_contextmenu_get_clicked_item(void *menu) {
    RT_ASSERT_MAIN_THREAD();
    vg_contextmenu_t *cm = rt_contextmenu_checked(menu);
    if (!cm)
        return NULL;
    if (cm->clicked_index >= 0 && cm->clicked_index < (int)cm->item_count) {
        vg_menu_item_t *item = cm->items[cm->clicked_index];
        cm->clicked_index = -1; // Edge-triggered: clear after read
        return rt_gui_wrap_menu_item(item);
    }
    return NULL;
}

#else  /* !ZANNA_ENABLE_GRAPHICS */

// ===========================================================================
// Headless stubs — same prototypes as the real implementations above so
// non-graphical builds (server / CLI) can link without pulling
// in the GUI subsystem. Each stub safely no-ops or returns a sentinel
// (NULL pointer, 0, or empty string) without referencing any GUI state.
// Each stub retains an adjacent contract that names ignored inputs and its
// deterministic sentinel, allowing either preprocessor branch to stand alone.
// ===========================================================================

/// @brief Stub: graphics disabled — returns NULL; no menu bar widget is created.
/// @param parent Ignored candidate parent handle.
/// @return Always NULL.
void *rt_menubar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: ignore MenuBar destruction when graphics is disabled.
/// @param menubar Ignored candidate MenuBar handle.
void rt_menubar_destroy(void *menubar) {
    (void)menubar;
}

/// @brief Stub: graphics disabled — returns NULL; no menu is added to the menu bar.
/// @param menubar Ignored candidate MenuBar handle.
/// @param title Ignored runtime title.
/// @return Always NULL.
void *rt_menubar_add_menu(void *menubar, rt_string title) {
    (void)menubar;
    (void)title;
    return NULL;
}

/// @brief Stub: ignore MenuBar removal when graphics is disabled.
/// @param menubar Ignored candidate MenuBar handle.
/// @param menu Ignored candidate Menu handle.
void rt_menubar_remove_menu(void *menubar, void *menu) {
    (void)menubar;
    (void)menu;
}

/// @brief Stub: report no MenuBar menus when graphics is disabled.
/// @param menubar Ignored candidate MenuBar handle.
/// @return Always zero.
int64_t rt_menubar_get_menu_count(void *menubar) {
    (void)menubar;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no menu bar exists to retrieve from.
/// @param menubar Ignored candidate MenuBar handle.
/// @param index Ignored menu index.
/// @return Always NULL.
void *rt_menubar_get_menu(void *menubar, int64_t index) {
    (void)menubar;
    (void)index;
    return NULL;
}

/// @brief Stub: ignore MenuBar visibility changes when graphics is disabled.
/// @param menubar Ignored candidate MenuBar handle.
/// @param visible Ignored visibility flag.
void rt_menubar_set_visible(void *menubar, int64_t visible) {
    (void)menubar;
    (void)visible;
}

/// @brief Stub: report the MenuBar hidden when graphics is disabled.
/// @param menubar Ignored candidate MenuBar handle.
/// @return Always zero.
int64_t rt_menubar_is_visible(void *menubar) {
    (void)menubar;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no menu item is created.
/// @param menu Ignored candidate Menu handle.
/// @param text Ignored runtime label.
/// @return Always NULL.
void *rt_menu_add_item(void *menu, rt_string text) {
    (void)menu;
    (void)text;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no menu item with shortcut is created.
/// @param menu Ignored candidate Menu handle.
/// @param text Ignored runtime label.
/// @param shortcut Ignored runtime shortcut text.
/// @return Always NULL.
void *rt_menu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut) {
    (void)menu;
    (void)text;
    (void)shortcut;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no separator item is created.
/// @param menu Ignored candidate Menu handle.
/// @return Always NULL.
void *rt_menu_add_separator(void *menu) {
    (void)menu;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no submenu is created.
/// @param menu Ignored candidate parent Menu handle.
/// @param title Ignored runtime submenu title.
/// @return Always NULL.
void *rt_menu_add_submenu(void *menu, rt_string title) {
    (void)menu;
    (void)title;
    return NULL;
}

/// @brief Stub: ignore menu-item removal when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @param item Ignored candidate MenuItem handle.
void rt_menu_remove_item(void *menu, void *item) {
    (void)menu;
    (void)item;
}

/// @brief Stub: ignore Menu clearing when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
void rt_menu_clear(void *menu) {
    (void)menu;
}

/// @brief Stub: ignore Menu title changes when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @param title Ignored runtime title.
void rt_menu_set_title(void *menu, rt_string title) {
    (void)menu;
    (void)title;
}

/// @brief Stub: return an empty Menu title when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @return Canonical empty runtime string.
rt_string rt_menu_get_title(void *menu) {
    (void)menu;
    return rt_str_empty();
}

/// @brief Stub: report no Menu items when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @return Always zero.
int64_t rt_menu_get_item_count(void *menu) {
    (void)menu;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no menu exists to retrieve items from.
/// @param menu Ignored candidate Menu handle.
/// @param index Ignored item index.
/// @return Always NULL.
void *rt_menu_get_item(void *menu, int64_t index) {
    (void)menu;
    (void)index;
    return NULL;
}

/// @brief Stub: ignore Menu enabled-state changes when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @param enabled Ignored enabled flag.
void rt_menu_set_enabled(void *menu, int64_t enabled) {
    (void)menu;
    (void)enabled;
}

/// @brief Stub: report Menu disabled when graphics is disabled.
/// @param menu Ignored candidate Menu handle.
/// @return Always zero.
int64_t rt_menu_is_enabled(void *menu) {
    (void)menu;
    return 0;
}

/// @brief Stub: ignore MenuItem text changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param text Ignored runtime label.
void rt_menuitem_set_text(void *item, rt_string text) {
    (void)item;
    (void)text;
}

/// @brief Stub: return empty MenuItem text when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Canonical empty runtime string.
rt_string rt_menuitem_get_text(void *item) {
    (void)item;
    return rt_str_empty();
}

/// @brief Stub: ignore MenuItem shortcut changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param shortcut Ignored runtime shortcut text.
void rt_menuitem_set_shortcut(void *item, rt_string shortcut) {
    (void)item;
    (void)shortcut;
}

/// @brief Stub: return an empty MenuItem shortcut when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Canonical empty runtime string.
rt_string rt_menuitem_get_shortcut(void *item) {
    (void)item;
    return rt_str_empty();
}

/// @brief Stub: ignore MenuItem icon changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param pixels Ignored candidate Pixels object.
void rt_menuitem_set_icon(void *item, void *pixels) {
    (void)item;
    (void)pixels;
}

/// @brief Stub: ignore MenuItem checkability changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param checkable Ignored checkable flag.
void rt_menuitem_set_checkable(void *item, int64_t checkable) {
    (void)item;
    (void)checkable;
}

/// @brief Stub: report MenuItem non-checkable when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
int64_t rt_menuitem_is_checkable(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: ignore MenuItem checked-state changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param checked Ignored checked flag.
void rt_menuitem_set_checked(void *item, int64_t checked) {
    (void)item;
    (void)checked;
}

/// @brief Stub: report MenuItem unchecked when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
int64_t rt_menuitem_is_checked(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: ignore MenuItem enabled-state changes when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @param enabled Ignored enabled flag.
void rt_menuitem_set_enabled(void *item, int64_t enabled) {
    (void)item;
    (void)enabled;
}

/// @brief Stub: report MenuItem disabled when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
int64_t rt_menuitem_is_enabled(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: report MenuItem non-separator when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
int64_t rt_menuitem_is_separator(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: report no MenuItem click when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
int64_t rt_menuitem_was_clicked(void *item) {
    (void)item;
    return 0;
}

/// @brief Stub: report zero MenuItem screen X when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
double rt_menuitem_get_screen_x(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero MenuItem screen Y when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
double rt_menuitem_get_screen_y(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero MenuItem screen width when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
double rt_menuitem_get_screen_width(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: report zero MenuItem screen height when graphics is disabled.
/// @param item Ignored candidate MenuItem handle.
/// @return Always zero.
double rt_menuitem_get_screen_height(void *item) {
    (void)item;
    return 0.0;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu is created.
/// @return Always NULL.
void *rt_contextmenu_new(void) {
    return NULL;
}

/// @brief Stub: ignore ContextMenu destruction when graphics is disabled.
/// @param menu Ignored candidate ContextMenu handle.
void rt_contextmenu_destroy(void *menu) {
    (void)menu;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu item is created.
/// @param menu Ignored candidate ContextMenu handle.
/// @param text Ignored runtime item label.
/// @return Always NULL.
void *rt_contextmenu_add_item(void *menu, rt_string text) {
    (void)menu;
    (void)text;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu item with shortcut is created.
/// @param menu Ignored candidate ContextMenu handle.
/// @param text Ignored runtime item label.
/// @param shortcut Ignored runtime shortcut text.
/// @return Always NULL.
void *rt_contextmenu_add_item_with_shortcut(void *menu, rt_string text, rt_string shortcut) {
    (void)menu;
    (void)text;
    (void)shortcut;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu separator is created.
/// @param menu Ignored candidate ContextMenu handle.
/// @return Always NULL.
void *rt_contextmenu_add_separator(void *menu) {
    (void)menu;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu submenu is created.
/// @param menu Ignored candidate parent ContextMenu handle.
/// @param title Ignored runtime submenu title.
/// @return Always NULL.
void *rt_contextmenu_add_submenu(void *menu, rt_string title) {
    (void)menu;
    (void)title;
    return NULL;
}

/// @brief Stub: ignore ContextMenu clearing when graphics is disabled.
/// @param menu Ignored candidate ContextMenu handle.
void rt_contextmenu_clear(void *menu) {
    (void)menu;
}

/// @brief Stub: ignore ContextMenu display requests when graphics is disabled.
/// @param menu Ignored candidate ContextMenu handle.
/// @param x Ignored screen-space X coordinate.
/// @param y Ignored screen-space Y coordinate.
void rt_contextmenu_show(void *menu, int64_t x, int64_t y) {
    (void)menu;
    (void)x;
    (void)y;
}

/// @brief Stub: ignore ContextMenu dismissal when graphics is disabled.
/// @param menu Ignored candidate ContextMenu handle.
void rt_contextmenu_hide(void *menu) {
    (void)menu;
}

/// @brief Stub: report ContextMenu hidden when graphics is disabled.
/// @param menu Ignored candidate ContextMenu handle.
/// @return Always zero.
int64_t rt_contextmenu_is_visible(void *menu) {
    (void)menu;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no context menu item was clicked.
/// @param menu Ignored candidate ContextMenu handle.
/// @return Always NULL.
void *rt_contextmenu_get_clicked_item(void *menu) {
    (void)menu;
    return NULL;
}
#endif /* ZANNA_ENABLE_GRAPHICS */
