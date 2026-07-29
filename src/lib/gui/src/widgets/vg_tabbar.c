//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_tabbar.c
// Purpose: Tab bar widget — a horizontal strip of draggable, closable tabs with
//          overflow scrolling, modified-indicator dots, and drag-to-reorder support.
// Key invariants:
//   - Tabs are maintained in a doubly-linked list (first_tab … last_tab);
//     tab_count shadows the list length for O(1) access.
//   - active_tab is never a dangling pointer; vg_tabbar_remove_tab updates it
//     before freeing the removed tab.
//   - active_change_version is a monotonic counter incremented whenever
//     active_tab changes; used to detect stale references externally.
//   - scroll_offset tracks horizontal scroll in pixels when tabs overflow.
//   - dragging / drag_tab tracks an in-progress drag-to-reorder gesture.
// Ownership/Lifetime:
//   - Each vg_tab_t owns its title and tooltip strings. Removed tabs are kept as
//     inert tombstones until tabbar destroy or explicit retired-tab pruning so
//     stale external handles fail vg_tab_is_live() safely.
//   - The tabbar does not own any external user_data pointers stored on tabs.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements scrollable, closable, reorderable tabs and stable
///        tombstone-backed external tab handles.
/// @details Tab geometry, title fitting, close hit testing, hover tooltips, and
///          drag insertion all account for horizontal overflow. Removed tabs
///          become inert until explicit or final retirement cleanup.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABBAR_DEFAULT_HEIGHT 35.0f
#define TABBAR_DEFAULT_PADDING 12.0f
#define TABBAR_DEFAULT_CLOSE_SIZE 14.0f
#define TABBAR_DEFAULT_MAX_WIDTH 200.0f
#define TABBAR_CLOSE_GAP 4.0f
#define TABBAR_DRAG_THRESHOLD 6.0f
#define VG_TAB_MAGIC UINT64_C(0x56475441424C4956)
#define VG_TAB_RETIRED_MAGIC UINT64_C(0x5647544142524554)

//=============================================================================
// Forward Declarations
//=============================================================================

static void tabbar_destroy(vg_widget_t *widget);
static void tabbar_measure(vg_widget_t *widget, float available_width, float available_height);
static void tabbar_paint(vg_widget_t *widget, void *canvas);
static bool tabbar_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool tabbar_can_focus(vg_widget_t *widget);
static float get_tab_width(vg_tabbar_t *tabbar, vg_tab_t *tab);
static bool tab_close_button_hit(vg_tabbar_t *tabbar, vg_tab_t *tab, float local_x, float local_y);
static void tabbar_sync_hover_tooltip(vg_tabbar_t *tabbar);
static void tabbar_record_close_clicked(vg_tabbar_t *tabbar, int index);
static char *make_tab_title(const vg_tab_t *tab);
static void tabbar_ensure_tab_visible(vg_tabbar_t *tabbar, vg_tab_t *tab);
static void free_tab(vg_tab_t *tab);

//=============================================================================
// TabBar VTable
//=============================================================================

static vg_widget_vtable_t g_tabbar_vtable = {.destroy = tabbar_destroy,
                                             .measure = tabbar_measure,
                                             .arrange = NULL,
                                             .paint = tabbar_paint,
                                             .handle_event = tabbar_handle_event,
                                             .can_focus = tabbar_can_focus,
                                             .on_focus = NULL};

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Validate that a tab handle remains attached to a live tab bar.
/// @param tab Tab handle to inspect.
/// @return `true` when live magic and an owning tab bar are present.
bool vg_tab_is_live(const vg_tab_t *tab) {
    return tab && tab->magic == VG_TAB_MAGIC && tab->owner != NULL;
}

/// @brief Free a tab and its owned title/tooltip strings (NULL-safe).
/// @param tab Owned live or retired tab to destroy.
static void free_tab(vg_tab_t *tab) {
    if (!tab)
        return;
    free(tab->title);
    free(tab->tooltip);
    free(tab->stable_id);
    if (tab->owns_user_data)
        free(tab->user_data);
    free(tab);
}

/// @brief Tear down @p tab's owned strings/links and park it on @p tabbar's
///        retired list for deferred freeing (use-after-free defense when a
///        tab is closed during event dispatch).
/// @param tabbar Tab bar receiving the retired record.
/// @param tab Detached tab whose external handle becomes inert.
static void retire_tab(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || !tab)
        return;
    free(tab->title);
    tab->title = NULL;
    free(tab->tooltip);
    tab->tooltip = NULL;
    free(tab->stable_id);
    tab->stable_id = NULL;
    tab->stable_id_len = 0;
    if (tab->owns_user_data)
        free(tab->user_data);
    tab->owner = NULL;
    tab->user_data = NULL;
    tab->owns_user_data = false;
    tab->next = NULL;
    tab->prev = NULL;
    tab->magic = VG_TAB_RETIRED_MAGIC;
    tab->retired_next = tabbar->retired_tabs;
    tabbar->retired_tabs = tab;
}

/// @brief Drain and free every tab on @p tabbar's retired list.
/// @param tabbar Tab bar whose retirement chain is destroyed.
static void free_retired_tabs(vg_tabbar_t *tabbar) {
    if (!tabbar)
        return;
    vg_tab_t *tab = tabbar->retired_tabs;
    while (tab) {
        vg_tab_t *next = tab->retired_next;
        tab->retired_next = NULL;
        free_tab(tab);
        tab = next;
    }
    tabbar->retired_tabs = NULL;
}

/// @brief Returns the pixel width of @p tab's button including title text, padding, and optional
/// close-button gutter; clamped to max_tab_width.
/// @param tabbar Tab bar supplying typography and size policy.
/// @param tab Live tab to measure.
/// @return Physical tab-button width.
static float get_tab_width(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar->font || !tab->title) {
        return tabbar->max_tab_width > 0.0f ? tabbar->max_tab_width : 100.0f;
    }

    char *title = make_tab_title(tab);
    vg_text_metrics_t metrics;
    vg_font_measure_text(tabbar->font, tabbar->font_size, title ? title : "", &metrics);
    free(title);

    float width = metrics.width + tabbar->tab_padding * 2;

    // Add the leading vector-icon slot when one is set (ADR 0220).
    if (tab->icon_vector_id >= 0)
        width += tabbar->font_size + 6.0f;

    // Add close button width if closable
    if (tab->closable) {
        float close_gap =
            (tabbar->close_button_size / TABBAR_DEFAULT_CLOSE_SIZE) * TABBAR_CLOSE_GAP;
        width += tabbar->close_button_size + close_gap;
    }

    // Clamp to max
    if (tabbar->max_tab_width > 0 && width > tabbar->max_tab_width) {
        width = tabbar->max_tab_width;
    }

    return width;
}

/// @brief Returns a heap-allocated copy of the tab title, appending " *" when the tab is marked
/// modified; caller must free.
/// @param tab Tab whose presentation title is built.
/// @return Newly allocated display title, or `NULL` on allocation failure.
static char *make_tab_title(const vg_tab_t *tab) {
    if (!tab || !tab->title)
        return vg_strdup("");

    if (!tab->modified)
        return vg_strdup(tab->title);

    size_t len = strlen(tab->title);
    char *title = (char *)malloc(len + 3); /* " *\0" */
    if (!title)
        return NULL;
    memcpy(title, tab->title, len);
    title[len] = ' ';
    title[len + 1] = '*';
    title[len + 2] = '\0';
    return title;
}

