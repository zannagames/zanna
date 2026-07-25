//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_menubar.c
// Purpose: Horizontal menu bar widget with drop-down menus, keyboard navigation,
//          and a parsed accelerator table for global shortcut dispatch.
// Key invariants:
//   - Menus and items are stored as doubly-linked lists (first/last + prev/next
//     pointers); free_menu and free_menu_item are recursive to handle submenus.
//   - accel_table is a singly-linked list of vg_accel_entry_t built from
//     shortcut strings at item-creation time; vg_menubar_rebuild_accelerators
//     tears it down and rebuilds from the live menu tree.
//   - Ctrl and Super (⌘) modifiers are treated as interchangeable in
//     vg_menubar_handle_accelerator so a single registration works on both
//     macOS and other platforms.
//   - open_menu tracks the currently expanded drop-down; NULL when closed.
//     menubar_get_open_dropdown_bounds computes its geometry on demand.
// Ownership/Lifetime:
//   - All vg_menu_t and vg_menu_item_t instances are owned by the menubar and
//     freed in menubar_destroy via free_menu/free_menu_item.
//   - Item text, shortcut strings, and submenu titles are strdup'd internally.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements retained menu bars, hierarchical menu ownership,
///        drop-down interaction, and keyboard accelerators.
/// @details Menus and items use explicit live and retired states so removals
///          during callbacks do not invalidate active stack references. Custom
///          drop-down geometry is shared by painting, hit testing, and visual
///          bounds reporting.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widget.h"
#include <stdlib.h>
#include <string.h>

// For strcasecmp: Windows uses _stricmp, POSIX uses strcasecmp
// For strtok_r: Windows uses strtok_s
#ifdef _WIN32
#define strcasecmp _stricmp
#define strtok_r strtok_s
#else
#include <strings.h>
#endif

#define VG_MENU_ITEM_MAGIC UINT64_C(0x56474D454E554954)
#define VG_MENU_ITEM_RETIRED_MAGIC UINT64_C(0x56474D454E555258)
#define VG_MENU_MAGIC UINT64_C(0x56474D454E554D4E)
#define VG_MENU_RETIRED_MAGIC UINT64_C(0x56474D454E55524D)

//=============================================================================
// Forward Declarations
//=============================================================================

static void menubar_destroy(vg_widget_t *widget);
static void menubar_measure(vg_widget_t *widget, float available_width, float available_height);
static void menubar_paint(vg_widget_t *widget, void *canvas);
static void menubar_paint_overlay(vg_widget_t *widget, void *canvas);
static void menubar_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height);
static bool menubar_handle_event(vg_widget_t *widget, vg_event_t *event);
void vg_menu_remove_item(vg_menu_t *menu, vg_menu_item_t *item);

//=============================================================================
// MenuBar VTable
//=============================================================================

static vg_widget_vtable_t g_menubar_vtable = {.destroy = menubar_destroy,
                                              .measure = menubar_measure,
                                              .arrange = NULL,
                                              .paint = menubar_paint,
                                              .paint_overlay = menubar_paint_overlay,
                                              .handle_event = menubar_handle_event,
                                              .can_focus = NULL,
                                              .on_focus = NULL,
                                              .get_visual_bounds = menubar_get_visual_bounds};

//=============================================================================
// Helper Functions
//=============================================================================

static void free_retired_menu_items(vg_menu_t *menu);
static bool menubar_contains_menu(const vg_menubar_t *menubar, const vg_menu_t *target);
static void retire_menu(vg_menubar_t *menubar, vg_menu_t *menu);

/// @brief Recursively free an item's text, shortcut, icon, submenu tree, and the item itself.
/// @param item Owned live or retired item record to destroy; `NULL` is ignored.
static void free_menu_item(vg_menu_item_t *item) {
    if (!item)
        return;

    if (item->text)
        free(item->text);
    if (item->shortcut)
        free(item->shortcut);
    vg_icon_destroy(&item->icon);

    // Recursively free submenu
    if (item->submenu) {
        vg_menu_item_t *sub = item->submenu->first_item;
        while (sub) {
            vg_menu_item_t *next = sub->next;
            free_menu_item(sub);
            sub = next;
        }
        if (item->submenu->title)
            free(item->submenu->title);
        free_retired_menu_items(item->submenu);
        free(item->submenu);
    }

    free(item);
}

/// @brief Validate that a menu-item handle still names an attached live item.
/// @param item Item record to inspect.
/// @return `true` when the live magic and either menu or context-menu ownership
///         link are present.
bool vg_menu_item_is_live(const vg_menu_item_t *item) {
    return item && item->magic == VG_MENU_ITEM_MAGIC &&
           (item->parent_menu != NULL || item->owner_contextmenu != NULL);
}

/// @brief Validate that a menu remains reachable from a live owning menu bar.
/// @param menu Menu handle to inspect.
/// @return `true` when the magic, owner widget, and recursive tree membership
///         checks all succeed.
bool vg_menu_is_live(const vg_menu_t *menu) {
    if (!menu || menu->magic != VG_MENU_MAGIC || !menu->owner_menubar)
        return false;
    if (!vg_widget_is_live(&menu->owner_menubar->base))
        return false;
    return menubar_contains_menu(menu->owner_menubar, menu);
}

/// @brief Free every menu item parked on @p menu's deferred-deletion list.
/// @details Items are "retired" (not freed immediately) while a menu may
///          still be referenced mid-event; this drains that list safely.
/// @param menu Menu whose retirement chain is drained.
static void free_retired_menu_items(vg_menu_t *menu) {
    if (!menu)
        return;
    vg_menu_item_t *item = menu->retired_items;
    while (item) {
        vg_menu_item_t *next = item->retired_next;
        item->retired_next = NULL;
        free_menu_item(item);
        item = next;
    }
    menu->retired_items = NULL;
}

/// @brief Return true if @p target is reachable from @p menu, including nested submenus.
/// @param menu Root of the menu tree to search.
/// @param target Menu record sought by identity.
/// @return `true` when @p target is @p menu or a recursively owned submenu.
static bool menu_tree_contains(const vg_menu_t *menu, const vg_menu_t *target) {
    if (!menu || !target)
        return false;
    if (menu == target)
        return true;
    for (vg_menu_item_t *item = menu->first_item; item; item = item->next) {
        if (item->submenu && menu_tree_contains(item->submenu, target))
            return true;
    }
    return false;
}

/// @brief Return true if @p target is part of @p menubar's live menu tree.
/// @param menubar Menu bar whose top-level roots are searched.
/// @param target Menu record sought by identity.
/// @return `true` when any top-level menu contains @p target.
static bool menubar_contains_menu(const vg_menubar_t *menubar, const vg_menu_t *target) {
    if (!menubar || !target)
        return false;
    for (vg_menu_t *menu = menubar->first_menu; menu; menu = menu->next) {
        if (menu_tree_contains(menu, target))
            return true;
    }
    return false;
}

/// @brief Close @p menu (or the currently open menu if @p menu is NULL):
///        clears the open/highlight state, releases input capture, and
///        requests a repaint.
/// @param menubar Menu bar whose transient interaction state is updated.
/// @param menu Specific menu to close, or `NULL` for the current open menu.
static void close_menubar_menu(vg_menubar_t *menubar, vg_menu_t *menu) {
    if (!menubar)
        return;
    if (!menu || menubar->open_menu == menu) {
        if (menubar->open_menu)
            menubar->open_menu->open = false;
        menubar->open_menu = NULL;
        menubar->highlighted = NULL;
        menubar->menu_active = false;
        if (vg_widget_get_input_capture() == &menubar->base)
            vg_widget_release_input_capture();
        menubar->base.needs_paint = true;
        return;
    }
    if (menubar->highlighted && menubar->highlighted->parent_menu == menu) {
        menubar->highlighted = NULL;
        menubar->base.needs_paint = true;
    }
}

