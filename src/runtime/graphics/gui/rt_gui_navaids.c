//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_navaids.c
// Purpose: Navigation/overview GUI widgets — Breadcrumb (path navigation) and
//          Minimap (document overview) — for the Zanna runtime. Split out of
//          rt_gui_features.c; shares GUI types via rt_gui_internal.h.
//
// Key invariants:
//   - Mirrors rt_gui_features.c's ZANNA_ENABLE_GRAPHICS guard: real widgets
//     when graphics is enabled, no-op stubs otherwise.
//   - Widgets are self-contained — no cross-feature coupling (the command
//     palette interaction stays with drag-and-drop in rt_gui_features.c).
//
// Ownership/Lifetime:
//   - GC-managed wrappers own detached backing widgets; after attachment the widget tree may retire
//     a backing widget first, and the vtable override clears the wrapper's borrowed back-pointer.
//   - Breadcrumb labels and per-item click payloads are copied; Minimap editor targets are borrowed
//     and proactively disconnected before an editor subtree is destroyed.
//
// Links: src/runtime/graphics/gui/rt_gui_features.c (other Phase 6-8 features),
//        src/runtime/graphics/gui/rt_gui_internal.h (shared GUI types + API)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements Breadcrumb navigation and Minimap overview runtime widgets.
 *
 * @details Managed wrappers authenticate backing toolkit widgets, bridge
 *          copied Breadcrumb labels and click payloads, synchronize Minimap
 *          editor targets and viewport state, and clear borrowed back-pointers
 *          when widget-tree ownership retires lower objects before wrappers.
 */

#include "rt_gui_internal.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

//=============================================================================
// Phase 8: Breadcrumb Implementation
//=============================================================================

/// @brief Magic value authenticating live Breadcrumb wrapper state.
#define RT_BREADCRUMB_DATA_MAGIC UINT64_C(0x5254425245414443)

/// @brief Managed Breadcrumb wrapper and edge-triggered click snapshot.
typedef struct rt_breadcrumb_data {
    uint64_t magic;                     ///< Must equal @ref RT_BREADCRUMB_DATA_MAGIC while live.
    vg_breadcrumb_t *breadcrumb;        ///< Borrowed backing widget, or NULL after retirement.
    int64_t clicked_index;              ///< Last captured zero-based segment index.
    rt_gui_string_data_t *clicked_data; ///< Owned length-aware clicked-item payload.
    int64_t was_clicked;                ///< Pending edge-triggered click flag.
} rt_breadcrumb_data_t;

/// @brief Original lower Breadcrumb vtable delegated to by the runtime override.
static const vg_widget_vtable_t *s_breadcrumb_original_vtable = NULL;
/// @brief Process-wide Breadcrumb vtable copy with lifetime interception.
static vg_widget_vtable_t s_breadcrumb_runtime_vtable;

/// @brief Authenticate a Breadcrumb handle via its magic tag (NULL if not).
/// @param crumb Candidate GC-managed wrapper handle.
/// @return Borrowed live wrapper, or NULL when the magic tag is absent.
static rt_breadcrumb_data_t *rt_breadcrumb_checked(void *crumb) {
    rt_breadcrumb_data_t *data = (rt_breadcrumb_data_t *)crumb;
    return data && data->magic == RT_BREADCRUMB_DATA_MAGIC ? data : NULL;
}

/// @brief Widget vtable `destroy` override — clears the back-pointer before calling the
///        original vtable's destroy so dangling pointer re-use in the teardown is safe.
/// @param widget Backing Breadcrumb widget being destroyed by its owner tree.
static void rt_breadcrumb_widget_destroy(vg_widget_t *widget) {
    rt_breadcrumb_data_t *data = widget ? (rt_breadcrumb_data_t *)widget->user_data : NULL;
    if (data && data->breadcrumb == (vg_breadcrumb_t *)widget) {
        data->breadcrumb = NULL;
        free(data->clicked_data);
        data->clicked_data = NULL;
        data->clicked_index = -1;
        data->was_clicked = 0;
    }
    if (s_breadcrumb_original_vtable && s_breadcrumb_original_vtable->destroy)
        s_breadcrumb_original_vtable->destroy(widget);
}

/// @brief Free the cached clicked-item string and reset the click index/flag,
///        clearing any pending breadcrumb click for the next poll.
/// @param data Breadcrumb wrapper whose owned click snapshot should be cleared; NULL is a no-op.
static void rt_breadcrumb_clear_click_state(rt_breadcrumb_data_t *data) {
    if (!data)
        return;
    free(data->clicked_data);
    data->clicked_data = NULL;
    data->clicked_index = -1;
    data->was_clicked = 0;
}

/// @brief Release breadcrumb widget and free the clicked-item string buffer.
/// @details Safe for explicit destruction and finalization: clears the magic tag after releasing
///          the backing widget and owned click snapshot so subsequent checked calls fail closed.
/// @param data Breadcrumb wrapper to dispose; NULL is a no-op.
static void rt_breadcrumb_dispose(rt_breadcrumb_data_t *data) {
    if (!data)
        return;
    if (data->breadcrumb) {
        vg_breadcrumb_destroy(data->breadcrumb);
        data->breadcrumb = NULL;
    }
    rt_breadcrumb_clear_click_state(data);
    data->magic = 0;
}