/// @brief Back @p len up to the nearest UTF-8 codepoint boundary so title
///        truncation never splits a multi-byte character.
/// @param text UTF-8 title containing @p len as a valid index.
/// @param len Candidate byte prefix.
/// @return Greatest codepoint-safe prefix no longer than @p len.
static size_t utf8_prev_boundary(const char *text, size_t len) {
    while (len > 0 && (((unsigned char)text[len] & 0xC0u) == 0x80u))
        len--;
    return len;
}

/// @brief Returns a heap-allocated copy of @p title truncated with "..." to fit within @p max_width
/// pixels; caller must free.
/// @param tabbar Tab bar supplying font metrics.
/// @param title Presentation title to fit.
/// @param max_width Maximum rendered width.
/// @return Newly allocated full, ellipsized, or empty title; `NULL` on failure.
static char *fit_tab_title(vg_tabbar_t *tabbar, const char *title, float max_width) {
    if (!title)
        return vg_strdup("");
    if (!tabbar->font || max_width <= 0.0f)
        return vg_strdup("");

    vg_text_metrics_t metrics;
    vg_font_measure_text(tabbar->font, tabbar->font_size, title, &metrics);
    if (metrics.width <= max_width)
        return vg_strdup(title);

    vg_text_metrics_t ellipsis_metrics;
    vg_font_measure_text(tabbar->font, tabbar->font_size, "...", &ellipsis_metrics);
    if (ellipsis_metrics.width > max_width)
        return vg_strdup("");

    size_t len = strlen(title);
    char *buf = (char *)malloc(len + 4); /* original text + "...\0" */
    if (!buf)
        return NULL;

    while (len > 0) {
        memcpy(buf, title, len);
        memcpy(buf + len, "...", 4);
        vg_font_measure_text(tabbar->font, tabbar->font_size, buf, &metrics);
        if (metrics.width <= max_width)
            return buf;
        len--;
        len = utf8_prev_boundary(title, len);
    }

    memcpy(buf, "...", 4);
    return buf;
}

/// @brief Returns the tab whose button spans widget-local @p x (accounting for scroll_x), or NULL
/// if no tab covers that coordinate.
/// @param tabbar Tab bar whose button geometry is scanned.
/// @param x Widget-local horizontal coordinate.
/// @return Borrowed live tab under the coordinate, or `NULL`.
static vg_tab_t *find_tab_at_x(vg_tabbar_t *tabbar, float x) {
    float tab_x = -tabbar->scroll_x;

    for (vg_tab_t *tab = tabbar->first_tab; tab; tab = tab->next) {
        float width = get_tab_width(tabbar, tab);
        if (x >= tab_x && x < tab_x + width) {
            return tab;
        }
        tab_x += width;
    }
    return NULL;
}

/// @brief Returns the 0-based index of the tab under widget coordinates (x, y), or -1.
/// @details `x`/`y` are in the same canvas-pixel space as the tab strip's bounds
///          (i.e. the values reported by the input layer). Used for right-click
///          tab context menus. Scroll offset is handled by find_tab_at_x.
/// @copydetails vg_tabbar_index_at
int vg_tabbar_index_at(vg_tabbar_t *tabbar, int x, int y) {
    if (!tabbar)
        return -1;
    if ((float)y < tabbar->base.y || (float)y >= tabbar->base.y + tabbar->base.height)
        return -1;
    vg_tab_t *tab = find_tab_at_x(tabbar, (float)x - tabbar->base.x);
    if (!tab)
        return -1;
    return vg_tabbar_get_tab_index(tabbar, tab);
}

/// @brief Returns the absolute X offset (before scroll) of @p target's left edge within the tab
/// strip.
/// @param tabbar Tab bar whose list is traversed.
/// @param target Target tab.
/// @return Unscrolled horizontal offset accumulated from preceding tabs.
static float get_tab_x(vg_tabbar_t *tabbar, vg_tab_t *target) {
    float x = 0;
    for (vg_tab_t *tab = tabbar->first_tab; tab && tab != target; tab = tab->next) {
        x += get_tab_width(tabbar, tab);
    }
    return x;
}

/// @brief Returns the insertion index (0-based) at which a dragged tab should be dropped based on
/// @p local_x position.
/// @param tabbar Tab bar supplying tab midpoint geometry.
/// @param local_x Pointer X relative to the widget.
/// @return Clamped insertion index.
static int tabbar_target_index_from_x(vg_tabbar_t *tabbar, float local_x) {
    if (!tabbar || tabbar->tab_count <= 1)
        return 0;

    float tab_x = -tabbar->scroll_x;
    int index = 0;
    for (vg_tab_t *tab = tabbar->first_tab; tab; tab = tab->next, index++) {
        float width = get_tab_width(tabbar, tab);
        if (local_x < tab_x + width * 0.5f)
            return index;
        tab_x += width;
    }
    return tabbar->tab_count - 1;
}

/// @brief Moves @p tab to @p new_index in the doubly-linked list, updating first_tab/last_tab;
/// returns true if the order changed.
/// @param tabbar Owning tab bar.
/// @param tab Live tab to relocate.
/// @param new_index Requested zero-based destination index.
/// @return `true` when list order and reorder metadata changed.
static bool tabbar_move_tab_to_index(vg_tabbar_t *tabbar, vg_tab_t *tab, int new_index) {
    if (!tabbar || !tab || tabbar->tab_count < 2)
        return false;

    int current_index = vg_tabbar_get_tab_index(tabbar, tab);
    if (current_index < 0)
        return false;
    if (new_index < 0)
        new_index = 0;
    if (new_index >= tabbar->tab_count)
        new_index = tabbar->tab_count - 1;
    if (new_index == current_index)
        return false;

    if (tab->prev) {
        tab->prev->next = tab->next;
    } else {
        tabbar->first_tab = tab->next;
    }
    if (tab->next) {
        tab->next->prev = tab->prev;
    } else {
        tabbar->last_tab = tab->prev;
    }

    tab->prev = NULL;
    tab->next = NULL;

    if (new_index <= 0 || !tabbar->first_tab) {
        tab->next = tabbar->first_tab;
        if (tabbar->first_tab)
            tabbar->first_tab->prev = tab;
        tabbar->first_tab = tab;
        if (!tabbar->last_tab)
            tabbar->last_tab = tab;
    } else {
        vg_tab_t *insert_before = vg_tabbar_get_tab_at(tabbar, new_index);
        if (!insert_before) {
            tab->prev = tabbar->last_tab;
            if (tabbar->last_tab)
                tabbar->last_tab->next = tab;
            tabbar->last_tab = tab;
            if (!tabbar->first_tab)
                tabbar->first_tab = tab;
        } else {
            tab->next = insert_before;
            tab->prev = insert_before->prev;
            if (insert_before->prev) {
                insert_before->prev->next = tab;
            } else {
                tabbar->first_tab = tab;
            }
            insert_before->prev = tab;
        }
    }

    tabbar->base.needs_layout = true;
    tabbar->base.needs_paint = true;
    tabbar->reordered_from = current_index;
    tabbar->reordered_to = new_index;
    if (tabbar->reorder_version < UINT64_MAX)
        tabbar->reorder_version++;
    vg_widget_note_revision(&tabbar->base);
    return true;
}

