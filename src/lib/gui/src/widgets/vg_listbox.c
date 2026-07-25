//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_listbox.c
// Purpose: ListBox widget implementation — scrollable list of selectable items
//          supporting normal and virtual-scroll modes, multi-select, item
//          retirement pool, and a data-provider callback for large datasets.
// Key invariants:
//   - Items carry a magic value (VG_LISTBOX_ITEM_MAGIC) and an owner pointer;
//     vg_listbox_item_is_live validates both before any item operation.
//   - Virtual mode allocates a selection bitmap and a visible-item cache;
//     non-virtual mode uses a doubly-linked item list.
//   - Retired items are pooled (up to VG_LISTBOX_RETIRE_POOL_SIZE) to reduce
//     malloc/free churn during rapid updates.
//   - Overflowing row text paints with an ellipsis and temporarily becomes the
//     widget tooltip while hovered; the caller's original tooltip is restored.
// Ownership/Lifetime:
//   - Items are owned by the listbox; freed on remove, clear, or destroy.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_event.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements owned and virtual list-box models, scrolling, selection,
///        visible-row caching, painting, and input handling.
/// @details Normal mode stores live item records in an owned linked list.
///          Virtual mode stores only packed selection state and a bounded cache
///          populated by the caller's provider. Item retirement and model-unbind
///          callbacks provide explicit lifetime boundaries for managed callers.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VG_LISTBOX_ITEM_MAGIC UINT64_C(0x56474C424954454D)
#define VG_LISTBOX_ITEM_RETIRED_MAGIC UINT64_C(0x56474C4244524F50)
#define VG_LISTBOX_MEASURE_TEXT_SCAN_LIMIT 512u

//=============================================================================
// Forward declarations
//=============================================================================

static void listbox_measure(vg_widget_t *widget, float avail_w, float avail_h);
static void listbox_arrange(vg_widget_t *widget, float x, float y, float w, float h);
static void listbox_paint(vg_widget_t *widget, void *canvas);
static bool listbox_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool listbox_can_focus(vg_widget_t *widget);
static void listbox_destroy(vg_widget_t *widget);
static float listbox_default_item_height(vg_listbox_t *lb);
static float listbox_text_baseline(vg_listbox_t *lb, float item_y, float item_h);
static const char *listbox_fit_text(vg_listbox_t *lb, const char *text, float max_width);
static void listbox_sync_hover_tooltip(vg_listbox_t *lb);
static vg_listbox_item_t *listbox_item_at_index_nonvirtual(vg_listbox_t *lb, size_t index);
static size_t listbox_index_of_item(vg_listbox_t *lb, vg_listbox_item_t *target);
static bool listbox_clear_nonvirtual_selection(vg_listbox_t *lb);
static bool listbox_clear_virtual_selection(vg_listbox_t *lb);
static void listbox_select_nonvirtual_with_modifiers(vg_listbox_t *lb,
                                                     vg_listbox_item_t *item,
                                                     bool toggle,
                                                     bool range);
static void listbox_select_virtual_with_modifiers(vg_listbox_t *lb,
                                                  size_t index,
                                                  bool toggle,
                                                  bool range);
static void listbox_free_item_payload(vg_listbox_item_t *item);
static void listbox_retire_item(vg_listbox_t *lb, vg_listbox_item_t *item);
static void listbox_free_retired_items(vg_listbox_t *lb);
static vg_listbox_item_t *listbox_first_selected_nonvirtual(vg_listbox_t *lb);
static size_t listbox_first_selected_virtual(vg_listbox_t *lb);
static void listbox_refresh_virtual_cache_entry(vg_listbox_t *lb, size_t cache_index);
static void listbox_notify_virtual_unbound(vg_listbox_t *lb);
static void listbox_compute_virtual_range(vg_listbox_t *lb,
                                          float viewport_height,
                                          size_t *out_start,
                                          size_t *out_count);

//=============================================================================
// VTable
//=============================================================================

static vg_widget_vtable_t g_listbox_vtable = {
    .destroy = listbox_destroy,
    .measure = listbox_measure,
    .arrange = listbox_arrange,
    .paint = listbox_paint,
    .handle_event = listbox_handle_event,
    .can_focus = listbox_can_focus,
    .on_focus = NULL,
};

//=============================================================================
// VTable Implementations
//=============================================================================

/// @brief Return the number of bytes needed for @p item_count packed selection bits.
/// @param item_count Number of logical selection bits.
/// @return Byte count rounded up to hold every bit.
static size_t listbox_selection_bitmap_bytes(size_t item_count) {
    return (item_count + 7u) / 8u;
}

/// @brief Read one virtual-mode selection bit.
/// @param lb List box containing the packed bitmap.
/// @param index Zero-based logical row index.
/// @return `true` only when @p index is in range and its bit is set.
static bool listbox_selection_get(const vg_listbox_t *lb, size_t index) {
    if (!lb || !lb->selection_bitmap || index >= lb->selection_bitmap_size)
        return false;
    return (lb->selection_bitmap[index / 8u] & (uint8_t)(1u << (index % 8u))) != 0;
}

/// @brief Set one virtual-mode selection bit.
/// @param lb List box containing the packed bitmap.
/// @param index Zero-based logical row index.
/// @param selected Desired bit state.
/// @return `true` if the bit changed; `false` for invalid input or an
///         already-matching state.
static bool listbox_selection_set(vg_listbox_t *lb, size_t index, bool selected) {
    if (!lb || !lb->selection_bitmap || index >= lb->selection_bitmap_size)
        return false;
    uint8_t mask = (uint8_t)(1u << (index % 8u));
    uint8_t *byte = &lb->selection_bitmap[index / 8u];
    bool was_selected = (*byte & mask) != 0;
    if (selected)
        *byte |= mask;
    else
        *byte &= (uint8_t)~mask;
    return was_selected != selected;
}

/// @brief Clear any unused high bits in the last logical selection byte.
/// @param lb List box whose bitmap tail is normalized.
static void listbox_selection_mask_tail(vg_listbox_t *lb) {
    if (!lb || !lb->selection_bitmap || lb->selection_bitmap_size == 0)
        return;
    size_t used_bits = lb->selection_bitmap_size % 8u;
    if (used_bits == 0)
        return;
    uint8_t mask = (uint8_t)((1u << used_bits) - 1u);
    lb->selection_bitmap[listbox_selection_bitmap_bytes(lb->selection_bitmap_size) - 1u] &= mask;
}

/// @brief Frees the text strings inside each cached visible row slot without releasing the cache
/// array itself.
/// @param lb List box whose cache-entry payloads are cleared.
static void listbox_free_virtual_cache(vg_listbox_t *lb) {
    if (!lb->visible_cache)
        return;

    for (size_t i = 0; i < lb->cache_capacity; i++) {
        free(lb->visible_cache[i].text);
        lb->visible_cache[i].text = NULL;
        lb->visible_cache[i].selected = false;
    }
}

/// @brief Walks the non-virtual item list and returns the item at zero-based @p index, or NULL.
/// @param lb Non-virtual list box to traverse.
/// @param index Zero-based item index.
/// @return Borrowed live item pointer, or `NULL` when out of range.
static vg_listbox_item_t *listbox_item_at_index_nonvirtual(vg_listbox_t *lb, size_t index) {
    if (!lb)
        return NULL;
    vg_listbox_item_t *item = lb->first_item;
    for (size_t i = 0; i < index && item; i++)
        item = item->next;
    return item;
}

/// @brief Returns the zero-based index of @p target in the non-virtual list, or SIZE_MAX if not
/// found.
/// @param lb List box expected to own @p target.
/// @param target Live item to locate.
/// @return Zero-based position, or `SIZE_MAX` when validation or lookup fails.
static size_t listbox_index_of_item(vg_listbox_t *lb, vg_listbox_item_t *target) {
    if (!lb || !vg_listbox_item_is_live(target) || target->owner != lb)
        return SIZE_MAX;
    size_t index = 0;
    for (vg_listbox_item_t *item = lb->first_item; item; item = item->next, index++) {
        if (item == target)
            return index;
    }
    return SIZE_MAX;
}

/// @brief Returns true if @p item is non-NULL, has the correct magic value, and still belongs to a
/// listbox.
/// @param item Item record to validate without transferring ownership.
/// @return `true` when the record is live and associated with an owner.
bool vg_listbox_item_is_live(const vg_listbox_item_t *item) {
    return item && item->magic == VG_LISTBOX_ITEM_MAGIC && item->owner != NULL;
}

/// @brief Override the text color for one live item.
/// @param item Live item to modify; invalid or retired records are ignored.
/// @param color Packed renderer color used for this row's text.
void vg_listbox_item_set_text_color(vg_listbox_item_t *item, uint32_t color) {
    if (!vg_listbox_item_is_live(item))
        return;
    item->text_color = color;
    item->has_text_color = true;
    item->owner->base.needs_paint = true;
}

/// @brief Frees the text string inside @p item and zeroes its length, leaving the item struct
/// itself intact.
/// @details Owned user data is also released and per-item visual overrides are
///          reset, preparing the record for retirement or destruction.
/// @param item Item whose owned payload is cleared; `NULL` is ignored.
static void listbox_free_item_payload(vg_listbox_item_t *item) {
    if (!item)
        return;
    free(item->text);
    item->text = NULL;
    item->text_len = 0;
    if (item->owns_user_data)
        free(item->user_data);
    item->user_data = NULL;
    item->owns_user_data = false;
    item->has_text_color = false;
    item->text_color = 0;
}

/// @brief Frees @p item's payload and then frees the item struct itself.
/// @param item Owned item record to destroy; `NULL` is ignored.
static void listbox_free_item(vg_listbox_item_t *item) {
    if (!item)
        return;
    listbox_free_item_payload(item);
    free(item);
}