/// @brief Move @p item (and its submenu items) onto @p menu's retired list
///        for deferred freeing, so a removal during event handling does not
///        free memory still referenced up the call stack.
/// @param menu Menu receiving the retired record.
/// @param item Detached item whose owned payload and submenu are retired.
static void retire_menu_item(vg_menu_t *menu, vg_menu_item_t *item) {
    if (!menu || !item)
        return;
    if (item->submenu) {
        vg_menu_t *submenu = item->submenu;
        if (submenu->owner_menubar && vg_widget_is_live(&submenu->owner_menubar->base)) {
            retire_menu(submenu->owner_menubar, submenu);
        } else {
            vg_menu_item_t *sub = submenu->first_item;
            while (sub) {
                vg_menu_item_t *next = sub->next;
                retire_menu_item(menu, sub);
                sub = next;
            }
            free_retired_menu_items(submenu);
            free(submenu->title);
            free(submenu);
        }
        item->submenu = NULL;
    }
    free(item->text);
    item->text = NULL;
    free(item->shortcut);
    item->shortcut = NULL;
    vg_icon_destroy(&item->icon);
    item->action = NULL;
    item->action_data = NULL;
    item->parent_menu = NULL;
    item->owner_contextmenu = NULL;
    item->next = NULL;
    item->prev = NULL;
    item->magic = VG_MENU_ITEM_RETIRED_MAGIC;
    item->retired_next = menu->retired_items;
    menu->retired_items = item;
}

/// @brief Free all items in a menu list and the menu struct itself.
/// @param menu Owned menu tree to destroy; `NULL` is ignored.
static void free_menu(vg_menu_t *menu) {
    if (!menu)
        return;

    vg_menu_item_t *item = menu->first_item;
    while (item) {
        vg_menu_item_t *next = item->next;
        free_menu_item(item);
        item = next;
    }

    if (menu->title)
        free(menu->title);
    free_retired_menu_items(menu);
    free(menu);
}

/// @brief Move a removed menu onto the menubar retired list so stale runtime
///        menu handles become inert until the menubar itself is destroyed.
/// @param menubar Menu bar receiving the retired menu record.
/// @param menu Detached menu whose item tree is retired.
static void retire_menu(vg_menubar_t *menubar, vg_menu_t *menu) {
    if (!menubar || !menu)
        return;

    vg_menu_item_t *item = menu->first_item;
    while (item) {
        vg_menu_item_t *next = item->next;
        retire_menu_item(menu, item);
        item = next;
    }

    menu->first_item = NULL;
    menu->last_item = NULL;
    menu->item_count = 0;
    free(menu->title);
    menu->title = NULL;
    menu->owner_menubar = menubar;
    menu->next = NULL;
    menu->prev = NULL;
    menu->open = false;
    menu->enabled = false;
    menu->magic = VG_MENU_RETIRED_MAGIC;
    menu->retired_next = menubar->retired_menus;
    menubar->retired_menus = menu;
}

/// @brief Compute the screen bounds and per-item height for the currently open drop-down.
/// @details Width accounts for labels, checks, shortcuts, submenu arrows, and
///          theme scaling. Every output is initialized to zero when no menu is
///          open.
/// @param menubar Menu bar whose open custom drop-down is measured.
/// @param out_x Optional destination for screen-space left coordinate.
/// @param out_y Optional destination for screen-space top coordinate.
/// @param out_width Optional destination for panel width.
/// @param out_height Optional destination for panel height.
/// @param out_item_height Optional destination for uniform row height.
static void menubar_get_open_dropdown_bounds(vg_menubar_t *menubar,
                                             float *out_x,
                                             float *out_y,
                                             float *out_width,
                                             float *out_height,
                                             float *out_item_height) {
    if (out_x)
        *out_x = 0.0f;
    if (out_y)
        *out_y = 0.0f;
    if (out_width)
        *out_width = 0.0f;
    if (out_height)
        *out_height = 0.0f;
    if (out_item_height)
        *out_item_height = 0.0f;
    if (!menubar || !menubar->open_menu)
        return;

    float dropdown_x = 0.0f;
    float dropdown_y = 0.0f;
    vg_widget_get_screen_bounds(&menubar->base, &dropdown_x, &dropdown_y, NULL, NULL);
    for (vg_menu_t *menu = menubar->first_menu; menu != menubar->open_menu; menu = menu->next) {
        if (!menu->title || !menubar->font)
            continue;
        vg_text_metrics_t metrics = {0};
        vg_font_measure_text(menubar->font, menubar->font_size, menu->title, &metrics);
        dropdown_x += metrics.width + menubar->menu_padding * 2.0f;
    }

    float scale = vg_theme_get_current()->ui_scale;
    if (scale <= 0.0f)
        scale = 1.0f;
    float item_height = 28.0f * scale;
    float dropdown_width = 120.0f * scale;
    if (menubar->font) {
        for (vg_menu_item_t *item = menubar->open_menu->first_item; item; item = item->next) {
            if (item->separator)
                continue;

            float item_width = menubar->item_padding * 2.0f;
            if (item->text && item->text[0]) {
                vg_text_metrics_t metrics = {0};
                vg_font_measure_text(menubar->font, menubar->font_size, item->text, &metrics);
                item_width += metrics.width;
            }
            if (item->checked)
                item_width += 10.0f * scale;
            if (item->shortcut && item->shortcut[0]) {
                vg_text_metrics_t shortcut_metrics = {0};
                vg_font_measure_text(
                    menubar->font, menubar->font_size, item->shortcut, &shortcut_metrics);
                item_width += shortcut_metrics.width + menubar->item_padding * 2.0f;
            }
            if (item->submenu)
                item_width += 14.0f * scale;
            if (item_width > dropdown_width)
                dropdown_width = item_width;
        }
    }
    float dropdown_height = (float)menubar->open_menu->item_count * item_height;

    if (out_x)
        *out_x = dropdown_x;
    if (out_y)
        *out_y = dropdown_y + menubar->base.height;
    if (out_width)
        *out_width = dropdown_width;
    if (out_height)
        *out_height = dropdown_height;
    if (out_item_height)
        *out_item_height = item_height;
}

/// @brief Report the currently open menu panel's absolute rectangle.
/// @details Native platform menus draw outside the retained framebuffer and are
///          therefore excluded. Custom menus reuse the exact geometry helper used
///          by overlay painting and pointer hit testing, including nested-parent
///          screen-coordinate conversion.
/// @param widget Menu bar being queried.
/// @param x Receives panel X, or zero when no custom menu is open.
/// @param y Receives panel Y, or zero when no custom menu is open.
/// @param width Receives panel width, or zero when no custom menu is open.
/// @param height Receives panel height, or zero when no custom menu is open.
static void menubar_get_visual_bounds(
    vg_widget_t *widget, float *x, float *y, float *width, float *height) {
    if (x)
        *x = 0.0f;
    if (y)
        *y = 0.0f;
    if (width)
        *width = 0.0f;
    if (height)
        *height = 0.0f;
    vg_menubar_t *menubar = (vg_menubar_t *)widget;
    if (!menubar || menubar->native_main_menu || !menubar->open_menu || !menubar->font)
        return;
    menubar_get_open_dropdown_bounds(menubar, x, y, width, height, NULL);
}