/// @brief Returns true if @p local_x/local_y falls within @p tab's close button rectangle (only
/// when the tab is closable).
/// @param tabbar Tab bar supplying scrolled tab geometry.
/// @param tab Candidate tab.
/// @param local_x Pointer X relative to the widget.
/// @param local_y Pointer Y relative to the widget.
/// @return `true` when the closable tab's close affordance contains the point.
static bool tab_close_button_hit(vg_tabbar_t *tabbar, vg_tab_t *tab, float local_x, float local_y) {
    if (!tabbar || !tab || !tab->closable)
        return false;

    float tab_x = get_tab_x(tabbar, tab) - tabbar->scroll_x;
    float width = get_tab_width(tabbar, tab);
    float close_x = tab_x + width - tabbar->tab_padding - tabbar->close_button_size;
    float close_y = (tabbar->tab_height - tabbar->close_button_size) / 2.0f;
    float close_w = tabbar->close_button_size;
    float close_h = tabbar->close_button_size;

    return local_x >= close_x && local_x < close_x + close_w && local_y >= close_y &&
           local_y < close_y + close_h;
}

/// @brief Updates the widget tooltip to show the hovered tab's tooltip, saving and restoring the
/// global tooltip when hover changes.
/// @param tabbar TabBar whose hovered tab and tooltip state are synchronized.
static void tabbar_sync_hover_tooltip(vg_tabbar_t *tabbar) {
    if (!tabbar)
        return;

    const char *hover_tooltip =
        (tabbar->hovered_tab && tabbar->hovered_tab->tooltip) ? tabbar->hovered_tab->tooltip : NULL;

    if (hover_tooltip) {
        if (!tabbar->hover_tooltip_active) {
            char *saved_tooltip =
                tabbar->base.tooltip_text ? vg_strdup(tabbar->base.tooltip_text) : NULL;
            if (tabbar->base.tooltip_text && !saved_tooltip)
                return;
            free(tabbar->saved_tooltip_text);
            tabbar->saved_tooltip_text = saved_tooltip;
            tabbar->hover_tooltip_active = true;
        }
        if (!tabbar->base.tooltip_text || strcmp(tabbar->base.tooltip_text, hover_tooltip) != 0) {
            vg_widget_set_tooltip_text(&tabbar->base, hover_tooltip);
        }
        return;
    }

    if (tabbar->hover_tooltip_active) {
        vg_widget_set_tooltip_text(&tabbar->base, tabbar->saved_tooltip_text);
        free(tabbar->saved_tooltip_text);
        tabbar->saved_tooltip_text = NULL;
        tabbar->hover_tooltip_active = false;
    }
}

/// @brief Record a close-click as a versioned runtime-visible event.
/// @param tabbar TabBar whose close-click latch is updated.
/// @param index Zero-based index of the tab whose close affordance was activated.
static void tabbar_record_close_clicked(vg_tabbar_t *tabbar, int index) {
    if (!tabbar || index < 0)
        return;
    tabbar->close_clicked_index = index;
    tabbar->close_click_version++;
    if (tabbar->close_click_version == 0)
        tabbar->close_click_version = 1;
}

/// @brief VTable can_focus: returns true when the widget is both enabled and visible.
/// @param widget TabBar base widget whose focus eligibility is queried.
/// @return True when the widget is enabled and visible.
static bool tabbar_can_focus(vg_widget_t *widget) {
    return widget && widget->enabled && widget->visible;
}

/// @brief Adjusts scroll_x so that @p tab's button is fully visible within the current widget
/// width.
/// @param tabbar TabBar whose horizontal scroll offset may be changed.
/// @param tab Tab that must be brought fully into view.
static void tabbar_ensure_tab_visible(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || !tab)
        return;

    float tab_x = get_tab_x(tabbar, tab);
    float tab_w = get_tab_width(tabbar, tab);
    // Measurement is the freshest width during a resize or hidden-to-visible
    // transition; arranged width can still describe the previous frame (or a
    // collapsed pane). Programmatic activation outside layout falls back to the
    // arranged width when no measured viewport exists yet.
    float viewport_w =
        tabbar->base.measured_width > 0.0f ? tabbar->base.measured_width : tabbar->base.width;
    if (viewport_w <= 0.0f)
        return;

    if (tab_x < tabbar->scroll_x) {
        tabbar->scroll_x = tab_x;
    } else if (tab_x + tab_w > tabbar->scroll_x + viewport_w) {
        tabbar->scroll_x = tab_x + tab_w - viewport_w;
    }

    float max_scroll = tabbar->total_width - viewport_w;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    if (tabbar->scroll_x < 0.0f)
        tabbar->scroll_x = 0.0f;
    if (tabbar->scroll_x > max_scroll)
        tabbar->scroll_x = max_scroll;
}

//=============================================================================
// TabBar Implementation
//=============================================================================

/// @brief Create an empty tab bar widget.
///
/// @details Default appearance is taken from the current theme.  No tabs are
///          created; add them with vg_tabbar_add_tab.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_tabbar_t, or NULL on allocation failure.
vg_tabbar_t *vg_tabbar_create(vg_widget_t *parent) {
    vg_tabbar_t *tabbar = calloc(1, sizeof(vg_tabbar_t));
    if (!tabbar)
        return NULL;

    // Initialize base widget
    vg_widget_init(&tabbar->base, VG_WIDGET_TABBAR, &g_tabbar_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Initialize tabbar-specific fields
    tabbar->first_tab = NULL;
    tabbar->last_tab = NULL;
    tabbar->active_tab = NULL;
    tabbar->retired_tabs = NULL;
    tabbar->tab_count = 0;

    tabbar->font = theme->typography.font_regular;
    tabbar->font_size = theme->typography.size_normal;

    // Appearance
    float s = theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    tabbar->tab_height = TABBAR_DEFAULT_HEIGHT * s;
    tabbar->tab_padding = TABBAR_DEFAULT_PADDING * s;
    tabbar->close_button_size = TABBAR_DEFAULT_CLOSE_SIZE * s;
    tabbar->max_tab_width = TABBAR_DEFAULT_MAX_WIDTH * s;
    tabbar->active_bg = theme->colors.bg_primary;
    tabbar->inactive_bg =
        vg_color_blend(theme->colors.bg_secondary, theme->colors.bg_primary, 0.35f);
    tabbar->text_color = theme->colors.fg_primary;
    tabbar->close_color = theme->colors.fg_secondary;

    // Scrolling
    tabbar->scroll_x = 0;
    tabbar->total_width = 0;

    // Callbacks
    tabbar->on_select = NULL;
    tabbar->on_select_data = NULL;
    tabbar->on_close = NULL;
    tabbar->on_close_data = NULL;
    tabbar->on_reorder = NULL;
    tabbar->on_reorder_data = NULL;

    // State
    tabbar->hovered_tab = NULL;
    tabbar->close_button_hovered = false;
    tabbar->dragging = false;
    tabbar->drag_pending = false;
    tabbar->drag_tab = NULL;
    tabbar->drag_origin_x = 0;
    tabbar->drag_x = 0;

    // Per-frame tracking
    tabbar->prev_active_tab = NULL;
    tabbar->close_clicked_index = -1;
    tabbar->close_click_version = 0;
    tabbar->reported_close_click_version = 0;
    tabbar->auto_close = true;
    tabbar->active_change_version = 0;
    tabbar->reported_active_change_version = 0;
    tabbar->reorder_version = 0;
    tabbar->reported_reorder_version = 0;
    tabbar->reordered_from = -1;
    tabbar->reordered_to = -1;
    tabbar->saved_tooltip_text = NULL;
    tabbar->hover_tooltip_active = false;

    // Set size
    tabbar->base.constraints.min_height = tabbar->tab_height;
    tabbar->base.constraints.preferred_height = tabbar->tab_height;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &tabbar->base);
    }

    return tabbar;
}