/// @brief GC finalizer — releases the backing widget and click payload.
/// @param crumb GC-managed Breadcrumb wrapper being finalized.
static void rt_breadcrumb_finalize(void *crumb) {
    rt_breadcrumb_data_t *data = (rt_breadcrumb_data_t *)crumb;
    if (!data)
        return;
    rt_breadcrumb_dispose(data);
}

/// @brief Breadcrumb segment click callback — captures index + per-item data for polling.
///
/// Each breadcrumb item may carry an opaque user-data string
/// (e.g. a path component). On click we deep-copy that into the
/// wrapper so the language layer can read it via
/// `rt_breadcrumb_get_clicked_data` without lifetime concerns.
/// @param bc Borrowed backing Breadcrumb that reported the activation.
/// @param index Zero-based activated segment index.
/// @param user_data Borrowed owning @ref rt_breadcrumb_data_t wrapper.
static void rt_breadcrumb_on_click(vg_breadcrumb_t *bc, int index, void *user_data) {
    rt_breadcrumb_data_t *data = (rt_breadcrumb_data_t *)user_data;
    if (!data || data->magic != RT_BREADCRUMB_DATA_MAGIC)
        return;

    data->clicked_index = index;
    data->was_clicked = 1;

    // Store the clicked item's data
    free(data->clicked_data);
    data->clicked_data = NULL;

    if (index >= 0 && (size_t)index < bc->item_count) {
        if (bc->items[index].owns_user_data &&
            rt_gui_string_data_is_owned(bc->items[index].user_data))
            data->clicked_data = rt_gui_string_data_new_bytes(
                ((rt_gui_string_data_t *)bc->items[index].user_data)->bytes,
                ((rt_gui_string_data_t *)bc->items[index].user_data)->len);
    }
}

/// @brief Append a copied path segment as both visible label and owned click data.
/// @details Allocates label and length-aware payload copies before appending. The lower widget
///          copies the label and takes payload ownership only when its item count increases;
///          all temporary allocations are released on rejection.
/// @param breadcrumb Borrowed backing widget receiving the segment.
/// @param segment Borrowed byte span, not required to be independently terminated.
/// @param len Positive number of bytes to copy.
static void rt_breadcrumb_push_path_segment(vg_breadcrumb_t *breadcrumb,
                                            const char *segment,
                                            size_t len) {
    if (!breadcrumb || !segment || len == 0)
        return;
    if (len > SIZE_MAX - 1)
        return;
    char *label = (char *)malloc(len + 1);
    if (!label)
        return;
    memcpy(label, segment, len);
    label[len] = '\0';

    rt_gui_string_data_t *payload = rt_gui_string_data_new_bytes(segment, len);
    if (!payload) {
        free(label);
        return;
    }

    size_t prev_count = breadcrumb->item_count;
    vg_breadcrumb_push(breadcrumb, label, payload);
    if (breadcrumb->item_count > prev_count) {
        breadcrumb->items[breadcrumb->item_count - 1].owns_user_data = true;
    } else {
        free(payload);
    }
    free(label);
}

/// @brief Create a breadcrumb navigation widget (e.g. `Home > Docs > Project`).
///
/// Returns a wrapper struct (`rt_breadcrumb_data_t`) so callers can
/// poll segment clicks via `rt_breadcrumb_was_item_clicked`.
/// @param parent App/container parent handle, or NULL for a detached widget.
/// @return New GC-managed Breadcrumb wrapper, or NULL for invalid parent or allocation failure.
void *rt_breadcrumb_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_breadcrumb_t *bc = vg_breadcrumb_create();
    if (!bc)
        return NULL;

    rt_breadcrumb_data_t *data =
        (rt_breadcrumb_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_breadcrumb_data_t));
    if (!data) {
        vg_breadcrumb_destroy(bc);
        return NULL;
    }
    data->breadcrumb = bc;
    data->magic = RT_BREADCRUMB_DATA_MAGIC;
    data->clicked_index = -1;
    data->clicked_data = NULL;
    data->was_clicked = 0;
    if (!s_breadcrumb_original_vtable && bc->base.vtable) {
        s_breadcrumb_original_vtable = bc->base.vtable;
        s_breadcrumb_runtime_vtable = *bc->base.vtable;
        s_breadcrumb_runtime_vtable.destroy = rt_breadcrumb_widget_destroy;
    }
    if (s_breadcrumb_original_vtable) {
        bc->base.vtable = &s_breadcrumb_runtime_vtable;
        bc->base.user_data = data;
    }
    rt_obj_set_finalizer(data, rt_breadcrumb_finalize);

    vg_breadcrumb_set_on_click(bc, rt_breadcrumb_on_click, data);
    if (parent_widget) {
        vg_widget_add_child(parent_widget, &bc->base);
    }
    if (app)
        rt_gui_activate_app(app);
    rt_gui_ensure_default_font();
    if (app && app->default_font) {
        vg_breadcrumb_set_font(bc, app->default_font, rt_gui_app_effective_font_size(app));
    }

    return data;
}

/// @brief Release resources and destroy the breadcrumb.
/// @param crumb Candidate live Breadcrumb wrapper; invalid or already disposed handles are ignored.
void rt_breadcrumb_destroy(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data)
        return;
    rt_breadcrumb_dispose(data);
}

