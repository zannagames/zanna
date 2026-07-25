//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_contextmenu.c
/// @brief Implements nested context menus and widget right-click registration.
///
/// @details Context menus support text, shortcuts, checkmarks, raster, vector,
/// and glyph icons, separators, owned submenus, pointer interaction, and
/// keyboard navigation. A process-local registry maps live widgets to menus and
/// purges stale entries during traversal using swap-with-last removal.
///
/// Each menu owns its item pointer array and every active or retired item.
/// Menu items own their copied strings, icons, and attached submenus. Parent and
/// child links are detached during destruction to avoid stale references.
/// Dismissing a submenu transfers input capture back to a visible parent.
/// Window-edge clamping occurs during paint because that is where the window
/// dimensions are available and can reflect live resize.
///
/// @see vg_ide_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void contextmenu_destroy(vg_widget_t *widget);
static void contextmenu_measure(vg_widget_t *widget, float available_width, float available_height);
static void contextmenu_paint(vg_widget_t *widget, void *canvas);
static bool contextmenu_handle_event(vg_widget_t *widget, vg_event_t *event);

//=============================================================================
// ContextMenu VTable
//=============================================================================

static vg_widget_vtable_t g_contextmenu_vtable = {.destroy = contextmenu_destroy,
                                                  .measure = contextmenu_measure,
                                                  .arrange = NULL,
                                                  .paint = contextmenu_paint,
                                                  .handle_event = contextmenu_handle_event,
                                                  .can_focus = NULL,
                                                  .on_focus = NULL};

//=============================================================================
// Right-click Registry
//=============================================================================

typedef struct {
    vg_widget_t *widget;
    vg_contextmenu_t *menu;
} contextmenu_registry_entry_t;

static contextmenu_registry_entry_t *s_registry = NULL;
static size_t s_registry_count = 0;
static size_t s_registry_cap = 0;

/// @brief Ensures the global widget-to-menu registry can store an entry count.
///
/// @param needed Required number of registry slots.
/// @return `true` when capacity is sufficient; `false` on overflow or
///         allocation failure.
static bool contextmenu_ensure_registry_capacity(size_t needed) {
    if (needed <= s_registry_cap)
        return true;
    size_t new_cap = s_registry_cap ? s_registry_cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(*s_registry))
        return false;
    contextmenu_registry_entry_t *entries = realloc(s_registry, new_cap * sizeof(*s_registry));
    if (!entries)
        return false;
    s_registry = entries;
    s_registry_cap = new_cap;
    return true;
}

/// @brief Reports whether a context-menu pointer identifies a live menu widget.
///
/// @param menu Menu to validate.
/// @return `true` when @p menu is non-null, live, and has menu widget type.
bool vg_contextmenu_is_live(const vg_contextmenu_t *menu) {
    return menu && vg_widget_is_live(&menu->base) && menu->base.type == VG_WIDGET_MENU;
}

/// @brief Removes registry entries for a menu and any stale widget entries.
///
/// @param menu Menu whose registrations are removed; may be null.
static void contextmenu_unregister_menu(vg_contextmenu_t *menu) {
    if (!menu)
        return;
    for (size_t i = 0; i < s_registry_count;) {
        if (s_registry[i].menu == menu || !vg_widget_is_live(s_registry[i].widget)) {
            s_registry[i] = s_registry[--s_registry_count];
        } else {
            i++;
        }
    }
}

//=============================================================================
// Constants
//=============================================================================

#define ITEM_HEIGHT 28.0f
#define ITEM_PADDING_X 12.0f
#define ITEM_PADDING_Y 4.0f
#define SEPARATOR_HEIGHT 9.0f
#define SUBMENU_ARROW_WIDTH 20.0f
#define SHORTCUT_GAP 30.0f
#define SUBMENU_DELAY_MS 200
#define ICON_SLOT_WIDTH 22.0f
#define ICON_TEXT_GAP 8.0f
#define ICON_DRAW_SIZE 16.0f
#define VG_MENU_ITEM_MAGIC UINT64_C(0x56474D454E554954)
#define VG_MENU_ITEM_RETIRED_MAGIC UINT64_C(0x56474D454E555258)

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Allocates and initializes a regular context-menu item.
///
/// @details Label and shortcut strings are copied before the item is allocated.
/// Partial allocation failure releases all temporary ownership.
///
/// @param label Display label to copy; may be null.
/// @param shortcut Shortcut hint to copy; may be null.
/// @param action Callback invoked when the item is activated; may be null.
/// @param user_data Opaque pointer forwarded to @p action.
/// @return Newly allocated item, or null on allocation failure.
static vg_menu_item_t *create_menu_item(const char *label,
                                        const char *shortcut,
                                        void (*action)(void *),
                                        void *user_data) {
    char *label_copy = label ? vg_strdup(label) : NULL;
    char *shortcut_copy = shortcut ? vg_strdup(shortcut) : NULL;
    if ((label && !label_copy) || (shortcut && !shortcut_copy)) {
        free(label_copy);
        free(shortcut_copy);
        return NULL;
    }

    vg_menu_item_t *item = calloc(1, sizeof(vg_menu_item_t));
    if (!item) {
        free(label_copy);
        free(shortcut_copy);
        return NULL;
    }

    item->magic = VG_MENU_ITEM_MAGIC;
    item->text = label_copy;
    item->shortcut = shortcut_copy;
    item->action = action;
    item->action_data = user_data;
    item->enabled = true;
    item->checked = false;
    item->checkable = false;
    item->separator = false;
    item->icon.type = VG_ICON_NONE;
    item->submenu = NULL;

    return item;
}

/// @brief Releases a menu item and everything it owns.
///
/// @details An attached submenu is detached and destroyed before the item's
/// copied strings, icon resource, and structure are released.
///
/// @param item Item to destroy; may be null.
static void free_menu_item(vg_menu_item_t *item) {
    if (item) {
        if (item->submenu) {
            vg_contextmenu_t *submenu = (vg_contextmenu_t *)item->submenu;
            item->submenu = NULL;
            if (submenu->parent_item == item) {
                submenu->parent_item = NULL;
                submenu->parent_menu = NULL;
            }
            vg_contextmenu_destroy(submenu);
        }
        free(item->text);
        free(item->shortcut);
        vg_icon_destroy(&item->icon);
        free(item);
    }
}