/// @brief VTable destroy: releases input capture if held, frees all tab nodes (title, tooltip,
/// struct), and saved tooltip text.
/// @param widget TabBar base widget being destroyed.
static void tabbar_destroy(vg_widget_t *widget) {
    vg_tabbar_t *tabbar = (vg_tabbar_t *)widget;

    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();

    // Free all tabs
    vg_tab_t *tab = tabbar->first_tab;
    while (tab) {
        vg_tab_t *next = tab->next;
        free_tab(tab);
        tab = next;
    }
    free_retired_tabs(tabbar);
    free(tabbar->saved_tooltip_text);
}

/// @brief VTable measure: sums all tab widths into total_width, claims available_width, and
/// re-clamps scroll_x to prevent over-scroll after tab removal.
/// @param widget TabBar base widget whose measured dimensions are updated.
/// @param available_width Parent-provided horizontal space.
/// @param available_height Parent-provided height; used only by fixed strip sizing.
static void tabbar_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_tabbar_t *tabbar = (vg_tabbar_t *)widget;
    (void)available_height;

    // Calculate total width of all tabs
    float total = 0;
    for (vg_tab_t *tab = tabbar->first_tab; tab; tab = tab->next) {
        total += get_tab_width(tabbar, tab);
    }
    tabbar->total_width = total;

    widget->measured_width = available_width > 0 ? available_width : total;
    widget->measured_height = tabbar->tab_height;

    // Re-clamp scroll_x against the new total. When tabs are removed, the old
    // scroll_x can exceed the new max and the paint loop culls every tab.
    float visible_width = widget->measured_width;
    float max_scroll = total - visible_width;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    if (tabbar->scroll_x > max_scroll)
        tabbar->scroll_x = max_scroll;
    if (tabbar->scroll_x < 0.0f)
        tabbar->scroll_x = 0.0f;

    // The active tab can be selected while this bar belongs to a collapsed or
    // hidden container, when there is no viewport to scroll against. Reveal it
    // as soon as measurement supplies a real width; otherwise an opened panel
    // can present a tab strip that does not contain its own active tab.
    tabbar_ensure_tab_visible(tabbar, tabbar->active_tab);
}