/// @brief Detaches @p item from the live list and moves it onto @p lb->retired_items for deferred
/// freeing.
/// @param lb List box receiving the retired record.
/// @param item Detached item whose payload is released and liveness revoked.
static void listbox_retire_item(vg_listbox_t *lb, vg_listbox_item_t *item) {
    if (!lb || !item)
        return;
    listbox_free_item_payload(item);
    item->magic = VG_LISTBOX_ITEM_RETIRED_MAGIC;
    item->owner = NULL;
    item->selected = false;
    item->next = NULL;
    item->prev = NULL;
    item->retired_next = lb->retired_items;
    lb->retired_items = item;
}

/// @brief Walks @p lb->retired_items and frees every item in the deferred-free list.
/// @param lb List box whose complete retirement chain is destroyed.
static void listbox_free_retired_items(vg_listbox_t *lb) {
    if (!lb)
        return;
    vg_listbox_item_t *item = lb->retired_items;
    while (item) {
        vg_listbox_item_t *next = item->retired_next;
        listbox_free_item(item);
        item = next;
    }
    lb->retired_items = NULL;
}

/// @brief Unlink and free one exact retained ListBox item record.
/// @details See the public header for managed-wrapper preconditions. Pointer-to-pointer traversal
///          avoids dereferencing the caller-supplied address unless it is owned by the chain.
/// @param listbox List box whose retirement chain is searched.
/// @param item Exact retired-record address to reclaim.
/// @return `true` when the address was found and freed; otherwise `false`.
bool vg_listbox_reclaim_retired_item(vg_listbox_t *listbox, vg_listbox_item_t *item) {
    if (!listbox || !item)
        return false;
    vg_listbox_item_t **link = &listbox->retired_items;
    while (*link) {
        vg_listbox_item_t *candidate = *link;
        if (candidate == item) {
            *link = candidate->retired_next;
            candidate->retired_next = NULL;
            listbox_free_item(candidate);
            return true;
        }
        link = &candidate->retired_next;
    }
    return false;
}

/// @brief Returns the total item count: total_item_count in virtual mode, item_count otherwise.
/// @param lb List box to inspect.
/// @return Logical row count for the active storage mode.
static size_t listbox_virtual_item_count(const vg_listbox_t *lb) {
    return lb->virtual_mode ? lb->total_item_count : (size_t)lb->item_count;
}

/// @brief Clear and invoke the external-model lifetime callback exactly once.
/// @details Callback fields are cleared before invocation so a callback that indirectly enters a
///          legacy ListBox cleanup path cannot receive a duplicate notification.
/// @param lb ListBox whose binding is being detached; NULL is ignored.
static void listbox_notify_virtual_unbound(vg_listbox_t *lb) {
    if (!lb)
        return;
    vg_listbox_virtual_unbind_callback_t callback = lb->virtual_unbind;
    void *user_data = lb->virtual_unbind_user_data;
    lb->virtual_unbind = NULL;
    lb->virtual_unbind_user_data = NULL;
    if (callback)
        callback(&lb->base, user_data);
}

/// @brief Clamps lb->scroll_y to [0, max_scroll] based on total item count and viewport height.
/// @param lb List box whose vertical offset is normalized.
/// @param viewport_height Current visible height in pixels.
static void listbox_clamp_scroll(vg_listbox_t *lb, float viewport_height) {
    if (!lb)
        return;

    if (lb->scroll_y < 0.0f)
        lb->scroll_y = 0.0f;

    float max_scroll = (float)listbox_virtual_item_count(lb) * lb->item_height - viewport_height;
    if (max_scroll < 0.0f)
        max_scroll = 0.0f;
    if (lb->scroll_y > max_scroll)
        lb->scroll_y = max_scroll;
}

/// @brief Compute virtual viewport bounds without fetching or allocating row text.
/// @details The same two trailing safety rows and 4096-row defensive cap used by painting are
///          applied, keeping public visibility queries consistent with the cache.
/// @param lb Virtual ListBox to inspect; NULL and non-virtual lists produce an empty range.
/// @param viewport_height Current physical viewport height; negative/non-finite values normalize
///                        to zero.
/// @param out_start Receives the first materialized row when non-NULL.
/// @param out_count Receives the number of materialized rows when non-NULL.
static void listbox_compute_virtual_range(vg_listbox_t *lb,
                                          float viewport_height,
                                          size_t *out_start,
                                          size_t *out_count) {
    size_t start = 0;
    size_t count = 0;
    if (lb && lb->virtual_mode && lb->total_item_count > 0 && lb->item_height > 0.0f &&
        isfinite(lb->item_height)) {
        if (!isfinite(viewport_height) || viewport_height < 0.0f)
            viewport_height = 0.0f;
        listbox_clamp_scroll(lb, viewport_height);
        start = (size_t)(lb->scroll_y / lb->item_height);
        if (start >= lb->total_item_count)
            start = lb->total_item_count - 1u;
        double rows = (double)viewport_height / (double)lb->item_height;
        if (rows > 4094.0)
            count = 4096u;
        else
            count = (size_t)rows + 2u;
        if (count > lb->total_item_count - start)
            count = lb->total_item_count - start;
        if (count > 4096u)
            count = 4096u;
    }
    if (out_start)
        *out_start = start;
    if (out_count)
        *out_count = count;
}

/// @brief Grows the visible-row cache array to at least @p needed entries, zeroing new slots.
/// @details Failure or size overflow leaves the old allocation and capacity intact.
/// @param lb Virtual list box owning the cache.
/// @param needed Minimum number of cache entries required.
static void listbox_ensure_virtual_cache(vg_listbox_t *lb, size_t needed) {
    if (needed <= lb->cache_capacity)
        return;
    if (needed > SIZE_MAX / sizeof(vg_listbox_cache_entry_t))
        return;

    vg_listbox_cache_entry_t *new_cache =
        realloc(lb->visible_cache, needed * sizeof(vg_listbox_cache_entry_t));
    if (!new_cache)
        return;

    memset(new_cache + lb->cache_capacity,
           0,
           (needed - lb->cache_capacity) * sizeof(vg_listbox_cache_entry_t));
    lb->visible_cache = new_cache;
    lb->cache_capacity = needed;
}

/// @brief Re-fetches text and selection state for the visible-cache slot at @p cache_index via the
/// data provider.
/// @param lb Virtual list box with a materialized visible range.
/// @param cache_index Offset within the current visible cache.
static void listbox_refresh_virtual_cache_entry(vg_listbox_t *lb, size_t cache_index) {
    if (!lb || !lb->visible_cache || cache_index >= lb->visible_count ||
        cache_index >= lb->cache_capacity)
        return;

    size_t index = lb->visible_start + cache_index;
    const char *text = "";
    if (lb->data_provider)
        lb->data_provider(&lb->base, index, &text, NULL, lb->data_provider_user_data);

    char *copy = vg_strdup(text ? text : "");
    if (!copy)
        return;

    free(lb->visible_cache[cache_index].text);
    lb->visible_cache[cache_index].text = copy;
}

/// @brief Updates the virtual visible-row cache to match the current scroll position, re-fetching
/// changed rows.
/// @param lb Virtual list box whose cache is synchronized.
/// @param viewport_height Current visible height in pixels.
static void listbox_sync_virtual_cache(vg_listbox_t *lb, float viewport_height) {
    if (!lb || !lb->virtual_mode)
        return;

    size_t total = lb->total_item_count;
    if (total == 0 || lb->item_height <= 0.0f || !isfinite(lb->item_height)) {
        listbox_free_virtual_cache(lb);
        lb->visible_start = 0;
        lb->visible_count = 0;
        return;
    }

    size_t start = 0;
    size_t visible = 0;
    listbox_compute_virtual_range(lb, viewport_height, &start, &visible);

    listbox_ensure_virtual_cache(lb, visible);
    if (!lb->visible_cache)
        return;

    if (lb->visible_start == start && lb->visible_count == visible) {
        for (size_t i = 0; i < visible; i++) {
            size_t index = start + i;
            if (!lb->visible_cache[i].text)
                listbox_refresh_virtual_cache_entry(lb, i);
            lb->visible_cache[i].selected = listbox_selection_get(lb, index);
        }
        return;
    }

    listbox_free_virtual_cache(lb);
    lb->visible_start = start;
    lb->visible_count = visible;

    for (size_t i = 0; i < visible; i++) {
        const char *text = "";
        size_t index = start + i;
        if (lb->data_provider) {
            lb->data_provider(&lb->base, index, &text, NULL, lb->data_provider_user_data);
        }
        lb->visible_cache[i].text = vg_strdup(text ? text : "");
        lb->visible_cache[i].selected = listbox_selection_get(lb, index);
    }
}

/// @brief Converts @p local_y (relative to the viewport top) to an item index accounting for
/// scroll; false if out of range.
/// @param lb List box providing row height, count, and scroll offset.
/// @param widget Widget whose viewport receives the local coordinate.
/// @param local_y Pointer Y coordinate relative to the viewport top.
/// @param index Receives the zero-based logical row on success.
/// @return `true` when the coordinate maps to an existing row.
static bool listbox_item_at_y(vg_listbox_t *lb, vg_widget_t *widget, float local_y, size_t *index) {
    if (!lb || !widget || !index || lb->item_height <= 0.0f)
        return false;

    float ry = local_y + lb->scroll_y;
    if (ry < 0.0f)
        return false;

    size_t idx = (size_t)(ry / lb->item_height);
    size_t total = listbox_virtual_item_count(lb);
    if (idx >= total)
        return false;

    *index = idx;
    return true;
}