/// @brief Ensures a menu's item-pointer array can hold a requested count.
///
/// @details Capacity grows geometrically from eight entries. Overflow and
/// allocation failure leave the existing array unchanged.
///
/// @param menu Menu whose owned pointer array may grow.
/// @param needed Required number of item slots.
/// @return `true` when capacity is sufficient; otherwise `false`.
static bool contextmenu_ensure_item_capacity(vg_contextmenu_t *menu, size_t needed) {
    if (!menu)
        return false;
    if (needed <= menu->item_capacity)
        return true;
    size_t new_cap = menu->item_capacity ? menu->item_capacity : 8;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(vg_menu_item_t *))
        return false;
    vg_menu_item_t **new_items = realloc(menu->items, new_cap * sizeof(vg_menu_item_t *));
    if (!new_items)
        return false;
    menu->items = new_items;
    menu->item_capacity = new_cap;
    return true;
}

/// @brief Retires an item while preserving an inert handle record.
///
/// @details Owned resources and any submenu are released, behavior and owner
/// links are cleared, and the structure is moved to the menu's retired list.
/// The retired magic value allows stale external handles to fail safely.
///
/// @param menu Menu that retains the retired record.
/// @param item Active item to retire.
static void retire_menu_item(vg_contextmenu_t *menu, vg_menu_item_t *item) {
    if (!menu || !item)
        return;
    if (item->submenu) {
        vg_contextmenu_t *submenu = (vg_contextmenu_t *)item->submenu;
        item->submenu = NULL;
        if (submenu->parent_item == item) {
            submenu->parent_item = NULL;
            submenu->parent_menu = NULL;
        }
        vg_contextmenu_destroy(submenu);
    }
    free(item->text);
    item->text = NULL;
    free(item->shortcut);
    item->shortcut = NULL;
    vg_icon_destroy(&item->icon);
    item->action = NULL;
    item->action_data = NULL;
    item->owner_contextmenu = NULL;
    item->parent_menu = NULL;
    item->next = NULL;
    item->prev = NULL;
    item->magic = VG_MENU_ITEM_RETIRED_MAGIC;
    item->retired_next = menu->retired_items;
    menu->retired_items = item;
}

/// @brief Reports whether an item requires the leading icon/check column.
///
/// @param item Item to inspect.
/// @return `true` for a non-separator that is checkable, checked, or has an icon.
static bool item_uses_leading_column(const vg_menu_item_t *item) {
    return item && !item->separator &&
           (item->checkable || item->checked || item->icon.type != VG_ICON_NONE);
}

/// @brief Reports whether any item requires a shared leading column.
///
/// @param menu Menu to inspect.
/// @return `true` when at least one active item uses the column.
static bool menu_uses_leading_column(const vg_contextmenu_t *menu) {
    if (!menu)
        return false;
    for (size_t i = 0; i < menu->item_count; i++) {
        if (item_uses_leading_column(menu->items[i]))
            return true;
    }
    return false;
}

/// @brief Clamps an integer to an inclusive range.
///
/// @param value Value to clamp.
/// @param min_value Inclusive lower bound.
/// @param max_value Inclusive upper bound.
/// @return Clamped value.
static int contextmenu_clampi(int value, int min_value, int max_value) {
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Alpha-blends one RGBA source pixel over a destination pixel.
///
/// @param[in,out] dst Destination RGBA pixel updated in place.
/// @param src Source RGBA pixel whose color channels are blended.
/// @param alpha Effective source opacity in the range `[0, 255]`.
static void contextmenu_blend_pixel(uint8_t *dst, const uint8_t *src, uint8_t alpha) {
    if (alpha == 0)
        return;

    if (alpha == 255) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = 255;
        return;
    }

    const uint32_t inv = (uint32_t)(255 - alpha);
    dst[0] = (uint8_t)(((uint32_t)src[0] * alpha + (uint32_t)dst[0] * inv + 127u) / 255u);
    dst[1] = (uint8_t)(((uint32_t)src[1] * alpha + (uint32_t)dst[1] * inv + 127u) / 255u);
    dst[2] = (uint8_t)(((uint32_t)src[2] * alpha + (uint32_t)dst[2] * inv + 127u) / 255u);
    dst[3] = (uint8_t)(alpha + (((uint32_t)dst[3] * inv + 127u) / 255u));
}

/// @brief Draws a raster icon into a menu slot with nearest-neighbor scaling.
///
/// @details The image is aspect-fitted and centered in the requested box,
/// clipped to both framebuffer and canvas bounds, and alpha-blended directly
/// into the framebuffer. Disabled items reduce source opacity.
///
/// @param win Destination window framebuffer.
/// @param icon Raster icon to render.
/// @param x Left edge of the available icon box.
/// @param y Top edge of the available icon box.
/// @param w Available width.
/// @param h Available height.
/// @param enabled Whether to use full opacity rather than disabled attenuation.
static void contextmenu_draw_image_icon(
    vgfx_window_t win, const vg_icon_t *icon, float x, float y, float w, float h, bool enabled) {
    if (!icon || icon->type != VG_ICON_IMAGE || !icon->data.image.pixels || w <= 0.0f || h <= 0.0f)
        return;

    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(win, &fb))
        return;

    const int sw = (int)icon->data.image.width;
    const int sh = (int)icon->data.image.height;
    if (sw <= 0 || sh <= 0)
        return;

    float draw_w = w;
    float draw_h = h;
    float scale = w / (float)sw;
    if ((float)sh * scale > h)
        scale = h / (float)sh;
    if (scale <= 0.0f)
        return;

    draw_w = (float)sw * scale;
    draw_h = (float)sh * scale;
    float draw_x = x + (w - draw_w) * 0.5f;
    float draw_y = y + (h - draw_h) * 0.5f;

    int start_x = contextmenu_clampi((int)draw_x, 0, fb.width);
    int start_y = contextmenu_clampi((int)draw_y, 0, fb.height);
    int end_x = contextmenu_clampi((int)(draw_x + draw_w), 0, fb.width);
    int end_y = contextmenu_clampi((int)(draw_y + draw_h), 0, fb.height);
    int clip_x = 0;
    int clip_y = 0;
    int clip_w = fb.width;
    int clip_h = fb.height;

    (void)vgfx_get_clip(win, &clip_x, &clip_y, &clip_w, &clip_h);
    if (start_x < clip_x)
        start_x = clip_x;
    if (start_y < clip_y)
        start_y = clip_y;
    if (end_x > clip_x + clip_w)
        end_x = clip_x + clip_w;
    if (end_y > clip_y + clip_h)
        end_y = clip_y + clip_h;

    for (int fb_y = start_y; fb_y < end_y; fb_y++) {
        for (int fb_x = start_x; fb_x < end_x; fb_x++) {
            float u = draw_w > 0.0f ? ((float)fb_x - draw_x) / draw_w : 0.0f;
            float v = draw_h > 0.0f ? ((float)fb_y - draw_y) / draw_h : 0.0f;
            int sx = contextmenu_clampi((int)(u * (float)sw), 0, sw - 1);
            int sy = contextmenu_clampi((int)(v * (float)sh), 0, sh - 1);
            const uint8_t *src = &icon->data.image.pixels[(sy * sw + sx) * 4];
            uint8_t alpha = src[3];
            if (!enabled)
                alpha = (uint8_t)(((uint32_t)alpha * 144u + 127u) / 255u);
            if (alpha == 0)
                continue;

            uint8_t *dst = &fb.pixels[fb_y * fb.stride + fb_x * 4];
            contextmenu_blend_pixel(dst, src, alpha);
        }
    }
}