/// @brief VTable paint: renders the bar background, clips tabs, draws each tab with background,
/// active accent bar, truncated title, modified dot, and close button.
/// @param widget Arranged TabBar base widget to render.
/// @param canvas Backend canvas used for text and primitive drawing.
static void tabbar_paint(vg_widget_t *widget, void *canvas) {
    vg_tabbar_t *tabbar = (vg_tabbar_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    vgfx_window_t win = (vgfx_window_t)canvas;

    // Draw tab bar background
    vgfx_fill_rect(win,
                   (int32_t)widget->x,
                   (int32_t)widget->y,
                   (int32_t)widget->width,
                   (int32_t)widget->height,
                   theme->colors.bg_secondary);
    if (widget->width > 2.0f) {
        vgfx_fill_rect(win,
                       (int32_t)widget->x,
                       (int32_t)widget->y,
                       (int32_t)widget->width,
                       1,
                       vg_color_lighten(theme->colors.bg_secondary, 0.08f));
        vgfx_fill_rect(win,
                       (int32_t)widget->x,
                       (int32_t)(widget->y + widget->height - 1.0f),
                       (int32_t)widget->width,
                       1,
                       theme->colors.border_primary);
    }

    float tab_x = widget->x - tabbar->scroll_x;
    vgfx_set_clip(win,
                  (int32_t)widget->x,
                  (int32_t)widget->y,
                  (int32_t)widget->width,
                  (int32_t)widget->height);

    for (vg_tab_t *tab = tabbar->first_tab; tab; tab = tab->next) {
        float width = get_tab_width(tabbar, tab);

        // Skip if not visible
        if (tab_x + width < widget->x || tab_x > widget->x + widget->width) {
            tab_x += width;
            continue;
        }

        // Determine background color
        uint32_t bg = (tab == tabbar->active_tab) ? tabbar->active_bg : tabbar->inactive_bg;
        if (tab == tabbar->hovered_tab && tab != tabbar->active_tab) {
            bg = vg_color_blend(bg, theme->colors.bg_hover, 0.55f);
        }
        uint32_t text_color =
            (tab == tabbar->active_tab) ? tabbar->text_color : theme->colors.fg_secondary;

        // Draw tab background — active tab as a raised rounded pill with an
        // accent underline; hovered tabs get a subtle rounded highlight;
        // inactive tabs are separated by a faint divider.
        if (tab == tabbar->active_tab) {
            vg_draw_round_rect_fill(win,
                                    tab_x + 2.0f,
                                    widget->y + 3.0f,
                                    width - 4.0f,
                                    widget->height - 3.0f,
                                    theme->radius.md,
                                    bg);
            vg_draw_inner_highlight_top(win,
                                        tab_x + 2.0f,
                                        widget->y + 4.0f,
                                        width - 4.0f,
                                        theme->radius.md,
                                        vg_color_lighten(bg, 0.07f));
            vg_draw_round_rect_fill(win,
                                    tab_x + 10.0f,
                                    widget->y + widget->height - 5.0f,
                                    width - 20.0f,
                                    3.0f,
                                    1.5f,
                                    theme->colors.accent_primary);
        } else {
            if (tab == tabbar->hovered_tab) {
                vg_draw_round_rect_fill(win,
                                        tab_x + 2.0f,
                                        widget->y + 4.0f,
                                        width - 4.0f,
                                        widget->height - 8.0f,
                                        theme->radius.sm,
                                        bg);
            }
            vgfx_fill_rect(win,
                           (int32_t)(tab_x + width - 1.0f),
                           (int32_t)widget->y + 6,
                           1,
                           (int32_t)widget->height - 12,
                           theme->colors.border_secondary);
        }

        // Draw tab title
        if (tabbar->font && tab->title) {
            vg_font_metrics_t font_metrics;
            vg_font_get_metrics(tabbar->font, tabbar->font_size, &font_metrics);

            float text_x = tab_x + tabbar->tab_padding;
            float text_y =
                widget->y + (widget->height + font_metrics.ascent + font_metrics.descent) / 2.0f;

            float text_max_width = width - tabbar->tab_padding * 2.0f;
            if (tab->icon_vector_id >= 0) {
                float icon_sz = tabbar->font_size + 2.0f;
                float icon_y = widget->y + (widget->height - icon_sz) / 2.0f;
                vg_icon_vector_draw(win,
                                    tab->icon_vector_id,
                                    (int32_t)(text_x + 0.5f),
                                    (int32_t)(icon_y + 0.5f),
                                    (int32_t)(icon_sz + 0.5f),
                                    text_color);
                text_x += icon_sz + 4.0f;
                text_max_width -= icon_sz + 4.0f;
            }
            if (tab->closable) {
                float close_gap =
                    (tabbar->close_button_size / TABBAR_DEFAULT_CLOSE_SIZE) * TABBAR_CLOSE_GAP;
                text_max_width -= tabbar->close_button_size + close_gap;
            }

            char *title = make_tab_title(tab);
            char *fitted_title = fit_tab_title(tabbar, title ? title : "", text_max_width);
            if (fitted_title && fitted_title[0] != '\0') {
                vg_font_draw_text(canvas,
                                  tabbar->font,
                                  tabbar->font_size,
                                  text_x,
                                  text_y,
                                  fitted_title,
                                  text_color);
            }
            free(fitted_title);
            free(title);
        }

        // Draw close button if closable
        if (tab->closable) {
            float close_x = tab_x + width - tabbar->tab_padding - tabbar->close_button_size;
            float close_y = widget->y + (widget->height - tabbar->close_button_size) / 2.0f;

            uint32_t close_color = tabbar->close_color;
            if (tab == tabbar->hovered_tab && tabbar->close_button_hovered) {
                close_color = theme->colors.accent_danger;
                vgfx_fill_rect(win,
                               (int32_t)close_x - 2,
                               (int32_t)close_y - 1,
                               (int32_t)tabbar->close_button_size + 4,
                               (int32_t)tabbar->close_button_size + 2,
                               vg_color_blend(bg, theme->colors.bg_hover, 0.65f));
            }

            // Close glyph: the shared vector icon (anti-aliased, brand-
            // consistent); fall back to two raw lines if the library is absent.
            static int32_t s_close_icon = -2;
            if (s_close_icon == -2)
                s_close_icon = vg_icon_vector_find("close");
            if (s_close_icon >= 0) {
                vg_icon_vector_draw(win,
                                    s_close_icon,
                                    (int32_t)(close_x + 0.5f),
                                    (int32_t)(close_y + 0.5f),
                                    (int32_t)(tabbar->close_button_size + 0.5f),
                                    close_color);
            } else {
                int32_t cx = (int32_t)(close_x + tabbar->close_button_size / 2.0f);
                int32_t cy = (int32_t)(close_y + tabbar->close_button_size / 2.0f);
                int32_t r = (int32_t)(tabbar->close_button_size / 2.0f) - 1;
                vgfx_line(win, cx - r, cy - r, cx + r, cy + r, close_color);
                vgfx_line(win, cx - r, cy + r, cx + r, cy - r, close_color);
            }
        }

        tab_x += width;
    }

    vgfx_clear_clip(win);
}

/// @brief VTable handle_event: handles tab click/select, close-button click, drag-to-reorder
/// gesture, hover tracking, mouse-wheel scroll, and left/right arrow key navigation.
/// @param widget TabBar base widget receiving the event.
/// @param event Mutable GUI event to interpret.
/// @return True when tab interaction consumes the event.
static bool tabbar_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_tabbar_t *tabbar = (vg_tabbar_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            float local_x = event->mouse.x;
            vg_tab_t *old_hover = tabbar->hovered_tab;
            bool old_close_hover = tabbar->close_button_hovered;

            tabbar->hovered_tab = find_tab_at_x(tabbar, local_x);

            // Check if hovering close button
            tabbar->close_button_hovered =
                tab_close_button_hit(tabbar, tabbar->hovered_tab, local_x, event->mouse.y);
            tabbar_sync_hover_tooltip(tabbar);

            if (old_hover != tabbar->hovered_tab ||
                old_close_hover != tabbar->close_button_hovered) {
                widget->needs_paint = true;
            }

            if (tabbar->drag_pending && tabbar->drag_tab) {
                float threshold =
                    TABBAR_DRAG_THRESHOLD * (theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f);
                if (fabsf(local_x - tabbar->drag_origin_x) >= threshold) {
                    tabbar->drag_pending = false;
                    tabbar->dragging = true;
                }
            }

            // Handle dragging
            if (tabbar->dragging && tabbar->drag_tab) {
                tabbar->drag_x = local_x;
                int new_index = tabbar_target_index_from_x(tabbar, local_x);
                if (tabbar_move_tab_to_index(tabbar, tabbar->drag_tab, new_index) &&
                    tabbar->on_reorder) {
                    tabbar->on_reorder(
                        widget, tabbar->drag_tab, new_index, tabbar->on_reorder_data);
                }
                widget->needs_paint = true;
                return true;
            }

            return false;
        }

        case VG_EVENT_MOUSE_LEAVE:
            tabbar->hovered_tab = NULL;
            tabbar->close_button_hovered = false;
            tabbar_sync_hover_tooltip(tabbar);
            widget->needs_paint = true;
            return false;

        case VG_EVENT_MOUSE_DOWN: {
            float local_x = event->mouse.x;
            vg_tab_t *clicked = find_tab_at_x(tabbar, local_x);

            if (clicked) {
                // Check if clicking close button
                if (tab_close_button_hit(tabbar, clicked, local_x, event->mouse.y)) {
                    tabbar->pressed_close_tab = clicked;
                    tabbar->pressed_tab = NULL;
                    vg_widget_set_input_capture(widget);
                    return true;
                }

                // Start potential drag
                tabbar->dragging = false;
                tabbar->drag_pending = true;
                tabbar->drag_tab = clicked;
                tabbar->drag_origin_x = local_x;
                tabbar->drag_x = local_x;
                tabbar->pressed_tab = clicked;
                tabbar->pressed_close_tab = NULL;
                vg_widget_set_input_capture(widget);
                widget->needs_paint = true;
                return true;
            }
            return false;
        }

        case VG_EVENT_MOUSE_UP: {
            float local_x = event->mouse.x;
            vg_tab_t *released = find_tab_at_x(tabbar, local_x);
            if (tabbar->pressed_close_tab) {
                vg_tab_t *close_tab = tabbar->pressed_close_tab;
                bool close_hit = released == close_tab &&
                                 tab_close_button_hit(tabbar, close_tab, local_x, event->mouse.y);
                tabbar->pressed_close_tab = NULL;
                tabbar->dragging = false;
                tabbar->drag_pending = false;
                tabbar->drag_tab = NULL;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                if (!close_hit)
                    return false;

                tabbar_record_close_clicked(tabbar, vg_tabbar_get_tab_index(tabbar, close_tab));
                bool allow_close = true;
                if (tabbar->on_close)
                    allow_close = tabbar->on_close(widget, close_tab, tabbar->on_close_data);
                if (allow_close && tabbar->auto_close)
                    vg_tabbar_remove_tab(tabbar, close_tab);
                return true;
            }

            if (tabbar->pressed_tab && released == tabbar->pressed_tab) {
                vg_tabbar_set_active(tabbar, tabbar->pressed_tab);
            }
            tabbar->pressed_tab = NULL;
            tabbar->dragging = false;
            tabbar->drag_pending = false;
            tabbar->drag_tab = NULL;
            if (vg_widget_get_input_capture() == widget)
                vg_widget_release_input_capture();
            return released != NULL;
        }

        case VG_EVENT_MOUSE_WHEEL:
            // Horizontal scroll with mouse wheel
            tabbar->scroll_x -= event->wheel.delta_y * 30.0f;
            if (tabbar->scroll_x < 0)
                tabbar->scroll_x = 0;
            if (tabbar->scroll_x > tabbar->total_width - widget->width) {
                tabbar->scroll_x = tabbar->total_width - widget->width;
                if (tabbar->scroll_x < 0)
                    tabbar->scroll_x = 0;
            }
            widget->needs_paint = true;
            return true;

        case VG_EVENT_KEY_DOWN: {
            if (tabbar->tab_count <= 0)
                return false;

            uint32_t mods = event->modifiers;
            bool has_ctrl = (mods & VG_MOD_CTRL) != 0 || (mods & VG_MOD_SUPER) != 0;
            bool has_shift = (mods & VG_MOD_SHIFT) != 0;
            vg_tab_t *active = tabbar->active_tab ? tabbar->active_tab : tabbar->first_tab;
            if (!active)
                return false;

            int current = vg_tabbar_get_tab_index(tabbar, active);
            if (current < 0)
                return false;

            if (has_ctrl && has_shift &&
                (event->key.key == VG_KEY_LEFT || event->key.key == VG_KEY_RIGHT)) {
                int target = current + (event->key.key == VG_KEY_LEFT ? -1 : 1);
                if (tabbar_move_tab_to_index(tabbar, active, target)) {
                    if (tabbar->on_reorder)
                        tabbar->on_reorder(widget,
                                           active,
                                           vg_tabbar_get_tab_index(tabbar, active),
                                           tabbar->on_reorder_data);
                    tabbar_ensure_tab_visible(tabbar, active);
                    widget->needs_paint = true;
                    event->handled = true;
                    return true;
                }
                return false;
            }

            if ((has_ctrl && event->key.key == VG_KEY_W) || event->key.key == VG_KEY_DELETE) {
                if (active->closable) {
                    bool allow_close = true;
                    tabbar_record_close_clicked(tabbar, current);
                    if (tabbar->on_close)
                        allow_close = tabbar->on_close(widget, active, tabbar->on_close_data);
                    if (allow_close && tabbar->auto_close)
                        vg_tabbar_remove_tab(tabbar, active);
                    event->handled = true;
                    return true;
                }
                return false;
            }

            switch (event->key.key) {
                case VG_KEY_LEFT:
                    if (current > 0)
                        vg_tabbar_set_active(tabbar, vg_tabbar_get_tab_at(tabbar, current - 1));
                    else
                        vg_tabbar_set_active(tabbar, vg_tabbar_get_tab_at(tabbar, 0));
                    tabbar_ensure_tab_visible(tabbar, tabbar->active_tab);
                    event->handled = true;
                    return true;
                case VG_KEY_RIGHT:
                    if (current < tabbar->tab_count - 1)
                        vg_tabbar_set_active(tabbar, vg_tabbar_get_tab_at(tabbar, current + 1));
                    else
                        vg_tabbar_set_active(tabbar,
                                             vg_tabbar_get_tab_at(tabbar, tabbar->tab_count - 1));
                    tabbar_ensure_tab_visible(tabbar, tabbar->active_tab);
                    event->handled = true;
                    return true;
                case VG_KEY_HOME:
                    vg_tabbar_set_active(tabbar, tabbar->first_tab);
                    tabbar_ensure_tab_visible(tabbar, tabbar->active_tab);
                    event->handled = true;
                    return true;
                case VG_KEY_END:
                    vg_tabbar_set_active(tabbar, tabbar->last_tab);
                    tabbar_ensure_tab_visible(tabbar, tabbar->active_tab);
                    event->handled = true;
                    return true;
                default:
                    break;
            }
            break;
        }

        default:
            break;
    }

    return false;
}