/// @brief Clears the selected flag on every item in the non-virtual linked list.
/// @param lb Non-virtual list box to update.
/// @return `true` when at least one selected flag was cleared.
static bool listbox_clear_nonvirtual_selection(vg_listbox_t *lb) {
    if (!lb)
        return false;
    bool changed = false;
    for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
        if (item->selected)
            changed = true;
        item->selected = false;
    }
    return changed;
}

/// @brief Advance selection state and the general widget change revision.
/// @param lb List box whose logical selection changed; `NULL` is ignored.
static void listbox_note_selection_changed(vg_listbox_t *lb) {
    if (lb) {
        if (lb->selection_revision < UINT64_MAX)
            lb->selection_revision++;
        vg_widget_note_change(&lb->base);
    }
}

/// @brief Test whether any non-virtual row is selected.
/// @param lb Non-virtual list box to inspect.
/// @return `true` when the primary pointer or any item flag records selection.
static bool listbox_has_nonvirtual_selection(vg_listbox_t *lb) {
    if (!lb)
        return false;
    if (lb->selected)
        return true;
    for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
        if (item->selected)
            return true;
    }
    return false;
}

/// @brief Test for selected rows other than one excluded item.
/// @param lb Non-virtual list box to inspect.
/// @param except Item omitted from the search.
/// @return `true` when another row has its selected flag set.
static bool listbox_has_nonvirtual_selection_except(vg_listbox_t *lb, vg_listbox_item_t *except) {
    if (!lb)
        return false;
    for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
        if (item != except && item->selected)
            return true;
    }
    return false;
}

/// @brief Zeroes the entire selection bitmap in virtual mode, deselecting all rows.
/// @param lb Virtual list box whose bitmap is cleared.
/// @return `true` when at least one bit was previously set.
static bool listbox_clear_virtual_selection(vg_listbox_t *lb) {
    if (!lb || !lb->selection_bitmap)
        return false;
    bool changed = false;
    size_t bytes = listbox_selection_bitmap_bytes(lb->selection_bitmap_size);
    for (size_t i = 0; i < bytes; i++) {
        if (lb->selection_bitmap[i]) {
            changed = true;
            break;
        }
    }
    memset(lb->selection_bitmap, 0, bytes);
    return changed;
}

/// @brief Returns the first selected item in the non-virtual list, or NULL if none is selected.
/// @param lb Non-virtual list box to scan.
/// @return Borrowed first selected item, or `NULL`.
static vg_listbox_item_t *listbox_first_selected_nonvirtual(vg_listbox_t *lb) {
    if (!lb)
        return NULL;
    for (vg_listbox_item_t *candidate = lb->first_item; candidate; candidate = candidate->next) {
        if (candidate->selected)
            return candidate;
    }
    return NULL;
}

/// @brief Returns the index of the first selected row in the bitmap, or SIZE_MAX if nothing is
/// selected.
/// @param lb Virtual list box to scan.
/// @return First selected logical index, or `SIZE_MAX`.
static size_t listbox_first_selected_virtual(vg_listbox_t *lb) {
    if (!lb || !lb->selection_bitmap)
        return SIZE_MAX;
    for (size_t i = 0; i < lb->selection_bitmap_size; i++) {
        if (listbox_selection_get(lb, i))
            return i;
    }
    return SIZE_MAX;
}

/// @brief Applies Ctrl/Shift selection semantics for @p item in non-virtual mode: range, toggle, or
/// replace.
/// @param lb Owning non-virtual list box.
/// @param item Live target item.
/// @param toggle `true` to toggle the target in multi-select mode.
/// @param range `true` to select the inclusive anchor-to-target range.
static void listbox_select_nonvirtual_with_modifiers(vg_listbox_t *lb,
                                                     vg_listbox_item_t *item,
                                                     bool toggle,
                                                     bool range) {
    if (!lb || !vg_listbox_item_is_live(item) || item->owner != lb)
        return;
    vg_listbox_item_t *old_selected = lb->selected;

    if (!lb->multi_select || (!toggle && !range)) {
        bool changed = listbox_clear_nonvirtual_selection(lb);
        changed = changed || !item->selected || lb->selected != item;
        item->selected = true;
        lb->selected = item;
        lb->anchor_selected = item;
        if (changed || lb->selected != old_selected)
            listbox_note_selection_changed(lb);
        return;
    }

    if (range) {
        bool changed = listbox_clear_nonvirtual_selection(lb);
        vg_listbox_item_t *anchor = lb->anchor_selected ? lb->anchor_selected : lb->selected;
        size_t anchor_index = listbox_index_of_item(lb, anchor ? anchor : item);
        size_t target_index = listbox_index_of_item(lb, item);
        if (anchor_index == SIZE_MAX || target_index == SIZE_MAX) {
            changed = changed || !item->selected;
            item->selected = true;
        } else {
            size_t start = anchor_index < target_index ? anchor_index : target_index;
            size_t end = anchor_index > target_index ? anchor_index : target_index;
            for (size_t index = start; index <= end; index++) {
                vg_listbox_item_t *selected_item = listbox_item_at_index_nonvirtual(lb, index);
                if (selected_item) {
                    if (!selected_item->selected)
                        changed = true;
                    selected_item->selected = true;
                }
            }
        }
        lb->selected = item;
        if (changed || lb->selected != old_selected)
            listbox_note_selection_changed(lb);
        return;
    }

    bool was_selected = item->selected;
    item->selected = !item->selected;
    if (item->selected) {
        lb->selected = item;
        lb->anchor_selected = item;
    } else {
        if (lb->selected == item)
            lb->selected = listbox_first_selected_nonvirtual(lb);
        if (lb->anchor_selected == item)
            lb->anchor_selected = lb->selected;
    }
    if (was_selected != item->selected || lb->selected != old_selected)
        listbox_note_selection_changed(lb);
}

/// @brief Applies Ctrl/Shift selection semantics for row @p index in virtual mode using the
/// selection bitmap.
/// @param lb Virtual list box to update.
/// @param index Zero-based target row.
/// @param toggle `true` to toggle the target bit in multi-select mode.
/// @param range `true` to select the inclusive anchor-to-target range.
static void listbox_select_virtual_with_modifiers(vg_listbox_t *lb,
                                                  size_t index,
                                                  bool toggle,
                                                  bool range) {
    if (!lb || index >= lb->total_item_count)
        return;
    size_t old_selected = lb->selected_index;

    if (!lb->multi_select || (!toggle && !range)) {
        bool changed = listbox_clear_virtual_selection(lb);
        changed = changed || lb->selected_index != index || !listbox_selection_get(lb, index);
        lb->selected_index = index;
        changed = listbox_selection_set(lb, index, true) || changed;
        lb->anchor_selected_index = index;
        if (changed || lb->selected_index != old_selected)
            listbox_note_selection_changed(lb);
        return;
    }

    if (range) {
        bool changed = listbox_clear_virtual_selection(lb);
        size_t anchor =
            lb->anchor_selected_index < lb->total_item_count ? lb->anchor_selected_index : index;
        size_t start = anchor < index ? anchor : index;
        size_t end = anchor > index ? anchor : index;
        for (size_t i = start; i <= end && i < lb->selection_bitmap_size; i++) {
            changed = listbox_selection_set(lb, i, true) || changed;
        }
        lb->selected_index = index;
        if (changed || lb->selected_index != old_selected)
            listbox_note_selection_changed(lb);
        return;
    }

    if (!lb->selection_bitmap || index >= lb->selection_bitmap_size)
        return;

    bool was_selected = listbox_selection_get(lb, index);
    bool now_selected = !was_selected;
    (void)listbox_selection_set(lb, index, now_selected);
    if (now_selected) {
        lb->selected_index = index;
        lb->anchor_selected_index = index;
    } else {
        if (lb->selected_index == index)
            lb->selected_index = listbox_first_selected_virtual(lb);
        if (lb->anchor_selected_index == index)
            lb->anchor_selected_index = lb->selected_index;
    }
    if (was_selected != now_selected || lb->selected_index != old_selected)
        listbox_note_selection_changed(lb);
}

/// @brief VTable destroy: clears all items, frees the virtual cache, selection bitmap, and
/// retired-item list.
/// @details Also restores tooltip ownership and releases fitting scratch storage.
/// @param widget List-box widget base being destroyed.
static void listbox_destroy(vg_widget_t *widget) {
    vg_listbox_t *lb = (vg_listbox_t *)widget;
    vg_listbox_clear(lb);
    listbox_free_virtual_cache(lb);
    free(lb->visible_cache);
    lb->visible_cache = NULL;
    lb->cache_capacity = 0;
    free(lb->selection_bitmap);
    lb->selection_bitmap = NULL;
    lb->selection_bitmap_size = 0;
    lb->selection_bitmap_capacity = 0;
    listbox_free_retired_items(lb);
    free(lb->saved_tooltip_text);
    lb->saved_tooltip_text = NULL;
    lb->hover_tooltip_active = false;
    free(lb->ellipsis_scratch);
    lb->ellipsis_scratch = NULL;
    lb->ellipsis_scratch_capacity = 0;
}

/// @brief Computes the default row height from the theme input height and font metrics, taking the
/// larger value.
/// @param lb List box supplying the optional active font.
/// @return Theme input height or padded font line height, whichever is larger.
static float listbox_default_item_height(vg_listbox_t *lb) {
    vg_theme_t *theme = vg_theme_get_current();
    float scale = theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
    float height = theme ? theme->input.height : 28.0f * scale;

    if (lb && lb->font) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(lb->font, lb->font_size, &metrics);
        float metrics_height = (float)metrics.line_height + 8.0f * scale;
        if (metrics_height > height)
            height = metrics_height;
    }

    return height;
}