/// @brief Set the path of the breadcrumb.
/// @details Clears prior items and pending click state, then splits GUI-safe @p path on the
/// complete
///          literal separator string. Empty segments are skipped; each accepted segment owns an
///          independent label and click payload.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param path Runtime path text to split.
/// @param separator Runtime literal separator; an empty/failed conversion produces an empty path.
void rt_breadcrumb_set_path(void *crumb, rt_string path, rt_string separator) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;

    char *cpath = rt_string_to_gui_cstr(path);
    char *csep = rt_string_to_gui_cstr(separator);

    rt_breadcrumb_clear_click_state(data);
    // Clear existing items
    vg_breadcrumb_clear(data->breadcrumb);

    // Parse path and add items. The separator is a literal string, not a
    // strtok-style set of delimiter characters.
    if (cpath && csep && csep[0]) {
        size_t sep_len = strlen(csep);
        const char *cursor = cpath;
        while (cursor && *cursor) {
            const char *next = strstr(cursor, csep);
            size_t len = next ? (size_t)(next - cursor) : strlen(cursor);
            rt_breadcrumb_push_path_segment(data->breadcrumb, cursor, len);
            if (!next)
                break;
            cursor = next + sep_len;
        }
    }

    if (cpath)
        free(cpath);
    if (csep)
        free(csep);
}

/// @brief Set the items of the breadcrumb.
/// @details Clears existing state, splits converted text on commas, trims ASCII spaces at both
///          ends, and stores each token as both copied label and length-aware click payload.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param items Runtime comma-separated item list.
void rt_breadcrumb_set_items(void *crumb, rt_string items) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;

    char *citems = rt_string_to_gui_cstr(items);

    rt_breadcrumb_clear_click_state(data);
    // Clear existing items
    vg_breadcrumb_clear(data->breadcrumb);

    // Parse comma-separated items
    if (citems) {
        char *saveptr = NULL;
        char *token = rt_strtok_r(citems, ",", &saveptr);
        while (token) {
            // Trim whitespace
            while (*token == ' ')
                token++;
            char *end = token + strlen(token);
            while (end > token && end[-1] == ' ')
                *--end = '\0';

            rt_gui_string_data_t *payload = rt_gui_string_data_new_bytes(token, strlen(token));
            if (payload) {
                size_t prev_count = data->breadcrumb->item_count;
                vg_breadcrumb_push(data->breadcrumb, token, payload);
                if (data->breadcrumb->item_count > prev_count) {
                    data->breadcrumb->items[data->breadcrumb->item_count - 1].owns_user_data = true;
                } else {
                    free(payload);
                }
            }
            token = rt_strtok_r(NULL, ",", &saveptr);
        }
        free(citems);
    }
}

/// @brief Add a path segment to a breadcrumb navigation widget.
/// @details Converts and copies the visible label and, when supplied, stores an independently owned
///          length-aware data payload. Allocation or append failure preserves ownership correctly.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param text Runtime segment label.
/// @param item_data Optional runtime value returned when this segment is clicked.
void rt_breadcrumb_add_item(void *crumb, rt_string text, rt_string item_data) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;

    char *ctext = rt_string_to_gui_cstr(text);
    rt_gui_string_data_t *payload = item_data ? rt_gui_string_data_new(item_data) : NULL;

    if (ctext) {
        size_t prev_count = data->breadcrumb->item_count;
        vg_breadcrumb_push(data->breadcrumb, ctext, payload);
        if (data->breadcrumb->item_count > prev_count) {
            if (payload) {
                data->breadcrumb->items[data->breadcrumb->item_count - 1].owns_user_data = true;
            }
        } else if (payload) {
            free(payload);
        }
        free(ctext);
    } else {
        free(payload);
    }
}

/// @brief Remove all entries from the breadcrumb.
/// @details Clears pending click state before releasing all lower item labels and owned payloads.
/// @param crumb Candidate live Breadcrumb wrapper.
void rt_breadcrumb_clear(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;
    rt_breadcrumb_clear_click_state(data);
    vg_breadcrumb_clear(data->breadcrumb);
}

/// @brief Check if a breadcrumb segment was clicked this frame (edge-triggered).
/// @details Consumes only the click flag; index and copied data remain readable after a true
/// result.
///          A false poll clears any stale snapshot.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @return One when a pending segment click was consumed, otherwise zero.
int64_t rt_breadcrumb_was_item_clicked(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data)
        return 0;
    int64_t result = data->was_clicked;
    data->was_clicked = 0; // Reset after checking
    if (!result)
        rt_breadcrumb_clear_click_state(data);
    return result;
}

/// @brief Get the clicked index of the breadcrumb.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @return Last captured zero-based segment index, or -1 for invalid/no retained click.
int64_t rt_breadcrumb_get_clicked_index(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data)
        return -1;
    return data->clicked_index;
}

/// @brief Get the clicked data of the breadcrumb.
/// @details Reconstructs a runtime string from the wrapper-owned length-aware snapshot, preserving
///          embedded NUL bytes and exposing no lower storage.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @return Runtime copy of clicked item data, or the canonical empty string when unavailable.
rt_string rt_breadcrumb_get_clicked_data(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data)
        return rt_str_empty();
    return rt_gui_string_data_to_rt_string(data->clicked_data);
}