//=============================================================================
// TabBar API
//=============================================================================

/// @brief Append a new tab to the end of the tab bar.
///
/// @details The new tab becomes the active tab if no tab is currently active.
///
/// @param tabbar   The tab bar to add to.
/// @param title    Null-terminated display title (copied internally); NULL
///                 defaults to "Untitled".
/// @param closable true to show a close button on the tab.
/// @return Newly allocated vg_tab_t owned by the tabbar, or NULL on failure.
vg_tab_t *vg_tabbar_add_tab(vg_tabbar_t *tabbar, const char *title, bool closable) {
    if (!tabbar)
        return NULL;

    vg_tab_t *tab = calloc(1, sizeof(vg_tab_t));
    if (!tab)
        return NULL;
    tab->icon_vector_id = -1;

    char *title_copy = title ? vg_strdup(title) : vg_strdup("Untitled");
    if (!title_copy) {
        free(tab);
        return NULL;
    }
    char *tooltip_copy = vg_strdup(title_copy);
    if (!tooltip_copy) {
        free(title_copy);
        free(tab);
        return NULL;
    }

    tab->magic = VG_TAB_MAGIC;
    tab->owner = tabbar;
    tab->title = title_copy;
    tab->tooltip = tooltip_copy;
    tab->stable_id = NULL;
    tab->stable_id_len = 0;
    tab->user_data = NULL;
    tab->owns_user_data = false;
    tab->closable = closable;
    tab->modified = false;

    // Add to end of list
    if (tabbar->last_tab) {
        tabbar->last_tab->next = tab;
        tab->prev = tabbar->last_tab;
        tabbar->last_tab = tab;
    } else {
        tabbar->first_tab = tab;
        tabbar->last_tab = tab;
    }
    tabbar->tab_count++;

    // Set as active if first tab
    if (!tabbar->active_tab) {
        tabbar->active_tab = tab;
        tabbar->prev_active_tab = tab;
    }

    tabbar->base.needs_layout = true;
    tabbar->base.needs_paint = true;
    vg_widget_note_revision(&tabbar->base);

    return tab;
}

/// @brief Remove and free a tab, updating the active tab if necessary.
///
/// @details If the removed tab was active, the next sibling is activated; if
///          none exists, the previous sibling; if none exists, active_tab is
///          set to NULL.
///
/// @param tabbar The tab bar that owns the tab.
/// @param tab    The tab to remove and free.
void vg_tabbar_remove_tab(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || !vg_tab_is_live(tab) || tab->owner != tabbar)
        return;

    // Update active tab if needed
    bool active_changed = false;
    if (tabbar->active_tab == tab) {
        if (tab->next) {
            tabbar->active_tab = tab->next;
        } else if (tab->prev) {
            tabbar->active_tab = tab->prev;
        } else {
            tabbar->active_tab = NULL;
        }
        active_changed = true;
    }

    // Update hover if needed
    if (tabbar->hovered_tab == tab) {
        tabbar->hovered_tab = NULL;
        tabbar_sync_hover_tooltip(tabbar);
    }
    if (tabbar->drag_tab == tab) {
        tabbar->drag_tab = NULL;
        tabbar->dragging = false;
        if (vg_widget_get_input_capture() == &tabbar->base)
            vg_widget_release_input_capture();
    }
    if (tabbar->pressed_tab == tab)
        tabbar->pressed_tab = NULL;
    if (tabbar->pressed_close_tab == tab)
        tabbar->pressed_close_tab = NULL;

    // Remove from list
    if (tab->prev) {
        tab->prev->next = tab->next;
    } else {
        tabbar->first_tab = tab->next;
    }
    if (tab->next) {
        tab->next->prev = tab->prev;
    } else {
        tabbar->last_tab = tab->prev;
    }
    tabbar->tab_count--;

    retire_tab(tabbar, tab);

    if (active_changed) {
        if (tabbar->active_change_version < UINT64_MAX)
            tabbar->active_change_version++;
        tabbar->prev_active_tab = tabbar->active_tab;
        vg_widget_note_change(&tabbar->base);
    } else {
        vg_widget_note_revision(&tabbar->base);
    }

    tabbar->base.needs_layout = true;
    tabbar->base.needs_paint = true;
}