/// @brief Returns the Y coordinate of the text baseline vertically centred within an item row.
/// @param lb List box supplying font metrics.
/// @param item_y Row's top coordinate.
/// @param item_h Row height.
/// @return Baseline Y coordinate, or @p item_y when no font is available.
static float listbox_text_baseline(vg_listbox_t *lb, float item_y, float item_h) {
    if (!lb || !lb->font)
        return item_y;

    vg_font_metrics_t metrics = {0};
    vg_font_get_metrics(lb->font, lb->font_size, &metrics);
    return item_y + (item_h + (float)metrics.ascent + (float)metrics.descent) / 2.0f;
}

/// @brief Back a byte length up to a UTF-8 codepoint boundary.
/// @param text UTF-8 byte string containing @p length as a valid index.
/// @param length Candidate prefix length.
/// @return Greatest prefix no longer than @p length that does not begin on a
///         continuation byte.
static size_t listbox_utf8_previous_boundary(const char *text, size_t length) {
    if (!text)
        return 0;
    while (length > 0 && (((unsigned char)text[length] & 0xC0u) == 0x80u))
        length--;
    return length;
}

/// @brief Return paint text fitted to @p max_width, reusing listbox-owned scratch storage.
/// @details The original item text is never mutated. A failed scratch allocation falls back to
///          the clipped original, preserving content even under memory pressure.
/// @param lb List box supplying font metrics and scratch storage.
/// @param text Null-terminated row text.
/// @param max_width Maximum rendered width in pixels.
/// @return Borrowed original text, list-box-owned ellipsized scratch text, or
///         an empty string when no text can be drawn.
static const char *listbox_fit_text(vg_listbox_t *lb, const char *text, float max_width) {
    if (!text)
        return "";
    if (!lb || !lb->font || max_width <= 0.0f)
        return "";

    vg_text_metrics_t metrics = {0};
    vg_font_measure_text(lb->font, lb->font_size, text, &metrics);
    if (metrics.width <= max_width)
        return text;

    vg_text_metrics_t ellipsis_metrics = {0};
    vg_font_measure_text(lb->font, lb->font_size, "...", &ellipsis_metrics);
    if (ellipsis_metrics.width > max_width)
        return "";

    size_t length = strlen(text);
    if (length > SIZE_MAX - 4u)
        return text;
    size_t needed = length + 4u;
    if (needed > lb->ellipsis_scratch_capacity) {
        char *next = (char *)realloc(lb->ellipsis_scratch, needed);
        if (!next)
            return text;
        lb->ellipsis_scratch = next;
        lb->ellipsis_scratch_capacity = needed;
    }

    size_t low = 0;
    size_t high = length;
    while (low < high) {
        size_t candidate = low + (high - low + 1u) / 2u;
        candidate = listbox_utf8_previous_boundary(text, candidate);
        if (candidate <= low) {
            candidate = low + 1u;
            while (candidate < length && (((unsigned char)text[candidate] & 0xC0u) == 0x80u))
                candidate++;
            if (candidate > high)
                break;
        }

        memcpy(lb->ellipsis_scratch, text, candidate);
        memcpy(lb->ellipsis_scratch + candidate, "...", 4u);
        vg_font_measure_text(lb->font, lb->font_size, lb->ellipsis_scratch, &metrics);
        if (metrics.width <= max_width) {
            low = candidate;
        } else {
            high = listbox_utf8_previous_boundary(text, candidate - 1u);
        }
    }

    memcpy(lb->ellipsis_scratch, text, low);
    memcpy(lb->ellipsis_scratch + low, "...", 4u);
    return lb->ellipsis_scratch;
}

/// @brief Return the text under the pointer in normal or virtual mode.
/// @param lb List box whose current hover state is inspected.
/// @return Borrowed row text, or `NULL` when no materialized row is hovered.
static const char *listbox_hovered_text(vg_listbox_t *lb) {
    if (!lb)
        return NULL;
    if (!lb->virtual_mode)
        return lb->hovered ? lb->hovered->text : NULL;
    if (lb->hovered_index == SIZE_MAX || lb->hovered_index < lb->visible_start ||
        lb->hovered_index - lb->visible_start >= lb->visible_count)
        return NULL;
    size_t cache_index = lb->hovered_index - lb->visible_start;
    if (!lb->visible_cache || cache_index >= lb->cache_capacity)
        return NULL;
    return lb->visible_cache[cache_index].text;
}

/// @brief Promote clipped hovered-row text to the widget tooltip and restore caller text on exit.
/// @details The original tooltip is copied before the first overflow promotion
///          and restored when hover leaves or the row no longer overflows.
/// @param lb List box whose tooltip state is synchronized.
static void listbox_sync_hover_tooltip(vg_listbox_t *lb) {
    if (!lb)
        return;

    const char *hover_text = listbox_hovered_text(lb);
    bool overflow = false;
    if (hover_text && hover_text[0] != '\0') {
        vg_theme_t *theme = vg_theme_get_current();
        float padding = theme ? theme->input.padding_h : 0.0f;
        float available = lb->base.width - padding - 6.0f;
        float text_width = 0.0f;
        if (lb->font) {
            vg_text_metrics_t metrics = {0};
            vg_font_measure_text(lb->font, lb->font_size, hover_text, &metrics);
            text_width = metrics.width;
        } else {
            // Headless controls may not have a raster font yet. A conservative
            // half-em estimate still makes otherwise undiscoverable text
            // available without treating every short row as overflow.
            text_width = (float)strlen(hover_text) * lb->font_size * 0.5f;
        }
        overflow = available <= 0.0f || text_width > available;
    }

    if (overflow) {
        if (!lb->hover_tooltip_active) {
            char *saved = lb->base.tooltip_text ? vg_strdup(lb->base.tooltip_text) : NULL;
            if (lb->base.tooltip_text && !saved)
                return;
            free(lb->saved_tooltip_text);
            lb->saved_tooltip_text = saved;
            lb->hover_tooltip_active = true;
        }
        if (!lb->base.tooltip_text || strcmp(lb->base.tooltip_text, hover_text) != 0)
            vg_widget_set_tooltip_text(&lb->base, hover_text);
        return;
    }

    if (lb->hover_tooltip_active) {
        vg_widget_set_tooltip_text(&lb->base, lb->saved_tooltip_text);
        free(lb->saved_tooltip_text);
        lb->saved_tooltip_text = NULL;
        lb->hover_tooltip_active = false;
    }
}

/// @brief VTable measure: sizes to the widest item text (up to avail_w) and up to 5 visible rows
/// tall.
/// @details Text scanning is bounded for large lists; virtual and very large
///          lists prefer the offered width instead of fetching every row.
/// @param widget List-box widget base to measure.
/// @param avail_w Width offered by the parent layout.
/// @param avail_h Height offered by the parent; row count determines intrinsic
///                height before constraints.
static void listbox_measure(vg_widget_t *widget, float avail_w, float avail_h) {
    vg_listbox_t *lb = (vg_listbox_t *)widget;
    (void)avail_h;
    size_t count = lb->virtual_mode ? lb->total_item_count
                                    : (lb->item_count > 0 ? (size_t)lb->item_count : 0u);
    size_t visible = count > 0 ? (count < 5u ? count : 5u) : 5u;
    float measured_width = 200.0f;
    if (lb->font && !lb->virtual_mode && count <= VG_LISTBOX_MEASURE_TEXT_SCAN_LIMIT) {
        vg_text_metrics_t metrics = {0};
        for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
            if (!item->text)
                continue;
            vg_font_measure_text(lb->font, lb->font_size, item->text, &metrics);
            float candidate =
                metrics.width + vg_theme_get_current()->input.padding_h * 2.0f + 12.0f;
            if (candidate > measured_width)
                measured_width = candidate;
        }
    } else if (count > VG_LISTBOX_MEASURE_TEXT_SCAN_LIMIT && avail_w > 0.0f) {
        measured_width = avail_w;
    }
    if (avail_w > 0.0f && measured_width > avail_w)
        measured_width = avail_w;
    widget->measured_width = measured_width;
    widget->measured_height = (float)visible * lb->item_height;
    vg_widget_apply_constraints(widget);
}

/// @brief VTable arrange: sets the widget's final position and size from the layout pass.
/// @param widget List-box widget base to arrange.
/// @param x Final left coordinate.
/// @param y Final top coordinate.
/// @param w Final viewport width.
/// @param h Final viewport height.
static void listbox_arrange(vg_widget_t *widget, float x, float y, float w, float h) {
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
    listbox_sync_hover_tooltip((vg_listbox_t *)widget);
}