//=============================================================================
// MenuBar Implementation
//=============================================================================

/// @brief Create a menu bar widget, optionally as a child of parent.
///
/// @details Allocates and initialises a vg_menubar_t with an empty menu list,
///          default theme colours, and no open drop-down. The bar is designed to
///          be placed at the top of a container.
///
/// @param parent Optional parent widget to attach to; may be NULL.
/// @return       Heap-allocated menu bar, or NULL on allocation failure.
vg_menubar_t *vg_menubar_create(vg_widget_t *parent) {
    vg_menubar_t *menubar = calloc(1, sizeof(vg_menubar_t));
    if (!menubar)
        return NULL;

    // Initialize base widget
    vg_widget_init(&menubar->base, VG_WIDGET_MENUBAR, &g_menubar_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize menubar-specific fields
    menubar->first_menu = NULL;
    menubar->last_menu = NULL;
    menubar->menu_count = 0;
    menubar->open_menu = NULL;
    menubar->highlighted = NULL;

    menubar->font = NULL;
    menubar->font_size = theme->typography.size_normal;

    // Appearance — scale pixel constants by ui_scale so the menubar is the
    // correct visual size on HiDPI displays (e.g. 56px physical = 28pt visual
    // on a 2× Retina when ui_scale = 2.0).
    float s = theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    menubar->height = 28.0f * s;
    menubar->menu_padding = 10.0f * s;
    menubar->item_padding = 8.0f * s;
    menubar->bg_color = theme->colors.bg_secondary;
    menubar->text_color = theme->colors.fg_primary;
    menubar->highlight_bg = theme->colors.bg_selected;
    menubar->disabled_color = theme->colors.fg_disabled;

    // State
    menubar->menu_active = false;
    menubar->native_main_menu = false;
    menubar->retired_menus = NULL;

    // Set size
    menubar->base.constraints.min_height = menubar->height;
    menubar->base.constraints.preferred_height = menubar->height;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &menubar->base);
    }

    return menubar;
}

/// @brief vtable destroy — releases input capture, frees all menus and the accelerator table.
/// @param widget Menu-bar widget base being destroyed.
static void menubar_destroy(vg_widget_t *widget) {
    vg_menubar_t *menubar = (vg_menubar_t *)widget;

    // Release input capture if this menubar holds it
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();

    vg_menu_t *menu = menubar->first_menu;
    while (menu) {
        vg_menu_t *next = menu->next;
        free_menu(menu);
        menu = next;
    }
    menu = menubar->retired_menus;
    while (menu) {
        vg_menu_t *next = menu->retired_next;
        menu->retired_next = NULL;
        free_menu(menu);
        menu = next;
    }
    menubar->retired_menus = NULL;

    vg_accel_entry_t *entry = menubar->accel_table;
    while (entry) {
        vg_accel_entry_t *next = entry->next;
        free(entry);
        entry = next;
    }
    menubar->accel_table = NULL;
}

/// @brief vtable measure — width fills available_width; height is the theme bar height.
/// @details A native main menu occupies no retained-layout height.
/// @param widget Menu-bar widget base to measure.
/// @param available_width Width offered by the parent layout.
/// @param available_height Offered height; unused because the bar has a fixed
///                         theme-derived height.
static void menubar_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_menubar_t *menubar = (vg_menubar_t *)widget;
    (void)available_height;

    float target_height = menubar->native_main_menu ? 0.0f : menubar->height;
    widget->constraints.min_height = target_height;
    widget->constraints.preferred_height = target_height;
    widget->measured_width = available_width > 0 ? available_width : 400;
    widget->measured_height = target_height;
}

/// @brief vtable paint — draws the bar background and menu title buttons.
/// @param widget Arranged menu-bar widget base to paint.
/// @param canvas Backend canvas used for primitives and text.
static void menubar_paint(vg_widget_t *widget, void *canvas) {
    vg_menubar_t *menubar = (vg_menubar_t *)widget;
    if (menubar->native_main_menu)
        return;
    vgfx_window_t win = (vgfx_window_t)canvas;

    // Draw menubar background
    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   menubar->bg_color);

    if (!menubar->font)
        return;

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(menubar->font, menubar->font_size, &font_metrics);

    // Correct vertical centering: descent is negative per vg_font.h contract,
    // so +descent correctly subtracts the absolute descender depth from the centre.
    float text_y = widget->y + (widget->height + font_metrics.ascent + font_metrics.descent) / 2.0f;
    float menu_x = widget->x;

    // Draw each menu title
    for (vg_menu_t *menu = menubar->first_menu; menu; menu = menu->next) {
        if (!menu->title)
            continue;

        vg_text_metrics_t metrics;
        vg_font_measure_text(menubar->font, menubar->font_size, menu->title, &metrics);

        float menu_width = metrics.width + menubar->menu_padding * 2;

        // Draw highlight if this menu is open
        if (menu == menubar->open_menu) {
            vg_draw_round_rect_fill(win,
                                    menu_x + 2.0f,
                                    widget->y + 3.0f,
                                    menu_width - 4.0f,
                                    widget->height - 6.0f,
                                    vg_theme_get_current()->radius.sm,
                                    menubar->highlight_bg);
        }

        // Draw menu title
        float text_x = menu_x + menubar->menu_padding;
        vg_font_draw_text(canvas,
                          menubar->font,
                          menubar->font_size,
                          text_x,
                          text_y,
                          menu->title,
                          menu->enabled ? menubar->text_color : menubar->disabled_color);

        menu_x += menu_width;
    }

    // NOTE: Dropdown is painted in paint_overlay() so it appears on top of other widgets
}