/// @brief Encodes a Unicode scalar as a null-terminated UTF-8 sequence.
///
/// @param cp Unicode scalar to encode.
/// @param[out] out Five-byte output buffer, including room for the terminator.
/// @return `true` on success; `false` for a null output, surrogate, or
///         out-of-range code point.
static bool contextmenu_encode_utf8(uint32_t cp, char out[5]) {
    if (!out)
        return false;
    memset(out, 0, 5);
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return false;
    if (cp < 0x80) {
        out[0] = (char)cp;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
    } else {
        return false;
    }
    return true;
}

/// @brief Draws one Unicode glyph centered in a bounding rectangle.
///
/// @param canvas Destination drawing context.
/// @param font Font used for measurement and rendering; null suppresses drawing.
/// @param font_size Font size used for measurement and rendering.
/// @param codepoint Unicode scalar to encode and draw.
/// @param x Left edge of the bounding rectangle.
/// @param y Top edge of the bounding rectangle.
/// @param w Width of the bounding rectangle.
/// @param h Height of the bounding rectangle.
/// @param color Packed glyph color.
static void contextmenu_draw_glyph(void *canvas,
                                   vg_font_t *font,
                                   float font_size,
                                   uint32_t codepoint,
                                   float x,
                                   float y,
                                   float w,
                                   float h,
                                   uint32_t color) {
    if (!font)
        return;

    char buf[5];
    if (!contextmenu_encode_utf8(codepoint, buf))
        return;

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(font, font_size, &font_metrics);
    vg_text_metrics_t text_metrics = {0};
    vg_font_measure_text(font, font_size, buf, &text_metrics);

    float glyph_x = x + (w - text_metrics.width) * 0.5f;
    float glyph_y = y + (h + font_metrics.ascent + font_metrics.descent) * 0.5f;
    vg_font_draw_text(canvas, font, font_size, glyph_x, glyph_y, buf, color);
}

/// @brief Invalidates an item's owning menu after item mutation.
///
/// @details Visible menus are synchronously remeasured when geometry may have
/// changed. Layout and paint flags are then set as appropriate.
///
/// @param item Item whose owning menu is invalidated.
/// @param layout_changed Whether the mutation can change menu dimensions.
static void contextmenu_mark_item_changed(vg_menu_item_t *item, bool layout_changed) {
    if (!item || !item->owner_contextmenu)
        return;

    vg_contextmenu_t *menu = item->owner_contextmenu;
    if (layout_changed && menu->is_visible) {
        contextmenu_measure(&menu->base, 0, 0);
        menu->base.width = menu->base.measured_width;
        menu->base.height = menu->base.measured_height;
    }
    if (layout_changed)
        menu->base.needs_layout = true;
    menu->base.needs_paint = true;
}

/// @brief Returns the fixed row height for an item.
///
/// @param item Item whose separator state is inspected.
/// @return Separator height for separators; regular item height otherwise.
static float get_item_height(vg_menu_item_t *item) {
    return item->separator ? SEPARATOR_HEIGHT : ITEM_HEIGHT;
}

/// @brief Calculates the menu height from item rows and vertical padding.
///
/// @param menu Menu to measure.
/// @return Unclamped content height.
static float calculate_menu_height(vg_contextmenu_t *menu) {
    float height = ITEM_PADDING_Y * 2; // Top and bottom padding
    for (size_t i = 0; i < menu->item_count; i++) {
        height += get_item_height(menu->items[i]);
    }
    return height;
}

/// @brief Calculates the width required by the widest menu item.
///
/// @details Measurement accounts for shared leading-column space, label and
/// shortcut metrics, submenu arrows, horizontal padding, and the minimum width.
///
/// @param menu Menu to measure.
/// @return Required menu width.
static float calculate_menu_width(vg_contextmenu_t *menu) {
    float max_width = (float)menu->min_width;
    vg_font_t *font = menu->font;
    float font_size = menu->font_size;
    bool has_leading_column = menu_uses_leading_column(menu);

    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item->separator)
            continue;

        float width = ITEM_PADDING_X * 2;
        if (has_leading_column)
            width += ICON_SLOT_WIDTH + ICON_TEXT_GAP;

        if (font && item->text) {
            vg_text_metrics_t metrics;
            vg_font_measure_text(font, font_size, item->text, &metrics);
            width += metrics.width;
        }

        if (font && item->shortcut) {
            vg_text_metrics_t metrics;
            vg_font_measure_text(font, font_size, item->shortcut, &metrics);
            width += SHORTCUT_GAP + metrics.width;
        }

        if (item->submenu) {
            width += SUBMENU_ARROW_WIDTH;
        }

        if (width > max_width)
            max_width = width;
    }

    return max_width;
}

/// @brief Finds the menu item at a local vertical coordinate.
///
/// @param menu Menu whose rows are hit-tested.
/// @param y Vertical coordinate relative to the menu.
/// @return Zero-based item index, or `-1` outside all rows.
static int get_item_at_y(vg_contextmenu_t *menu, float y) {
    float current_y = ITEM_PADDING_Y;
    for (size_t i = 0; i < menu->item_count; i++) {
        float item_height = get_item_height(menu->items[i]);
        if (y >= current_y && y < current_y + item_height) {
            return (int)i;
        }
        current_y += item_height;
    }
    return -1;
}

//=============================================================================
// ContextMenu Implementation
//=============================================================================