/// @brief VTable paint: draws background, selection highlights, row text, scrollbar thumb, and
/// focus border.
/// @details Virtual mode synchronizes only the visible range. Both modes clip
///          row text, apply zebra backgrounds and hover/selection pills, and
///          finish with a focus-sensitive border.
/// @param widget Arranged list-box widget base to paint.
/// @param canvas Backend canvas used by primitive and font drawing routines.
static void listbox_paint(vg_widget_t *widget, void *canvas) {
    vg_listbox_t *lb = (vg_listbox_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();
    vgfx_window_t win = (vgfx_window_t)canvas;
    int32_t x = (int32_t)widget->x, y = (int32_t)widget->y;
    int32_t w = (int32_t)widget->width, h = (int32_t)widget->height;
    float text_padding = theme->input.padding_h;

    /* Background */
    vgfx_fill_rect(win, x, y, w, h, lb->bg_color);
    if (w > 2)
        vgfx_fill_rect(win, x + 1, y + 1, w - 2, 1, vg_color_lighten(lb->bg_color, 0.05f));
    if (w <= 0 || h <= 0)
        return;
    vgfx_set_clip(win, x, y, w, h);

    /* Draw items */
    float item_y = widget->y - lb->scroll_y;
    float ih = lb->item_height;
    int32_t text_clip_x = x + (int32_t)text_padding;
    int32_t text_clip_y = y + 1;
    int32_t text_clip_w = w - (int32_t)text_padding - 6;
    int32_t text_clip_h = h - 2;

    if (lb->virtual_mode) {
        listbox_sync_virtual_cache(lb, widget->height);
        item_y = widget->y + (float)lb->visible_start * ih - lb->scroll_y;
        for (size_t i = 0; i < lb->visible_count; i++) {
            size_t index = lb->visible_start + i;
            float item_bottom = item_y + ih;
            if (item_bottom <= widget->y) {
                item_y += ih;
                continue;
            }
            if (item_y >= widget->y + widget->height)
                break;

            uint32_t zebra = ((index & 1u) == 0u)
                                 ? lb->item_bg
                                 : vg_color_blend(lb->item_bg, theme->colors.bg_secondary, 0.35f);
            bool is_selected = listbox_selection_get(lb, index);
            bool is_hover = (index == lb->hovered_index);

            int32_t iy = (int32_t)item_y;
            int32_t ih32 = (int32_t)ih;
            vgfx_fill_rect(win, x + 1, iy, w - 2, ih32, zebra);
            if (is_selected || is_hover) {
                uint32_t pill =
                    is_selected
                        ? vg_color_blend(lb->selected_bg, theme->colors.accent_primary, 0.28f)
                        : lb->hover_bg;
                vg_draw_round_rect_fill(win,
                                        (float)(x + 6),
                                        (float)(iy + 2),
                                        (float)(w - 12),
                                        (float)(ih32 - 4),
                                        theme->radius.lg,
                                        pill);
                if (is_selected)
                    vg_draw_round_rect_fill(win,
                                            (float)(x + 9),
                                            (float)(iy + 6),
                                            3.0f,
                                            (float)(ih32 - 12),
                                            1.5f,
                                            theme->colors.accent_primary);
            }

            if (lb->visible_cache[i].text && lb->font) {
                const char *paint_text =
                    listbox_fit_text(lb, lb->visible_cache[i].text, (float)text_clip_w);
                if (text_clip_w > 0 && text_clip_h > 0)
                    vgfx_set_clip(win, text_clip_x, text_clip_y, text_clip_w, text_clip_h);
                vg_font_draw_text(canvas,
                                  lb->font,
                                  lb->font_size,
                                  widget->x + text_padding,
                                  listbox_text_baseline(lb, item_y, ih),
                                  paint_text,
                                  lb->text_color);
                if (text_clip_w > 0 && text_clip_h > 0)
                    vgfx_set_clip(win, x, y, w, h);
            }

            item_y += ih;
        }
    } else {
        int row_index = 0;
        for (vg_listbox_item_t *item = lb->first_item; item; item = item->next) {
            float item_bottom = item_y + ih;
            if (item_bottom < widget->y) {
                item_y += ih;
                row_index++;
                continue;
            }
            if (item_y > widget->y + widget->height)
                break;

            uint32_t zebra = (row_index & 1) == 0
                                 ? lb->item_bg
                                 : vg_color_blend(lb->item_bg, theme->colors.bg_secondary, 0.35f);
            bool is_selected = item->selected;
            bool is_hover = (item == lb->hovered);

            int32_t iy = (int32_t)item_y;
            int32_t ih32 = (int32_t)ih;
            vgfx_fill_rect(win, x + 1, iy, w - 2, ih32, zebra);
            if (is_selected || is_hover) {
                uint32_t pill =
                    is_selected
                        ? vg_color_blend(lb->selected_bg, theme->colors.accent_primary, 0.28f)
                        : lb->hover_bg;
                vg_draw_round_rect_fill(win,
                                        (float)(x + 6),
                                        (float)(iy + 2),
                                        (float)(w - 12),
                                        (float)(ih32 - 4),
                                        theme->radius.lg,
                                        pill);
                if (is_selected)
                    vg_draw_round_rect_fill(win,
                                            (float)(x + 9),
                                            (float)(iy + 6),
                                            3.0f,
                                            (float)(ih32 - 12),
                                            1.5f,
                                            theme->colors.accent_primary);
            }

            if (item->text && lb->font) {
                const char *paint_text = listbox_fit_text(lb, item->text, (float)text_clip_w);
                if (text_clip_w > 0 && text_clip_h > 0)
                    vgfx_set_clip(win, text_clip_x, text_clip_y, text_clip_w, text_clip_h);
                vg_font_draw_text(canvas,
                                  lb->font,
                                  lb->font_size,
                                  widget->x + text_padding,
                                  listbox_text_baseline(lb, item_y, ih),
                                  paint_text,
                                  item->has_text_color ? item->text_color : lb->text_color);
                if (text_clip_w > 0 && text_clip_h > 0)
                    vgfx_set_clip(win, x, y, w, h);
            }

            item_y += ih;
            row_index++;
        }
    }

    vgfx_clear_clip(win);

    /* Border: use focus color when the listbox has keyboard focus */
    uint32_t border = (widget->state & VG_STATE_FOCUSED)
                          ? vg_theme_get_current()->colors.border_focus
                          : lb->border_color;
    vgfx_rect(win, x, y, w, h, border);
}

/// @brief VTable handle_event: dispatches mouse (click/scroll/keys) to selection and scroll logic.
/// @details Supports hover tooltips, wheel scrolling, double-click/Enter
///          activation, Ctrl/Super toggling, Shift ranges, and keyboard paging
///          while keeping the selected row visible.
/// @param widget List-box widget receiving input.
/// @param event Mutable event marked handled when consumed.
/// @return `true` when the list box consumed the event.
static bool listbox_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_listbox_t *lb = (vg_listbox_t *)widget;

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN: {
            uint32_t mods = event->modifiers;
            bool toggle =
                lb->multi_select && ((mods & VG_MOD_CTRL) != 0 || (mods & VG_MOD_SUPER) != 0);
            bool range = lb->multi_select && (mods & VG_MOD_SHIFT) != 0;
            size_t idx = 0;
            if (lb->virtual_mode) {
                if (listbox_item_at_y(lb, widget, event->mouse.y, &idx)) {
                    listbox_select_virtual_with_modifiers(lb, idx, toggle, range);
                    if (lb->on_select)
                        lb->on_select(widget, NULL, lb->on_select_data);
                    widget->needs_paint = true;
                    event->handled = true;
                    return true;
                }
            } else {
                /* Find which item was clicked */
                float ry = event->mouse.y + lb->scroll_y;
                int idx32 = (lb->item_height > 0) ? (int)(ry / lb->item_height) : -1;
                if (idx32 >= 0 && idx32 < lb->item_count) {
                    vg_listbox_item_t *item = listbox_item_at_index_nonvirtual(lb, (size_t)idx32);
                    if (item) {
                        listbox_select_nonvirtual_with_modifiers(lb, item, toggle, range);
                        if (lb->on_select)
                            lb->on_select(widget, item, lb->on_select_data);
                        widget->needs_paint = true;
                        event->handled = true;
                        return true;
                    }
                }
            }
            break;
        }

        case VG_EVENT_MOUSE_MOVE: {
            if (lb->virtual_mode) {
                listbox_sync_virtual_cache(lb, widget->height);
                size_t idx = 0;
                size_t old_hover = lb->hovered_index;
                lb->hovered_index = SIZE_MAX;
                if (listbox_item_at_y(lb, widget, event->mouse.y, &idx))
                    lb->hovered_index = idx;
                if (old_hover != lb->hovered_index)
                    widget->needs_paint = true;
            } else {
                float ry = event->mouse.y + lb->scroll_y;
                int idx = (lb->item_height > 0) ? (int)(ry / lb->item_height) : -1;
                vg_listbox_item_t *old_hover = lb->hovered;
                vg_listbox_item_t *hovered = NULL;
                if (idx >= 0 && idx < lb->item_count) {
                    hovered = lb->first_item;
                    for (int i = 0; i < idx && hovered; i++)
                        hovered = hovered->next;
                }
                lb->hovered = hovered;
                if (old_hover != lb->hovered)
                    widget->needs_paint = true;
            }
            listbox_sync_hover_tooltip(lb);
            break;
        }

        case VG_EVENT_MOUSE_LEAVE: {
            bool changed = lb->hovered != NULL || lb->hovered_index != SIZE_MAX;
            lb->hovered = NULL;
            lb->hovered_index = SIZE_MAX;
            listbox_sync_hover_tooltip(lb);
            if (changed)
                widget->needs_paint = true;
            break;
        }

        case VG_EVENT_MOUSE_WHEEL: {
            float scroll_delta = -event->wheel.delta_y * lb->item_height * vg_get_wheel_speed();
            lb->scroll_y += scroll_delta;
            listbox_clamp_scroll(lb, widget->height);
            widget->needs_paint = true;
            event->handled = true;
            return true;
        }

        case VG_EVENT_DOUBLE_CLICK: {
            if (lb->virtual_mode) {
                if (lb->selected_index != SIZE_MAX &&
                    listbox_selection_get(lb, lb->selected_index)) {
                    vg_widget_note_activation(widget);
                    if (lb->on_activate)
                        lb->on_activate(widget, NULL, lb->on_activate_data);
                    event->handled = true;
                    return true;
                }
            } else if (lb->selected) {
                vg_widget_note_activation(widget);
                if (lb->on_activate)
                    lb->on_activate(widget, lb->selected, lb->on_activate_data);
                event->handled = true;
                return true;
            }
            break;
        }

        case VG_EVENT_KEY_DOWN: {
            size_t total = lb->virtual_mode ? lb->total_item_count
                                            : (lb->item_count > 0 ? (size_t)lb->item_count : 0u);
            if (total == 0)
                return false;
            uint32_t mods = event->modifiers;
            bool range = lb->multi_select && (mods & VG_MOD_SHIFT) != 0;
            size_t page_items =
                (lb->item_height > 0.0f) ? (size_t)(widget->height / lb->item_height) : 8u;
            if (page_items < 1)
                page_items = 1;

            size_t current_idx = vg_listbox_get_selected_index(lb);
            bool has_current = current_idx != SIZE_MAX && current_idx < total;
            size_t new_idx = has_current ? current_idx : 0u;
            bool navigated = true;

            switch (event->key.key) {
                case VG_KEY_UP:
                    new_idx = (has_current && current_idx > 0) ? current_idx - 1 : 0;
                    break;
                case VG_KEY_DOWN:
                    new_idx = has_current && current_idx < total - 1 ? current_idx + 1 : 0;
                    break;
                case VG_KEY_HOME:
                    new_idx = 0;
                    break;
                case VG_KEY_END:
                    new_idx = total - 1;
                    break;
                case VG_KEY_PAGE_UP:
                    new_idx =
                        has_current && current_idx > page_items ? current_idx - page_items : 0;
                    break;
                case VG_KEY_PAGE_DOWN:
                    if (!has_current)
                        new_idx = page_items > 0 ? page_items - 1 : 0;
                    else if (current_idx > SIZE_MAX - page_items)
                        new_idx = total - 1;
                    else
                        new_idx = current_idx + page_items;
                    if (new_idx >= total)
                        new_idx = total - 1;
                    break;
                case VG_KEY_ENTER:
                    if (lb->virtual_mode) {
                        if (lb->selected_index != SIZE_MAX &&
                            listbox_selection_get(lb, lb->selected_index)) {
                            vg_widget_note_activation(widget);
                            if (lb->on_activate)
                                lb->on_activate(widget, NULL, lb->on_activate_data);
                        }
                    } else if (lb->selected) {
                        vg_widget_note_activation(widget);
                        if (lb->on_activate)
                            lb->on_activate(widget, lb->selected, lb->on_activate_data);
                    }
                    event->handled = true;
                    return true;
                default:
                    navigated = false;
                    break;
            }

            if (navigated && (!has_current || new_idx != current_idx) && new_idx < total) {
                if (lb->virtual_mode) {
                    listbox_select_virtual_with_modifiers(lb, new_idx, false, range);
                    if (lb->on_select)
                        lb->on_select(widget, NULL, lb->on_select_data);
                } else {
                    vg_listbox_item_t *item = listbox_item_at_index_nonvirtual(lb, new_idx);
                    if (item) {
                        listbox_select_nonvirtual_with_modifiers(lb, item, false, range);
                        if (lb->on_select)
                            lb->on_select(widget, item, lb->on_select_data);
                    }
                }
                /* Scroll the selected item into view */
                float item_top = new_idx * lb->item_height;
                float item_bot = item_top + lb->item_height;
                if (item_top < lb->scroll_y)
                    lb->scroll_y = item_top;
                else if (item_bot > lb->scroll_y + widget->height)
                    lb->scroll_y = item_bot - widget->height;
                widget->needs_paint = true;
                event->handled = true;
                return true;
            }
            break;
        }

        default:
            break;
    }
    return false;
}