/// @brief Free retired tab tombstones once callers have released stale handles.
///
/// @details Removed tabs are normally retained as inert tombstones so
///          vg_tab_is_live() can reject stale handles safely. This explicit
///          pruning hook lets owners reclaim memory after they have discarded
///          all tab handles returned before the corresponding remove calls.
/// @copydetails vg_tabbar_prune_retired_tabs
void vg_tabbar_prune_retired_tabs(vg_tabbar_t *tabbar) {
    free_retired_tabs(tabbar);
}

/// @brief Unlink and free one exact retained tab record.
/// @details The function compares addresses while walking the owner's valid retirement chain and
///          therefore never dereferences a foreign caller-supplied pointer.
/// @copydetails vg_tabbar_reclaim_retired_tab
bool vg_tabbar_reclaim_retired_tab(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || !tab)
        return false;
    vg_tab_t **link = &tabbar->retired_tabs;
    while (*link) {
        vg_tab_t *candidate = *link;
        if (candidate == tab) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            free_tab(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Set the active tab and fire the on_select callback.
///
/// @details No-op if tab is already the active tab.  Scrolls the tab into view.
///
/// @param tabbar The tab bar to update.
/// @param tab    The tab to activate; must belong to this tabbar.
void vg_tabbar_set_active(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || tabbar->active_tab == tab)
        return;
    if (tab && (!vg_tab_is_live(tab) || tab->owner != tabbar))
        return;

    tabbar->active_tab = tab;
    if (tabbar->active_change_version < UINT64_MAX)
        tabbar->active_change_version++;
    tabbar->prev_active_tab = tab;
    tabbar_ensure_tab_visible(tabbar, tab);
    tabbar->base.needs_paint = true;
    vg_widget_note_change(&tabbar->base);

    if (tabbar->on_select && tab) {
        tabbar->on_select(&tabbar->base, tab, tabbar->on_select_data);
    }
}

/// @brief Return the currently active tab.
///
/// @param tabbar The tab bar to query.
/// @return Active vg_tab_t pointer, or NULL if no tab is active or tabbar is NULL.
vg_tab_t *vg_tabbar_get_active(vg_tabbar_t *tabbar) {
    return tabbar ? tabbar->active_tab : NULL;
}

/// @brief Return the zero-based index of a tab within the tab bar.
///
/// @param tabbar The tab bar to search.
/// @param tab    The tab to look up.
/// @return Zero-based index, or -1 if the tab is not found or either arg is NULL.
int vg_tabbar_get_tab_index(vg_tabbar_t *tabbar, vg_tab_t *tab) {
    if (!tabbar || !vg_tab_is_live(tab) || tab->owner != tabbar)
        return -1;
    int index = 0;
    for (vg_tab_t *t = tabbar->first_tab; t; t = t->next) {
        if (t == tab)
            return index;
        index++;
    }
    return -1;
}

/// @brief Return the tab at the given zero-based index, or NULL if out of range.
///
/// @param tabbar The tab bar to query.
/// @param index  Zero-based tab index.
/// @return Corresponding vg_tab_t pointer, or NULL.
vg_tab_t *vg_tabbar_get_tab_at(vg_tabbar_t *tabbar, int index) {
    if (!tabbar || index < 0)
        return NULL;
    int i = 0;
    for (vg_tab_t *t = tabbar->first_tab; t; t = t->next) {
        if (i == index)
            return t;
        i++;
    }
    return NULL;
}

/// @brief Set the display title of a tab.
///
/// @details The old title is freed.  If the tab had no explicit tooltip, or if
///          its tooltip was identical to the old title, the tooltip is updated to
///          match the new title.
///
/// @param tab   The tab to update.
/// @param title New null-terminated title string; NULL defaults to "Untitled".
void vg_tab_set_title(vg_tab_t *tab, const char *title) {
    if (!vg_tab_is_live(tab))
        return;

    const char *value = title ? title : "Untitled";
    if (tab->title && strcmp(tab->title, value) == 0)
        return;

    bool tooltip_tracks_title = false;
    if (!tab->tooltip) {
        tooltip_tracks_title = true;
    } else if (!tab->title) {
        tooltip_tracks_title = false;
    } else if (strcmp(tab->tooltip, tab->title) == 0) {
        tooltip_tracks_title = true;
    }

    char *new_title = vg_strdup(value);
    if (!new_title)
        return;

    char *new_tooltip = NULL;
    if (tooltip_tracks_title) {
        new_tooltip = vg_strdup(new_title);
        if (!new_tooltip) {
            free(new_title);
            return;
        }
    }

    if (tab->title)
        free(tab->title);
    tab->title = new_title;

    if (tooltip_tracks_title) {
        if (tab->tooltip)
            free(tab->tooltip);
        tab->tooltip = new_tooltip;
    }
    if (tab->owner) {
        if (tab->owner->hovered_tab == tab)
            tabbar_sync_hover_tooltip(tab->owner);
        tab->owner->base.needs_layout = true;
        tab->owner->base.needs_paint = true;
        vg_widget_note_revision(&tab->owner->base);
    }
}

/// @brief Attach or clear a leading vector icon on one live tab (ADR 0220).
/// @param tab Live tab to modify; invalid or retired records are ignored.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_tab_set_icon_vector(vg_tab_t *tab, int32_t vector_id) {
    if (!vg_tab_is_live(tab))
        return;
    if (tab->icon_vector_id == vector_id)
        return;
    tab->icon_vector_id = vector_id;
    if (tab->owner) {
        tab->owner->base.needs_layout = true;
        tab->owner->base.needs_paint = true;
        vg_widget_note_revision(&tab->owner->base);
    }
}

/// @brief Return a live tab's borrowed title or an empty sentinel.
/// @param tab Tab to inspect.
/// @return Borrowed title, or a static empty string for an invalid/missing title.
const char *vg_tab_get_title(const vg_tab_t *tab) {
    return vg_tab_is_live(tab) && tab->title ? tab->title : "";
}

/// @brief Set the modified indicator on the tab (dot shown next to the title).
///
/// @param tab      The tab to update.
/// @param modified true to show the modified dot; false to hide it.
void vg_tab_set_modified(vg_tab_t *tab, bool modified) {
    if (vg_tab_is_live(tab)) {
        if (tab->modified == modified)
            return;
        tab->modified = modified;
        if (tab->owner) {
            tab->owner->base.needs_layout = true;
            tab->owner->base.needs_paint = true;
            vg_widget_note_revision(&tab->owner->base);
        }
    }
}

/// @brief Set the hover tooltip text for a tab (independent of the title).
///
/// @param tab     The tab to update.
/// @param tooltip Null-terminated tooltip string; NULL clears the tooltip.
void vg_tab_set_tooltip(vg_tab_t *tab, const char *tooltip) {
    if (!vg_tab_is_live(tab))
        return;

    if ((!tab->tooltip && !tooltip) ||
        (tab->tooltip && tooltip && strcmp(tab->tooltip, tooltip) == 0))
        return;
    char *copy = tooltip ? vg_strdup(tooltip) : NULL;
    if (tooltip && !copy)
        return;
    if (tab->tooltip)
        free(tab->tooltip);
    tab->tooltip = copy;
    if (tab->owner) {
        if (tab->owner->hovered_tab == tab)
            tabbar_sync_hover_tooltip(tab->owner);
        tab->owner->base.needs_paint = true;
        vg_widget_note_revision(&tab->owner->base);
    }
}