/// @brief Creates an initially hidden context menu with theme-derived colors.
///
/// @details The item arrays and parent links begin empty, hover and click
/// indices begin at `-1`, and the default minimum width and maximum height are
/// 150 and 400 logical pixels respectively.
///
/// @return Heap-allocated context menu, or null on allocation failure.
vg_contextmenu_t *vg_contextmenu_create(void) {
    vg_contextmenu_t *menu = calloc(1, sizeof(vg_contextmenu_t));
    if (!menu)
        return NULL;

    // Initialize base widget
    vg_widget_init(&menu->base, VG_WIDGET_MENU, &g_contextmenu_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Initialize context menu fields
    menu->items = NULL;
    menu->item_count = 0;
    menu->item_capacity = 0;

    menu->anchor_x = 0;
    menu->anchor_y = 0;

    menu->is_visible = false;
    menu->hovered_index = -1;
    menu->clicked_index = -1;
    menu->active_submenu = NULL;
    menu->parent_menu = NULL;
    menu->parent_item = NULL;

    menu->min_width = 150;
    menu->max_height = 400;

    menu->font = NULL;
    menu->font_size = theme->typography.size_normal;

    menu->bg_color = theme->colors.bg_primary;
    menu->hover_color = theme->colors.bg_hover;
    menu->text_color = theme->colors.fg_primary;
    menu->disabled_color = theme->colors.fg_secondary;
    menu->border_color = theme->colors.border_primary;
    menu->separator_color = theme->colors.border_secondary;

    menu->user_data = NULL;
    menu->on_select_data = NULL;
    menu->on_dismiss_data = NULL;
    menu->on_select = NULL;
    menu->on_dismiss = NULL;

    return menu;
}

/// @brief Releases all resources and relationships owned by a context menu.
///
/// @details Destruction unregisters the menu, releases input capture when held,
/// detaches parent and submenu links, destroys active items, and finally frees
/// every inert retired-item record.
///
/// @param widget Menu base widget being destroyed.
static void contextmenu_destroy(vg_widget_t *widget) {
    vg_contextmenu_t *menu = (vg_contextmenu_t *)widget;
    contextmenu_unregister_menu(menu);
    if (vg_widget_get_input_capture() == &menu->base)
        vg_widget_release_input_capture();

    vg_contextmenu_t *parent =
        menu->parent_item ? menu->parent_item->owner_contextmenu : menu->parent_menu;
    if (parent && parent->active_submenu == menu)
        parent->active_submenu = NULL;
    if (menu->parent_item && menu->parent_item->submenu == (struct vg_menu *)menu)
        menu->parent_item->submenu = NULL;
    menu->parent_item = NULL;
    menu->parent_menu = NULL;

    // Free all items
    for (size_t i = 0; i < menu->item_count; i++) {
        free_menu_item(menu->items[i]);
    }
    free(menu->items);
    vg_menu_item_t *retired = menu->retired_items;
    while (retired) {
        vg_menu_item_t *next = retired->retired_next;
        retired->retired_next = NULL;
        free_menu_item(retired);
        retired = next;
    }
    menu->retired_items = NULL;
}

/// @brief Destroys a context menu and its owned item hierarchy.
///
/// @param menu Menu to destroy; may be null.
void vg_contextmenu_destroy(vg_contextmenu_t *menu) {
    if (!menu)
        return;

    vg_widget_destroy(&menu->base);
}

/// @brief Measures menu content and applies the configured height cap.
///
/// @param widget Menu base widget whose measured dimensions are updated.
/// @param available_width Width offered by the parent; currently unused.
/// @param available_height Height offered by the parent; currently unused.
static void contextmenu_measure(vg_widget_t *widget,
                                float available_width,
                                float available_height) {
    vg_contextmenu_t *menu = (vg_contextmenu_t *)widget;
    (void)available_width;
    (void)available_height;

    widget->measured_width = calculate_menu_width(menu);
    widget->measured_height = calculate_menu_height(menu);

    // Apply max height
    if (menu->max_height > 0 && widget->measured_height > menu->max_height) {
        widget->measured_height = (float)menu->max_height;
    }
}

/// @brief Paints the menu panel and all of its active items.
///
/// @details Each paint clamps the panel to current window bounds, draws an
/// elevated rounded surface, and renders separators, hover backgrounds,
/// checkmarks or icons, labels, shortcuts, and submenu arrows. Disabled items
/// use the menu's secondary text color.
///
/// @param widget Menu base widget to render.
/// @param canvas Destination drawing context and window handle.
static void contextmenu_paint(vg_widget_t *widget, void *canvas) {
    vg_contextmenu_t *menu = (vg_contextmenu_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    if (!menu->is_visible)
        return;

    vgfx_window_t win = (vgfx_window_t)canvas;

    // Clamp menu position to keep it on-screen. show_at runs without a window
    // pointer, so we resolve and apply the clamp here on every paint — it also
    // adjusts correctly if the window is resized while the menu is open.
    int32_t win_w = 0, win_h = 0;
    if (win && vgfx_get_size(win, &win_w, &win_h)) {
        float mw = widget->width;
        float mh = widget->height;
        if (widget->x + mw > (float)win_w)
            widget->x = (float)win_w - mw;
        if (widget->y + mh > (float)win_h)
            widget->y = (float)win_h - mh;
        if (widget->x < 0.0f)
            widget->x = 0.0f;
        if (widget->y < 0.0f)
            widget->y = 0.0f;
    }

    float x = widget->x;
    float y = widget->y;
    float w = widget->width;
    float h = widget->height;

    // Real soft drop shadow + anti-aliased rounded panel (Refined Depth).
    float crad = theme->radius.md;
    vg_elevation_t el = theme->elevation.level2;
    vg_draw_round_rect_shadow(
        win, x, y, w, h, crad, el.blur, el.dx, el.dy, el.alpha, theme->elevation.shadow_rgb);
    vg_draw_round_rect_fill(win, x, y, w, h, crad, menu->bg_color);
    vg_draw_round_rect_stroke(win, x, y, w, h, crad, 1.0f, menu->border_color);

    // Draw items
    bool has_leading_column = menu_uses_leading_column(menu);
    float item_y = y + ITEM_PADDING_Y;
    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        float item_height = get_item_height(item);

        if (item->separator) {
            // Draw separator line
            float sep_y = item_y + item_height / 2;
            vgfx_fill_rect(
                win, (int32_t)(x + 4), (int32_t)sep_y, (int32_t)(w - 8), 1, menu->separator_color);
        } else {
            // Draw hover background
            if ((int)i == menu->hovered_index && item->enabled) {
                vgfx_fill_rect(win,
                               (int32_t)x,
                               (int32_t)item_y,
                               (int32_t)w,
                               (int32_t)item_height,
                               menu->hover_color);
            }

            uint32_t text_color = item->enabled ? menu->text_color : menu->disabled_color;
            float text_x = x + ITEM_PADDING_X;
            if (has_leading_column) {
                float icon_slot_x = x + ITEM_PADDING_X;
                if (item->checked) {
                    contextmenu_draw_glyph(canvas,
                                           menu->font,
                                           menu->font_size,
                                           0x2713u,
                                           icon_slot_x,
                                           item_y,
                                           ICON_SLOT_WIDTH,
                                           item_height,
                                           text_color);
                } else if (item->icon.type == VG_ICON_GLYPH) {
                    contextmenu_draw_glyph(canvas,
                                           menu->font,
                                           menu->font_size,
                                           item->icon.data.glyph,
                                           icon_slot_x,
                                           item_y,
                                           ICON_SLOT_WIDTH,
                                           item_height,
                                           text_color);
                } else if (item->icon.type == VG_ICON_IMAGE) {
                    float icon_x = icon_slot_x + (ICON_SLOT_WIDTH - ICON_DRAW_SIZE) * 0.5f;
                    float icon_y = item_y + (item_height - ICON_DRAW_SIZE) * 0.5f;
                    contextmenu_draw_image_icon(win,
                                                &item->icon,
                                                icon_x,
                                                icon_y,
                                                ICON_DRAW_SIZE,
                                                ICON_DRAW_SIZE,
                                                item->enabled);
                } else if (item->icon.type == VG_ICON_VECTOR) {
                    float icon_x = icon_slot_x + (ICON_SLOT_WIDTH - ICON_DRAW_SIZE) * 0.5f;
                    float icon_y = item_y + (item_height - ICON_DRAW_SIZE) * 0.5f;
                    vg_icon_vector_draw(win,
                                        item->icon.data.vector_id,
                                        (int32_t)(icon_x + 0.5f),
                                        (int32_t)(icon_y + 0.5f),
                                        (int32_t)(ICON_DRAW_SIZE + 0.5f),
                                        text_color);
                }
                text_x += ICON_SLOT_WIDTH + ICON_TEXT_GAP;
            }

            if (menu->font) {
                vg_font_metrics_t font_metrics;
                vg_font_get_metrics(menu->font, menu->font_size, &font_metrics);
                float text_y =
                    item_y + (item_height + font_metrics.ascent + font_metrics.descent) / 2;

                // Draw label
                if (item->text) {
                    vg_font_draw_text(canvas,
                                      menu->font,
                                      menu->font_size,
                                      text_x,
                                      text_y,
                                      item->text,
                                      text_color);
                }

                // Draw shortcut
                if (item->shortcut) {
                    vg_text_metrics_t shortcut_metrics;
                    vg_font_measure_text(
                        menu->font, menu->font_size, item->shortcut, &shortcut_metrics);
                    float shortcut_x = x + w - ITEM_PADDING_X - shortcut_metrics.width;
                    if (item->submenu)
                        shortcut_x -= SUBMENU_ARROW_WIDTH;

                    vg_font_draw_text(canvas,
                                      menu->font,
                                      menu->font_size,
                                      shortcut_x,
                                      text_y,
                                      item->shortcut,
                                      menu->disabled_color);
                }

                // Draw submenu arrow
                if (item->submenu) {
                    float arrow_x = x + w - ITEM_PADDING_X - 10;
                    vg_font_draw_text(
                        canvas, menu->font, menu->font_size, arrow_x, text_y, ">", text_color);
                }
            }
        }

        item_y += item_height;
    }
}

/// @brief Handles pointer and keyboard interaction for a visible menu.
///
/// @details Pointer movement updates hover and opens enabled submenus. Pointer
/// activation executes regular items or dismisses the root menu when outside.
/// Arrow keys navigate and enter submenus, Enter activates, and Escape dismisses
/// the full menu chain.
///
/// @param widget Menu base widget receiving the event.
/// @param event Event to inspect.
/// @return `true` when the visible menu consumes the event; otherwise `false`.
static bool contextmenu_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_contextmenu_t *menu = (vg_contextmenu_t *)widget;

    if (!menu->is_visible)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE: {
            float local_x = event->mouse.x;
            float local_y = event->mouse.y;

            // Check if inside menu
            if (local_x >= 0 && local_x < widget->width && local_y >= 0 &&
                local_y < widget->height) {
                int new_hover = get_item_at_y(menu, local_y);
                if (new_hover != menu->hovered_index) {
                    menu->hovered_index = new_hover;
                    widget->needs_paint = true;

                    // Close active submenu when moving to different item
                    if (menu->active_submenu) {
                        vg_contextmenu_dismiss(menu->active_submenu);
                        menu->active_submenu = NULL;
                    }

                    // Open submenu if hovering over submenu item
                    if (new_hover >= 0 && (size_t)new_hover < menu->item_count) {
                        vg_menu_item_t *item = menu->items[new_hover];
                        if (item->submenu && item->enabled) {
                            // Calculate submenu position
                            float item_y = ITEM_PADDING_Y;
                            for (int j = 0; j < new_hover; j++) {
                                item_y += get_item_height(menu->items[j]);
                            }
                            vg_contextmenu_t *submenu = (vg_contextmenu_t *)item->submenu;
                            vg_contextmenu_show_at(submenu,
                                                   (int)(widget->x + widget->width),
                                                   (int)(widget->y + item_y));
                            submenu->parent_menu = menu;
                            menu->active_submenu = submenu;
                        }
                    }
                }
                return true;
            } else {
                if (menu->hovered_index != -1) {
                    menu->hovered_index = -1;
                    widget->needs_paint = true;
                }
            }
            return false;
        }

        case VG_EVENT_MOUSE_DOWN: {
            float local_x = event->mouse.x;
            float local_y = event->mouse.y;

            // Check if inside menu
            if (local_x >= 0 && local_x < widget->width && local_y >= 0 &&
                local_y < widget->height) {
                int clicked = get_item_at_y(menu, local_y);
                if (clicked >= 0 && (size_t)clicked < menu->item_count) {
                    vg_menu_item_t *item = menu->items[clicked];

                    if (!item->separator && item->enabled && !item->submenu) {
                        // Record clicked index for rt_contextmenu_get_clicked_item
                        menu->clicked_index = clicked;
                        item->was_clicked = true;

                        // Invoke action
                        if (item->action) {
                            item->action(item->action_data);
                        }

                        // Invoke on_select callback
                        if (menu->on_select)
                            menu->on_select(menu, item, menu->on_select_data);

                        // Dismiss entire menu chain
                        vg_contextmenu_t *root = menu;
                        while (root->parent_menu) {
                            root = root->parent_menu;
                        }
                        vg_contextmenu_dismiss(root);

                        return true;
                    }
                }
                return true;
            } else {
                // Clicked outside - dismiss
                // Find root menu
                vg_contextmenu_t *root = menu;
                while (root->parent_menu) {
                    root = root->parent_menu;
                }
                vg_contextmenu_dismiss(root);
                return true;
            }
        }

        case VG_EVENT_KEY_DOWN: {
            if (event->key.key == VG_KEY_ESCAPE) {
                vg_contextmenu_t *root = menu;
                while (root->parent_menu) {
                    root = root->parent_menu;
                }
                vg_contextmenu_dismiss(root);
                return true;
            }

            if (event->key.key == VG_KEY_UP) {
                // Move selection up
                int new_index = menu->hovered_index - 1;
                while (new_index >= 0 && menu->items[new_index]->separator) {
                    new_index--;
                }
                if (new_index >= 0) {
                    menu->hovered_index = new_index;
                    widget->needs_paint = true;
                }
                return true;
            }

            if (event->key.key == VG_KEY_DOWN) {
                // Move selection down
                int new_index = menu->hovered_index + 1;
                while ((size_t)new_index < menu->item_count && menu->items[new_index]->separator) {
                    new_index++;
                }
                if ((size_t)new_index < menu->item_count) {
                    menu->hovered_index = new_index;
                    widget->needs_paint = true;
                }
                return true;
            }

            if (event->key.key == VG_KEY_ENTER) {
                if (menu->hovered_index >= 0 && (size_t)menu->hovered_index < menu->item_count) {
                    vg_menu_item_t *item = menu->items[menu->hovered_index];
                    if (!item->separator && item->enabled && !item->submenu) {
                        menu->clicked_index = menu->hovered_index;
                        item->was_clicked = true;
                        if (item->action) {
                            item->action(item->action_data);
                        }
                        if (menu->on_select)
                            menu->on_select(menu, item, menu->on_select_data);
                        vg_contextmenu_t *root = menu;
                        while (root->parent_menu) {
                            root = root->parent_menu;
                        }
                        vg_contextmenu_dismiss(root);
                    }
                }
                return true;
            }

            if (event->key.key == VG_KEY_RIGHT) {
                // Open submenu
                if (menu->hovered_index >= 0 && (size_t)menu->hovered_index < menu->item_count) {
                    vg_menu_item_t *item = menu->items[menu->hovered_index];
                    if (item->submenu && item->enabled) {
                        float item_y = ITEM_PADDING_Y;
                        for (int j = 0; j < menu->hovered_index; j++) {
                            item_y += get_item_height(menu->items[j]);
                        }
                        vg_contextmenu_t *submenu = (vg_contextmenu_t *)item->submenu;
                        vg_contextmenu_show_at(
                            submenu, (int)(widget->x + widget->width), (int)(widget->y + item_y));
                        submenu->parent_menu = menu;
                        menu->active_submenu = submenu;
                        // Move focus to submenu
                        submenu->hovered_index = 0;
                    }
                }
                return true;
            }

            if (event->key.key == VG_KEY_LEFT) {
                // Close submenu / return to parent
                if (menu->parent_menu) {
                    vg_contextmenu_dismiss(menu);
                }
                return true;
            }

            return false;
        }

        default:
            break;
    }

    return false;
}