// Paint overlay - called after all widgets are painted to draw popups on top
/// @brief vtable paint_overlay — draws the open drop-down panel and its items on top of other
/// widgets.
/// @details Renders separators, highlight state, shortcuts, check marks, and
///          submenu indicators using the same geometry as event hit testing.
/// @param widget Menu-bar widget owning the overlay.
/// @param canvas Backend canvas used for overlay primitives and text.
static void menubar_paint_overlay(vg_widget_t *widget, void *canvas) {
    vg_menubar_t *menubar = (vg_menubar_t *)widget;
    if (menubar->native_main_menu)
        return;

    // Only draw if a menu is open
    if (!menubar->open_menu || !menubar->font)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(menubar->font, menubar->font_size, &font_metrics);

    float dropdown_x = 0.0f;
    float dropdown_y = 0.0f;
    float dropdown_width = 0.0f;
    float dropdown_height = 0.0f;
    float item_height = 0.0f;
    menubar_get_open_dropdown_bounds(
        menubar, &dropdown_x, &dropdown_y, &dropdown_width, &dropdown_height, &item_height);

    // Get theme for colors
    vg_theme_t *theme = vg_theme_get_current();

    // Draw dropdown background (dark panel)
    vgfx_fill_rect(win,
                   (int32_t)dropdown_x,
                   (int32_t)dropdown_y,
                   (int32_t)dropdown_width,
                   (int32_t)dropdown_height,
                   menubar->bg_color);

    // Draw dropdown border
    uint32_t border_color = theme->colors.border_primary;
    // Top border
    vgfx_fill_rect(
        win, (int32_t)dropdown_x, (int32_t)dropdown_y, (int32_t)dropdown_width, 1, border_color);
    // Left border
    vgfx_fill_rect(
        win, (int32_t)dropdown_x, (int32_t)dropdown_y, 1, (int32_t)dropdown_height, border_color);
    // Right border
    vgfx_fill_rect(win,
                   (int32_t)(dropdown_x + dropdown_width - 1),
                   (int32_t)dropdown_y,
                   1,
                   (int32_t)dropdown_height,
                   border_color);
    // Bottom border
    vgfx_fill_rect(win,
                   (int32_t)dropdown_x,
                   (int32_t)(dropdown_y + dropdown_height - 1),
                   (int32_t)dropdown_width,
                   1,
                   border_color);

    // Draw menu items
    float item_y = dropdown_y;
    for (vg_menu_item_t *item = menubar->open_menu->first_item; item; item = item->next) {
        if (item->separator) {
            // Draw separator line
            int32_t sep_margin = 8;
            int32_t sep_y = (int32_t)(item_y + item_height / 2);
            vgfx_fill_rect(win,
                           (int32_t)dropdown_x + sep_margin,
                           sep_y,
                           (int32_t)dropdown_width - sep_margin * 2,
                           1,
                           theme->colors.border_secondary);
        } else {
            // Draw highlight if highlighted
            if (item == menubar->highlighted) {
                vgfx_fill_rect(win,
                               (int32_t)dropdown_x + 1,
                               (int32_t)item_y,
                               (int32_t)dropdown_width - 2,
                               (int32_t)item_height,
                               menubar->highlight_bg);
            }

            // Draw item text
            if (item->text) {
                float item_text_y =
                    item_y + (item_height + font_metrics.ascent + font_metrics.descent) / 2.0f;
                uint32_t color = item->enabled ? menubar->text_color : menubar->disabled_color;
                vg_font_draw_text(canvas,
                                  menubar->font,
                                  menubar->font_size,
                                  dropdown_x + menubar->item_padding,
                                  item_text_y,
                                  item->text,
                                  color);

                // Draw shortcut if present
                if (item->shortcut) {
                    vg_text_metrics_t shortcut_metrics;
                    vg_font_measure_text(
                        menubar->font, menubar->font_size, item->shortcut, &shortcut_metrics);
                    float reserved_right = menubar->item_padding;
                    if (item->submenu)
                        reserved_right += 12.0f;
                    float shortcut_x =
                        dropdown_x + dropdown_width - reserved_right - shortcut_metrics.width;
                    vg_font_draw_text(canvas,
                                      menubar->font,
                                      menubar->font_size,
                                      shortcut_x,
                                      item_text_y,
                                      item->shortcut,
                                      menubar->disabled_color);
                }

                // Draw check mark if checked
                if (item->checked) {
                    float check_x = dropdown_x + 4;
                    float check_y = item_y + item_height / 2;
                    vgfx_fill_rect(
                        win, (int32_t)check_x, (int32_t)check_y, 3, 1, menubar->text_color);
                    vgfx_fill_rect(win,
                                   (int32_t)(check_x + 2),
                                   (int32_t)(check_y - 3),
                                   1,
                                   4,
                                   menubar->text_color);
                }

                // Draw submenu arrow if has submenu
                if (item->submenu) {
                    float arrow_x = dropdown_x + dropdown_width - menubar->item_padding - 4.0f;
                    float arrow_y = item_y + item_height / 2;
                    vgfx_fill_rect(
                        win, (int32_t)arrow_x, (int32_t)(arrow_y - 2), 1, 5, menubar->text_color);
                    vgfx_fill_rect(win,
                                   (int32_t)(arrow_x + 1),
                                   (int32_t)(arrow_y - 1),
                                   1,
                                   3,
                                   menubar->text_color);
                    vgfx_fill_rect(
                        win, (int32_t)(arrow_x + 2), (int32_t)arrow_y, 1, 1, menubar->text_color);
                }
            }
        }

        item_y += item_height;
    }
}

/// @brief Return the top-level menu whose title button contains screen X, or NULL.
/// @param menubar Menu bar supplying titles, padding, and font metrics.
/// @param x Horizontal coordinate relative to the bar.
/// @return Borrowed containing menu, or `NULL` when no enabled geometry matches.
static vg_menu_t *find_menu_at_x(vg_menubar_t *menubar, float x) {
    if (!menubar->font)
        return NULL;

    float menu_x = 0;
    for (vg_menu_t *menu = menubar->first_menu; menu; menu = menu->next) {
        if (!menu->title)
            continue;

        vg_text_metrics_t metrics;
        vg_font_measure_text(menubar->font, menubar->font_size, menu->title, &metrics);
        float menu_width = metrics.width + menubar->menu_padding * 2;

        if (x >= menu_x && x < menu_x + menu_width) {
            return menu;
        }

        menu_x += menu_width;
    }
    return NULL;
}

/// @brief Return the next enabled sibling menu after menu, or NULL if none.
/// @param menu Current top-level menu.
/// @return Borrowed next enabled sibling, or `NULL`.
static vg_menu_t *find_next_enabled_menu(vg_menu_t *menu) {
    for (vg_menu_t *it = menu ? menu->next : NULL; it; it = it->next) {
        if (it->enabled)
            return it;
    }
    return NULL;
}

/// @brief Return the previous enabled sibling menu before menu, or NULL if none.
/// @param menu Current top-level menu.
/// @return Borrowed previous enabled sibling, or `NULL`.
static vg_menu_t *find_prev_enabled_menu(vg_menu_t *menu) {
    for (vg_menu_t *it = menu ? menu->prev : NULL; it; it = it->prev) {
        if (it->enabled)
            return it;
    }
    return NULL;
}

/// @brief Return the first non-separator enabled item in menu, or NULL.
/// @param menu Menu whose item chain is scanned.
/// @return Borrowed first keyboard-selectable item, or `NULL`.
static vg_menu_item_t *find_first_enabled_item(vg_menu_t *menu) {
    for (vg_menu_item_t *item = menu ? menu->first_item : NULL; item; item = item->next) {
        if (!item->separator && item->enabled)
            return item;
    }
    return NULL;
}

/// @brief Return the next non-separator enabled item after item, or NULL.
/// @param item Current menu item.
/// @return Borrowed next keyboard-selectable sibling, or `NULL`.
static vg_menu_item_t *find_next_enabled_item(vg_menu_item_t *item) {
    for (vg_menu_item_t *it = item ? item->next : NULL; it; it = it->next) {
        if (!it->separator && it->enabled)
            return it;
    }
    return NULL;
}

/// @brief Return the previous non-separator enabled item before item, or NULL.
/// @param item Current menu item.
/// @return Borrowed previous keyboard-selectable sibling, or `NULL`.
static vg_menu_item_t *find_prev_enabled_item(vg_menu_item_t *item) {
    for (vg_menu_item_t *it = item ? item->prev : NULL; it; it = it->prev) {
        if (!it->separator && it->enabled)
            return it;
    }
    return NULL;
}