/// @brief Set the separator of the breadcrumb.
/// @details Converts the runtime string to GUI-safe UTF-8 and lets the lower widget copy it.
///          Conversion failure preserves the current separator.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param sep Runtime separator displayed between adjacent segments.
void rt_breadcrumb_set_separator(void *crumb, rt_string sep) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;
    char *csep = rt_string_to_gui_cstr(sep);
    if (csep) {
        vg_breadcrumb_set_separator(data->breadcrumb, csep);
        free(csep);
    }
}

/// @brief Set the max items of the breadcrumb.
/// @details Saturates the public value to the toolkit's non-negative signed-int domain.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param max Maximum visible/retained segment count; negative values become zero.
void rt_breadcrumb_set_max_items(void *crumb, int64_t max) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;
    vg_breadcrumb_set_max_items(data->breadcrumb, rt_gui_clamp_i64_to_i32(max, 0, INT32_MAX));
}

/// @brief Show or hide the breadcrumb widget.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @param visible Non-zero to show the backing widget, zero to hide it.
void rt_breadcrumb_set_visible(void *crumb, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return;
    vg_widget_set_visible(&data->breadcrumb->base, visible != 0);
}

/// @brief Check whether the breadcrumb widget is visible.
/// @param crumb Candidate live Breadcrumb wrapper.
/// @return One when the backing widget is live and visible, otherwise zero.
int64_t rt_breadcrumb_is_visible(void *crumb) {
    RT_ASSERT_MAIN_THREAD();
    rt_breadcrumb_data_t *data = rt_breadcrumb_checked(crumb);
    if (!data || !data->breadcrumb)
        return 0;
    return data->breadcrumb->base.visible ? 1 : 0;
}

//=============================================================================
// Phase 8: Minimap Implementation
//=============================================================================

/// @brief Magic value authenticating live Minimap wrapper state.
#define RT_MINIMAP_DATA_MAGIC UINT64_C(0x52544D494E494D50)

/// @brief Managed Minimap wrapper and per-instance lifetime-intercept vtable.
typedef struct rt_minimap_data {
    uint64_t magic;        ///< Must equal @ref RT_MINIMAP_DATA_MAGIC while live.
    vg_minimap_t *minimap; ///< Borrowed backing widget, or NULL after retirement.
    int64_t width;         ///< Requested logical minimap width.
    const vg_widget_vtable_t *original_vtable; ///< Lower vtable used for delegation.
    vg_widget_vtable_t vtable; ///< Instance copy intercepting backing-widget destruction.
} rt_minimap_data_t;

/// @brief Global registry authenticating opaque Minimap wrapper handles.
static rt_minimap_data_t **s_minimap_wrappers = NULL;
/// @brief Number of live entries in @ref s_minimap_wrappers.
static size_t s_minimap_wrapper_count = 0;
/// @brief Allocated Minimap registry capacity.
static size_t s_minimap_wrapper_cap = 0;

/// @brief Record a wrapper in the global minimap registry (idempotent).
/// @details The registry is the source of truth for handle validation: a checked
///          cast only trusts an opaque `void*` once it is found here (then verifies
///          the magic tag), guarding against forged/freed handles. Capacity doubles from 8.
/// @param data GC-managed Minimap wrapper to register; ownership is not transferred.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int rt_minimap_register_wrapper(rt_minimap_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_minimap_wrapper_count; i++) {
        if (s_minimap_wrappers[i] == data)
            return 1;
    }
    if (s_minimap_wrapper_count >= s_minimap_wrapper_cap) {
        size_t new_cap = 0;
        if (!rt_gui_next_collection_capacity(s_minimap_wrapper_cap,
                                             s_minimap_wrapper_count + 1u,
                                             8u,
                                             sizeof(rt_minimap_data_t *),
                                             &new_cap))
            return 0;
        void *p = realloc(s_minimap_wrappers, new_cap * sizeof(rt_minimap_data_t *));
        if (!p)
            return 0;
        s_minimap_wrappers = (rt_minimap_data_t **)p;
        s_minimap_wrapper_cap = new_cap;
    }
    s_minimap_wrappers[s_minimap_wrapper_count++] = data;
    return 1;
}

/// @brief Remove a wrapper from the minimap registry, compacting the array. No-op if absent.
/// @param data Wrapper identity to remove; may be NULL.
static void rt_minimap_unregister_wrapper(rt_minimap_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < s_minimap_wrapper_count; i++) {
        if (s_minimap_wrappers[i] != data)
            continue;
        memmove(&s_minimap_wrappers[i],
                &s_minimap_wrappers[i + 1],
                (s_minimap_wrapper_count - i - 1) * sizeof(*s_minimap_wrappers));
        s_minimap_wrapper_count--;
        return;
    }
}

/// @brief Disconnect every live Minimap whose borrowed editor lies in a retiring subtree.
/// @details Scans the registered wrappers without allocation, clears matching lower editor
///          pointers, and invalidates each affected preview before editor storage is released.
/// @param subtree Root of the editor/widget subtree being destroyed; NULL is a no-op.
void rt_minimap_forget_editor_subtree(vg_widget_t *subtree) {
    if (!subtree)
        return;
    for (size_t i = 0; i < s_minimap_wrapper_count; i++) {
        rt_minimap_data_t *data = s_minimap_wrappers[i];
        if (!data || !data->minimap || !vg_widget_is_live(&data->minimap->base))
            continue;
        vg_widget_t *target = (vg_widget_t *)data->minimap->editor;
        if (target && rt_gui_widget_tree_contains(subtree, target)) {
            vg_minimap_set_editor(data->minimap, NULL);
            vg_widget_invalidate(&data->minimap->base);
        }
    }
}