//=============================================================================
// ContextMenu API
//=============================================================================

/// @brief Appends a labeled action item and returns its retained handle.
///
/// @details Both strings are copied. The action receives @p user_data when the
/// item is activated. The menu owns the returned handle and eventually destroys
/// it or retains it as an inert retired record after clearing.
///
/// @param menu Context menu to append to; may be null.
/// @param label Display label to copy; may be null.
/// @param shortcut Shortcut hint to copy; may be null.
/// @param action Callback invoked on activation; may be null.
/// @param user_data Opaque pointer forwarded to action.
/// @return New menu-item handle, or null on allocation failure.
vg_menu_item_t *vg_contextmenu_add_item(vg_contextmenu_t *menu,
                                        const char *label,
                                        const char *shortcut,
                                        void (*action)(void *),
                                        void *user_data) {
    if (!menu)
        return NULL;

    vg_menu_item_t *item = create_menu_item(label, shortcut, action, user_data);
    if (!item)
        return NULL;
    item->owner_contextmenu = menu;

    if (menu->item_count == SIZE_MAX ||
        !contextmenu_ensure_item_capacity(menu, menu->item_count + 1)) {
        free_menu_item(item);
        return NULL;
    }

    menu->items[menu->item_count++] = item;
    return item;
}

/// @brief Appends a labeled item that owns and opens a submenu.
///
/// @details The parent item assumes ownership of @p submenu. Destroying the
/// parent destroys the attached submenu; destroying the child directly first
/// detaches the parent. Hover or right-arrow navigation opens it beside the
/// corresponding row.
///
/// @param menu Parent context menu to append to; may be null.
/// @param label Display label to copy; may be null.
/// @param submenu Live nested menu whose ownership transfers to the new item.
/// @return New submenu-item handle, or null for invalid arguments or allocation
///         failure.
vg_menu_item_t *vg_contextmenu_add_submenu(vg_contextmenu_t *menu,
                                           const char *label,
                                           vg_contextmenu_t *submenu) {
    if (!menu || !submenu)
        return NULL;

    vg_menu_item_t *item = create_menu_item(label, NULL, NULL, NULL);
    if (!item)
        return NULL;
    if (menu->item_count == SIZE_MAX ||
        !contextmenu_ensure_item_capacity(menu, menu->item_count + 1)) {
        free_menu_item(item);
        return NULL;
    }

    item->owner_contextmenu = menu;
    item->submenu = (struct vg_menu *)submenu;
    submenu->parent_menu = menu;
    submenu->parent_item = item;

    menu->items[menu->item_count++] = item;
    return item;
}