/// @brief VTable can_focus: returns true when the listbox is both enabled and visible.
/// @param widget Candidate list-box widget.
/// @return `true` when keyboard focus is permitted.
static bool listbox_can_focus(vg_widget_t *widget) {
    return widget->enabled && widget->visible;
}

/// @brief Create a list box widget.
/// @details Initializes theme-derived colors, typography, row height, empty
///          normal-mode storage, and no current or anchor selection.
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_listbox_t, or NULL on allocation failure.
vg_listbox_t *vg_listbox_create(vg_widget_t *parent) {
    vg_listbox_t *listbox = calloc(1, sizeof(vg_listbox_t));
    if (!listbox)
        return NULL;

    vg_widget_init(&listbox->base, VG_WIDGET_LISTBOX, &g_listbox_vtable);

    // Default appearance — scale pixel constants by ui_scale for HiDPI displays.
    vg_theme_t *_lb_theme = vg_theme_get_current();
    listbox->font = _lb_theme->typography.font_regular;
    listbox->font_size = _lb_theme->typography.size_normal;
    listbox->bg_color = _lb_theme->colors.bg_primary;
    listbox->item_bg = _lb_theme->colors.bg_primary;
    listbox->selected_bg = _lb_theme->colors.bg_selected;
    listbox->hover_bg = _lb_theme->colors.bg_hover;
    listbox->text_color = _lb_theme->colors.fg_primary;
    listbox->border_color = _lb_theme->colors.border_primary;
    listbox->selected_index = SIZE_MAX;
    listbox->prev_selected_index = SIZE_MAX;
    listbox->anchor_selected_index = SIZE_MAX;
    listbox->hovered_index = SIZE_MAX;
    listbox->item_height = listbox_default_item_height(listbox);

    if (parent) {
        vg_widget_add_child(parent, &listbox->base);
    }

    return listbox;
}

/// @brief Append an item to the list box.
/// @details The item record and its text are owned by the list box. @p user_data
///          remains caller-owned unless ownership is changed through other API.
/// @param listbox   The list box to modify.
/// @param text      Display text for the item (copied internally).
/// @param user_data Opaque pointer stored in the item; not dereferenced here.
/// @return Pointer to the new item, or NULL on allocation failure or invalid args.
vg_listbox_item_t *vg_listbox_add_item(vg_listbox_t *listbox, const char *text, void *user_data) {
    if (!listbox || !text)
        return NULL;

    vg_listbox_item_t *item = calloc(1, sizeof(vg_listbox_item_t));
    if (!item)
        return NULL;

    item->text = vg_strdup(text);
    if (!item->text) {
        free(item);
        return NULL;
    }
    item->text_len = strlen(item->text);
    item->magic = VG_LISTBOX_ITEM_MAGIC;
    item->owner = listbox;
    item->user_data = user_data;

    // Add to end of list
    if (listbox->last_item) {
        listbox->last_item->next = item;
        item->prev = listbox->last_item;
        listbox->last_item = item;
    } else {
        listbox->first_item = listbox->last_item = item;
    }
    listbox->item_count++;
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;

    return item;
}

/// @brief Remove and retire a single item from the list box.
/// @details Selection, hover, and anchor references are detached before the
///          item's owned payload is released and its stable record is moved to
///          the retirement chain.
/// @param listbox The list box to modify.
/// @param item    Item to remove; must be live and owned by @p listbox.
void vg_listbox_remove_item(vg_listbox_t *listbox, vg_listbox_item_t *item) {
    if (!listbox || !vg_listbox_item_is_live(item) || item->owner != listbox)
        return;

    if (item->prev)
        item->prev->next = item->next;
    else
        listbox->first_item = item->next;

    if (item->next)
        item->next->prev = item->prev;
    else
        listbox->last_item = item->prev;

    if (listbox->selected == item) {
        listbox->selected = NULL;
        listbox_note_selection_changed(listbox);
    }
    if (listbox->hovered == item) {
        listbox->hovered = NULL;
        listbox_sync_hover_tooltip(listbox);
    }
    if (listbox->anchor_selected == item)
        listbox->anchor_selected = listbox->selected;
    if (listbox->prev_selected == item)
        listbox->prev_selected = NULL;

    listbox_retire_item(listbox, item);
    listbox->item_count--;
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;
}

/// @brief Remove and retire all items from the list box.
/// @details Also detaches any virtual model, releases its cache and bitmap,
///          restores tooltip state, clears selection, and schedules layout and
///          painting.
/// @param listbox The list box to clear.
void vg_listbox_clear(vg_listbox_t *listbox) {
    if (!listbox)
        return;

    listbox_notify_virtual_unbound(listbox);

    bool had_selection = listbox_has_nonvirtual_selection(listbox);
    bool had_virtual_selection =
        listbox->virtual_mode && (listbox->selected_index != SIZE_MAX ||
                                  listbox_first_selected_virtual(listbox) != SIZE_MAX);
    vg_listbox_item_t *item = listbox->first_item;
    while (item) {
        vg_listbox_item_t *next = item->next;
        listbox_retire_item(listbox, item);
        item = next;
    }

    listbox->first_item = listbox->last_item = NULL;
    listbox->selected = listbox->hovered = NULL;
    listbox->prev_selected = NULL;
    listbox->anchor_selected = NULL;
    listbox->item_count = 0;
    if (listbox->virtual_mode) {
        listbox_free_virtual_cache(listbox);
        free(listbox->visible_cache);
        listbox->visible_cache = NULL;
        listbox->cache_capacity = 0;
        free(listbox->selection_bitmap);
        listbox->selection_bitmap = NULL;
        listbox->selection_bitmap_size = 0;
        listbox->selection_bitmap_capacity = 0;
        listbox->virtual_mode = false;
        listbox->total_item_count = 0;
        listbox->selected_index = SIZE_MAX;
        listbox->prev_selected_index = SIZE_MAX;
        listbox->anchor_selected_index = SIZE_MAX;
        listbox->hovered_index = SIZE_MAX;
        listbox->visible_start = 0;
        listbox->visible_count = 0;
        listbox->scroll_y = 0.0f;
    }
    listbox_sync_hover_tooltip(listbox);
    if (had_selection || had_virtual_selection)
        listbox_note_selection_changed(listbox);
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;
}