/// @brief Set an opaque user-data pointer on the tab for caller use.
///
/// @param tab  The tab to update.
/// @param data Arbitrary pointer stored on the tab; not owned or freed by the tabbar.
void vg_tab_set_data(vg_tab_t *tab, void *data) {
    if (vg_tab_is_live(tab)) {
        if (tab->user_data == data)
            return;
        if (tab->owns_user_data)
            free(tab->user_data);
        tab->user_data = data;
        tab->owns_user_data = false;
        if (tab->owner)
            vg_widget_note_revision(&tab->owner->base);
    }
}

/// @brief Return a live tab's borrowed opaque data pointer.
/// @param tab Tab to inspect.
/// @return Stored borrowed pointer, or `NULL` for an invalid tab.
void *vg_tab_get_data(const vg_tab_t *tab) {
    return vg_tab_is_live(tab) ? tab->user_data : NULL;
}

/// @brief Set whether a live tab displays and accepts its close affordance.
/// @param tab Live tab to configure.
/// @param closable `true` to reserve and enable the close-button gutter.
void vg_tab_set_closable(vg_tab_t *tab, bool closable) {
    if (!vg_tab_is_live(tab) || tab->closable == closable)
        return;
    tab->closable = closable;
    tab->owner->base.needs_layout = true;
    tab->owner->base.needs_paint = true;
    vg_widget_note_revision(&tab->owner->base);
}

/// @brief Return whether a live tab is closable.
/// @param tab Tab to inspect.
/// @return `true` only for a live closable tab.
bool vg_tab_is_closable(const vg_tab_t *tab) {
    return vg_tab_is_live(tab) && tab->closable;
}

/// @brief Replace a live tab's stable identifier atomically.
/// @param tab Live tab to update.
/// @param stable_id Identifier to copy, or `NULL` to clear.
/// @return `true` after a successful replacement or identical no-op; `false`
///         for an invalid tab or allocation failure.
bool vg_tab_set_stable_id(vg_tab_t *tab, const char *stable_id) {
    if (!vg_tab_is_live(tab))
        return false;
    const char *value = stable_id ? stable_id : "";
    if ((tab->stable_id && strcmp(tab->stable_id, value) == 0) ||
        (!tab->stable_id && value[0] == '\0'))
        return true;
    char *copy = value[0] != '\0' ? vg_strdup(value) : NULL;
    if (value[0] != '\0' && !copy)
        return false;
    free(tab->stable_id);
    tab->stable_id = copy;
    tab->stable_id_len = copy ? strlen(copy) : 0;
    vg_widget_note_revision(&tab->owner->base);
    return true;
}

/// @brief Return a live tab's borrowed stable identifier or an empty sentinel.
/// @param tab Tab to inspect.
/// @return Borrowed identifier, or a static empty string when absent/invalid.
const char *vg_tab_get_stable_id(const vg_tab_t *tab) {
    return vg_tab_is_live(tab) && tab->stable_id ? tab->stable_id : "";
}

/// @brief Set the font and size used to render all tab titles.
///
/// @param tabbar The tab bar to configure.
/// @param font   Font to use; may be NULL.
/// @param size   Font size in logical pixels; values ≤ 0 default to the theme's
///               normal text size.
void vg_tabbar_set_font(vg_tabbar_t *tabbar, vg_font_t *font, float size) {
    if (!tabbar)
        return;

    float effective_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    if (tabbar->font == font && tabbar->font_size == effective_size)
        return;
    tabbar->font = font;
    tabbar->font_size = effective_size;
    tabbar->base.needs_layout = true;
    tabbar->base.needs_paint = true;
    vg_widget_note_revision(&tabbar->base);
}

/// @brief Move one indexed tab and notify the callback after committing the new order.
/// @param tabbar Tab bar to reorder.
/// @param from_index Existing zero-based source index.
/// @param to_index Zero-based destination index.
/// @return `true` when order changed and callbacks were notified.
bool vg_tabbar_move_tab(vg_tabbar_t *tabbar, int from_index, int to_index) {
    if (!tabbar || from_index < 0 || to_index < 0 || from_index >= tabbar->tab_count ||
        to_index >= tabbar->tab_count)
        return false;
    vg_tab_t *tab = vg_tabbar_get_tab_at(tabbar, from_index);
    if (!tabbar_move_tab_to_index(tabbar, tab, to_index))
        return false;
    if (tabbar->on_reorder)
        tabbar->on_reorder(&tabbar->base, tab, tabbar->reordered_to, tabbar->on_reorder_data);
    tabbar_ensure_tab_visible(tabbar, tab);
    return true;
}

/// @brief Consume the independent reorder edge without clearing its index payload.
/// @param tabbar Tab bar to query.
/// @return `true` once after one or more unreported successful reorders.
bool vg_tabbar_was_reordered(vg_tabbar_t *tabbar) {
    if (!tabbar || tabbar->reported_reorder_version == tabbar->reorder_version)
        return false;
    tabbar->reported_reorder_version = tabbar->reorder_version;
    return true;
}

/// @brief Return the source index from the latest successful reorder.
/// @param tabbar Tab bar to inspect.
/// @return Previous source index, or -1 for `NULL`.
int vg_tabbar_get_reordered_from(const vg_tabbar_t *tabbar) {
    return tabbar ? tabbar->reordered_from : -1;
}

/// @brief Return the destination index from the latest successful reorder.
/// @param tabbar Tab bar to inspect.
/// @return Latest destination index, or -1 for `NULL`.
int vg_tabbar_get_reordered_to(const vg_tabbar_t *tabbar) {
    return tabbar ? tabbar->reordered_to : -1;
}

/// @brief Register a callback invoked when the active tab changes.
///
/// @param tabbar    The tab bar to configure.
/// @param callback  Function called with (widget, tab, user_data).  May be NULL.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_tabbar_set_on_select(vg_tabbar_t *tabbar,
                             vg_tab_select_callback_t callback,
                             void *user_data) {
    if (!tabbar)
        return;
    tabbar->on_select = callback;
    tabbar->on_select_data = user_data;
}

/// @brief Register a callback invoked when a tab's close button is clicked.
///
/// @details The callback is responsible for actually removing the tab if desired.
///
/// @param tabbar    The tab bar to configure.
/// @param callback  Function called with (widget, tab, user_data).  May be NULL.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_tabbar_set_on_close(vg_tabbar_t *tabbar,
                            vg_tab_close_callback_t callback,
                            void *user_data) {
    if (!tabbar)
        return;
    tabbar->on_close = callback;
    tabbar->on_close_data = user_data;
}

/// @brief Register a callback invoked after a drag-to-reorder operation completes.
///
/// @param tabbar    The tab bar to configure.
/// @param callback  Function called with (widget, tab, new_index, user_data).
///                  May be NULL.
/// @param user_data Opaque pointer forwarded unchanged to the callback.
void vg_tabbar_set_on_reorder(vg_tabbar_t *tabbar,
                              vg_tab_reorder_callback_t callback,
                              void *user_data) {
    if (!tabbar)
        return;
    tabbar->on_reorder = callback;
    tabbar->on_reorder_data = user_data;
}