/// @brief Synchronize source observations for every live Minimap owned by one app.
/// @details This allocation-free scheduler bridge runs before app dirty-state inspection so editor
///          text/layout/viewport transitions invalidate the minimap in the same retained frame.
/// @param app_ptr Borrowed opaque app identity used to filter registered Minimap widgets.
void rt_minimap_sync_app(void *app_ptr) {
    if (!app_ptr)
        return;
    for (size_t index = 0; index < s_minimap_wrapper_count; ++index) {
        rt_minimap_data_t *data = s_minimap_wrappers[index];
        if (!data || data->magic != RT_MINIMAP_DATA_MAGIC || !data->minimap ||
            !vg_widget_is_live(&data->minimap->base)) {
            continue;
        }
        if (rt_gui_app_from_widget(&data->minimap->base) == (rt_gui_app_t *)app_ptr)
            (void)vg_minimap_sync_source(data->minimap);
    }
}

/// @brief Authenticate a Minimap handle via its magic tag (NULL if not).
/// @param minimap Candidate GC-managed Minimap wrapper.
/// @return Borrowed wrapper when the magic tag matches, otherwise NULL.
static rt_minimap_data_t *rt_minimap_checked(void *minimap) {
    rt_minimap_data_t *data = (rt_minimap_data_t *)minimap;
    return data && data->magic == RT_MINIMAP_DATA_MAGIC ? data : NULL;
}

/// @brief Widget vtable `destroy` override — clears back-pointer before chaining to the
///        original vtable's destroy, matching the breadcrumb pattern.
/// @param widget Backing Minimap widget being destroyed by its owner tree.
static void rt_minimap_widget_destroy(vg_widget_t *widget) {
    rt_minimap_data_t *data = widget ? (rt_minimap_data_t *)widget->user_data : NULL;
    if (data && data->minimap == (vg_minimap_t *)widget)
        data->minimap = NULL;
    if (data && data->original_vtable && data->original_vtable->destroy)
        data->original_vtable->destroy(widget);
}

/// @brief Destroy the underlying minimap widget and zero the pointer.
/// @details Clears the wrapper magic and unregisters it after destroying any still-live backing
///          widget, making explicit destruction and GC finalization idempotent.
/// @param data Minimap wrapper to dispose; NULL is a no-op.
static void rt_minimap_dispose(rt_minimap_data_t *data) {
    if (!data)
        return;
    if (data->minimap) {
        vg_minimap_destroy(data->minimap);
        data->minimap = NULL;
    }
    data->magic = 0;
    rt_minimap_unregister_wrapper(data);
}

/// @brief GC finalizer — delegates to `rt_minimap_dispose`.
/// @param minimap GC-managed Minimap wrapper being finalized.
static void rt_minimap_finalize(void *minimap) {
    rt_minimap_dispose((rt_minimap_data_t *)minimap);
}

/// @brief Create a minimap widget — a small scaled-down preview of a larger document.
///
/// Used by code editors and large-canvas views. The minimap
/// observes its target widget (set via `rt_minimap_set_target`)
/// and reflects the visible viewport as an overlay rectangle.
/// @param parent App/container parent handle, or NULL for a detached widget.
/// @return New GC-managed Minimap wrapper, or NULL for invalid parent, allocation, or registry
///         failure.
void *rt_minimap_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_minimap_t *minimap = vg_minimap_create(NULL);
    if (!minimap)
        return NULL;

    rt_minimap_data_t *data =
        (rt_minimap_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_minimap_data_t));
    if (!data) {
        vg_minimap_destroy(minimap);
        return NULL;
    }
    data->minimap = minimap;
    data->magic = RT_MINIMAP_DATA_MAGIC;
    data->width = 100;
    data->original_vtable = minimap->base.vtable;
    if (data->original_vtable) {
        data->vtable = *data->original_vtable;
        data->vtable.destroy = rt_minimap_widget_destroy;
        minimap->base.vtable = &data->vtable;
        minimap->base.user_data = data;
    }
    if (!rt_minimap_register_wrapper(data)) {
        rt_minimap_dispose(data);
        return NULL;
    }
    rt_obj_set_finalizer(data, rt_minimap_finalize);
    minimap->base.constraints.min_width = (float)data->width;
    minimap->base.constraints.preferred_width = (float)data->width;
    minimap->base.constraints.max_width = (float)data->width;
    if (parent_widget) {
        vg_widget_add_child(parent_widget, &minimap->base);
    }
    return data;
}

/// @brief Release resources and destroy the minimap.
/// @param minimap Candidate live Minimap wrapper; invalid or already disposed handles are ignored.
void rt_minimap_destroy(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data)
        return;
    rt_minimap_dispose(data);
}