/// @brief vtable handle_event — routes mouse, keyboard, and Escape for the bar and its drop-down.
/// @details Handles title switching under capture, screen-space drop-down hit
///          testing, item activation, accelerator dispatch, directional menu
///          navigation, and dismissal.
/// @param widget Menu-bar widget receiving input.
/// @param event Mutable event to interpret.
/// @return `true` when an action or menu-state transition consumes the event.
static bool menubar_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_menubar_t *menubar = (vg_menubar_t *)widget;
    if (menubar->native_main_menu)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            float local_x = event->mouse.x;
            float local_y = event->mouse.y;

            // Check if in menu bar area
            if (local_y < menubar->height) {
                vg_menu_t *menu = find_menu_at_x(menubar, local_x);
                if (menubar->menu_active && menu && menu->enabled && menu != menubar->open_menu) {
                    menubar->open_menu = menu;
                    menu->open = true;
                    menubar->highlighted = NULL;
                    widget->needs_paint = true;
                }
            } else if (menubar->open_menu) {
                vg_menu_item_t *old_highlight = menubar->highlighted;
                menubar->highlighted = NULL;

                float dropdown_x = 0.0f;
                float dropdown_y = 0.0f;
                float dropdown_width = 0.0f;
                float dropdown_height = 0.0f;
                float item_height = 0.0f;
                menubar_get_open_dropdown_bounds(menubar,
                                                 &dropdown_x,
                                                 &dropdown_y,
                                                 &dropdown_width,
                                                 &dropdown_height,
                                                 &item_height);
                float screen_x = event->mouse.screen_x;
                float screen_y = event->mouse.screen_y;
                bool inside_dropdown =
                    screen_x >= dropdown_x && screen_x < dropdown_x + dropdown_width &&
                    screen_y >= dropdown_y && screen_y < dropdown_y + dropdown_height;
                if (inside_dropdown && item_height > 0.0f) {
                    int item_index = (int)((screen_y - dropdown_y) / item_height);
                    int idx = 0;
                    for (vg_menu_item_t *item = menubar->open_menu->first_item; item;
                         item = item->next) {
                        if (idx == item_index && !item->separator && item->enabled) {
                            menubar->highlighted = item;
                            break;
                        }
                        idx++;
                    }
                }

                if (old_highlight != menubar->highlighted) {
                    widget->needs_paint = true;
                }
            }
            return false;
        }

        case VG_EVENT_CLICK: {
            float local_x = event->mouse.x;
            float local_y = event->mouse.y;

            // Check if clicking menu bar
            if (local_y >= 0 && local_y < menubar->height && local_x >= 0) {
                vg_menu_t *menu = find_menu_at_x(menubar, local_x);
                if (menu && menu->enabled) {
                    if (menubar->open_menu == menu) {
                        // Close menu
                        menubar->open_menu = NULL;
                        menubar->menu_active = false;
                        menu->open = false;
                        vg_widget_release_input_capture();
                    } else {
                        // Open menu
                        if (menubar->open_menu) {
                            menubar->open_menu->open = false;
                        }
                        menubar->open_menu = menu;
                        menubar->menu_active = true;
                        menu->open = true;
                        vg_widget_set_input_capture(widget);
                    }
                    widget->needs_paint = true;
                    return true;
                }
            }

            if (menubar->open_menu && menubar->highlighted) {
                float dropdown_x = 0.0f;
                float dropdown_y = 0.0f;
                float dropdown_width = 0.0f;
                float dropdown_height = 0.0f;
                menubar_get_open_dropdown_bounds(
                    menubar, &dropdown_x, &dropdown_y, &dropdown_width, &dropdown_height, NULL);
                bool inside_dropdown = event->mouse.screen_x >= dropdown_x &&
                                       event->mouse.screen_x < dropdown_x + dropdown_width &&
                                       event->mouse.screen_y >= dropdown_y &&
                                       event->mouse.screen_y < dropdown_y + dropdown_height;
                if (inside_dropdown) {
                    // Execute highlighted item
                    vg_menu_item_t *item = menubar->highlighted;
                    if (item->enabled)
                        item->was_clicked = true;
                    if (item->enabled && item->action) {
                        item->action(item->action_data);
                    }

                    // Close menu
                    menubar->open_menu->open = false;
                    menubar->open_menu = NULL;
                    menubar->menu_active = false;
                    menubar->highlighted = NULL;
                    vg_widget_release_input_capture();
                    widget->needs_paint = true;
                    return true;
                }
            }

            // Click outside both menubar and dropdown area — close menu
            if (menubar->open_menu) {
                menubar->open_menu->open = false;
                menubar->open_menu = NULL;
                menubar->menu_active = false;
                menubar->highlighted = NULL;
                vg_widget_release_input_capture();
                widget->needs_paint = true;
                return true;
            }

            return false;
        }

        case VG_EVENT_MOUSE_LEAVE:
            if (!menubar->menu_active) {
                widget->needs_paint = true;
            }
            return false;

        case VG_EVENT_KEY_DOWN:
            if (vg_menubar_handle_accelerator(menubar, event->key.key, event->modifiers)) {
                widget->needs_paint = true;
                return true;
            }

            if (menubar->open_menu) {
                switch (event->key.key) {
                    case VG_KEY_ESCAPE:
                        menubar->open_menu->open = false;
                        menubar->open_menu = NULL;
                        menubar->menu_active = false;
                        menubar->highlighted = NULL;
                        vg_widget_release_input_capture();
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_UP:
                        // Move highlight up
                        if (menubar->highlighted) {
                            menubar->highlighted = find_prev_enabled_item(menubar->highlighted);
                            widget->needs_paint = true;
                        }
                        return true;

                    case VG_KEY_DOWN:
                        // Move highlight down
                        if (menubar->highlighted) {
                            menubar->highlighted = find_next_enabled_item(menubar->highlighted);
                        } else if (menubar->open_menu->first_item) {
                            menubar->highlighted = find_first_enabled_item(menubar->open_menu);
                        }
                        widget->needs_paint = true;
                        return true;

                    case VG_KEY_LEFT:
                        // Move to previous menu
                        if (find_prev_enabled_menu(menubar->open_menu)) {
                            menubar->open_menu->open = false;
                            menubar->open_menu = find_prev_enabled_menu(menubar->open_menu);
                            menubar->open_menu->open = true;
                            menubar->highlighted = NULL;
                            widget->needs_paint = true;
                        }
                        return true;

                    case VG_KEY_RIGHT:
                        // Move to next menu
                        if (find_next_enabled_menu(menubar->open_menu)) {
                            menubar->open_menu->open = false;
                            menubar->open_menu = find_next_enabled_menu(menubar->open_menu);
                            menubar->open_menu->open = true;
                            menubar->highlighted = NULL;
                            widget->needs_paint = true;
                        }
                        return true;

                    case VG_KEY_ENTER:
                        // Execute highlighted item
                        if (menubar->highlighted && menubar->highlighted->enabled) {
                            menubar->highlighted->was_clicked = true;
                            if (menubar->highlighted->action) {
                                menubar->highlighted->action(menubar->highlighted->action_data);
                            }
                            menubar->open_menu->open = false;
                            menubar->open_menu = NULL;
                            menubar->menu_active = false;
                            menubar->highlighted = NULL;
                            vg_widget_release_input_capture();
                            widget->needs_paint = true;
                        }
                        return true;

                    default:
                        break;
                }
            }
            return false;

        default:
            break;
    }

    return false;
}

//=============================================================================
// MenuBar API
//=============================================================================