/// @brief Appends a non-interactive horizontal separator.
///
/// @param menu Context menu to append to; may be null.
/// @return New separator handle, or null on allocation failure.
vg_menu_item_t *vg_contextmenu_add_separator(vg_contextmenu_t *menu) {
    if (!menu)
        return NULL;

    vg_menu_item_t *item = calloc(1, sizeof(vg_menu_item_t));
    if (!item)
        return NULL;

    item->magic = VG_MENU_ITEM_MAGIC;
    item->separator = true;
    item->owner_contextmenu = menu;

    if (menu->item_count == SIZE_MAX ||
        !contextmenu_ensure_item_capacity(menu, menu->item_count + 1)) {
        free(item);
        return NULL;
    }

    menu->items[menu->item_count++] = item;
    return item;
}

/// @brief Clears all active items without destroying the menu.
///
/// @details Any open submenu is dismissed. Items are retired rather than
/// immediately freeing their structures so stale external handles remain inert.
/// The item pointer-array allocation is retained for reuse.
///
/// @param menu Context menu to clear; may be null.
void vg_contextmenu_clear(vg_contextmenu_t *menu) {
    if (!menu)
        return;

    if (menu->active_submenu) {
        vg_contextmenu_dismiss(menu->active_submenu);
        menu->active_submenu = NULL;
    }
    menu->hovered_index = -1;
    menu->clicked_index = -1;

    for (size_t i = 0; i < menu->item_count; i++) {
        retire_menu_item(menu, menu->items[i]);
    }
    menu->item_count = 0;
    menu->base.needs_layout = true;
    menu->base.needs_paint = true;
}