/// @brief Bind the editor of the minimap.
/// @details Accepts only a live CodeEditor widget, installs it as a borrowed source, and
/// invalidates
///          the preview. Editor destruction is handled by @ref rt_minimap_forget_editor_subtree.
/// @param minimap Candidate live Minimap wrapper.
/// @param editor Candidate live CodeEditor widget handle.
void rt_minimap_bind_editor(void *minimap, void *editor) {
    RT_ASSERT_MAIN_THREAD();
    if (!editor)
        return;
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_codeeditor_t *target =
        (vg_codeeditor_t *)rt_gui_widget_handle_checked_type(editor, VG_WIDGET_CODEEDITOR);
    if (!target)
        return;
    vg_minimap_set_editor(data->minimap, target);
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Unbind the editor of the minimap.
/// @details Clears the borrowed source pointer and invalidates the preview without affecting the
///          editor's lifetime.
/// @param minimap Candidate live Minimap wrapper.
void rt_minimap_unbind_editor(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_minimap_set_editor(data->minimap, NULL);
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Set the width of the minimap.
/// @details Saturates @p width to [64, @ref RT_GUI_MAX_LAYOUT_VALUE], pins all three horizontal
///          constraints to the result, and invalidates both layout and paint.
/// @param minimap Candidate live Minimap wrapper.
/// @param width Requested toolkit width.
void rt_minimap_set_width(void *minimap, int64_t width) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    int32_t clamped_width = rt_gui_clamp_i64_to_i32(width, 64, (int32_t)RT_GUI_MAX_LAYOUT_VALUE);
    data->width = clamped_width;
    data->minimap->base.constraints.min_width = (float)clamped_width;
    data->minimap->base.constraints.preferred_width = (float)clamped_width;
    data->minimap->base.constraints.max_width = (float)clamped_width;
    vg_widget_invalidate_layout(&data->minimap->base);
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Show or hide the minimap widget.
/// @details Updates retained visibility and invalidates layout plus paint so surrounding chrome
///          immediately reflows.
/// @param minimap Candidate live Minimap wrapper.
/// @param visible Non-zero to show the preview, zero to hide it.
void rt_minimap_set_visible(void *minimap, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_widget_set_visible(&data->minimap->base, visible != 0);
    vg_widget_invalidate_layout(&data->minimap->base);
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Check whether the minimap widget is visible.
/// @param minimap Candidate live Minimap wrapper.
/// @return One when the backing widget is live and visible, otherwise zero.
int64_t rt_minimap_is_visible(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return 0;
    return data->minimap->base.visible ? 1 : 0;
}

/// @brief Get the width of the minimap.
/// @param minimap Candidate live Minimap wrapper.
/// @return Stored constrained width, or zero for an invalid/disposed wrapper.
int64_t rt_minimap_get_width(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return 0;
    return data->width;
}

/// @brief Set the scale of the minimap.
/// @details Non-finite input becomes 0.1; finite values are clamped to [0.05, 0.5]. The lower
///          preview is invalidated after the scale update.
/// @param minimap Candidate live Minimap wrapper.
/// @param scale Requested document-to-preview scale.
void rt_minimap_set_scale(void *minimap, double scale) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    double sanitized = rt_gui_double_is_finite(scale) ? scale : 0.1;
    vg_minimap_set_scale(data->minimap, (float)rt_gui_clamp_f64(sanitized, 0.05, 0.5));
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Set the show slider of the minimap.
/// @details Controls the overlay that depicts the editor's visible viewport and invalidates paint.
/// @param minimap Candidate live Minimap wrapper.
/// @param show Non-zero to show the viewport slider, zero to suppress it.
void rt_minimap_set_show_slider(void *minimap, int64_t show) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_minimap_set_show_viewport(data->minimap, show != 0);
    vg_widget_invalidate(&data->minimap->base);
}

/// @brief Synchronize and return the minimap's combined editor-source revision.
/// @details The lower getter first observes text, layout, and viewport revisions. Values exceeding
///          the public signed domain saturate at @c INT64_MAX.
/// @param minimap Candidate live Minimap wrapper.
/// @return Current combined source revision, or zero for invalid/disposed input.
int64_t rt_minimap_get_source_revision(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return 0;
    const uint64_t revision = vg_minimap_get_source_revision(data->minimap);
    return rt_gui_saturating_u64_to_i64(revision);
}

/// @brief Invalidate only cached summaries intersecting a source-line range.
/// @details Requires non-negative @p first and positive @p count, converts the inclusive final line
///          with saturation to @c UINT32_MAX, and leaves unrelated cache entries reusable.
/// @param minimap Candidate live Minimap wrapper.
/// @param first Zero-based first source line.
/// @param count Positive number of consecutive lines.
void rt_minimap_invalidate_lines(void *minimap, int64_t first, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap || first < 0 || count <= 0 || first > UINT32_MAX)
        return;
    uint64_t last = (uint64_t)first + (uint64_t)count - 1u;
    if (last > UINT32_MAX)
        last = UINT32_MAX;
    vg_minimap_invalidate_lines(data->minimap, (uint32_t)first, (uint32_t)last);
}

/// @brief Atomically resize the bounded minimap line-summary cache.
/// @details Clamps @p count to [0, 1,000,000] and delegates allocation-first replacement to the
///          lower widget; failure preserves the existing cache and limit.
/// @param minimap Candidate live Minimap wrapper.
/// @param count Requested maximum cached line summaries; zero disables retention.
void rt_minimap_set_maximum_cached_lines(void *minimap, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    const int64_t clamped = count < 0 ? 0 : (count > 1000000 ? 1000000 : count);
    (void)vg_minimap_set_maximum_cached_lines(data->minimap, (uint32_t)clamped);
}

/// @brief Return the number of valid bounded line-summary cache entries.
/// @param minimap Candidate live Minimap wrapper.
/// @return Current valid cache-entry count, or zero for invalid/disposed input.
int64_t rt_minimap_get_cached_line_count(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    return data && data->minimap ? (int64_t)vg_minimap_get_cached_line_count(data->minimap) : 0;
}

/// @brief Add a highlighted marker region to the minimap.
/// @details Grows the owned marker array geometrically with overflow checks, narrows line and type
///          by saturation, stores the low 32 bits of @p color, and marks the preview dirty.
/// @param minimap Candidate live Minimap wrapper.
/// @param line Source-line coordinate associated with the marker.
/// @param color Packed marker color narrowed to 32 bits.
/// @param type Caller-defined marker category narrowed to signed 32 bits.
void rt_minimap_add_marker(void *minimap, int64_t line, int64_t color, int64_t type) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_minimap_t *mm = data->minimap;
    if (mm->marker_count < 0 || mm->marker_cap < 0 || mm->marker_count > mm->marker_cap)
        return;

    if (mm->marker_count >= mm->marker_cap) {
        size_t new_cap = 0;
        if (mm->marker_count < 0 ||
            !rt_gui_next_collection_capacity((size_t)(mm->marker_cap < 0 ? 0 : mm->marker_cap),
                                             (size_t)mm->marker_count + 1u,
                                             8u,
                                             sizeof(*mm->markers),
                                             &new_cap) ||
            new_cap > (size_t)INT_MAX)
            return;
        void *p = realloc(mm->markers, new_cap * sizeof(*mm->markers));
        if (!p)
            return;
        mm->markers = p;
        mm->marker_cap = (int)new_cap;
    }
    struct vg_minimap_marker *m = &mm->markers[mm->marker_count++];
    m->line = rt_gui_clamp_i64_to_i32(line, INT32_MIN, INT32_MAX);
    m->color = (uint32_t)color;
    m->type = rt_gui_clamp_i64_to_i32(type, INT32_MIN, INT32_MAX);
    mm->base.needs_paint = true;
}