/// @brief Append a top-level menu with the given title to the menu bar.
///
/// @details Allocates a vg_menu_t, copies the title, links it to the bar's
///          doubly-linked menu list, and triggers a repaint. Items are added
///          via vg_menu_add_item on the returned handle.
///
/// @param menubar The menu bar to append to; may be NULL (returns NULL).
/// @param title   Display text shown in the bar; copied internally.
/// @return        New menu handle, or NULL on allocation failure.
vg_menu_t *vg_menubar_add_menu(vg_menubar_t *menubar, const char *title) {
    if (!menubar)
        return NULL;

    vg_menu_t *menu = calloc(1, sizeof(vg_menu_t));
    if (!menu)
        return NULL;

    menu->magic = VG_MENU_MAGIC;
    menu->title = title ? vg_strdup(title) : vg_strdup("Menu");
    if (!menu->title) {
        free(menu);
        return NULL;
    }
    menu->first_item = NULL;
    menu->last_item = NULL;
    menu->item_count = 0;
    menu->open = false;
    menu->enabled = true;
    menu->owner_menubar = menubar;

    // Add to end of list
    if (menubar->last_menu) {
        menubar->last_menu->next = menu;
        menu->prev = menubar->last_menu;
        menubar->last_menu = menu;
    } else {
        menubar->first_menu = menu;
        menubar->last_menu = menu;
    }
    menubar->menu_count++;

    menubar->base.needs_paint = true;

    return menu;
}

/// @brief Append a labelled item to a drop-down menu.
///
/// @details Both text and shortcut are copied internally. The item is initialised
///          as enabled, unchecked, and non-separator. action is invoked with data
///          when the item is clicked; NULL action is allowed.
///
/// @param menu     The menu to append to; may be NULL (returns NULL).
/// @param text     Display label; copied internally, may be NULL.
/// @param shortcut Keyboard hint shown on the right (e.g., "Ctrl+S"); may be NULL.
/// @param action   Callback invoked on activation, or NULL.
/// @param data     Opaque pointer forwarded to action.
/// @return         New item handle, or NULL on allocation failure.
vg_menu_item_t *vg_menu_add_item(
    vg_menu_t *menu, const char *text, const char *shortcut, void (*action)(void *), void *data) {
    if (!menu)
        return NULL;

    char *item_text = text ? vg_strdup(text) : NULL;
    char *item_shortcut = shortcut ? vg_strdup(shortcut) : NULL;
    if ((text && !item_text) || (shortcut && !item_shortcut)) {
        free(item_text);
        free(item_shortcut);
        return NULL;
    }

    vg_menu_item_t *item = calloc(1, sizeof(vg_menu_item_t));
    if (!item) {
        free(item_text);
        free(item_shortcut);
        return NULL;
    }

    item->magic = VG_MENU_ITEM_MAGIC;
    item->text = item_text;
    item->shortcut = item_shortcut;
    item->action = action;
    item->action_data = data;
    item->enabled = true;
    item->checked = false;
    item->separator = false;
    item->parent_menu = menu;
    item->submenu = NULL;

    // Add to end of list
    if (menu->last_item) {
        menu->last_item->next = item;
        item->prev = menu->last_item;
        menu->last_item = item;
    } else {
        menu->first_item = item;
        menu->last_item = item;
    }
    menu->item_count++;

    return item;
}

/// @brief Append a horizontal separator line to a drop-down menu.
///
/// @param menu The menu to append to; may be NULL (returns NULL).
/// @return     New separator item handle, or NULL on allocation failure.
vg_menu_item_t *vg_menu_add_separator(vg_menu_t *menu) {
    if (!menu)
        return NULL;

    vg_menu_item_t *item = calloc(1, sizeof(vg_menu_item_t));
    if (!item)
        return NULL;

    item->magic = VG_MENU_ITEM_MAGIC;
    item->separator = true;
    item->enabled = false;
    item->parent_menu = menu;

    // Add to end of list
    if (menu->last_item) {
        menu->last_item->next = item;
        item->prev = menu->last_item;
        menu->last_item = item;
    } else {
        menu->first_item = item;
        menu->last_item = item;
    }
    menu->item_count++;

    return item;
}

/// @brief Append a labelled item that opens a nested sub-menu, and return the sub-menu handle.
///
/// @details Creates a parent item via vg_menu_add_item, then allocates a vg_menu_t
///          struct stored in item->submenu. The sub-menu is owned by the item and
///          freed recursively when the parent is freed.
///
/// @param menu  The menu to append to; may be NULL (returns NULL).
/// @param title Label for the parent item and title for the sub-menu; copied internally.
/// @return      New sub-menu handle, or NULL on allocation failure.
vg_menu_t *vg_menu_add_submenu(vg_menu_t *menu, const char *title) {
    if (!menu)
        return NULL;

    vg_menu_item_t *item = vg_menu_add_item(menu, title, NULL, NULL, NULL);
    if (!item)
        return NULL;

    item->submenu = calloc(1, sizeof(vg_menu_t));
    if (!item->submenu) {
        vg_menu_remove_item(menu, item);
        return NULL;
    }

    item->submenu->title = title ? vg_strdup(title) : vg_strdup("Submenu");
    if (!item->submenu->title) {
        free(item->submenu);
        item->submenu = NULL;
        vg_menu_remove_item(menu, item);
        return NULL;
    }
    item->submenu->owner_menubar = menu->owner_menubar;
    item->submenu->retired_items = NULL;
    item->submenu->magic = VG_MENU_MAGIC;
    item->submenu->enabled = true;

    return item->submenu;
}

/// @brief Enable or disable a menu item (disabled items are greyed out and non-clickable).
///
/// @param item    The item to modify; may be NULL.
/// @param enabled false to grey out and ignore clicks.
void vg_menu_item_set_enabled(vg_menu_item_t *item, bool enabled) {
    if (vg_menu_item_is_live(item)) {
        item->enabled = enabled;
    }
}

/// @brief Set the checked state of a menu item, showing or hiding the checkmark indicator.
///
/// @param item    The item to modify; may be NULL.
/// @param checked true to show a checkmark next to the item label.
void vg_menu_item_set_checked(vg_menu_item_t *item, bool checked) {
    if (vg_menu_item_is_live(item)) {
        item->checkable = true;
        item->checked = checked;
    }
}

/// @brief Remove and free a specific item from its parent menu.
///
/// @details Verifies item->parent_menu == menu and that item is in the list
///          before unlinking and freeing it. No-op if item does not belong to menu.
///
/// @param menu The menu that owns item; may be NULL (no-op).
/// @param item The item to remove and free; may be NULL (no-op).
void vg_menu_remove_item(vg_menu_t *menu, vg_menu_item_t *item) {
    if (!menu || !vg_menu_item_is_live(item))
        return;
    if (item->parent_menu != menu)
        return;

    bool found = false;
    for (vg_menu_item_t *it = menu->first_item; it; it = it->next) {
        if (it == item) {
            found = true;
            break;
        }
    }
    if (!found)
        return;

    if (item->prev)
        item->prev->next = item->next;
    else
        menu->first_item = item->next;

    if (item->next)
        item->next->prev = item->prev;
    else
        menu->last_item = item->prev;

    menu->item_count--;
    if (menu->owner_menubar && menu->owner_menubar->highlighted == item)
        close_menubar_menu(menu->owner_menubar, menu);
    retire_menu_item(menu, item);
}

/// @brief Remove and free all items from a menu, leaving the menu itself intact.
///
/// @param menu The menu to clear; may be NULL.
void vg_menu_clear(vg_menu_t *menu) {
    if (!menu)
        return;

    if (menu->owner_menubar && menu->owner_menubar->open_menu == menu)
        close_menubar_menu(menu->owner_menubar, menu);
    else if (menu->owner_menubar && menu->owner_menubar->highlighted &&
             menu->owner_menubar->highlighted->parent_menu == menu)
        menu->owner_menubar->highlighted = NULL;

    vg_menu_item_t *item = menu->first_item;
    while (item) {
        vg_menu_item_t *next = item->next;
        retire_menu_item(menu, item);
        item = next;
    }

    menu->first_item = menu->last_item = NULL;
    menu->item_count = 0;
    if (menu->owner_menubar)
        menu->owner_menubar->base.needs_paint = true;
}