/// @brief Set the selected item; fires on_select if the item changed.
/// @details In single-select mode all other item flags are cleared. Passing
///          `NULL` deselects every row. Invalid or foreign item records are
///          ignored.
/// @param listbox The list box to update.
/// @param item    Item to select (must be live and owned by @p listbox), or NULL to deselect.
void vg_listbox_select(vg_listbox_t *listbox, vg_listbox_item_t *item) {
    if (!listbox)
        return;
    if (item && (!vg_listbox_item_is_live(item) || item->owner != listbox))
        return;
    vg_listbox_item_t *old_selected = listbox->selected;
    bool item_was_selected = item && item->selected;
    bool selection_changed = false;

    if (!listbox->multi_select || item == NULL) {
        bool had_other_selected =
            item ? listbox_has_nonvirtual_selection_except(listbox, item) : false;
        bool had_selected_flags = listbox_clear_nonvirtual_selection(listbox);
        selection_changed = (old_selected != item) ||
                            (item ? had_other_selected : had_selected_flags) ||
                            (item && !item_was_selected);
    }

    listbox->selected = item;
    if (item) {
        selection_changed = selection_changed || !item_was_selected;
        item->selected = true;
        listbox->anchor_selected = item;
        if (listbox->on_select) {
            listbox->on_select(&listbox->base, item, listbox->on_select_data);
        }
    } else {
        listbox->anchor_selected = NULL;
    }
    if (selection_changed || listbox->selected != old_selected)
        listbox_note_selection_changed(listbox);
    listbox->base.needs_paint = true;
}

/// @brief Return the currently selected item, or NULL if nothing is selected.
///
/// @param listbox The list box to query.
/// @return Pointer to the selected item, or NULL.
vg_listbox_item_t *vg_listbox_get_selected(vg_listbox_t *listbox) {
    return listbox ? listbox->selected : NULL;
}

/// @brief Override the list item font and size.
///
/// @param listbox The list box to configure.
/// @param font    Font to use; NULL keeps the existing font.
/// @param size    Font size in pixels; <= 0 falls back to the theme normal size.
void vg_listbox_set_font(vg_listbox_t *listbox, vg_font_t *font, float size) {
    if (!listbox)
        return;
    listbox->font = font;
    listbox->font_size = size > 0 ? size : vg_theme_get_current()->typography.size_normal;
    {
        float min_item_height = listbox_default_item_height(listbox);
        if (listbox->item_height < min_item_height)
            listbox->item_height = min_item_height;
    }
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;
}

/// @brief Set the selection callback invoked when the selected item changes.
///
/// @param listbox   The list box to configure.
/// @param callback  Function called with (widget, item, user_data) on selection.
/// @param user_data Opaque pointer passed to @p callback.
void vg_listbox_set_on_select(vg_listbox_t *listbox,
                              vg_listbox_callback_t callback,
                              void *user_data) {
    if (!listbox)
        return;
    listbox->on_select = callback;
    listbox->on_select_data = user_data;
}

//=============================================================================
// Virtual Scrolling
//=============================================================================

/// @brief Enables or disables virtual-scroll mode, allocating a selection bitmap and visible-row
/// cache for @p total_count items.
/// @details Switching modes detaches an externally bound model, resets selection
///          and scroll state, and invalidates layout. Allocation failure leaves
///          virtual mode disabled.
/// @param listbox List box to reconfigure.
/// @param enabled `true` to use provider-backed logical rows.
/// @param total_count Logical row count for virtual mode.
/// @param item_height Positive row height override; non-positive values retain
///                    the current height.
void vg_listbox_set_virtual_mode(vg_listbox_t *listbox,
                                 bool enabled,
                                 size_t total_count,
                                 float item_height) {
    if (!listbox)
        return;

    if (enabled && (total_count > SIZE_MAX - 7u || (item_height > 0.0f && !isfinite(item_height))))
        return;

    listbox_notify_virtual_unbound(listbox);

    size_t old_virtual_selected = listbox->selected_index;
    vg_listbox_item_t *old_selected = listbox->selected;
    listbox->virtual_mode = enabled;
    listbox->total_item_count = enabled ? total_count : 0;
    listbox->hovered_index = SIZE_MAX;
    listbox->prev_selected_index = SIZE_MAX;
    listbox->anchor_selected_index = SIZE_MAX;
    if (item_height > 0) {
        listbox->item_height = item_height;
    }

    // Initialize selection bitmap
    if (enabled && total_count > 0) {
        size_t selection_bytes = listbox_selection_bitmap_bytes(total_count);
        uint8_t *new_selection = calloc(selection_bytes, 1);
        if (!new_selection) {
            listbox->virtual_mode = false;
            listbox->total_item_count = 0;
            listbox->selected_index = SIZE_MAX;
            listbox->prev_selected_index = SIZE_MAX;
            listbox->anchor_selected_index = SIZE_MAX;
            listbox->base.needs_paint = true;
            listbox->base.needs_layout = true;
            return;
        }

        free(listbox->selection_bitmap);
        listbox->selection_bitmap = new_selection;
        listbox->selection_bitmap_size = total_count;
        listbox->selection_bitmap_capacity = total_count;
        listbox->selected_index = SIZE_MAX; // None selected
        listbox->prev_selected_index = SIZE_MAX;
        listbox->anchor_selected_index = SIZE_MAX;

        // Allocate visible-item cache (capped at 64 — enough for one viewport page)
        size_t cap = total_count < 64 ? total_count : 64;
        free(listbox->visible_cache);
        listbox->visible_cache = calloc(cap, sizeof(vg_listbox_cache_entry_t));
        listbox->cache_capacity = listbox->visible_cache ? cap : 0;
    } else {
        listbox_free_virtual_cache(listbox);
        free(listbox->visible_cache);
        listbox->visible_cache = NULL;
        listbox->cache_capacity = 0;
        free(listbox->selection_bitmap);
        listbox->selection_bitmap = NULL;
        listbox->selection_bitmap_size = 0;
        listbox->selection_bitmap_capacity = 0;
        listbox->selected_index = SIZE_MAX;
        listbox->prev_selected_index = SIZE_MAX;
        listbox->anchor_selected_index = SIZE_MAX;
    }

    // Reset scroll and invalidate
    listbox->scroll_y = 0;
    listbox->visible_start = 0;
    listbox->visible_count = 0;
    if (old_selected || old_virtual_selected != SIZE_MAX)
        listbox_note_selection_changed(listbox);
    listbox->base.needs_paint = true;
    listbox->base.needs_layout = true;
}

/// @brief Set the virtual-mode data provider callback used to populate visible rows.
/// @details Replacing a provider associated with an external-model unbind
///          callback first notifies the previous binding.
/// @param listbox   The list box to configure (must be in virtual mode).
/// @param provider  Callback that fills a vg_listbox_cache_entry_t for a given index.
/// @param user_data Opaque pointer passed to @p provider.
void vg_listbox_set_data_provider(vg_listbox_t *listbox,
                                  vg_listbox_data_provider_t provider,
                                  void *user_data) {
    if (!listbox)
        return;

    if (listbox->virtual_unbind &&
        (listbox->data_provider != provider || listbox->data_provider_user_data != user_data))
        listbox_notify_virtual_unbound(listbox);

    listbox->data_provider = provider;
    listbox->data_provider_user_data = user_data;
}

/// @brief Install a non-owning external virtual model atomically.
/// @details Selection and visible-cache allocations are completed before the
///          previous model is notified and detached. The provider's returned
///          text is copied when a row enters the visible cache.
/// @param listbox List box receiving the model.
/// @param total_count Logical number of model rows.
/// @param item_height Finite positive row height in pixels.
/// @param provider Callback supplying row data on demand.
/// @param user_data Opaque context passed to @p provider and @p on_unbind.
/// @param on_unbind Optional callback notified exactly once when detached.
/// @return `true` after the model and replacement storage are installed;
///         `false` with the previous binding intact on validation or allocation
///         failure.
bool vg_listbox_bind_virtual_model(vg_listbox_t *listbox,
                                   size_t total_count,
                                   float item_height,
                                   vg_listbox_data_provider_t provider,
                                   void *user_data,
                                   vg_listbox_virtual_unbind_callback_t on_unbind) {
    if (!listbox || !provider || total_count > SIZE_MAX - 7u || item_height <= 0.0f ||
        !isfinite(item_height))
        return false;

    size_t selection_bytes = listbox_selection_bitmap_bytes(total_count);
    uint8_t *new_selection = NULL;
    if (selection_bytes > 0) {
        new_selection = (uint8_t *)calloc(selection_bytes, 1);
        if (!new_selection)
            return false;
    }

    size_t cache_capacity = total_count < 64u ? total_count : 64u;
    vg_listbox_cache_entry_t *new_cache = NULL;
    if (cache_capacity > 0) {
        new_cache =
            (vg_listbox_cache_entry_t *)calloc(cache_capacity, sizeof(vg_listbox_cache_entry_t));
        if (!new_cache) {
            free(new_selection);
            return false;
        }
    }

    bool had_selection = listbox->virtual_mode && listbox->selected_index != SIZE_MAX;
    listbox_notify_virtual_unbound(listbox);
    listbox_free_virtual_cache(listbox);
    free(listbox->visible_cache);
    free(listbox->selection_bitmap);

    listbox->virtual_mode = true;
    listbox->total_item_count = total_count;
    listbox->item_height = item_height;
    listbox->data_provider = provider;
    listbox->data_provider_user_data = user_data;
    listbox->virtual_unbind = on_unbind;
    listbox->virtual_unbind_user_data = user_data;
    listbox->visible_cache = new_cache;
    listbox->cache_capacity = cache_capacity;
    listbox->selection_bitmap = new_selection;
    listbox->selection_bitmap_size = total_count;
    listbox->selection_bitmap_capacity = total_count;
    listbox->selected_index = SIZE_MAX;
    listbox->prev_selected_index = SIZE_MAX;
    listbox->anchor_selected_index = SIZE_MAX;
    listbox->hovered_index = SIZE_MAX;
    listbox->visible_start = 0;
    listbox->visible_count = 0;
    listbox->scroll_y = 0.0f;
    if (had_selection)
        listbox_note_selection_changed(listbox);
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;
    return true;
}