/// @brief Remove every marker associated with one source line.
/// @details Compacts the marker array in place and invalidates paint only when at least one marker
///          matched the saturated line value.
/// @param minimap Candidate live Minimap wrapper.
/// @param line Source-line value whose markers should be removed.
void rt_minimap_remove_markers(void *minimap, int64_t line) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_minimap_t *mm = data->minimap;
    int32_t clamped_line = rt_gui_clamp_i64_to_i32(line, INT32_MIN, INT32_MAX);
    int w = 0;
    for (int i = 0; i < mm->marker_count; i++) {
        if (mm->markers[i].line != clamped_line)
            mm->markers[w++] = mm->markers[i];
    }
    if (w != mm->marker_count) {
        mm->marker_count = w;
        mm->base.needs_paint = true;
    }
}

/// @brief Clear the markers of the minimap.
/// @details Releases the complete marker allocation, resets count/capacity, and marks paint dirty.
/// @param minimap Candidate live Minimap wrapper.
void rt_minimap_clear_markers(void *minimap) {
    RT_ASSERT_MAIN_THREAD();
    rt_minimap_data_t *data = rt_minimap_checked(minimap);
    if (!data || !data->minimap)
        return;
    vg_minimap_t *mm = data->minimap;
    free(mm->markers);
    mm->markers = NULL;
    mm->marker_count = 0;
    mm->marker_cap = 0;
    mm->base.needs_paint = true;
}

//=============================================================================

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: graphics disabled — no runtime minimaps require source synchronization.
/// @param app_ptr Ignored opaque app pointer.
void rt_minimap_sync_app(void *app_ptr) {
    (void)app_ptr;
}

/// @brief Stub: returns NULL — breadcrumb widget requires graphics.
/// @param parent Ignored candidate parent handle.
/// @return Always NULL.
void *rt_breadcrumb_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: ignore Breadcrumb destruction when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
void rt_breadcrumb_destroy(void *crumb) {
    (void)crumb;
}

/// @brief Stub: ignore Breadcrumb path replacement when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param path Ignored runtime path.
/// @param separator Ignored runtime separator.
void rt_breadcrumb_set_path(void *crumb, rt_string path, rt_string separator) {
    (void)crumb;
    (void)path;
    (void)separator;
}

/// @brief Stub: ignore Breadcrumb item replacement when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param items Ignored runtime item list.
void rt_breadcrumb_set_items(void *crumb, rt_string items) {
    (void)crumb;
    (void)items;
}

/// @brief Stub: ignore Breadcrumb segment addition when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param text Ignored runtime label.
/// @param item_data Ignored runtime click payload.
void rt_breadcrumb_add_item(void *crumb, rt_string text, rt_string item_data) {
    (void)crumb;
    (void)text;
    (void)item_data;
}

/// @brief Stub: ignore Breadcrumb clearing when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
void rt_breadcrumb_clear(void *crumb) {
    (void)crumb;
}