/// @brief Remove and free a top-level menu from the menu bar.
///
/// @details Verifies menu->owner_menubar == menubar before unlinking. Closes the
///          open drop-down if it refers to the removed menu. Triggers a repaint.
///
/// @param menubar The menu bar that owns menu; may be NULL (no-op).
/// @param menu    The menu to remove and free; may be NULL (no-op).
void vg_menubar_remove_menu(vg_menubar_t *menubar, vg_menu_t *menu) {
    if (!menubar || !menu)
        return;
    if (menu->owner_menubar != menubar)
        return;

    bool found = false;
    for (vg_menu_t *it = menubar->first_menu; it; it = it->next) {
        if (it == menu) {
            found = true;
            break;
        }
    }
    if (!found)
        return;

    if (menu->prev)
        menu->prev->next = menu->next;
    else
        menubar->first_menu = menu->next;

    if (menu->next)
        menu->next->prev = menu->prev;
    else
        menubar->last_menu = menu->prev;

    menubar->menu_count--;

    if (menubar->open_menu == menu)
        close_menubar_menu(menubar, menu);
    else if (menubar->highlighted && menubar->highlighted->parent_menu == menu)
        menubar->highlighted = NULL;

    retire_menu(menubar, menu);
    menubar->base.needs_paint = true;
}