/// @brief Detach an external ListBox model and release virtual-mode storage.
/// @details Invokes the registered unbind callback once, clears provider
///          references and selection, resets scrolling, and schedules layout.
/// @param listbox List box whose model is detached; `NULL` is ignored.
void vg_listbox_clear_virtual_model(vg_listbox_t *listbox) {
    if (!listbox)
        return;

    bool had_selection = listbox->virtual_mode && listbox->selected_index != SIZE_MAX;
    listbox_notify_virtual_unbound(listbox);
    listbox_free_virtual_cache(listbox);
    free(listbox->visible_cache);
    free(listbox->selection_bitmap);
    listbox->visible_cache = NULL;
    listbox->cache_capacity = 0;
    listbox->selection_bitmap = NULL;
    listbox->selection_bitmap_size = 0;
    listbox->selection_bitmap_capacity = 0;
    listbox->virtual_mode = false;
    listbox->total_item_count = 0;
    listbox->data_provider = NULL;
    listbox->data_provider_user_data = NULL;
    listbox->selected_index = SIZE_MAX;
    listbox->prev_selected_index = SIZE_MAX;
    listbox->anchor_selected_index = SIZE_MAX;
    listbox->hovered_index = SIZE_MAX;
    listbox->visible_start = 0;
    listbox->visible_count = 0;
    listbox->scroll_y = 0.0f;
    if (had_selection)
        listbox_note_selection_changed(listbox);
    listbox->base.needs_layout = true;
    listbox->base.needs_paint = true;
}

/// @brief Update the total item count in virtual-scroll mode.
///
/// @details Resizes the selection bitmap if @p count exceeds the current size
///          and clamps the scroll position to the new content height.
///
/// @param listbox The list box to update (must be in virtual mode).
/// @param count   New total number of items.
void vg_listbox_set_total_count(vg_listbox_t *listbox, size_t count) {
    if (!listbox || !listbox->virtual_mode)
        return;
    if (count > SIZE_MAX - 7u)
        return;

    bool truncated_selection = false;

    // Resize selection bitmap if needed. Capacity stays separate from logical
    // size so shrinking cannot leave stale selected rows addressable.
    if (count > listbox->selection_bitmap_capacity) {
        size_t old_bytes = listbox_selection_bitmap_bytes(listbox->selection_bitmap_capacity);
        size_t new_bytes = listbox_selection_bitmap_bytes(count);
        uint8_t *new_bitmap = realloc(listbox->selection_bitmap, new_bytes);
        if (!new_bitmap)
            return;
        memset(new_bitmap + old_bytes, 0, new_bytes - old_bytes);
        listbox->selection_bitmap = new_bitmap;
        listbox->selection_bitmap_capacity = count;
    } else if (count == 0) {
        free(listbox->selection_bitmap);
        listbox->selection_bitmap = NULL;
        listbox->selection_bitmap_capacity = 0;
    } else if (count < listbox->selection_bitmap_size) {
        for (size_t i = count; i < listbox->selection_bitmap_size; i++) {
            if (listbox_selection_get(listbox, i)) {
                truncated_selection = true;
                break;
            }
        }
        size_t new_bytes = listbox_selection_bitmap_bytes(count);
        size_t old_bytes = listbox_selection_bitmap_bytes(listbox->selection_bitmap_size);
        if (new_bytes < old_bytes)
            memset(listbox->selection_bitmap + new_bytes, 0, old_bytes - new_bytes);
    }

    listbox->selection_bitmap_size = count;
    listbox_selection_mask_tail(listbox);
    listbox->total_item_count = count;

    // Reset selection if out of bounds
    if (listbox->selected_index >= count) {
        listbox->selected_index = SIZE_MAX;
        listbox->prev_selected_index = SIZE_MAX;
        listbox->anchor_selected_index = SIZE_MAX;
        listbox_note_selection_changed(listbox);
        truncated_selection = false;
    }
    if (listbox->anchor_selected_index >= count)
        listbox->anchor_selected_index = SIZE_MAX;
    if (truncated_selection)
        listbox_note_selection_changed(listbox);

    // Clamp scroll position
    float max_scroll = count * listbox->item_height - listbox->base.height;
    if (max_scroll < 0)
        max_scroll = 0;
    if (listbox->scroll_y > max_scroll) {
        listbox->scroll_y = max_scroll;
    }

    listbox->base.needs_paint = true;
}

/// @brief Invalidate the entire visible-item cache, forcing a re-fetch on next paint.
///
/// @param listbox The list box to invalidate.
void vg_listbox_invalidate_items(vg_listbox_t *listbox) {
    if (!listbox)
        return;

    // Clear cache
    if (listbox->visible_cache) {
        for (size_t i = 0; i < listbox->cache_capacity; i++) {
            free(listbox->visible_cache[i].text);
            listbox->visible_cache[i].text = NULL;
        }
    }

    // Force re-fetch on next paint
    listbox->visible_start = SIZE_MAX;
    listbox->visible_count = 0;
    listbox->base.needs_paint = true;
}

/// @brief Invalidate a single item's cache entry by index; forces re-fetch on next paint.
///
/// @param listbox The list box to update (must be in virtual mode).
/// @param index   Zero-based index of the item to invalidate.
void vg_listbox_invalidate_item(vg_listbox_t *listbox, size_t index) {
    if (!listbox || !listbox->virtual_mode)
        return;

    // Check if item is in visible cache
    if (index >= listbox->visible_start &&
        index < listbox->visible_start + listbox->visible_count) {
        size_t cache_index = index - listbox->visible_start;
        if (cache_index < listbox->cache_capacity && listbox->visible_cache) {
            free(listbox->visible_cache[cache_index].text);
            listbox->visible_cache[cache_index].text = NULL;
        }
    }

    listbox->base.needs_paint = true;
}

/// @brief Return the first row in the current virtual viewport without fetching model data.
/// @param listbox Virtual list box to inspect.
/// @return First logical visible index, or zero for an empty/non-virtual list.
size_t vg_listbox_get_visible_first(vg_listbox_t *listbox) {
    size_t first = 0;
    listbox_compute_virtual_range(listbox, listbox ? listbox->base.height : 0.0f, &first, NULL);
    return first;
}

/// @brief Return the viewport-sized virtual cache row count without fetching model data.
/// @param listbox Virtual list box to inspect.
/// @return Number of logical rows in the bounded materialization range.
size_t vg_listbox_get_visible_count(vg_listbox_t *listbox) {
    size_t count = 0;
    listbox_compute_virtual_range(listbox, listbox ? listbox->base.height : 0.0f, NULL, &count);
    return count;
}

/// @brief Select the item at a zero-based index (works in both normal and virtual mode).
///
/// @param listbox The list box to update.
/// @param index   Zero-based item index to select.
void vg_listbox_select_index(vg_listbox_t *listbox, size_t index) {
    if (!listbox)
        return;

    if (!listbox->virtual_mode) {
        // Non-virtual mode: find item at index
        vg_listbox_item_t *item = listbox->first_item;
        for (size_t i = 0; i < index && item; i++) {
            item = item->next;
        }
        if (!item)
            return;
        vg_listbox_select(listbox, item);
        return;
    }

    // Virtual mode
    if (index == SIZE_MAX) {
        bool changed = listbox_clear_virtual_selection(listbox);
        changed = changed || listbox->selected_index != SIZE_MAX;
        listbox->selected_index = SIZE_MAX;
        listbox->prev_selected_index = SIZE_MAX;
        listbox->anchor_selected_index = SIZE_MAX;
        if (changed)
            listbox_note_selection_changed(listbox);
        listbox->base.needs_paint = true;
        return;
    }
    if (index >= listbox->total_item_count)
        return;

    listbox_select_virtual_with_modifiers(listbox, index, false, false);

    listbox->base.needs_paint = true;
}

/// @brief Return the zero-based index of the selected item.
///
/// @param listbox The list box to query.
/// @return Zero-based index of the selected item, or SIZE_MAX if nothing is selected.
size_t vg_listbox_get_selected_index(vg_listbox_t *listbox) {
    if (!listbox)
        return SIZE_MAX;

    if (!listbox->virtual_mode) {
        // Non-virtual mode: find index of selected item
        if (!listbox->selected)
            return SIZE_MAX;
        size_t index = 0;
        for (vg_listbox_item_t *item = listbox->first_item; item; item = item->next) {
            if (item == listbox->selected)
                return index;
            index++;
        }
        return SIZE_MAX;
    }

    return listbox->selected_index;
}

/// @brief Scroll to the first row without changing selection.
/// @param listbox List box whose scroll offset is reset; `NULL` is ignored.
void vg_listbox_scroll_to_top(vg_listbox_t *listbox) {
    if (!listbox)
        return;
    if (listbox->scroll_y == 0.0f)
        return;
    listbox->scroll_y = 0.0f;
    listbox->base.needs_paint = true;
}

/// @brief Scroll to the last row without changing selection.
/// @details Computes the maximum offset from logical row count, row height, and
///          current viewport height, then clamps it through the shared scroll
///          policy.
/// @param listbox List box to scroll; `NULL` is ignored.
void vg_listbox_scroll_to_bottom(vg_listbox_t *listbox) {
    if (!listbox)
        return;
    float viewport_height = listbox->base.height;
    if (viewport_height < 0.0f)
        viewport_height = 0.0f;
    float old_scroll = listbox->scroll_y;
    float max_scroll = 0.0f;
    size_t count = listbox_virtual_item_count(listbox);
    if (count > 0 && listbox->item_height > 0.0f) {
        max_scroll = (float)count * listbox->item_height - viewport_height;
        if (max_scroll < 0.0f)
            max_scroll = 0.0f;
    }
    listbox->scroll_y = max_scroll;
    listbox_clamp_scroll(listbox, viewport_height);
    if (listbox->scroll_y != old_scroll)
        listbox->base.needs_paint = true;
}