/// @brief Stub: report no Breadcrumb click when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @return Always zero.
int64_t rt_breadcrumb_was_item_clicked(void *crumb) {
    (void)crumb;
    return 0;
}

/// @brief Stub: report no clicked Breadcrumb index when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @return Always -1.
int64_t rt_breadcrumb_get_clicked_index(void *crumb) {
    (void)crumb;
    return -1;
}

/// @brief Stub: report no clicked Breadcrumb data when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @return Canonical empty runtime string.
rt_string rt_breadcrumb_get_clicked_data(void *crumb) {
    (void)crumb;
    return rt_str_empty();
}

/// @brief Stub: ignore Breadcrumb separator changes when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param sep Ignored runtime separator.
void rt_breadcrumb_set_separator(void *crumb, rt_string sep) {
    (void)crumb;
    (void)sep;
}

/// @brief Stub: ignore Breadcrumb item limits when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param max Ignored maximum item count.
void rt_breadcrumb_set_max_items(void *crumb, int64_t max) {
    (void)crumb;
    (void)max;
}

/// @brief Stub: ignore Breadcrumb visibility changes when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @param visible Ignored visibility flag.
void rt_breadcrumb_set_visible(void *crumb, int64_t visible) {
    (void)crumb;
    (void)visible;
}

/// @brief Stub: report Breadcrumb hidden when graphics is disabled.
/// @param crumb Ignored candidate Breadcrumb handle.
/// @return Always zero.
int64_t rt_breadcrumb_is_visible(void *crumb) {
    (void)crumb;
    return 0;
}

/// @brief Stub: returns NULL — minimap widget requires graphics.
/// @param parent Ignored candidate parent handle.
/// @return Always NULL.
void *rt_minimap_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: ignore Minimap destruction when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
void rt_minimap_destroy(void *minimap) {
    (void)minimap;
}

/// @brief Stub: ignore Minimap editor binding when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param editor Ignored candidate CodeEditor handle.
void rt_minimap_bind_editor(void *minimap, void *editor) {
    (void)minimap;
    (void)editor;
}

/// @brief Stub: ignore Minimap editor unbinding when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
void rt_minimap_unbind_editor(void *minimap) {
    (void)minimap;
}

/// @brief Stub: ignore Minimap width changes when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param width Ignored requested width.
void rt_minimap_set_width(void *minimap, int64_t width) {
    (void)minimap;
    (void)width;
}

/// @brief Stub: ignore Minimap visibility changes when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param visible Ignored visibility flag.
void rt_minimap_set_visible(void *minimap, int64_t visible) {
    (void)minimap;
    (void)visible;
}

/// @brief Stub: report Minimap hidden when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @return Always zero.
int64_t rt_minimap_is_visible(void *minimap) {
    (void)minimap;
    return 0;
}

/// @brief Stub: report zero Minimap width when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @return Always zero.
int64_t rt_minimap_get_width(void *minimap) {
    (void)minimap;
    return 0;
}

/// @brief Stub: ignore Minimap scale changes when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param scale Ignored preview scale.
void rt_minimap_set_scale(void *minimap, double scale) {
    (void)minimap;
    (void)scale;
}

/// @brief Stub: ignore Minimap viewport-slider changes when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param show Ignored slider visibility flag.
void rt_minimap_set_show_slider(void *minimap, int64_t show) {
    (void)minimap;
    (void)show;
}

/// @brief Stub: graphics disabled — no observed minimap source revision exists.
/// @param minimap Ignored candidate Minimap handle.
/// @return Always zero.
int64_t rt_minimap_get_source_revision(void *minimap) {
    (void)minimap;
    return 0;
}

/// @brief Stub: graphics disabled — no minimap cache range can be invalidated.
/// @param minimap Ignored candidate Minimap handle.
/// @param first Ignored first source line.
/// @param count Ignored line count.
void rt_minimap_invalidate_lines(void *minimap, int64_t first, int64_t count) {
    (void)minimap;
    (void)first;
    (void)count;
}

/// @brief Stub: graphics disabled — no minimap cache can be configured.
/// @param minimap Ignored candidate Minimap handle.
/// @param count Ignored cache limit.
void rt_minimap_set_maximum_cached_lines(void *minimap, int64_t count) {
    (void)minimap;
    (void)count;
}

/// @brief Stub: graphics disabled — no cached minimap lines exist.
/// @param minimap Ignored candidate Minimap handle.
/// @return Always zero.
int64_t rt_minimap_get_cached_line_count(void *minimap) {
    (void)minimap;
    return 0;
}

/// @brief Stub: ignore Minimap marker addition when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param line Ignored source line.
/// @param color Ignored packed color.
/// @param type Ignored marker category.
void rt_minimap_add_marker(void *minimap, int64_t line, int64_t color, int64_t type) {
    (void)minimap;
    (void)line;
    (void)color;
    (void)type;
}

/// @brief Stub: ignore line-specific Minimap marker removal when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
/// @param line Ignored source line.
void rt_minimap_remove_markers(void *minimap, int64_t line) {
    (void)minimap;
    (void)line;
}

/// @brief Stub: ignore Minimap marker clearing when graphics is disabled.
/// @param minimap Ignored candidate Minimap handle.
void rt_minimap_clear_markers(void *minimap) {
    (void)minimap;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