/// @brief Unlinks and frees one exact retired menu-item record.
///
/// @details Address comparison is performed against the owner's retired chain
/// before the candidate is dereferenced, so foreign pointers fail safely.
///
/// @param menu Menu that may own the retired record.
/// @param item Exact retired item address to reclaim.
/// @return `true` when a matching record was reclaimed; otherwise `false`.
bool vg_contextmenu_reclaim_retired_item(vg_contextmenu_t *menu, vg_menu_item_t *item) {
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

/// @brief Enables or disables an active menu item.
///
/// @param item Active item handle to modify; may be null or stale.
/// @param enabled `false` to grey out and make the item non-interactive.
void vg_contextmenu_item_set_enabled(vg_menu_item_t *item, bool enabled) {
    if (item && item->magic == VG_MENU_ITEM_MAGIC && item->owner_contextmenu) {
        item->enabled = enabled;
        contextmenu_mark_item_changed(item, false);
    }
}

/// @brief Set the checked state of an item, showing or hiding the checkmark glyph.
///
/// @details The item becomes checkable even when @p checked is false. Layout is
/// invalidated because a shared leading column may appear.
///
/// @param item Active item handle to modify; may be null or stale.
/// @param checked `true` to display a checkmark in the leading column.
void vg_contextmenu_item_set_checked(vg_menu_item_t *item, bool checked) {
    if (item && item->magic == VG_MENU_ITEM_MAGIC && item->owner_contextmenu) {
        item->checkable = true;
        item->checked = checked;
        contextmenu_mark_item_changed(item, true);
    }
}

/// @brief Replace the icon displayed in the leading column for an item.
///
/// @details Ownership of @p icon transfers on every call. A valid item destroys
/// its previous icon and adopts the new value; an invalid or stale item destroys
/// the incoming value. Layout is invalidated because leading-column use may
/// change.
///
/// @param item Active item handle to modify; may be null or stale.
/// @param icon The new icon value; ownership of pixel data transfers to the item.
void vg_contextmenu_item_set_icon(vg_menu_item_t *item, vg_icon_t icon) {
    if (!item || item->magic != VG_MENU_ITEM_MAGIC || !item->owner_contextmenu) {
        vg_icon_destroy(&icon);
        return;
    }

    vg_icon_destroy(&item->icon);
    item->icon = icon;
    contextmenu_mark_item_changed(item, true);
}

/// @brief Shows the menu at screen-space coordinates.
///
/// @details The menu is measured, positioned, reset for interaction, and given
/// input capture. Window-edge clamping is deferred to painting.
///
/// @param menu Menu to display; may be null.
/// @param x Requested screen X coordinate before clamping.
/// @param y Requested screen Y coordinate before clamping.
void vg_contextmenu_show_at(vg_contextmenu_t *menu, int x, int y) {
    if (!menu)
        return;

    menu->anchor_x = x;
    menu->anchor_y = y;
    menu->is_visible = true;
    menu->hovered_index = -1;
    menu->clicked_index = -1;

    // Calculate size
    contextmenu_measure(&menu->base, 0, 0);

    // Position menu (clamp-to-window happens in contextmenu_paint where the
    // window handle is reliably available via the canvas argument).
    menu->base.x = (float)x;
    menu->base.y = (float)y;
    menu->base.width = menu->base.measured_width;
    menu->base.height = menu->base.measured_height;

    menu->base.visible = true;
    menu->base.needs_paint = true;
    vg_widget_set_input_capture(&menu->base);
}

/// @brief Shows the menu below a widget with an optional offset.
///
/// @details The widget's screen bounds determine the anchor passed to
/// `vg_contextmenu_show_at()`.
///
/// @param menu Menu to display; may be null.
/// @param widget Widget whose lower-left corner provides the anchor.
/// @param offset_x Additional X offset in pixels.
/// @param offset_y Additional Y offset in pixels.
void vg_contextmenu_show_for_widget(vg_contextmenu_t *menu,
                                    vg_widget_t *widget,
                                    int offset_x,
                                    int offset_y) {
    if (!menu || !widget)
        return;

    float screen_x = 0.0f;
    float screen_y = 0.0f;
    float screen_h = 0.0f;
    vg_widget_get_screen_bounds(widget, &screen_x, &screen_y, NULL, &screen_h);

    int x = (int)screen_x + offset_x;
    int y = (int)(screen_y + screen_h) + offset_y;
    vg_contextmenu_show_at(menu, x, y);
}

/// @brief Hides a menu and its active submenu chain.
///
/// @details Active descendants are dismissed first. Input capture returns to a
/// live visible parent or is released, and the dismissal callback runs after
/// this menu's visibility and parent link are cleared.
///
/// @param menu Menu to dismiss; may be null.
void vg_contextmenu_dismiss(vg_contextmenu_t *menu) {
    if (!menu)
        return;
    vg_contextmenu_t *parent = menu->parent_menu;

    // Dismiss active submenu first
    if (menu->active_submenu) {
        vg_contextmenu_dismiss(menu->active_submenu);
        menu->active_submenu = NULL;
    }

    menu->is_visible = false;
    menu->hovered_index = -1;
    menu->parent_menu = NULL;
    menu->base.visible = false;

    if (vg_widget_get_input_capture() == &menu->base) {
        if (parent && parent->is_visible && vg_contextmenu_is_live(parent)) {
            vg_widget_set_input_capture(&parent->base);
        } else {
            vg_widget_release_input_capture();
        }
    }

    // Invoke dismiss callback
    if (menu->on_dismiss) {
        menu->on_dismiss(menu, menu->on_dismiss_data);
    }
}

/// @brief Registers the callback invoked when an item is selected.
///
/// @details The callback runs after the item's own action and before the menu
/// chain is dismissed.
///
/// @param menu Context menu to configure; may be null.
/// @param callback Function called with the menu, item, and @p user_data; may be
///                 null to clear.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_contextmenu_set_on_select(vg_contextmenu_t *menu,
                                  void (*callback)(vg_contextmenu_t *, vg_menu_item_t *, void *),
                                  void *user_data) {
    if (!menu)
        return;
    menu->on_select = callback;
    menu->on_select_data = user_data;
    menu->user_data = user_data;
}