/// @brief Unlink and free one exact retired item from a menu record.
/// @details Works for both live menus and menu records retained on a MenuBar retirement chain.
/// @param menu Menu record whose retirement chain is searched.
/// @param item Exact retired-item address to reclaim.
/// @return `true` when the record was found and destroyed.
bool vg_menu_reclaim_retired_item(vg_menu_t *menu, vg_menu_item_t *item) {
    if (!menu || !item)
        return false;
    vg_menu_item_t **link = &menu->retired_items;
    while (*link) {
        vg_menu_item_t *candidate = *link;
        if (candidate == item) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_menu_item(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Unlink and free one exact retired menu from a MenuBar.
/// @details The embedding runtime has already established that the menu and its retained items have
///          no managed wrappers, so the ordinary deep menu destructor is safe.
/// @param menubar Menu bar whose retired-menu chain is searched.
/// @param menu Exact retired-menu address to reclaim.
/// @return `true` when the record was found and recursively destroyed.
bool vg_menubar_reclaim_retired_menu(vg_menubar_t *menubar, vg_menu_t *menu) {
    if (!menubar || !menu)
        return false;
    vg_menu_t **link = &menubar->retired_menus;
    while (*link) {
        vg_menu_t *candidate = *link;
        if (candidate == menu) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_menu(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Set the font and size for all menu bar labels and drop-down items.
///
/// @param menubar The menu bar to configure; may be NULL.
/// @param font    Font to use; NULL retains the current font.
/// @param size    Font size in points; if <= 0 the theme's normal size is used.
void vg_menubar_set_font(vg_menubar_t *menubar, vg_font_t *font, float size) {
    if (!menubar)
        return;

    menubar->font = font;
    menubar->font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    menubar->base.needs_paint = true;
}

//=============================================================================
// Keyboard Accelerators
//=============================================================================

// Key name to key code mapping
typedef struct {
    const char *name;
    int key;
} key_mapping_t;

static const key_mapping_t g_key_mappings[] = {
    // Letters
    {"A", VG_KEY_A},
    {"B", VG_KEY_B},
    {"C", VG_KEY_C},
    {"D", VG_KEY_D},
    {"E", VG_KEY_E},
    {"F", VG_KEY_F},
    {"G", VG_KEY_G},
    {"H", VG_KEY_H},
    {"I", VG_KEY_I},
    {"J", VG_KEY_J},
    {"K", VG_KEY_K},
    {"L", VG_KEY_L},
    {"M", VG_KEY_M},
    {"N", VG_KEY_N},
    {"O", VG_KEY_O},
    {"P", VG_KEY_P},
    {"Q", VG_KEY_Q},
    {"R", VG_KEY_R},
    {"S", VG_KEY_S},
    {"T", VG_KEY_T},
    {"U", VG_KEY_U},
    {"V", VG_KEY_V},
    {"W", VG_KEY_W},
    {"X", VG_KEY_X},
    {"Y", VG_KEY_Y},
    {"Z", VG_KEY_Z},
    // Numbers
    {"0", VG_KEY_0},
    {"1", VG_KEY_1},
    {"2", VG_KEY_2},
    {"3", VG_KEY_3},
    {"4", VG_KEY_4},
    {"5", VG_KEY_5},
    {"6", VG_KEY_6},
    {"7", VG_KEY_7},
    {"8", VG_KEY_8},
    {"9", VG_KEY_9},
    // Function keys
    {"F1", VG_KEY_F1},
    {"F2", VG_KEY_F2},
    {"F3", VG_KEY_F3},
    {"F4", VG_KEY_F4},
    {"F5", VG_KEY_F5},
    {"F6", VG_KEY_F6},
    {"F7", VG_KEY_F7},
    {"F8", VG_KEY_F8},
    {"F9", VG_KEY_F9},
    {"F10", VG_KEY_F10},
    {"F11", VG_KEY_F11},
    {"F12", VG_KEY_F12},
    // Special keys
    {"Enter", VG_KEY_ENTER},
    {"Return", VG_KEY_ENTER},
    {"Tab", VG_KEY_TAB},
    {"Escape", VG_KEY_ESCAPE},
    {"Esc", VG_KEY_ESCAPE},
    {"Space", VG_KEY_SPACE},
    {"Backspace", VG_KEY_BACKSPACE},
    {"Delete", VG_KEY_DELETE},
    {"Del", VG_KEY_DELETE},
    {"Insert", VG_KEY_INSERT},
    {"Ins", VG_KEY_INSERT},
    {"Home", VG_KEY_HOME},
    {"End", VG_KEY_END},
    {"PageUp", VG_KEY_PAGE_UP},
    {"PgUp", VG_KEY_PAGE_UP},
    {"PageDown", VG_KEY_PAGE_DOWN},
    {"PgDn", VG_KEY_PAGE_DOWN},
    {"Up", VG_KEY_UP},
    {"Down", VG_KEY_DOWN},
    {"Left", VG_KEY_LEFT},
    {"Right", VG_KEY_RIGHT},
    // Punctuation
    {"-", VG_KEY_MINUS},
    {"=", VG_KEY_EQUAL},
    {"[", VG_KEY_LEFT_BRACKET},
    {"]", VG_KEY_RIGHT_BRACKET},
    {";", VG_KEY_SEMICOLON},
    {"'", VG_KEY_APOSTROPHE},
    {",", VG_KEY_COMMA},
    {".", VG_KEY_PERIOD},
    {"/", VG_KEY_SLASH},
    {"\\", VG_KEY_BACKSLASH},
    {"`", VG_KEY_GRAVE},
    {NULL, 0}};

/// @brief Look up a key name string in g_key_mappings and return its VG_KEY_* code.
/// @param name Case-insensitive key-token name.
/// @return Matching VG_KEY_* code, or VG_KEY_UNKNOWN.
static int lookup_key(const char *name) {
    for (const key_mapping_t *m = g_key_mappings; m->name; m++) {
        if (strcasecmp(name, m->name) == 0) {
            return m->key;
        }
    }
    return VG_KEY_UNKNOWN;
}

/// @brief Parse a "Ctrl+Shift+S"-style shortcut string into a vg_accelerator_t.
///
/// @details Tokenises on '+', recognises Ctrl/Control, Cmd/Command/Meta/Super,
///          Shift, and Alt/Option as modifier tokens, and requires exactly one
///          recognised key token. Multi-stroke chords are intentionally rejected:
///          menus may display them, but the app-level chord engine executes them.
///
/// @param shortcut Shortcut string (e.g., "Ctrl+S", "Cmd+Shift+Z"); may not be NULL.
/// @param accel    Out-parameter receiving the parsed key and modifier mask; may not be NULL.
/// @return         true on success; false if shortcut is empty or the key is unrecognised.
bool vg_parse_accelerator(const char *shortcut, vg_accelerator_t *accel) {
    if (!shortcut || !accel)
        return false;

    accel->key = VG_KEY_UNKNOWN;
    accel->modifiers = 0;

    // Copy string for tokenizing
    char *str = vg_strdup(shortcut);
    if (!str)
        return false;

    char *saveptr;
    char *token = strtok_r(str, "+", &saveptr);

    while (token) {
        // Trim whitespace
        while (*token == ' ')
            token++;
        char *end = token + strlen(token);
        while (end > token && end[-1] == ' ')
            *--end = '\0';

        // Check for modifiers
        if (strcasecmp(token, "Ctrl") == 0 || strcasecmp(token, "Control") == 0) {
            accel->modifiers |= VG_MOD_CTRL;
        } else if (strcasecmp(token, "Cmd") == 0 || strcasecmp(token, "Command") == 0 ||
                   strcasecmp(token, "Meta") == 0 || strcasecmp(token, "Super") == 0) {
            accel->modifiers |= VG_MOD_SUPER;
        } else if (strcasecmp(token, "Shift") == 0) {
            accel->modifiers |= VG_MOD_SHIFT;
        } else if (strcasecmp(token, "Alt") == 0 || strcasecmp(token, "Option") == 0) {
            accel->modifiers |= VG_MOD_ALT;
        } else {
            int parsed_key = lookup_key(token);
            if (parsed_key == VG_KEY_UNKNOWN || accel->key != VG_KEY_UNKNOWN) {
                accel->key = VG_KEY_UNKNOWN;
                accel->modifiers = 0;
                free(str);
                return false;
            }
            accel->key = parsed_key;
        }

        token = strtok_r(NULL, "+", &saveptr);
    }

    free(str);
    return accel->key != VG_KEY_UNKNOWN;
}

/// @brief Parse shortcut and register it as a global keyboard accelerator for item.
///
/// @details Calls vg_parse_accelerator; if parsing succeeds, stores the parsed
///          vg_accelerator_t in item->accel and prepends a vg_accel_entry_t to
///          menubar->accel_table. Does nothing if parsing fails.
///
/// @param menubar  The menu bar to register with; may be NULL (no-op).
/// @param item     The menu item to trigger on shortcut match; may be NULL (no-op).
/// @param shortcut Shortcut string (e.g., "Ctrl+S"); may be NULL (no-op).
void vg_menubar_register_accelerator(vg_menubar_t *menubar,
                                     vg_menu_item_t *item,
                                     const char *shortcut) {
    if (!menubar || !vg_menu_item_is_live(item) || !shortcut)
        return;

    vg_accelerator_t accel;
    if (!vg_parse_accelerator(shortcut, &accel))
        return;

    // Store parsed accelerator in item
    item->accel = accel;

    // Add to accelerator table
    vg_accel_entry_t *entry = calloc(1, sizeof(vg_accel_entry_t));
    if (!entry)
        return;

    entry->accel = accel;
    entry->item = item;
    entry->next = menubar->accel_table;
    menubar->accel_table = entry;
}

/// @brief Recursively register accelerators for all items in menu and its submenus.
/// @param menubar Menu bar receiving accelerator table entries.
/// @param menu Menu tree to traverse.
static void rebuild_accels_for_menu(vg_menubar_t *menubar, vg_menu_t *menu) {
    for (vg_menu_item_t *item = menu->first_item; item; item = item->next) {
        if (item->shortcut) {
            vg_menubar_register_accelerator(menubar, item, item->shortcut);
        }
        if (item->submenu) {
            rebuild_accels_for_menu(menubar, item->submenu);
        }
    }
}

/// @brief Tear down and rebuild the entire accelerator table from the current menu tree.
///
/// @details Frees all existing vg_accel_entry_t nodes, then walks every top-level
///          menu and all nested submenus via rebuild_accels_for_menu. Call this after
///          bulk item modifications when re-registering individually would be verbose.
///
/// @param menubar The menu bar to rebuild for; may be NULL.
void vg_menubar_rebuild_accelerators(vg_menubar_t *menubar) {
    if (!menubar)
        return;

    // Free existing accelerator table
    vg_accel_entry_t *entry = menubar->accel_table;
    while (entry) {
        vg_accel_entry_t *next = entry->next;
        free(entry);
        entry = next;
    }
    menubar->accel_table = NULL;

    // Rebuild from all menus
    for (vg_menu_t *menu = menubar->first_menu; menu; menu = menu->next) {
        rebuild_accels_for_menu(menubar, menu);
    }
}

/// @brief Dispatch a key event against the accelerator table, invoking the matching item's action.
///
/// @details Ctrl and Super are treated as equivalent so a single registration fires
///          on both platforms. Only dispatches to enabled items with a non-NULL action.
///
/// @param menubar   The menu bar whose accelerator table to search; may be NULL (returns false).
/// @param key       VG_KEY_* code from the key-down event.
/// @param modifiers Modifier bitmask (VG_MOD_CTRL | VG_MOD_SHIFT | VG_MOD_ALT | VG_MOD_SUPER).
/// @return          true if a matching accelerator was found and its action was invoked.
bool vg_menubar_handle_accelerator(vg_menubar_t *menubar, int key, uint32_t modifiers) {
    if (!menubar)
        return false;

    // Normalize modifiers: treat Ctrl and Super as equivalent on Mac
    uint32_t norm_mods = modifiers & (VG_MOD_CTRL | VG_MOD_SUPER | VG_MOD_SHIFT | VG_MOD_ALT);

    for (vg_accel_entry_t *entry = menubar->accel_table; entry; entry = entry->next) {
        // Check for match
        uint32_t entry_mods = entry->accel.modifiers;

        // Allow Ctrl and Super to be interchangeable
        bool ctrl_match = ((norm_mods & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0 &&
                           (entry_mods & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0) ||
                          ((norm_mods & (VG_MOD_CTRL | VG_MOD_SUPER)) == 0 &&
                           (entry_mods & (VG_MOD_CTRL | VG_MOD_SUPER)) == 0);

        bool shift_match = ((norm_mods & VG_MOD_SHIFT) != 0) == ((entry_mods & VG_MOD_SHIFT) != 0);
        bool alt_match = ((norm_mods & VG_MOD_ALT) != 0) == ((entry_mods & VG_MOD_ALT) != 0);

        if (entry->accel.key == key && ctrl_match && shift_match && alt_match) {
            vg_menu_item_t *item = entry->item;
            if (vg_menu_item_is_live(item) && item->enabled && item->action) {
                item->action(item->action_data);
                return true;
            }
        }
    }

    return false;
}