/// @brief Registers the callback invoked whenever the menu is dismissed.
///
/// @param menu Context menu to configure; may be null.
/// @param callback Function called with the menu and @p user_data after
///                 visibility is cleared; may be null.
/// @param user_data Opaque pointer forwarded to the callback.
void vg_contextmenu_set_on_dismiss(vg_contextmenu_t *menu,
                                   void (*callback)(vg_contextmenu_t *, void *),
                                   void *user_data) {
    if (!menu)
        return;
    menu->on_dismiss = callback;
    menu->on_dismiss_data = user_data;
}

/// @brief Associates a live context menu with a live widget.
///
/// @details An existing registration for the widget is replaced. Stale widget
/// or menu entries are evicted inline before new capacity is allocated.
///
/// @param widget Live widget that should respond to right-click.
/// @param menu Live context menu to show for that widget.
void vg_contextmenu_register_for_widget(vg_widget_t *widget, vg_contextmenu_t *menu) {
    if (!vg_widget_is_live(widget) || !vg_contextmenu_is_live(menu))
        return;

    // Update existing entry if widget already registered
    for (size_t i = 0; i < s_registry_count;) {
        if (!vg_widget_is_live(s_registry[i].widget) ||
            !vg_contextmenu_is_live(s_registry[i].menu)) {
            s_registry[i] = s_registry[--s_registry_count];
            continue;
        }
        if (s_registry[i].widget == widget) {
            s_registry[i].menu = menu;
            return;
        }
        i++;
    }

    if (!contextmenu_ensure_registry_capacity(s_registry_count + 1))
        return;
    s_registry[s_registry_count].widget = widget;
    s_registry[s_registry_count].menu = menu;
    s_registry_count++;
}

/// @brief Removes a widget's right-click registration.
///
/// @details The traversal also purges any other stale registry entries.
///
/// @param widget Widget to detach; may be null.
void vg_contextmenu_unregister_for_widget(vg_widget_t *widget) {
    if (!widget)
        return;

    for (size_t i = 0; i < s_registry_count;) {
        if (s_registry[i].widget == widget || !vg_widget_is_live(s_registry[i].widget) ||
            !vg_contextmenu_is_live(s_registry[i].menu)) {
            // Swap with last entry and shrink
            s_registry[i] = s_registry[--s_registry_count];
            continue;
        }
        i++;
    }
}

/// @brief Processes a right-click against the global context-menu registry.
///
/// @details Only right-button mouse-down events are eligible. A matching menu
/// opens at the event's screen coordinates, while stale registry entries are
/// purged during the search.
///
/// @param widget The widget that received the event; must be live.
/// @param event Event to process; may be null.
/// @return `true` when a registered live menu was shown; otherwise `false`.
bool vg_contextmenu_process_event(vg_widget_t *widget, vg_event_t *event) {
    if (!vg_widget_is_live(widget) || !event)
        return false;

    if (event->type != VG_EVENT_MOUSE_DOWN || event->mouse.button != VG_MOUSE_RIGHT)
        return false;

    for (size_t i = 0; i < s_registry_count;) {
        if (!vg_widget_is_live(s_registry[i].widget) ||
            !vg_contextmenu_is_live(s_registry[i].menu)) {
            s_registry[i] = s_registry[--s_registry_count];
            continue;
        }
        if (s_registry[i].widget == widget) {
            vg_contextmenu_show_at(
                s_registry[i].menu, (int)event->mouse.screen_x, (int)event->mouse.screen_y);
            return true;
        }
        i++;
    }
    return false;
}

/// @brief Sets the font and size throughout a context-menu subtree.
///
/// @details Null font and non-positive size arguments retain the corresponding
/// current value. Changes invalidate layout and paint and recurse through every
/// attached submenu. Fonts are borrowed rather than owned.
///
/// @param menu Context menu root to configure; may be null.
/// @param font Borrowed font to use, or null to retain the current font.
/// @param size Positive font size, or a non-positive value to retain the current
///             size.
void vg_contextmenu_set_font(vg_contextmenu_t *menu, vg_font_t *font, float size) {
    if (!menu)
        return;
    vg_font_t *new_font = font ? font : menu->font;
    float new_size = size > 0.0f ? size : menu->font_size;
    if (menu->font != new_font || menu->font_size != new_size) {
        menu->font = new_font;
        menu->font_size = new_size;
        menu->base.needs_layout = true;
        menu->base.needs_paint = true;
    }
    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item && item->submenu)
            vg_contextmenu_set_font((vg_contextmenu_t *)item->submenu, font, size);
    }
}

/// @brief Apply theme colors to a context-menu subtree.
///
/// @details Context menus are often long-lived standalone overlays. Copying
/// current colors lets them follow theme changes without rebuilding their menu
/// models. The update recursively covers attached submenus and schedules paint
/// only when visible style state changes.
///
/// @param menu Context-menu root to update; may be null.
/// @param theme Theme whose colors should be copied; null selects the current
///              theme.
void vg_contextmenu_apply_theme(vg_contextmenu_t *menu, const vg_theme_t *theme) {
    if (!menu)
        return;
    if (!theme)
        theme = vg_theme_get_current();
    if (!theme)
        return;

    float font_size = menu->font_size > 0.0f ? menu->font_size : theme->typography.size_normal;
    bool changed =
        menu->bg_color != theme->colors.bg_primary || menu->hover_color != theme->colors.bg_hover ||
        menu->text_color != theme->colors.fg_primary ||
        menu->disabled_color != theme->colors.fg_secondary ||
        menu->border_color != theme->colors.border_primary ||
        menu->separator_color != theme->colors.border_secondary || menu->font_size != font_size;
    menu->bg_color = theme->colors.bg_primary;
    menu->hover_color = theme->colors.bg_hover;
    menu->text_color = theme->colors.fg_primary;
    menu->disabled_color = theme->colors.fg_secondary;
    menu->border_color = theme->colors.border_primary;
    menu->separator_color = theme->colors.border_secondary;
    menu->font_size = font_size;
    if (changed)
        menu->base.needs_paint = true;

    for (size_t i = 0; i < menu->item_count; i++) {
        vg_menu_item_t *item = menu->items[i];
        if (item && item->submenu)
            vg_contextmenu_apply_theme((vg_contextmenu_t *)item->submenu, theme);
    }
}
