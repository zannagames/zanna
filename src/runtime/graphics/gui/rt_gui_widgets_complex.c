//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_widgets_complex.c
// Purpose: Runtime bindings for composite ZannaGUI widgets: TabBar (tab strip
//   with optional close buttons), SplitPane (resizable two-panel divider),
//   TreeView (hierarchical node list), ScrollView (scrollable container),
//   FloatingPanel (overlay panel drawn above all content), and CodeEditor (full
//   source-editor widget with syntax highlighting, gutters, and selection),
//   plus the viewport-aware interactive/sparse-virtualized data Grid.
//   Each widget wraps the corresponding vg_* C widget with a Zia-callable API.
//
// Key invariants:
//   - TabBar active-tab is tracked by the vg_tabbar_t; rt_tabbar_get_active()
//     returns the raw vg_tab_t* pointer — callers must not free it.
//   - SplitPane position is a float in [0,1] representing the divider fraction;
//     clamped by the vg layout engine to [min_pos, max_pos].
//   - TreeView nodes form a pointer-linked tree; removing a node frees its
//     subtree recursively via vg_treeview_remove_node.
//   - ScrollView scroll offsets are clamped to [0, content_size - viewport_size]
//     by the vg layout engine; GetScrollX/Y may return 0 if content fits.
//   - FloatingPanel children are reparented under the panel widget and rendered
//     during the overlay pass so hit testing and destruction stay tree-based.
//   - CodeEditor selection retrieval allocates a C string that the caller owns.
//   - Grid indices are rejected rather than clamped onto a different cell;
//     virtual row counts never allocate proportional row storage.
//
// Ownership/Lifetime:
//   - All widget objects are vg_widget_t* (or subtype) owned by the vg widget
//     tree; vg_widget_destroy() on the root frees the entire subtree.
//   - Tab objects (vg_tab_t*) are owned by the TabBar; do not free them
//     independently.
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/lib/gui/include/vg.h (ZannaGUI C API),
//        src/runtime/graphics/rt_gui_codeeditor.c (CodeEditor enhancements),
//        docs/adr/0167-spinner-mixed-value-state.md
//
//===----------------------------------------------------------------------===//

#include "rt_gui_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include <stdint.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Safe-cast a handle to a live TabBar widget, or NULL.
static vg_tabbar_t *rt_tabbar_checked(void *handle) {
    return (vg_tabbar_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_TABBAR);
}

/// @brief Safe-cast a handle to a live SplitPane widget, or NULL.
static vg_splitpane_t *rt_splitpane_checked(void *handle) {
    return (vg_splitpane_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_SPLITPANE);
}

/// @brief Safe-cast a handle to a live CodeEditor widget, or NULL.
static vg_codeeditor_t *rt_codeeditor_checked(void *handle) {
    return (vg_codeeditor_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_CODEEDITOR);
}

/// @brief Order a (line,col) range in place so start precedes or equals end,
///        swapping both coordinate pairs together when given backwards.
static void rt_codeeditor_normalize_range(int *start_line,
                                          int *start_col,
                                          int *end_line,
                                          int *end_col) {
    if (*start_line > *end_line || (*start_line == *end_line && *start_col > *end_col)) {
        int tmp = *start_line;
        *start_line = *end_line;
        *end_line = tmp;
        tmp = *start_col;
        *start_col = *end_col;
        *end_col = tmp;
    }
}

/// @brief Clamp a line index into the valid `[0, line_count-1]` range (0 if empty).
static int rt_codeeditor_clamp_line_index(const vg_codeeditor_t *ce, int line) {
    if (!ce || ce->line_count <= 0)
        return 0;
    if (line < 0)
        return 0;
    if (line >= ce->line_count)
        return ce->line_count - 1;
    return line;
}

/// @brief Clamp a byte column into `[0, line length]` for the given line (0 if line is invalid).
static size_t rt_codeeditor_clamp_col_index(const vg_codeeditor_t *ce, int line, int col) {
    if (!ce || line < 0 || line >= ce->line_count)
        return 0;
    if (col <= 0)
        return 0;
    size_t len = ce->lines[line].length;
    return (size_t)col > len ? len : (size_t)col;
}

/// @brief Extract the text spanning a (line,col) range as an rt_string, joining
///        lines with '\n'.
/// @details Two passes (measure, then copy) so the buffer is sized exactly, with
///          overflow-guarded length accumulation. The range is normalized, its line
///          indices clamped, then re-normalized — clamping can collapse endpoints and
///          flip their order. Returns the empty string on bad input or allocation failure.
static rt_string rt_codeeditor_range_to_rt_string(
    vg_codeeditor_t *ce, int start_line, int start_col, int end_line, int end_col) {
    if (!ce || ce->line_count <= 0)
        return rt_str_empty();

    rt_codeeditor_normalize_range(&start_line, &start_col, &end_line, &end_col);
    start_line = rt_codeeditor_clamp_line_index(ce, start_line);
    end_line = rt_codeeditor_clamp_line_index(ce, end_line);
    rt_codeeditor_normalize_range(&start_line, &start_col, &end_line, &end_col);

    size_t total = 0;
    for (int line = start_line; line <= end_line; line++) {
        size_t from = (line == start_line) ? rt_codeeditor_clamp_col_index(ce, line, start_col) : 0;
        size_t to = (line == end_line) ? rt_codeeditor_clamp_col_index(ce, line, end_col)
                                       : ce->lines[line].length;
        if (to < from)
            to = from;
        size_t chunk = to - from;
        if (chunk > SIZE_MAX - total)
            return rt_str_empty();
        total += chunk;
        if (line < end_line) {
            if (total == SIZE_MAX)
                return rt_str_empty();
            total++;
        }
    }

    if (total == 0)
        return rt_str_empty();

    char *buffer = (char *)malloc(total);
    if (!buffer)
        return rt_str_empty();

    char *out = buffer;
    for (int line = start_line; line <= end_line; line++) {
        size_t from = (line == start_line) ? rt_codeeditor_clamp_col_index(ce, line, start_col) : 0;
        size_t to = (line == end_line) ? rt_codeeditor_clamp_col_index(ce, line, end_col)
                                       : ce->lines[line].length;
        if (to < from)
            to = from;
        size_t chunk = to - from;
        if (chunk) {
            memcpy(out, ce->lines[line].text + from, chunk);
            out += chunk;
        }
        if (line < end_line)
            *out++ = '\n';
    }

    rt_string result = rt_string_from_bytes(buffer, total);
    free(buffer);
    return result;
}

/// @brief Serialize the editor's entire buffer to an rt_string, '\n'-joining lines.
/// @details Same overflow-guarded two-pass sizing as rt_codeeditor_range_to_rt_string;
///          returns the empty string when empty or on allocation failure.
static rt_string rt_codeeditor_all_text_to_rt_string(vg_codeeditor_t *ce) {
    if (!ce || ce->line_count <= 0)
        return rt_str_empty();

    size_t total = 0;
    for (int line = 0; line < ce->line_count; line++) {
        size_t len = ce->lines[line].length;
        if (len > SIZE_MAX - total)
            return rt_str_empty();
        total += len;
        if (line < ce->line_count - 1) {
            if (total == SIZE_MAX)
                return rt_str_empty();
            total++;
        }
    }

    if (total == 0)
        return rt_str_empty();

    char *buffer = (char *)malloc(total);
    if (!buffer)
        return rt_str_empty();

    char *out = buffer;
    for (int line = 0; line < ce->line_count; line++) {
        size_t len = ce->lines[line].length;
        if (len) {
            memcpy(out, ce->lines[line].text, len);
            out += len;
        }
        if (line < ce->line_count - 1)
            *out++ = '\n';
    }

    rt_string result = rt_string_from_bytes(buffer, total);
    if (ce->perf_stats.full_text_copies != UINT64_MAX)
        ce->perf_stats.full_text_copies++;
    if (ce->perf_stats.full_text_copy_bytes > UINT64_MAX - (uint64_t)total)
        ce->perf_stats.full_text_copy_bytes = UINT64_MAX;
    else
        ce->perf_stats.full_text_copy_bytes += (uint64_t)total;
    free(buffer);
    return result;
}

/// @brief Safe-cast a handle to a live Dropdown widget, or NULL.
static vg_outputpane_t *rt_outputpane_checked(void *handle) {
    return (vg_outputpane_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_OUTPUTPANE);
}

/// @brief Safe-cast a handle to a live RadioButton widget, or NULL.
static vg_radiobutton_t *rt_radiobutton_checked(void *handle) {
    return (vg_radiobutton_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_RADIO);
}

/// @brief Safe-cast a handle to a live Spinner widget, or NULL.
static vg_spinner_t *rt_spinner_checked(void *handle) {
    return (vg_spinner_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_SPINNER);
}

/// @brief Resolve a parent-container handle to its widget.
/// @details Three-state contract: a NULL handle returns NULL (legitimate top-level
///          placement); a valid handle returns its container widget; a non-NULL
///          handle that fails to resolve also returns NULL — an error the caller
///          must treat as "invalid parent", not "no parent".
static vg_widget_t *rt_widget_parent_or_null_if_invalid(void *parent) {
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    return parent_widget;
}

#define RT_RADIOGROUP_MAGIC UINT64_C(0x52474452554E544D)           // "RGDRUNTM"
#define RT_RADIOGROUP_DESTROYED_MAGIC UINT64_C(0x5247444445414444) // "RGDDEAD"

typedef struct rt_radiogroup_data {
    uint64_t magic;
    vg_radiogroup_t *group;
} rt_radiogroup_data_t;

static rt_radiogroup_data_t **s_radiogroup_handles = NULL;
static size_t s_radiogroup_handle_count = 0;
static size_t s_radiogroup_handle_cap = 0;

/// @brief Track a radio-group handle in the process-wide registry.
/// @details The registry lets rt_radiogroup_handle_checked validate opaque
///          handles (radio groups aren't widgets, so the generic widget
///          liveness check doesn't apply). Grows the backing array as needed.
/// @return 1 on success, 0 on allocation failure or NULL input.
static int rt_radiogroup_registry_add(rt_radiogroup_data_t *data) {
    if (!data)
        return 0;
    if (s_radiogroup_handle_count >= s_radiogroup_handle_cap) {
        if (s_radiogroup_handle_cap > SIZE_MAX / 2)
            return 0;
        size_t new_cap = s_radiogroup_handle_cap ? s_radiogroup_handle_cap * 2 : 16;
        if (new_cap > SIZE_MAX / sizeof(rt_radiogroup_data_t *))
            return 0;
        rt_radiogroup_data_t **new_handles = (rt_radiogroup_data_t **)realloc(
            s_radiogroup_handles, new_cap * sizeof(rt_radiogroup_data_t *));
        if (!new_handles)
            return 0;
        s_radiogroup_handles = new_handles;
        s_radiogroup_handle_cap = new_cap;
    }
    s_radiogroup_handles[s_radiogroup_handle_count++] = data;
    return 1;
}

/// @brief Remove a radio-group handle from the registry (swap-with-last).
static void rt_radiogroup_registry_remove(rt_radiogroup_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < s_radiogroup_handle_count; i++) {
        if (s_radiogroup_handles[i] == data) {
            s_radiogroup_handles[i] = s_radiogroup_handles[--s_radiogroup_handle_count];
            return;
        }
    }
}

/// @brief Safe-cast an opaque handle to a live radio-group wrapper.
/// @details Verifies the handle is registered AND its magic tag is intact and
///          its backing vg_radiogroup is non-NULL; returns NULL otherwise.
static rt_radiogroup_data_t *rt_radiogroup_handle_checked(void *handle) {
    if (!handle)
        return NULL;
    for (size_t i = 0; i < s_radiogroup_handle_count; i++) {
        if (s_radiogroup_handles[i] == handle) {
            rt_radiogroup_data_t *data = (rt_radiogroup_data_t *)handle;
            return data->magic == RT_RADIOGROUP_MAGIC && data->group ? data : NULL;
        }
    }
    return NULL;
}

/// @brief Destroy a radio group: unregister, free the vg_radiogroup, and
///        stamp the destroyed-magic so stale handles fail validation.
static void rt_radiogroup_dispose(rt_radiogroup_data_t *data) {
    data = rt_radiogroup_handle_checked(data);
    if (!data)
        return;
    rt_radiogroup_registry_remove(data);
    if (data->group) {
        vg_radiogroup_destroy(data->group);
        data->group = NULL;
    }
    data->magic = RT_RADIOGROUP_DESTROYED_MAGIC;
}

/// @brief GC finalizer trampoline → rt_radiogroup_dispose.
static void rt_radiogroup_finalize(void *handle) {
    rt_radiogroup_dispose((rt_radiogroup_data_t *)handle);
}

//=============================================================================
// TabBar Widget
//=============================================================================

/// @brief Create a new tab bar widget for tabbed navigation.
/// @details Creates a vg_tabbar_t strip that displays clickable tabs. Tabs can
///          be added, removed, activated, and optionally have close buttons.
///          Selection changes are detected via rt_tabbar_was_changed (edge-triggered).
/// @param parent Parent container or app handle.
/// @return Opaque tab bar widget handle, or NULL on failure.
void *rt_tabbar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_tabbar_t *tabbar = vg_tabbar_create(parent_widget);
    if (tabbar)
        rt_gui_apply_default_font((vg_widget_t *)tabbar);
    return tabbar;
}

/// @brief Add a new tab to the tab bar.
/// @details Creates a vg_tab_t with the given title and optional close button.
///          The tab is appended to the bar's tab list. If this is the first tab,
///          it becomes the active tab automatically.
/// @param tabbar   Tab bar widget handle.
/// @param title    Tab title text (runtime string, copied internally).
/// @param closable Non-zero to show a close (X) button on the tab.
/// @return Opaque tab handle for later reference, or NULL on failure.
void *rt_tabbar_add_tab(void *tabbar, rt_string title, int64_t closable) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return NULL;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_tab_t *tab = vg_tabbar_add_tab(tb, ctitle, closable != 0);
    free(ctitle);
    return rt_gui_wrap_tab(tab);
}

/// @brief Remove a tab from the tab bar and free its resources.
void rt_tabbar_remove_tab(void *tabbar, void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    vg_tab_t *t = tab ? rt_gui_tab_from_handle(tab) : NULL;
    if (tb && t && t->owner == tb) {
        vg_tabbar_remove_tab(tb, t);
        rt_gui_collect_retired_subhandles(&tb->base);
    }
}

/// @brief Reclaim retired tab tombstones after invalidating their managed wrappers.
/// @details Existing `Zanna.GUI.Tab` values remain valid managed objects, but their targets are
///          cleared before the lower toolkit frees tombstone storage. Subsequent calls through a
///          pruned tab are inert, while wrappers for tabs still present in the bar remain usable.
void rt_tabbar_prune_retired_tabs(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (tb) {
        rt_gui_invalidate_retired_tab_subhandles(tb);
        vg_tabbar_prune_retired_tabs(tb);
    }
}

/// @brief Set the currently active (selected) tab in the tab bar.
void rt_tabbar_set_active(void *tabbar, void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    vg_tab_t *t = tab ? rt_gui_tab_from_handle(tab) : NULL;
    if (tb && (!tab || (t && t->owner == tb)))
        vg_tabbar_set_active(tb, t);
}

/// @brief Update the title text of a tab.
void rt_tab_set_title(void *tab, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (!t)
        return;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_tab_set_title(t, ctitle);
    free(ctitle);
}

/// @brief Return a tab's current display title.
/// @param tab Managed Tab subhandle.
/// @return Fresh runtime string, or empty for an invalid/stale tab.
rt_string rt_tab_get_title(void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    const char *title = t ? vg_tab_get_title(t) : NULL;
    return title ? rt_string_from_bytes(title, strlen(title)) : rt_str_empty();
}

/// @brief Store byte-exact runtime string data on a tab.
/// @details Allocation occurs before the previous owned payload is released, so failure preserves
///          the old value. Embedded NUL bytes round-trip through @ref rt_tab_get_data.
/// @param tab Managed Tab subhandle.
/// @param data Runtime string to copy; NULL clears the payload.
void rt_tab_set_data(void *tab, rt_string data) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (!t)
        return;
    rt_gui_string_data_t *copy = data ? rt_gui_string_data_new(data) : NULL;
    if (data && !copy)
        return;
    if (t->owns_user_data)
        rt_gui_string_data_free_if_owned(t->user_data);
    t->user_data = copy;
    t->owns_user_data = copy != NULL;
    if (t->owner)
        vg_widget_note_revision(&t->owner->base);
}

/// @brief Return byte-exact runtime string data stored on a tab.
rt_string rt_tab_get_data(void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    return t && t->owns_user_data ? rt_gui_string_data_to_rt_string(vg_tab_get_data(t))
                                  : rt_str_empty();
}

/// @brief Set whether a tab displays and accepts its close affordance.
void rt_tab_set_closable(void *tab, int64_t closable) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (t)
        vg_tab_set_closable(t, closable != 0);
}

/// @brief Return whether a tab is closable.
int64_t rt_tab_is_closable(void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    return t && vg_tab_is_closable(t) ? 1 : 0;
}

/// @brief Set a tab's application-stable identifier after rejecting embedded NUL bytes.
void rt_tab_set_stable_id(void *tab, rt_string stable_id) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (!t)
        return;
    if (!stable_id) {
        (void)vg_tab_set_stable_id(t, "");
        return;
    }
    char *cid = rt_string_to_cstr_no_nul(stable_id);
    if (!cid)
        return;
    (void)vg_tab_set_stable_id(t, cid);
    free(cid);
}

/// @brief Return a tab's copied stable identifier.
rt_string rt_tab_get_stable_id(void *tab) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    const char *stable_id = t ? vg_tab_get_stable_id(t) : NULL;
    return stable_id ? rt_string_from_bytes(stable_id, t->stable_id_len) : rt_str_empty();
}

/// @brief Update the tooltip text of a tab.
void rt_tab_set_tooltip(void *tab, rt_string tooltip) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (!t)
        return;
    char *ctooltip = rt_string_to_gui_cstr(tooltip);
    vg_tab_set_tooltip(t, ctooltip);
    free(ctooltip);
}

/// @brief Mark a tab as modified (shows an unsaved-changes indicator).
void rt_tab_set_modified(void *tab, int64_t modified) {
    RT_ASSERT_MAIN_THREAD();
    vg_tab_t *t = rt_gui_tab_from_handle(tab);
    if (t)
        vg_tab_set_modified(t, modified != 0);
}

/// @brief Return the currently-active tab handle (NULL when no tabs / null bar).
void *rt_tabbar_get_active(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb ? rt_gui_wrap_tab(tb->active_tab) : NULL;
}

/// @brief Get the active index of the tabbar.
int64_t rt_tabbar_get_active_index(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return -1;
    return vg_tabbar_get_tab_index(tb, tb->active_tab);
}

/// @brief Check if the active tab changed since the last call (edge-triggered).
int64_t rt_tabbar_was_changed(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return 0;
    if (tb->reported_active_change_version != tb->active_change_version) {
        tb->reported_active_change_version = tb->active_change_version;
        return 1;
    }
    return 0;
}

/// @brief Return the TabBar's non-consuming state revision.
/// @details Structural mutations and active-tab transitions advance the
///          revision; consuming active or close edges never changes it.
/// @param tabbar TabBar widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_tabbar_get_revision(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb ? rt_widget_get_revision(&tb->base) : 0;
}

/// @brief Set the font and logical point size used for every tab title.
void rt_tabbar_set_font(void *tabbar, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!tb || !checked_font)
        return;
    double logical_size = rt_gui_sanitize_font_size(size, 14.0);
    float effective_size = rt_gui_logical_length_to_physical(&tb->base, logical_size);
    vg_tabbar_set_font(tb, checked_font, effective_size);
    tb->base.runtime_font_reference = checked_font;
}

/// @brief Consume the TabBar's independent successful-reorder edge.
int64_t rt_tabbar_was_reordered(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb && vg_tabbar_was_reordered(tb) ? 1 : 0;
}

/// @brief Return the source index from the most recent successful reorder.
int64_t rt_tabbar_get_reordered_from(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb ? (int64_t)vg_tabbar_get_reordered_from(tb) : -1;
}

/// @brief Return the destination index from the most recent successful reorder.
int64_t rt_tabbar_get_reordered_to(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb ? (int64_t)vg_tabbar_get_reordered_to(tb) : -1;
}

/// @brief Move a tab between validated zero-based indices.
int64_t rt_tabbar_move_tab(void *tabbar, int64_t from_index, int64_t to_index) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb || from_index < 0 || to_index < 0 || from_index > INT_MAX || to_index > INT_MAX)
        return 0;
    return vg_tabbar_move_tab(tb, (int)from_index, (int)to_index) ? 1 : 0;
}

/// @brief Get the number of tabs in the tab bar.
int64_t rt_tabbar_get_tab_count(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    return tb ? tb->tab_count : 0;
}

/// @brief Check if any tab's close button was clicked this frame.
int64_t rt_tabbar_was_close_clicked(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return 0;
    if (tb->reported_close_click_version != tb->close_click_version) {
        tb->reported_close_click_version = tb->close_click_version;
        return 1;
    }
    return 0;
}

/// @brief Get the index of the tab whose close button was clicked (clears after read).
int64_t rt_tabbar_get_close_clicked_index(void *tabbar) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return -1;
    if (tb->close_clicked_index < 0)
        return -1;
    int index = tb->close_clicked_index;
    tb->close_clicked_index = -1;
    tb->reported_close_click_version = tb->close_click_version;
    return index;
}

/// @brief Return the tab at position `index`, or NULL if out of range.
void *rt_tabbar_get_tab_at(void *tabbar, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return NULL;
    if (index < 0 || index > INT_MAX || index >= (int64_t)tb->tab_count)
        return NULL;
    return rt_gui_wrap_tab(vg_tabbar_get_tab_at(tb, (int)index));
}

/// @brief `TabBar.GetTabIndexAt(x, y)` — index of the tab under the point, or -1.
int64_t rt_tabbar_get_tab_index_at(void *tabbar, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (!tb)
        return -1;
    return (int64_t)vg_tabbar_index_at(tb, (int)x, (int)y);
}

/// @brief Enable or disable automatic tab removal on close-button click.
void rt_tabbar_set_auto_close(void *tabbar, int64_t auto_close) {
    RT_ASSERT_MAIN_THREAD();
    vg_tabbar_t *tb = rt_tabbar_checked(tabbar);
    if (tb) {
        tb->auto_close = auto_close != 0;
    }
}

//=============================================================================
// SplitPane Widget
//=============================================================================

/// @brief Create a new resizable split pane widget.
/// @details Creates a vg_splitpane_t that divides its area into two panels
///          separated by a draggable divider. The panels are accessible via
///          rt_splitpane_get_first/get_second; add children to those containers.
/// @param parent     Parent container or app handle.
/// @param horizontal Non-zero for a horizontal split (left|right), zero for vertical (top|bottom).
/// @return Opaque split pane widget handle, or NULL on failure.
void *rt_splitpane_new(void *parent, int64_t horizontal) {
    RT_ASSERT_MAIN_THREAD();
    vg_split_direction_t direction = horizontal ? VG_SPLIT_HORIZONTAL : VG_SPLIT_VERTICAL;
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_splitpane_create(parent_widget, direction);
}

/// @brief Set the divider position as a fraction of the split pane's size.
void rt_splitpane_set_position(void *split, double position) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (sp) {
        vg_splitpane_set_position(
            sp,
            (float)rt_gui_clamp_f64(rt_gui_double_is_finite(position) ? position : 0.5, 0.0, 1.0));
    }
}

// BINDING-006: SplitPane position query
/// @brief Get the position of the splitpane.
double rt_splitpane_get_position(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (!sp)
        return 0.5;
    return (double)vg_splitpane_get_position(sp);
}

/// @brief Set the first pane's minimum size in logical UI units.
/// @details The runtime scales the finite, non-negative input exactly once for the split pane's
///          current application. Invalid handles are ignored and non-finite values become zero.
/// @param split SplitPane widget handle.
/// @param size Minimum logical width for a horizontal split or height for a vertical split.
void rt_splitpane_set_min_first(void *split, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (!sp)
        return;
    double logical = rt_gui_double_is_finite(size) && size > 0.0 ? size : 0.0;
    vg_splitpane_set_min_first(sp, rt_gui_logical_length_to_physical(&sp->base, logical));
}

/// @brief Set the second pane's minimum size in logical UI units.
/// @details The runtime scales the finite, non-negative input exactly once for the split pane's
///          current application. Invalid handles are ignored and non-finite values become zero.
/// @param split SplitPane widget handle.
/// @param size Minimum logical width for a horizontal split or height for a vertical split.
void rt_splitpane_set_min_second(void *split, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (!sp)
        return;
    double logical = rt_gui_double_is_finite(size) && size > 0.0 ? size : 0.0;
    vg_splitpane_set_min_second(sp, rt_gui_logical_length_to_physical(&sp->base, logical));
}

/// @brief Return the first pane's configured minimum in logical UI units.
/// @param split SplitPane widget handle.
/// @return Non-negative logical minimum, or zero for an invalid handle.
double rt_splitpane_get_min_first(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    return sp ? rt_gui_physical_to_logical(&sp->base, vg_splitpane_get_min_first(sp)) : 0.0;
}

/// @brief Return the second pane's configured minimum in logical UI units.
/// @param split SplitPane widget handle.
/// @return Non-negative logical minimum, or zero for an invalid handle.
double rt_splitpane_get_min_second(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    return sp ? rt_gui_physical_to_logical(&sp->base, vg_splitpane_get_min_second(sp)) : 0.0;
}

/// @brief Return the split pane orientation.
/// @param split SplitPane widget handle.
/// @return Zero for horizontal (left/right), one for vertical (top/bottom), or -1 when invalid.
int64_t rt_splitpane_get_orientation(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    return sp ? (int64_t)vg_splitpane_get_direction(sp) : -1;
}

/// @brief Collapse the first (left or top) pane while retaining the restore fraction.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_collapse_first(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (sp)
        vg_splitpane_collapse_first(sp);
}

/// @brief Collapse the second (right or bottom) pane while retaining the restore fraction.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_collapse_second(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (sp)
        vg_splitpane_collapse_second(sp);
}

/// @brief Restore both panes to the divider fraction retained before collapse.
/// @param split SplitPane widget handle; invalid handles are ignored.
void rt_splitpane_restore(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (sp)
        vg_splitpane_restore(sp);
}

/// @brief Return the split pane's explicit collapsed-side state.
/// @param split SplitPane widget handle.
/// @return Zero for none, one for first, two for second, or -1 for an invalid handle.
int64_t rt_splitpane_get_collapsed_side(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    return sp ? (int64_t)vg_splitpane_get_collapsed_side(sp) : -1;
}

/// @brief Return the first (left/top) panel container of a split pane.
/// Add child widgets to this container to populate the leading half.
void *rt_splitpane_get_first(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (!sp)
        return NULL;
    return vg_splitpane_get_first(sp);
}

/// @brief Return the second (right/bottom) panel container of a split pane.
void *rt_splitpane_get_second(void *split) {
    RT_ASSERT_MAIN_THREAD();
    vg_splitpane_t *sp = rt_splitpane_checked(split);
    if (!sp)
        return NULL;
    return vg_splitpane_get_second(sp);
}

//=============================================================================
// CodeEditor Widget
//=============================================================================

/// @brief Create a new source code editor widget.
/// @details Creates a full-featured vg_codeeditor_t with line numbers, syntax
///          highlighting, text selection, clipboard support, and undo/redo.
///          Designed for displaying and editing source code in IDE-style UIs.
/// @param parent Parent container or app handle.
/// @return Opaque code editor widget handle, or NULL on failure.
void *rt_codeeditor_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_codeeditor_t *editor = vg_codeeditor_create(parent_widget);
    if (editor)
        rt_gui_apply_default_font((vg_widget_t *)editor);
    return editor;
}

/// @brief Replace the entire text content of a code editor.
void rt_codeeditor_set_text(void *editor, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return;
    int64_t len64 = text ? rt_str_len(text) : 0;
    if (len64 < 0)
        return;
    size_t len = (size_t)len64;
    const char *bytes = len ? rt_string_cstr(text) : "";
    if (len && !bytes)
        return;
    vg_codeeditor_set_text_bytes(ce, bytes, len);
}

/// @brief Retrieve the full text content of a code editor (caller frees the C string).
rt_string rt_codeeditor_get_text(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return rt_str_empty();
    return rt_codeeditor_all_text_to_rt_string(ce);
}

/// @brief Retrieve the code editor's monotonic content revision.
int64_t rt_codeeditor_get_revision(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return 0;
    uint64_t revision = vg_codeeditor_get_revision(ce);
    return revision > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)revision;
}

/// @brief Serialize the editor's buffered edit deltas after @p since_revision as
///        compact JSON for incremental language-service sync (plan 08).
/// @details Returns the literal "overflow" when a cold mutation (undo/redo/
///          SetText/buffer swap) or a journal wrap means the deltas cannot be
///          applied incrementally — the caller must then full-sync. Taking the
///          deltas drains the journal, so each delta is delivered exactly once.
rt_string rt_codeeditor_take_deltas(void *editor, int64_t since_revision) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return rt_str_empty();
    uint64_t since = since_revision < 0 ? 0u : (uint64_t)since_revision;
    char *json = vg_codeeditor_take_deltas_json(ce, since);
    if (!json)
        return rt_string_from_bytes("overflow", 8); // OOM: force a safe full-sync
    rt_string result = rt_string_from_bytes(json, strlen(json));
    free(json);
    return result;
}

/// @brief Retrieve the currently selected text in a code editor.
rt_string rt_codeeditor_get_selected_text(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce || !ce->has_selection)
        return rt_str_empty();
    return rt_codeeditor_range_to_rt_string(ce,
                                            ce->selection.start_line,
                                            ce->selection.start_col,
                                            ce->selection.end_line,
                                            ce->selection.end_col);
}

/// @brief Move the cursor to a specific line and column in the code editor.
void rt_codeeditor_set_cursor(void *editor, int64_t line, int64_t col) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (ce) {
        vg_codeeditor_set_cursor(ce,
                                 rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX),
                                 rt_gui_clamp_i64_to_i32(col, 0, INT32_MAX));
    }
}

/// @brief Scroll the code editor viewport to make a specific line visible.
void rt_codeeditor_scroll_to_line(void *editor, int64_t line) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (ce) {
        vg_codeeditor_scroll_to_line(ce, rt_gui_clamp_i64_to_i32(line, 0, INT32_MAX));
    }
}

/// @brief Get the total number of lines in the code editor.
int64_t rt_codeeditor_get_line_count(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_get_line_count(ce);
}

/// @brief Check whether the code editor's content has been modified since last clear.
int64_t rt_codeeditor_is_modified(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (!ce)
        return 0;
    return vg_codeeditor_is_modified(ce) ? 1 : 0;
}

/// @brief Reset the code editor's modified flag (e.g., after saving).
void rt_codeeditor_clear_modified(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (ce) {
        vg_codeeditor_clear_modified(ce);
    }
}

/// @brief Set the font of the codeeditor.
void rt_codeeditor_set_font(void *editor, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ce = rt_codeeditor_checked(editor);
    if (ce) {
        vg_font_t *checked_font = rt_gui_font_handle_checked(font);
        if (!checked_font)
            return;
        vg_codeeditor_set_font(ce, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
        ce->base.runtime_font_reference = checked_font;
        // Pin the editor's font so a later app-wide SetFont (which propagates the
        // proportional chrome font to the whole widget tree) cannot replace it and
        // desync char_width from the rendered glyph advances.
        ce->font_pinned = true;
    }
}

/// @brief Get the stored font size of the code editor in the same units used by SetFont.
double rt_codeeditor_get_font_size(void *editor) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ed = rt_codeeditor_checked(editor);
    if (!ed)
        return 14.0;
    return (double)ed->font_size;
}

/// @brief Set the code editor font size in the same units used by SetFont.
void rt_codeeditor_set_font_size(void *editor, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_codeeditor_t *ed = rt_codeeditor_checked(editor);
    if (!ed)
        return;
    if (rt_gui_double_is_finite(size) && size > 0.0) {
        vg_codeeditor_set_font(ed, ed->font, (float)rt_gui_sanitize_font_size(size, 14.0));
        // Explicit size change is an editor-owned font decision; pin it so the
        // app-wide chrome font cannot override it (see rt_codeeditor_set_font).
        ed->font_pinned = true;
    }
}

//=============================================================================
// Theme Functions
//=============================================================================

/// @brief Recompute the theme's HiDPI-scaled dimensions from the active window.
void rt_theme_apply_hidpi_scale(void) {
    rt_gui_refresh_theme(rt_gui_get_active_app());
}

/// @brief Switch the active theme to dark mode.
void rt_theme_set_dark(void) {
    rt_theme_set_mode(RT_GUI_THEME_DARK);
}

/// @brief Switch the active theme to light mode.
void rt_theme_set_light(void) {
    rt_theme_set_mode(RT_GUI_THEME_LIGHT);
}

/// @brief Get the stable lowercase name of the selected theme mode.
rt_string rt_theme_get_name(void) {
    RT_ASSERT_MAIN_THREAD();
    const char *name;
    switch (rt_theme_get_mode()) {
        case RT_GUI_THEME_LIGHT:
            name = "light";
            break;
        case RT_GUI_THEME_SYSTEM:
            name = "system";
            break;
        case RT_GUI_THEME_CUSTOM:
            name = "custom";
            break;
        case RT_GUI_THEME_DARK:
        default:
            name = "dark";
            break;
    }
    return rt_string_from_bytes(name, strlen(name));
}

//=============================================================================
// Layout Functions
//=============================================================================

/// @brief Create a vertical box-layout container (children stacked top-to-bottom).
void *rt_vbox_new(void) {
    RT_ASSERT_MAIN_THREAD();
    return vg_vbox_create(0.0f);
}

/// @brief Create a horizontal box-layout container (children laid out left-to-right).
void *rt_hbox_new(void) {
    RT_ASSERT_MAIN_THREAD();
    return vg_hbox_create(0.0f);
}

/// @brief Set the spacing of the container.
void rt_container_set_spacing(void *container, double spacing) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *widget = rt_gui_widget_handle_checked(container);
    if (!widget)
        return;
    vg_container_set_spacing(widget, rt_gui_logical_length_to_physical(widget, spacing));
}

/// @brief Set the padding of the container.
void rt_container_set_padding(void *container, double padding) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *widget = rt_gui_widget_handle_checked(container);
    if (widget) {
        vg_widget_set_padding(widget, rt_gui_logical_length_to_physical(widget, padding));
    }
}

//=============================================================================
// Widget State Functions
//=============================================================================

/// @brief Check whether the mouse cursor is currently over this widget.
int64_t rt_widget_is_hovered(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (w->state & VG_STATE_HOVERED) ? 1 : 0;
}

/// @brief Check whether the widget is currently being pressed (mouse down).
int64_t rt_widget_is_pressed(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (w->state & VG_STATE_PRESSED) ? 1 : 0;
}

/// @brief Check whether the widget currently has keyboard focus.
int64_t rt_widget_is_focused(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    return (w->state & VG_STATE_FOCUSED) ? 1 : 0;
}

/// @brief Move keyboard focus to a widget that participates in the tab order.
void rt_widget_focus(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    vg_widget_set_focus(w);
}

/// @brief Set the last clicked value.
/// @param widget
void rt_gui_set_last_clicked(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (widget && !w)
        return;
    rt_gui_app_t *app = w ? rt_gui_app_from_widget(w) : rt_gui_get_active_app();
    if (app)
        app->last_clicked = w;
}

/// @brief Check whether this widget was clicked during the current frame.
int64_t rt_widget_was_clicked(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    rt_gui_app_t *app = rt_gui_app_from_widget(w);
    return (app && app->last_clicked == w) ? 1 : 0;
}

/// @brief Set the position of the widget.
/// @details Intended for widgets that are manually positioned outside managed
///          layout containers. Managed layouts may override x/y on the next
///          layout pass. Public coordinates are logical and are converted once
///          using the owning app's effective scale; detached widgets use 1x.
/// @param widget Widget to position; invalid handles are ignored.
/// @param x Parent-relative logical X coordinate.
/// @param y Parent-relative logical Y coordinate.
void rt_widget_set_position(void *widget, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (w) {
        float physical_x = rt_gui_logical_coordinate_to_physical(w, (double)x);
        float physical_y = rt_gui_logical_coordinate_to_physical(w, (double)y);
        if (w->x == physical_x && w->y == physical_y && w->manual_position)
            return;
        w->x = physical_x;
        w->y = physical_y;
        w->manual_position = true;
        vg_widget_invalidate(w);
        vg_widget_note_revision(w);
    }
}

//=============================================================================
// OutputPane Widget
//=============================================================================

/// @brief Create an append-only ANSI-aware output pane.
void *rt_outputpane_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_outputpane_t *pane = vg_outputpane_create();
    if (!pane)
        return NULL;
    if (parent_widget)
        vg_widget_add_child(parent_widget, &pane->base);
    rt_gui_apply_default_font(&pane->base);
    return pane;
}

/// @brief Append text, parsing ANSI SGR escape sequences.
void rt_outputpane_append(void *pane, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_outputpane_append(out, ctext);
    free(ctext);
}

/// @brief Append text as a complete line.
void rt_outputpane_append_line(void *pane, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_outputpane_append_line(out, ctext);
    free(ctext);
}

/// @brief Append a single explicitly styled segment.
void rt_outputpane_append_styled(void *pane, rt_string text, int64_t fg, int64_t bg, int64_t bold) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_outputpane_append_styled(out, ctext, (uint32_t)fg, (uint32_t)bg, bold != 0);
    free(ctext);
}

/// @brief Clear all output and reset ANSI state.
void rt_outputpane_clear(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_clear(out);
}

/// @brief Scroll to the first output line and lock auto-scroll.
void rt_outputpane_scroll_to_top(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_scroll_to_top(out);
}

/// @brief Scroll to the latest output line and unlock auto-scroll.
void rt_outputpane_scroll_to_bottom(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_scroll_to_bottom(out);
}

/// @brief Enable or disable automatic scrolling on append.
void rt_outputpane_set_auto_scroll(void *pane, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_set_auto_scroll(out, enabled != 0);
}

/// @brief Return selected output text.
rt_string rt_outputpane_get_selection(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return rt_str_empty();
    char *selection = vg_outputpane_get_selection(out);
    if (!selection)
        return rt_str_empty();
    rt_string result = rt_string_from_bytes(selection, strlen(selection));
    free(selection);
    return result;
}

/// @brief Select all output text.
void rt_outputpane_select_all(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_select_all(out);
}

/// @brief Set the retained line cap.
void rt_outputpane_set_max_lines(void *pane, int64_t max_lines) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return;
    size_t max = max_lines <= 0 ? 1u : (size_t)max_lines;
    vg_outputpane_set_max_lines(out, max);
}

/// @brief Return the current retained line count.
int64_t rt_outputpane_get_line_count(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return 0;
    return out->line_count > (size_t)INT64_MAX ? INT64_MAX : (int64_t)out->line_count;
}

/// @brief Set the output pane font.
void rt_outputpane_set_font(void *pane, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_outputpane_set_font(out, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    out->base.runtime_font_reference = checked_font;
}

/// @brief Pixel advance of one monospace character cell ("M") in the pane's font.
int64_t rt_outputpane_get_cell_width(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    return out ? (int64_t)vg_outputpane_cell_width(out) : 0;
}

/// @brief Pixel height of one line in the pane's font.
int64_t rt_outputpane_get_cell_height(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    return out ? (int64_t)vg_outputpane_cell_height(out) : 0;
}

/// @brief Pixel width of @p text rendered in the pane's font.
int64_t rt_outputpane_measure_text(void *pane, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return 0;
    char *ctext = rt_string_to_gui_cstr(text);
    int64_t width = (int64_t)vg_outputpane_measure_text(out, ctext);
    free(ctext);
    return width;
}

/// @brief Whole character columns that fit across the pane's arranged width.
int64_t rt_outputpane_columns_for_width(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    return out ? (int64_t)vg_outputpane_columns_for_width(out) : 0;
}

/// @brief Whole rows that fit down the pane's arranged height.
int64_t rt_outputpane_rows_for_height(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    return out ? (int64_t)vg_outputpane_rows_for_height(out) : 0;
}

/// @brief Enable/disable interactive terminal mode (cursor model + keyboard capture).
void rt_outputpane_set_terminal_mode(void *pane, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (out)
        vg_outputpane_set_terminal_mode(out, enabled != 0);
}

/// @brief Drain queued terminal keystrokes (terminal mode); empty when none pending.
rt_string rt_outputpane_take_input(void *pane) {
    RT_ASSERT_MAIN_THREAD();
    vg_outputpane_t *out = rt_outputpane_checked(pane);
    if (!out)
        return rt_str_empty();
    size_t input_len = 0;
    char *input = vg_outputpane_take_input_bytes(out, &input_len);
    if (!input)
        return rt_str_empty();
    rt_string result = rt_string_from_bytes(input, input_len);
    free(input);
    return result;
}

//=============================================================================
// RadioButton Widget
//=============================================================================

/// @brief Create a radio-button group — only one member may be selected at a time.
void *rt_radiogroup_new(void) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiogroup_t *group = vg_radiogroup_create();
    if (!group)
        return NULL;
    rt_radiogroup_data_t *data =
        (rt_radiogroup_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_radiogroup_data_t));
    if (!data) {
        vg_radiogroup_destroy(group);
        return NULL;
    }
    data->magic = RT_RADIOGROUP_MAGIC;
    data->group = group;
    if (!rt_radiogroup_registry_add(data)) {
        vg_radiogroup_destroy(group);
        data->magic = RT_RADIOGROUP_DESTROYED_MAGIC;
        data->group = NULL;
        return NULL;
    }
    rt_obj_set_finalizer(data, rt_radiogroup_finalize);
    return data;
}

/// @brief Release resources and destroy the radiogroup.
void rt_radiogroup_destroy(void *group) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_dispose((rt_radiogroup_data_t *)group);
}

/// @brief Return the selected member index of a live radio group.
/// @param group RadioGroup handle.
/// @return Zero-based selected index, -1 for no selection, or -1 for an invalid handle.
int64_t rt_radiogroup_get_selected_index(void *group) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_data_t *data = rt_radiogroup_handle_checked(group);
    return data ? (int64_t)vg_radiogroup_get_selected(data->group) : -1;
}

/// @brief Attempt to select a radio-group member by index.
/// @details Index -1 clears selection. Values outside `[-1, GetCount())`, invalid handles, and
///          values that cannot fit the toolkit index type are rejected without mutation.
/// @param group RadioGroup handle.
/// @param index Zero-based member index or -1.
/// @return 1 when the request is valid, otherwise 0.
int64_t rt_radiogroup_set_selected_index(void *group, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_data_t *data = rt_radiogroup_handle_checked(group);
    if (!data || index < -1 || index > INT_MAX)
        return 0;
    return vg_radiogroup_try_set_selected(data->group, (int)index) ? 1 : 0;
}

/// @brief Return the number of live buttons registered with a radio group.
/// @param group RadioGroup handle.
/// @return Non-negative member count, or zero for an invalid handle.
int64_t rt_radiogroup_get_count(void *group) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_data_t *data = rt_radiogroup_handle_checked(group);
    return data ? (int64_t)vg_radiogroup_get_count(data->group) : 0;
}

/// @brief Consume the radio group's independent selected-index transition edge.
/// @details Multiple unreported transitions coalesce; membership-only changes and this read do
///          not alter the non-consuming general revision.
/// @param group RadioGroup handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_radiogroup_was_changed(void *group) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_data_t *data = rt_radiogroup_handle_checked(group);
    return data && vg_radiogroup_was_changed(data->group) ? 1 : 0;
}

/// @brief Return the radio group's non-consuming state revision.
/// @param group RadioGroup handle.
/// @return Monotonic signed revision saturated at INT64_MAX, or zero when invalid.
int64_t rt_radiogroup_get_revision(void *group) {
    RT_ASSERT_MAIN_THREAD();
    rt_radiogroup_data_t *data = rt_radiogroup_handle_checked(group);
    if (!data)
        return 0;
    uint64_t revision = vg_radiogroup_get_revision(data->group);
    return revision > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)revision;
}

/// @brief Create a single radio button bound to a given group.
/// Selecting one radio in the group automatically deselects the others.
void *rt_radiobutton_new(void *parent, rt_string text, void *group) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    rt_radiogroup_data_t *group_data = NULL;
    if (group) {
        group_data = rt_radiogroup_handle_checked(group);
        if (!group_data)
            return NULL;
    }
    char *ctext = rt_string_to_gui_cstr(text);
    vg_radiobutton_t *radio =
        vg_radiobutton_create(parent_widget, ctext, group_data ? group_data->group : NULL);
    free(ctext);
    if (radio)
        rt_gui_apply_default_font((vg_widget_t *)radio);
    return radio;
}

/// @brief Check whether a radio button is currently selected in its group.
int64_t rt_radiobutton_is_selected(void *radio) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    if (!rb)
        return 0;
    return vg_radiobutton_is_selected(rb) ? 1 : 0;
}

/// @brief Programmatically select a radio button (deselects siblings in the group).
void rt_radiobutton_set_selected(void *radio, int64_t selected) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    if (rb) {
        vg_radiobutton_set_selected(rb, selected != 0);
    }
}

/// @brief Replace a radio button's visible text.
/// @details Runtime strings are converted with the GUI UTF-8 boundary policy and copied
///          atomically by the widget. Invalid handles are ignored.
/// @param radio RadioButton widget handle.
/// @param text New visible text.
void rt_radiobutton_set_text(void *radio, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    if (!rb)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (text && !ctext)
        return;
    vg_radiobutton_set_text(rb, rt_gui_cstr_or_empty(ctext));
    free(ctext);
}

/// @brief Return a radio button's visible text as a fresh runtime string.
/// @param radio RadioButton widget handle.
/// @return Current UTF-8 text, or an empty string for an invalid handle.
rt_string rt_radiobutton_get_text(void *radio) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    const char *text = rb ? vg_radiobutton_get_text(rb) : NULL;
    return text ? rt_string_from_bytes(text, strlen(text)) : rt_str_empty();
}

/// @brief Store byte-exact runtime string data on a radio button.
/// @details Allocation succeeds before the prior payload is released, so allocation failure
///          preserves the old value. Embedded NUL bytes round-trip through GetData.
/// @param radio RadioButton widget handle.
/// @param data Runtime string to copy; NULL clears the payload.
void rt_radiobutton_set_data(void *radio, rt_string data) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    if (!rb)
        return;
    rt_gui_string_data_t *copy = data ? rt_gui_string_data_new(data) : NULL;
    if (data && !copy)
        return;
    vg_radiobutton_set_data(rb, copy);
    rb->owns_user_data = copy != NULL;
}

/// @brief Return byte-exact runtime string data stored on a radio button.
/// @param radio RadioButton widget handle.
/// @return Fresh runtime string, or empty when absent, borrowed-only, or invalid.
rt_string rt_radiobutton_get_data(void *radio) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    return rb && rb->owns_user_data ? rt_gui_string_data_to_rt_string(vg_radiobutton_get_data(rb))
                                    : rt_str_empty();
}

/// @brief Consume the radio button's independent selected-state edge.
/// @details Direct transitions and group-driven deselection both record the
///          edge before any optional callback runs.
/// @param radio RadioButton widget handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_radiobutton_was_changed(void *radio) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    return rb ? (vg_widget_was_changed(&rb->base) ? 1 : 0) : 0;
}

/// @brief Return the radio button's non-consuming state revision.
/// @param radio RadioButton widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_radiobutton_get_revision(void *radio) {
    RT_ASSERT_MAIN_THREAD();
    vg_radiobutton_t *rb = rt_radiobutton_checked(radio);
    return rb ? rt_widget_get_revision(&rb->base) : 0;
}

//=============================================================================
// Spinner Widget
//=============================================================================

/// @brief Create a numeric spinner widget (text field with up/down increment buttons).
void *rt_spinner_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_spinner_t *spinner = vg_spinner_create(parent_widget);
    if (spinner)
        rt_gui_apply_default_font((vg_widget_t *)spinner);
    return spinner;
}

/// @brief Set the value of the spinner.
void rt_spinner_set_value(void *spinner, double value) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (sp) {
        if (!rt_gui_double_is_finite(value))
            return;
        vg_spinner_set_value(
            sp, rt_gui_clamp_f64(value, -RT_GUI_MAX_LAYOUT_VALUE, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Get the value of the spinner.
double rt_spinner_get_value(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (!sp)
        return 0.0;
    return vg_spinner_get_value(sp);
}

/// @brief Set whether a spinner presents a mixed group value.
/// @details Mixed retains the current numeric value as an editing seed.
///          Assigning a concrete value or beginning user input clears it.
/// @param spinner Spinner widget handle.
/// @param indeterminate Non-zero to present mixed; zero to reveal the value.
void rt_spinner_set_indeterminate(void *spinner, int64_t indeterminate) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (sp)
        vg_spinner_set_indeterminate(sp, indeterminate != 0);
}

/// @brief Query whether a spinner presents a mixed group value.
/// @param spinner Spinner widget handle.
/// @return 1 while mixed, otherwise zero.
int64_t rt_spinner_is_indeterminate(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    return sp && vg_spinner_is_indeterminate(sp) ? 1 : 0;
}

/// @brief Set the range of the spinner.
void rt_spinner_set_range(void *spinner, double min_val, double max_val) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (sp) {
        if (!rt_gui_double_is_finite(min_val) || !rt_gui_double_is_finite(max_val))
            return;
        if (min_val > max_val) {
            double tmp = min_val;
            min_val = max_val;
            max_val = tmp;
        }
        min_val = rt_gui_clamp_f64(min_val, -RT_GUI_MAX_LAYOUT_VALUE, RT_GUI_MAX_LAYOUT_VALUE);
        max_val = rt_gui_clamp_f64(max_val, -RT_GUI_MAX_LAYOUT_VALUE, RT_GUI_MAX_LAYOUT_VALUE);
        vg_spinner_set_range(sp, min_val, max_val);
    }
}

/// @brief Set the step of the spinner.
void rt_spinner_set_step(void *spinner, double step) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (sp) {
        vg_spinner_set_step(
            sp, (double)rt_gui_sanitize_nonnegative_float(step, RT_GUI_MAX_LAYOUT_VALUE));
    }
}

/// @brief Set the decimals of the spinner.
void rt_spinner_set_decimals(void *spinner, int64_t decimals) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    if (sp) {
        vg_spinner_set_decimals(sp, rt_gui_clamp_i64_to_i32(decimals, 0, 9));
    }
}

/// @brief Consume the spinner's independent numeric-value change edge.
/// @details User input, programmatic assignments, and range clamping record
///          this edge only when the effective value changes.
/// @param spinner Spinner widget handle.
/// @return 1 once after one or more unreported value transitions, otherwise 0.
int64_t rt_spinner_was_changed(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    return sp ? (vg_widget_was_changed(&sp->base) ? 1 : 0) : 0;
}

/// @brief Consume the spinner's independent valid-submission edge.
/// @details Pressing Enter after valid numeric editing records submission even
///          when the resulting value equals the previous value.
/// @param spinner Spinner widget handle.
/// @return 1 once after one or more unreported submissions, otherwise 0.
int64_t rt_spinner_was_submitted(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    return sp ? (vg_widget_was_submitted(&sp->base) ? 1 : 0) : 0;
}

/// @brief Consume the spinner's value-scrub completion edge.
/// @details A scrub is a horizontal value-area drag; the edge fires once per
///          completed gesture so pollers can commit one coalesced edit.
/// @param spinner Spinner widget handle.
/// @return 1 once per completed scrub gesture, otherwise 0.
int64_t rt_spinner_was_scrub_finished(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    return sp && vg_spinner_was_scrub_finished(sp) ? 1 : 0;
}

/// @brief Return the spinner's non-consuming state revision.
/// @param spinner Spinner widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_spinner_get_revision(void *spinner) {
    RT_ASSERT_MAIN_THREAD();
    vg_spinner_t *sp = rt_spinner_checked(spinner);
    return sp ? rt_widget_get_revision(&sp->base) : 0;
}

//=============================================================================
// Grid (tabular data with auto-sized columns) — Zanna.GUI.Grid
//=============================================================================

static vg_datagrid_t *rt_datagrid_checked(void *handle) {
    return (vg_datagrid_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_DATAGRID);
}

/// @brief Convert a non-negative runtime integer to a native row/count value.
/// @details The conversion is explicit so 32-bit builds reject values that do not fit `size_t`
///          instead of truncating or redirecting the operation to a different row.
/// @param value Runtime integer to convert.
/// @param out_value Receives the exact native value when successful.
/// @return true when @p value is non-negative and representable by `size_t`.
static bool rt_datagrid_i64_to_size(int64_t value, size_t *out_value) {
    if (!out_value || value < 0 || (uint64_t)value > (uint64_t)SIZE_MAX)
        return false;
    *out_value = (size_t)value;
    return true;
}

/// @brief Convert a non-negative runtime integer to a native column index.
/// @param value Runtime integer to convert.
/// @param out_value Receives the exact native `int` value when successful.
/// @return true when @p value is in `[0, INT_MAX]`.
static bool rt_datagrid_i64_to_column(int64_t value, int *out_value) {
    if (!out_value || value < 0 || value > INT_MAX)
        return false;
    *out_value = (int)value;
    return true;
}

/// @brief Convert a native row/count value to the signed runtime domain safely.
/// @param value Native value to convert.
/// @return Exact value when representable, otherwise `INT64_MAX`.
static int64_t rt_datagrid_size_to_i64(size_t value) {
    return (uint64_t)value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

/// @brief Create a viewport-aware tabular grid attached to an optional parent.
/// @details The grid starts in display-only dense compatibility mode. Its runtime application's
///          default font is applied immediately so automatic widths and initial paint are usable.
/// @param parent Optional live parent widget handle.
/// @return New Grid handle, or NULL when the parent is invalid/allocation fails.
void *rt_datagrid_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_datagrid_t *grid = vg_datagrid_create(parent_widget);
    if (grid)
        rt_gui_apply_default_font(&grid->base);
    return grid;
}

/// @brief Set the grid's column count (clears existing headers and cells).
void rt_datagrid_set_columns(void *grid, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_set_columns(g, rt_gui_clamp_i64_to_i32(count, 0, 4096));
}

/// @brief Set a column header.
void rt_datagrid_set_header(void *grid, int64_t col, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_column(col, &native_col))
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    vg_datagrid_set_header(g, native_col, ctext);
    free(ctext);
}

/// @brief Set a cell's text, growing the row count as needed.
void rt_datagrid_set_cell(void *grid, int64_t row, int64_t col, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    int native_row = 0;
    int native_col = 0;
    if (!g || row < 0 || row >= INT_MAX || !rt_datagrid_i64_to_column(col, &native_col))
        return;
    native_row = (int)row;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    vg_datagrid_set_cell(g, native_row, native_col, ctext);
    free(ctext);
}

/// @brief Return a cell's text (empty string when out of range).
rt_string rt_datagrid_get_cell(void *grid, int64_t row, int64_t col) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_row = 0;
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_size(row, &native_row) ||
        !rt_datagrid_i64_to_column(col, &native_col))
        return rt_str_empty();
    const char *cell = vg_datagrid_get_cell(g, native_row, native_col);
    return cell ? rt_string_from_bytes(cell, (int64_t)strlen(cell)) : rt_str_empty();
}

/// @brief Remove all rows.
void rt_datagrid_clear(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_clear(g);
}

/// @brief Set the grid's header/cell font using a logical point size.
/// @details The logical size is sanitized and converted through the owning app's effective scale
///          exactly once before reaching the physical-pixel toolkit.
void rt_datagrid_set_font(void *grid, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (!g)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    double logical_size = rt_gui_sanitize_font_size(size, 14.0);
    vg_datagrid_set_font(
        g, checked_font, rt_gui_logical_length_to_physical(&g->base, logical_size));
    g->base.runtime_font_reference = checked_font;
}

/// @brief Auto-sized pixel width of a column.
int64_t rt_datagrid_get_column_width(void *grid, int64_t col) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_column_width(g, rt_gui_clamp_i64_to_i32(col, 0, 4096)) : 0;
}

/// @brief Exact logical row count, saturated only at the runtime signed boundary.
int64_t rt_datagrid_get_row_count(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? rt_datagrid_size_to_i64(vg_datagrid_logical_row_count(g)) : 0;
}

/// @brief Number of columns.
int64_t rt_datagrid_get_column_count(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_column_count(g) : 0;
}

/// @brief Set the first and maximum number of logical rows in the paint viewport.
/// @details No row-proportional storage is created. A count of zero selects height-derived
///          capacity. Negative or non-representable inputs preserve the previous viewport.
/// @param grid Grid widget handle.
/// @param first Zero-based first logical row.
/// @param count Maximum visible rows, or zero for automatic capacity.
void rt_datagrid_set_viewport_rows(void *grid, int64_t first, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_first = 0;
    size_t native_count = 0;
    if (g && rt_datagrid_i64_to_size(first, &native_first) &&
        rt_datagrid_i64_to_size(count, &native_count))
        vg_datagrid_set_viewport_rows(g, native_first, native_count);
}

/// @brief Switch the Grid to sparse virtual mode with a logical row count.
/// @details The operation clears dense rows only after input validation. It never allocates one
///          object per logical row, so very large model sizes remain bounded by materialized cells.
/// @param grid Grid widget handle.
/// @param count Non-negative logical row count.
void rt_datagrid_set_virtual_row_count(void *grid, int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_count = 0;
    if (g && rt_datagrid_i64_to_size(count, &native_count))
        vg_datagrid_set_virtual_row_count(g, native_count);
}

/// @brief Materialize, replace, or clear one copied sparse virtual-cell value.
/// @details Embedded NUL bytes are converted to U+FFFD. Invalid indices and conversion/allocation
///          failure preserve the previous sparse model atomically.
/// @param grid Virtual Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @param text Visible UTF-8 text; empty text clears an existing materialization.
void rt_datagrid_set_virtual_cell(void *grid, int64_t row, int64_t col, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_row = 0;
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_size(row, &native_row) ||
        !rt_datagrid_i64_to_column(col, &native_col))
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return;
    (void)vg_datagrid_set_virtual_cell(g, native_row, native_col, ctext);
    free(ctext);
}

/// @brief Enable or disable pointer and keyboard cell selection.
/// @param grid Grid widget handle.
/// @param enabled Non-zero to enable selection; zero to disable and clear it.
void rt_datagrid_set_selectable(void *grid, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_set_selectable(g, enabled != 0);
}

/// @brief Return the selected logical row.
/// @param grid Grid widget handle.
/// @return Zero-based row, or -1 when selection/handle is absent.
int64_t rt_datagrid_get_selected_row(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (!g)
        return -1;
    size_t row = vg_datagrid_get_selected_row(g);
    return row == SIZE_MAX ? -1 : rt_datagrid_size_to_i64(row);
}

/// @brief Return the selected column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when selection/handle is absent.
int64_t rt_datagrid_get_selected_column(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_get_selected_column(g) : -1;
}

/// @brief Select one valid logical cell.
/// @param grid Selectable Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return 1 when the cell is valid and selected/already selected, otherwise 0.
int64_t rt_datagrid_select_cell(void *grid, int64_t row, int64_t col) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_row = 0;
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_size(row, &native_row) ||
        !rt_datagrid_i64_to_column(col, &native_col))
        return 0;
    return vg_datagrid_select_cell(g, native_row, native_col) ? 1 : 0;
}

/// @brief Clear any current Grid selection.
/// @param grid Grid widget handle; invalid/already-clear handles are no-ops.
void rt_datagrid_clear_selection(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_clear_selection(g);
}

/// @brief Consume the independent Grid selection transition edge.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported selection transitions, otherwise 0.
int64_t rt_datagrid_was_selection_changed(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_datagrid_was_selection_changed(g) ? 1 : 0;
}

/// @brief Consume the independent Grid cell-activation edge.
/// @details Double-click and keyboard Enter record activation without consuming selection/change
///          edges or the non-consuming revision.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported activations, otherwise 0.
int64_t rt_datagrid_was_activated(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_widget_was_activated(&g->base) ? 1 : 0;
}

/// @brief Enable or disable sort requests for one column.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param enabled Non-zero to enable header/API sort requests.
void rt_datagrid_set_sortable(void *grid, int64_t col, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    int native_col = 0;
    if (g && rt_datagrid_i64_to_column(col, &native_col))
        vg_datagrid_set_sortable(g, native_col, enabled != 0);
}

/// @brief Record a normalized sort request without reordering caller-owned data.
/// @details Direction normalizes to -1, 0, or 1. Nonzero directions require an enabled sortable
///          column; invalid requests preserve existing state.
/// @param grid Grid widget handle.
/// @param col Zero-based column for nonzero direction.
/// @param direction Negative descending, zero unsorted, positive ascending.
void rt_datagrid_set_sort(void *grid, int64_t col, int64_t direction) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (!g)
        return;
    int native_col = -1;
    if (direction != 0 && !rt_datagrid_i64_to_column(col, &native_col))
        return;
    (void)vg_datagrid_set_sort(g, native_col, direction < 0 ? -1 : (direction > 0 ? 1 : 0));
}

/// @brief Return the active Grid sort column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when unsorted/invalid.
int64_t rt_datagrid_get_sort_column(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_get_sort_column(g) : -1;
}

/// @brief Return the active normalized Grid sort direction.
/// @param grid Grid widget handle.
/// @return -1 descending, 0 none/invalid, or 1 ascending.
int64_t rt_datagrid_get_sort_direction(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_get_sort_direction(g) : 0;
}

/// @brief Consume the independent Grid sort transition edge.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported sort changes, otherwise 0.
int64_t rt_datagrid_was_sort_changed(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_datagrid_was_sort_changed(g) ? 1 : 0;
}

/// @brief Set one explicit logical column width or reset it to automatic sizing.
/// @details Width zero selects automatic sizing. Finite non-negative values are converted through
///          effective UI scale exactly once; invalid values preserve the previous width.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param width Public logical width, or zero for automatic.
void rt_datagrid_set_column_width(void *grid, int64_t col, double width) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_column(col, &native_col) || !rt_gui_double_is_finite(width) ||
        width < 0.0)
        return;
    (void)vg_datagrid_set_column_width(
        g, native_col, rt_gui_logical_length_to_physical(&g->base, width));
}

/// @brief Enable or disable pointer resizing at one column boundary.
/// @param grid Grid widget handle.
/// @param col Zero-based column.
/// @param enabled Non-zero to enable pointer resizing.
void rt_datagrid_set_column_resizable(void *grid, int64_t col, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    int native_col = 0;
    if (g && rt_datagrid_i64_to_column(col, &native_col))
        vg_datagrid_set_column_resizable(g, native_col, enabled != 0);
}

/// @brief Consume the independent effective-column-resize edge.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported width changes, otherwise 0.
int64_t rt_datagrid_was_column_resized(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_datagrid_was_column_resized(g) ? 1 : 0;
}

/// @brief Return the most recently resized Grid column.
/// @param grid Grid widget handle.
/// @return Zero-based column, or -1 when no resize/invalid handle exists.
int64_t rt_datagrid_get_resized_column(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (int64_t)vg_datagrid_get_resized_column(g) : -1;
}

/// @brief Enable or disable externally-driven Grid cell editing.
/// @details Disabling cancels an active edit without changing cell content.
/// @param grid Grid widget handle.
/// @param enabled Non-zero to enable BeginEdit/CommitEdit.
void rt_datagrid_set_editable(void *grid, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_set_editable(g, enabled != 0);
}

/// @brief Begin editing one valid dense or sparse Grid cell.
/// @param grid Editable Grid widget handle.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @return 1 when editing began/already targets the cell, otherwise 0.
int64_t rt_datagrid_begin_edit(void *grid, int64_t row, int64_t col) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_row = 0;
    int native_col = 0;
    if (!g || !rt_datagrid_i64_to_size(row, &native_row) ||
        !rt_datagrid_i64_to_column(col, &native_col))
        return 0;
    return vg_datagrid_begin_edit(g, native_row, native_col) ? 1 : 0;
}

/// @brief Commit copied visible UTF-8 text to the active edit cell.
/// @details Embedded NUL bytes become U+FFFD. Conversion/allocation failure leaves edit mode and
///          content unchanged; effective text changes publish the independent edit edge.
/// @param grid Grid widget handle with an active edit.
/// @param text Replacement text; empty clears the cell.
/// @return 1 when the active edit committed successfully, otherwise 0.
int64_t rt_datagrid_commit_edit(void *grid, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (!g)
        return 0;
    char *ctext = rt_string_to_gui_cstr(text);
    if (!ctext)
        return 0;
    int64_t committed = vg_datagrid_commit_edit(g, ctext) ? 1 : 0;
    free(ctext);
    return committed;
}

/// @brief Cancel active Grid edit mode without modifying cell content.
/// @param grid Grid widget handle; invalid/already-idle handles are no-ops.
void rt_datagrid_cancel_edit(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    if (g)
        vg_datagrid_cancel_edit(g);
}

/// @brief Query whether one Grid edit controller is active.
/// @param grid Grid widget handle.
/// @return 1 while editing a valid cell, otherwise 0.
int64_t rt_datagrid_is_editing(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_datagrid_is_editing(g) ? 1 : 0;
}

/// @brief Consume the independent committed-cell-edit edge.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported effective edits, otherwise 0.
int64_t rt_datagrid_was_cell_edited(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g && vg_datagrid_was_cell_edited(g) ? 1 : 0;
}

/// @brief Scroll so one logical Grid row becomes the first viewport row.
/// @param grid Grid widget handle.
/// @param row Requested non-negative row, clamped by the lower Grid to its last row.
void rt_datagrid_scroll_to_row(void *grid, int64_t row) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    size_t native_row = 0;
    if (g && rt_datagrid_i64_to_size(row, &native_row))
        vg_datagrid_scroll_to_row(g, native_row);
}

/// @brief Return the first logical Grid viewport row.
/// @param grid Grid widget handle.
/// @return Zero-based row saturated at `INT64_MAX`, or zero when invalid.
int64_t rt_datagrid_get_scroll_row(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? rt_datagrid_size_to_i64(vg_datagrid_get_scroll_row(g)) : 0;
}

/// @brief Consume the Grid's independent model-content change edge.
/// @details Column, header, cell, and clear mutations share this edge; no-op
///          assignments do not create a transition.
/// @param grid Grid widget handle.
/// @return 1 once after one or more unreported content transitions, otherwise 0.
int64_t rt_datagrid_was_changed(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? (vg_widget_was_changed(&g->base) ? 1 : 0) : 0;
}

/// @brief Return the Grid's non-consuming state revision.
/// @param grid Grid widget handle.
/// @return Monotonic signed revision, or zero when the handle is invalid.
int64_t rt_datagrid_get_revision(void *grid) {
    RT_ASSERT_MAIN_THREAD();
    vg_datagrid_t *g = rt_datagrid_checked(grid);
    return g ? rt_widget_get_revision(&g->base) : 0;
}

//=============================================================================
// PopupList (caret-anchored filtered selection list) — Zanna.GUI.PopupList
//=============================================================================

static vg_popuplist_t *rt_popuplist_checked(void *handle) {
    return (vg_popuplist_t *)rt_gui_widget_handle_checked_type(handle, VG_WIDGET_POPUPLIST);
}

/// @brief Create a popup list attached to an optional parent (rendered in the overlay pass).
void *rt_popuplist_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = rt_widget_parent_or_null_if_invalid(parent);
    if (parent && !parent_widget)
        return NULL;
    return vg_popuplist_create(parent_widget);
}

/// @brief Append an item.
void rt_popuplist_add_item(void *list, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (!p)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_popuplist_add_item(p, ctext);
    free(ctext);
}

/// @brief Remove all items and reset filter/selection.
void rt_popuplist_clear(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_clear(p);
}

/// @brief Set the (case-insensitive substring) filter.
void rt_popuplist_set_filter(void *list, rt_string filter) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (!p)
        return;
    char *cfilter = rt_string_to_gui_cstr(filter);
    vg_popuplist_set_filter(p, cfilter);
    free(cfilter);
}

/// @brief Number of items currently visible (matching the filter).
int64_t rt_popuplist_visible_count(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    return p ? (int64_t)vg_popuplist_visible_count(p) : 0;
}

/// @brief Move the selection up one visible item.
void rt_popuplist_navigate_up(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_navigate_up(p);
}

/// @brief Move the selection down one visible item.
void rt_popuplist_navigate_down(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_navigate_down(p);
}

/// @brief Set the selection index within the visible items.
void rt_popuplist_set_selected_index(void *list, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_set_selected_index(p, rt_gui_clamp_i64_to_i32(index, 0, INT32_MAX));
}

/// @brief Selection index within the visible items, or -1 when none are visible.
int64_t rt_popuplist_get_selected_index(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    return p ? (int64_t)vg_popuplist_selected_index(p) : -1;
}

/// @brief Text of the selected visible item (empty when none).
rt_string rt_popuplist_get_selected(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (!p)
        return rt_str_empty();
    const char *text = vg_popuplist_selected_text(p);
    return text ? rt_string_from_bytes(text, (int64_t)strlen(text)) : rt_str_empty();
}

/// @brief Mark the current selection accepted.
void rt_popuplist_accept_selected(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_accept_selected(p);
}

/// @brief Whether AcceptSelected was called since the last query (consume-on-read).
int8_t rt_popuplist_was_accepted(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    return (p && vg_popuplist_was_accepted(p)) ? 1 : 0;
}

/// @brief Set the popup's anchor (top-left) position.
void rt_popuplist_anchor_at(void *list, double x, double y) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_anchor_at(p,
                               (float)rt_gui_sanitize_signed_float(x, RT_GUI_MAX_LAYOUT_VALUE),
                               (float)rt_gui_sanitize_signed_float(y, RT_GUI_MAX_LAYOUT_VALUE));
}

/// @brief Set the popup width.
void rt_popuplist_set_width(void *list, double width) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_set_width(
            p, (float)rt_gui_sanitize_nonnegative_float(width, RT_GUI_MAX_LAYOUT_VALUE));
}

/// @brief Set the maximum number of visible rows.
void rt_popuplist_set_max_rows(void *list, int64_t max_rows) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_set_max_rows(p, rt_gui_clamp_i64_to_i32(max_rows, 1, 4096));
}

/// @brief Set the item font.
void rt_popuplist_set_font(void *list, void *font, double size) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (!p)
        return;
    vg_font_t *checked_font = rt_gui_font_handle_checked(font);
    if (!checked_font)
        return;
    vg_popuplist_set_font(p, checked_font, (float)rt_gui_sanitize_font_size(size, 14.0));
    p->base.runtime_font_reference = checked_font;
}

/// @brief Show or hide the popup.
void rt_popuplist_set_visible(void *list, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    if (p)
        vg_popuplist_set_visible(p, visible != 0);
}

/// @brief Whether the popup is currently visible.
int8_t rt_popuplist_is_visible(void *list) {
    RT_ASSERT_MAIN_THREAD();
    vg_popuplist_t *p = rt_popuplist_checked(list);
    return (p && vg_popuplist_is_visible(p)) ? 1 : 0;
}

//=============================================================================
#else /* !ZANNA_ENABLE_GRAPHICS */

// ===========================================================================
// Headless stubs — same prototypes as the real implementations above so
// non-graphical builds (server / CLI) can link without pulling
// in the GUI subsystem. Each stub safely no-ops or returns a sentinel
// (NULL pointer, 0, -1, or empty string). Doc comments are inherited
// from the real implementations above by virtue of identical names.
// ===========================================================================

/// @brief Stub: graphics disabled — returns NULL; no tab bar widget is created.
void *rt_tabbar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no tab is added.
void *rt_tabbar_add_tab(void *tabbar, rt_string title, int64_t closable) {
    (void)tabbar;
    (void)title;
    (void)closable;
    return NULL;
}

/// @brief Remove a tab from the tab bar and free its resources.
void rt_tabbar_remove_tab(void *tabbar, void *tab) {
    (void)tabbar;
    (void)tab;
}

/// @brief Stub: retired tab pruning is a no-op without graphics.
void rt_tabbar_prune_retired_tabs(void *tabbar) {
    (void)tabbar;
}

/// @brief Set the currently active (selected) tab in the tab bar.
void rt_tabbar_set_active(void *tabbar, void *tab) {
    (void)tabbar;
    (void)tab;
}

/// @brief Update the title text of a tab.
void rt_tab_set_title(void *tab, rt_string title) {
    (void)tab;
    (void)title;
}

/// @brief Stub: return an empty tab title without graphics.
rt_string rt_tab_get_title(void *tab) {
    (void)tab;
    return rt_str_empty();
}

/// @brief Stub: ignore tab data without graphics.
void rt_tab_set_data(void *tab, rt_string data) {
    (void)tab;
    (void)data;
}

/// @brief Stub: return empty tab data without graphics.
rt_string rt_tab_get_data(void *tab) {
    (void)tab;
    return rt_str_empty();
}

/// @brief Stub: ignore tab closability without graphics.
void rt_tab_set_closable(void *tab, int64_t closable) {
    (void)tab;
    (void)closable;
}

/// @brief Stub: report a non-closable tab without graphics.
int64_t rt_tab_is_closable(void *tab) {
    (void)tab;
    return 0;
}

/// @brief Stub: ignore stable tab identifiers without graphics.
void rt_tab_set_stable_id(void *tab, rt_string stable_id) {
    (void)tab;
    (void)stable_id;
}

/// @brief Stub: return an empty stable tab identifier without graphics.
rt_string rt_tab_get_stable_id(void *tab) {
    (void)tab;
    return rt_str_empty();
}

/// @brief Update the tooltip text of a tab.
void rt_tab_set_tooltip(void *tab, rt_string tooltip) {
    (void)tab;
    (void)tooltip;
}

/// @brief Mark a tab as modified (shows an unsaved-changes indicator).
void rt_tab_set_modified(void *tab, int64_t modified) {
    (void)tab;
    (void)modified;
}

/// @brief Stub: graphics disabled — returns NULL; no active tab exists.
void *rt_tabbar_get_active(void *tabbar) {
    (void)tabbar;
    return NULL;
}

/// @brief Get the active index of the tabbar.
int64_t rt_tabbar_get_active_index(void *tabbar) {
    (void)tabbar;
    return -1;
}

/// @brief Check if the active tab changed since the last call (edge-triggered).
int64_t rt_tabbar_was_changed(void *tabbar) {
    (void)tabbar;
    return 0;
}

/// @brief Stub: no TabBar revision exists when graphics is disabled.
/// @param tabbar Ignored TabBar handle.
/// @return Always zero.
int64_t rt_tabbar_get_revision(void *tabbar) {
    (void)tabbar;
    return 0;
}

/// @brief Stub: ignore TabBar font changes without graphics.
void rt_tabbar_set_font(void *tabbar, void *font, double size) {
    (void)tabbar;
    (void)font;
    (void)size;
}

/// @brief Stub: report no TabBar reorder edge without graphics.
int64_t rt_tabbar_was_reordered(void *tabbar) {
    (void)tabbar;
    return 0;
}

/// @brief Stub: return no TabBar reorder source without graphics.
int64_t rt_tabbar_get_reordered_from(void *tabbar) {
    (void)tabbar;
    return -1;
}

/// @brief Stub: return no TabBar reorder destination without graphics.
int64_t rt_tabbar_get_reordered_to(void *tabbar) {
    (void)tabbar;
    return -1;
}

/// @brief Stub: reject TabBar moves without graphics.
int64_t rt_tabbar_move_tab(void *tabbar, int64_t from_index, int64_t to_index) {
    (void)tabbar;
    (void)from_index;
    (void)to_index;
    return 0;
}

/// @brief Get the number of tabs in the tab bar.
int64_t rt_tabbar_get_tab_count(void *tabbar) {
    (void)tabbar;
    return 0;
}

/// @brief Check if any tab's close button was clicked this frame.
int64_t rt_tabbar_was_close_clicked(void *tabbar) {
    (void)tabbar;
    return 0;
}

/// @brief Get the index of the tab whose close button was clicked (clears after read).
int64_t rt_tabbar_get_close_clicked_index(void *tabbar) {
    (void)tabbar;
    return -1;
}

/// @brief Stub: graphics disabled — returns NULL; no tab exists at any index.
void *rt_tabbar_get_tab_at(void *tabbar, int64_t index) {
    (void)tabbar;
    (void)index;
    return NULL;
}

/// @brief Stub: `TabBar.GetTabIndexAt` returns -1 without graphics.
int64_t rt_tabbar_get_tab_index_at(void *tabbar, int64_t x, int64_t y) {
    (void)tabbar;
    (void)x;
    (void)y;
    return -1;
}

/// @brief Enable or disable automatic tab removal on close-button click.
void rt_tabbar_set_auto_close(void *tabbar, int64_t auto_close) {
    (void)tabbar;
    (void)auto_close;
}

/// @brief Stub: graphics disabled — returns NULL; no split pane widget is created.
void *rt_splitpane_new(void *parent, int64_t horizontal) {
    (void)parent;
    (void)horizontal;
    return NULL;
}

/// @brief Set the position of the splitpane.
void rt_splitpane_set_position(void *split, double position) {
    (void)split;
    (void)position;
}

/// @brief Get the position of the splitpane.
double rt_splitpane_get_position(void *split) {
    (void)split;
    return 0.5;
}

/// @brief Graphics-disabled first-pane minimum setter stub.
/// @param split Ignored handle.
/// @param size Ignored logical size.
void rt_splitpane_set_min_first(void *split, double size) {
    (void)split;
    (void)size;
}

/// @brief Graphics-disabled second-pane minimum setter stub.
/// @param split Ignored handle.
/// @param size Ignored logical size.
void rt_splitpane_set_min_second(void *split, double size) {
    (void)split;
    (void)size;
}

/// @brief Graphics-disabled first-pane minimum query stub.
/// @param split Ignored handle.
/// @return Always zero.
double rt_splitpane_get_min_first(void *split) {
    (void)split;
    return 0.0;
}

/// @brief Graphics-disabled second-pane minimum query stub.
/// @param split Ignored handle.
/// @return Always zero.
double rt_splitpane_get_min_second(void *split) {
    (void)split;
    return 0.0;
}

/// @brief Graphics-disabled orientation query stub.
/// @param split Ignored handle.
/// @return Always -1 because no split pane exists.
int64_t rt_splitpane_get_orientation(void *split) {
    (void)split;
    return -1;
}

/// @brief Graphics-disabled first-pane collapse stub.
/// @param split Ignored handle.
void rt_splitpane_collapse_first(void *split) {
    (void)split;
}

/// @brief Graphics-disabled second-pane collapse stub.
/// @param split Ignored handle.
void rt_splitpane_collapse_second(void *split) {
    (void)split;
}

/// @brief Graphics-disabled split-pane restore stub.
/// @param split Ignored handle.
void rt_splitpane_restore(void *split) {
    (void)split;
}

/// @brief Graphics-disabled collapsed-side query stub.
/// @param split Ignored handle.
/// @return Always -1 because no split pane exists.
int64_t rt_splitpane_get_collapsed_side(void *split) {
    (void)split;
    return -1;
}

/// @brief Stub: graphics disabled — returns NULL; no first panel container exists.
void *rt_splitpane_get_first(void *split) {
    (void)split;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no second panel container exists.
void *rt_splitpane_get_second(void *split) {
    (void)split;
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no code editor widget is created.
void *rt_codeeditor_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Replace the entire text content of a code editor.
void rt_codeeditor_set_text(void *editor, rt_string text) {
    (void)editor;
    (void)text;
}

/// @brief Retrieve the full text content of a code editor (caller frees the C string).
rt_string rt_codeeditor_get_text(void *editor) {
    (void)editor;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no content revision exists.
int64_t rt_codeeditor_get_revision(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Stub: graphics disabled — no editor journal to serialize.
rt_string rt_codeeditor_take_deltas(void *editor, int64_t since_revision) {
    (void)editor;
    (void)since_revision;
    return rt_str_empty();
}

/// @brief Retrieve the currently selected text in a code editor.
rt_string rt_codeeditor_get_selected_text(void *editor) {
    (void)editor;
    return rt_str_empty();
}

/// @brief Move the cursor to a specific line and column in the code editor.
void rt_codeeditor_set_cursor(void *editor, int64_t line, int64_t col) {
    (void)editor;
    (void)line;
    (void)col;
}

/// @brief Scroll the code editor viewport to make a specific line visible.
void rt_codeeditor_scroll_to_line(void *editor, int64_t line) {
    (void)editor;
    (void)line;
}

/// @brief Get the total number of lines in the code editor.
int64_t rt_codeeditor_get_line_count(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Check whether the code editor's content has been modified since last clear.
int64_t rt_codeeditor_is_modified(void *editor) {
    (void)editor;
    return 0;
}

/// @brief Reset the code editor's modified flag (e.g., after saving).
void rt_codeeditor_clear_modified(void *editor) {
    (void)editor;
}

/// @brief Set the font of the codeeditor.
void rt_codeeditor_set_font(void *editor, void *font, double size) {
    (void)editor;
    (void)font;
    (void)size;
}

/// @brief Get or set the font size of the code editor (in logical points).
double rt_codeeditor_get_font_size(void *editor) {
    (void)editor;
    return 14.0;
}

/// @brief Get or set the font size of the code editor (in logical points).
void rt_codeeditor_set_font_size(void *editor, double size) {
    (void)editor;
    (void)size;
}

/// @brief Switch the active theme to dark mode.
void rt_theme_set_dark(void) {}

/// @brief Switch the active theme to light mode.
void rt_theme_set_light(void) {}

/// @brief Get the name of the theme.
rt_string rt_theme_get_name(void) {
    return rt_string_from_bytes("dark", 4);
}

/// @brief Stub: graphics disabled — returns NULL; no VBox container is created.
void *rt_vbox_new(void) {
    return NULL;
}

/// @brief Stub: graphics disabled — returns NULL; no HBox container is created.
void *rt_hbox_new(void) {
    return NULL;
}

/// @brief Set the spacing of the container.
void rt_container_set_spacing(void *container, double spacing) {
    (void)container;
    (void)spacing;
}

/// @brief Set the padding of the container.
void rt_container_set_padding(void *container, double padding) {
    (void)container;
    (void)padding;
}

/// @brief Check whether the mouse cursor is currently over this widget.
int64_t rt_widget_is_hovered(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Check whether the widget is currently being pressed (mouse down).
int64_t rt_widget_is_pressed(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Check whether the widget currently has keyboard focus.
int64_t rt_widget_is_focused(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Move keyboard focus to a widget.
void rt_widget_focus(void *widget) {
    (void)widget;
}

/// @brief Set the last clicked value.
/// @param widget
void rt_gui_set_last_clicked(void *widget) {
    (void)widget;
}

/// @brief Check whether this widget was clicked during the current frame.
int64_t rt_widget_was_clicked(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Set the position of the widget.
void rt_widget_set_position(void *widget, int64_t x, int64_t y) {
    (void)widget;
    (void)x;
    (void)y;
}

/// @brief Stub: graphics disabled — returns NULL; no output pane is created.
void *rt_outputpane_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_append(void *pane, rt_string text) {
    (void)pane;
    (void)text;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_append_line(void *pane, rt_string text) {
    (void)pane;
    (void)text;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_append_styled(void *pane, rt_string text, int64_t fg, int64_t bg, int64_t bold) {
    (void)pane;
    (void)text;
    (void)fg;
    (void)bg;
    (void)bold;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_clear(void *pane) {
    (void)pane;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_scroll_to_top(void *pane) {
    (void)pane;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_scroll_to_bottom(void *pane) {
    (void)pane;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_set_auto_scroll(void *pane, int64_t enabled) {
    (void)pane;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no selected output text exists.
rt_string rt_outputpane_get_selection(void *pane) {
    (void)pane;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_select_all(void *pane) {
    (void)pane;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_set_max_lines(void *pane, int64_t max_lines) {
    (void)pane;
    (void)max_lines;
}

/// @brief Stub: graphics disabled — no output pane exists.
int64_t rt_outputpane_get_line_count(void *pane) {
    (void)pane;
    return 0;
}

/// @brief Stub: graphics disabled — no output pane exists.
void rt_outputpane_set_font(void *pane, void *font, double size) {
    (void)pane;
    (void)font;
    (void)size;
}

/// @brief Stub: graphics disabled — no font metrics.
int64_t rt_outputpane_get_cell_width(void *pane) {
    (void)pane;
    return 0;
}

/// @brief Stub: graphics disabled — no font metrics.
int64_t rt_outputpane_get_cell_height(void *pane) {
    (void)pane;
    return 0;
}

/// @brief Stub: graphics disabled — no font metrics.
int64_t rt_outputpane_measure_text(void *pane, rt_string text) {
    (void)pane;
    (void)text;
    return 0;
}

/// @brief Stub: graphics disabled — no font metrics.
int64_t rt_outputpane_columns_for_width(void *pane) {
    (void)pane;
    return 0;
}

/// @brief Stub: graphics disabled — no font metrics.
int64_t rt_outputpane_rows_for_height(void *pane) {
    (void)pane;
    return 0;
}

/// @brief Stub: graphics disabled — no terminal mode.
void rt_outputpane_set_terminal_mode(void *pane, int64_t enabled) {
    (void)pane;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no terminal input is queued.
rt_string rt_outputpane_take_input(void *pane) {
    (void)pane;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — returns NULL; no radio group is created.
void *rt_radiogroup_new(void) {
    return NULL;
}

/// @brief Release resources and destroy the radiogroup.
void rt_radiogroup_destroy(void *group) {
    (void)group;
}

/// @brief Graphics-disabled selected-index query stub.
/// @param group Ignored handle.
/// @return Always -1.
int64_t rt_radiogroup_get_selected_index(void *group) {
    (void)group;
    return -1;
}

/// @brief Graphics-disabled selected-index setter stub.
/// @param group Ignored handle.
/// @param index Ignored index.
/// @return Always zero because no group exists.
int64_t rt_radiogroup_set_selected_index(void *group, int64_t index) {
    (void)group;
    (void)index;
    return 0;
}

/// @brief Graphics-disabled radio-group count query stub.
/// @param group Ignored handle.
/// @return Always zero.
int64_t rt_radiogroup_get_count(void *group) {
    (void)group;
    return 0;
}

/// @brief Graphics-disabled radio-group change edge stub.
/// @param group Ignored handle.
/// @return Always zero.
int64_t rt_radiogroup_was_changed(void *group) {
    (void)group;
    return 0;
}

/// @brief Graphics-disabled radio-group revision stub.
/// @param group Ignored handle.
/// @return Always zero.
int64_t rt_radiogroup_get_revision(void *group) {
    (void)group;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no radio button widget is created.
void *rt_radiobutton_new(void *parent, rt_string text, void *group) {
    (void)parent;
    (void)text;
    (void)group;
    return NULL;
}

/// @brief Check whether a radio button is currently selected in its group.
int64_t rt_radiobutton_is_selected(void *radio) {
    (void)radio;
    return 0;
}

/// @brief Programmatically select a radio button (deselects siblings in the group).
void rt_radiobutton_set_selected(void *radio, int64_t selected) {
    (void)radio;
    (void)selected;
}

/// @brief Graphics-disabled radio-button text setter stub.
/// @param radio Ignored handle.
/// @param text Ignored runtime string.
void rt_radiobutton_set_text(void *radio, rt_string text) {
    (void)radio;
    (void)text;
}

/// @brief Graphics-disabled radio-button text query stub.
/// @param radio Ignored handle.
/// @return Empty runtime string.
rt_string rt_radiobutton_get_text(void *radio) {
    (void)radio;
    return rt_str_empty();
}

/// @brief Graphics-disabled radio-button data setter stub.
/// @param radio Ignored handle.
/// @param data Ignored runtime string.
void rt_radiobutton_set_data(void *radio, rt_string data) {
    (void)radio;
    (void)data;
}

/// @brief Graphics-disabled radio-button data query stub.
/// @param radio Ignored handle.
/// @return Empty runtime string.
rt_string rt_radiobutton_get_data(void *radio) {
    (void)radio;
    return rt_str_empty();
}

/// @brief Stub: no RadioButton change edge exists when graphics is disabled.
/// @param radio Ignored RadioButton handle.
/// @return Always zero.
int64_t rt_radiobutton_was_changed(void *radio) {
    (void)radio;
    return 0;
}

/// @brief Stub: no RadioButton revision exists when graphics is disabled.
/// @param radio Ignored RadioButton handle.
/// @return Always zero.
int64_t rt_radiobutton_get_revision(void *radio) {
    (void)radio;
    return 0;
}

/// @brief Stub: graphics disabled — returns NULL; no spinner widget is created.
void *rt_spinner_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Set the value of the spinner.
void rt_spinner_set_value(void *spinner, double value) {
    (void)spinner;
    (void)value;
}

/// @brief Get the value of the spinner.
double rt_spinner_get_value(void *spinner) {
    (void)spinner;
    return 0.0;
}

/// @brief Stub: graphics disabled — no spinner can enter mixed state.
void rt_spinner_set_indeterminate(void *spinner, int64_t indeterminate) {
    (void)spinner;
    (void)indeterminate;
}

/// @brief Stub: graphics disabled — no spinner is mixed.
int64_t rt_spinner_is_indeterminate(void *spinner) {
    (void)spinner;
    return 0;
}

/// @brief Set the range of the spinner.
void rt_spinner_set_range(void *spinner, double min_val, double max_val) {
    (void)spinner;
    (void)min_val;
    (void)max_val;
}

/// @brief Set the step of the spinner.
void rt_spinner_set_step(void *spinner, double step) {
    (void)spinner;
    (void)step;
}

/// @brief Set the decimals of the spinner.
void rt_spinner_set_decimals(void *spinner, int64_t decimals) {
    (void)spinner;
    (void)decimals;
}

/// @brief Stub: no spinner change edge exists when graphics is disabled.
/// @param spinner Ignored spinner handle.
/// @return Always zero.
int64_t rt_spinner_was_changed(void *spinner) {
    (void)spinner;
    return 0;
}

/// @brief Stub: no spinner submission edge exists when graphics is disabled.
/// @param spinner Ignored spinner handle.
/// @return Always zero.
int64_t rt_spinner_was_submitted(void *spinner) {
    (void)spinner;
    return 0;
}

/// @brief Stub: no spinner scrub edge exists when graphics is disabled.
/// @param spinner Ignored spinner handle.
/// @return Always zero.
int64_t rt_spinner_was_scrub_finished(void *spinner) {
    (void)spinner;
    return 0;
}

/// @brief Stub: no spinner revision exists when graphics is disabled.
/// @param spinner Ignored spinner handle.
/// @return Always zero.
int64_t rt_spinner_get_revision(void *spinner) {
    (void)spinner;
    return 0;
}

// --- Grid stubs: graphics disabled — no data grid exists. ---
void *rt_datagrid_new(void *parent) {
    (void)parent;
    return NULL;
}

void rt_datagrid_set_columns(void *grid, int64_t count) {
    (void)grid;
    (void)count;
}

void rt_datagrid_set_header(void *grid, int64_t col, rt_string text) {
    (void)grid;
    (void)col;
    (void)text;
}

void rt_datagrid_set_cell(void *grid, int64_t row, int64_t col, rt_string text) {
    (void)grid;
    (void)row;
    (void)col;
    (void)text;
}

rt_string rt_datagrid_get_cell(void *grid, int64_t row, int64_t col) {
    (void)grid;
    (void)row;
    (void)col;
    return rt_str_empty();
}

void rt_datagrid_clear(void *grid) {
    (void)grid;
}

void rt_datagrid_set_font(void *grid, void *font, double size) {
    (void)grid;
    (void)font;
    (void)size;
}

int64_t rt_datagrid_get_column_width(void *grid, int64_t col) {
    (void)grid;
    (void)col;
    return 0;
}

int64_t rt_datagrid_get_row_count(void *grid) {
    (void)grid;
    return 0;
}

int64_t rt_datagrid_get_column_count(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: ignore Grid viewport changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param first Ignored first row.
/// @param count Ignored visible-row count.
void rt_datagrid_set_viewport_rows(void *grid, int64_t first, int64_t count) {
    (void)grid;
    (void)first;
    (void)count;
}

/// @brief Stub: ignore sparse virtual row-count changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param count Ignored logical row count.
void rt_datagrid_set_virtual_row_count(void *grid, int64_t count) {
    (void)grid;
    (void)count;
}

/// @brief Stub: ignore sparse virtual-cell changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param row Ignored logical row.
/// @param col Ignored column.
/// @param text Ignored runtime text.
void rt_datagrid_set_virtual_cell(void *grid, int64_t row, int64_t col, rt_string text) {
    (void)grid;
    (void)row;
    (void)col;
    (void)text;
}

/// @brief Stub: ignore Grid selection enablement when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param enabled Ignored boolean.
void rt_datagrid_set_selectable(void *grid, int64_t enabled) {
    (void)grid;
    (void)enabled;
}

/// @brief Stub: no selected Grid row exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always -1.
int64_t rt_datagrid_get_selected_row(void *grid) {
    (void)grid;
    return -1;
}

/// @brief Stub: no selected Grid column exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always -1.
int64_t rt_datagrid_get_selected_column(void *grid) {
    (void)grid;
    return -1;
}

/// @brief Stub: no Grid cell can be selected when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param row Ignored row.
/// @param col Ignored column.
/// @return Always zero.
int64_t rt_datagrid_select_cell(void *grid, int64_t row, int64_t col) {
    (void)grid;
    (void)row;
    (void)col;
    return 0;
}

/// @brief Stub: ignore Grid selection clearing when graphics is disabled.
/// @param grid Ignored Grid handle.
void rt_datagrid_clear_selection(void *grid) {
    (void)grid;
}

/// @brief Stub: no Grid selection edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_selection_changed(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no Grid activation edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_activated(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: ignore sortable-column changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param col Ignored column.
/// @param enabled Ignored boolean.
void rt_datagrid_set_sortable(void *grid, int64_t col, int64_t enabled) {
    (void)grid;
    (void)col;
    (void)enabled;
}

/// @brief Stub: ignore Grid sort requests when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param col Ignored column.
/// @param direction Ignored direction.
void rt_datagrid_set_sort(void *grid, int64_t col, int64_t direction) {
    (void)grid;
    (void)col;
    (void)direction;
}

/// @brief Stub: no Grid sort column exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always -1.
int64_t rt_datagrid_get_sort_column(void *grid) {
    (void)grid;
    return -1;
}

/// @brief Stub: no Grid sort direction exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_get_sort_direction(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no Grid sort edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_sort_changed(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: ignore Grid column-width changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param col Ignored column.
/// @param width Ignored logical width.
void rt_datagrid_set_column_width(void *grid, int64_t col, double width) {
    (void)grid;
    (void)col;
    (void)width;
}

/// @brief Stub: ignore resizable-column changes when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param col Ignored column.
/// @param enabled Ignored boolean.
void rt_datagrid_set_column_resizable(void *grid, int64_t col, int64_t enabled) {
    (void)grid;
    (void)col;
    (void)enabled;
}

/// @brief Stub: no Grid column-resize edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_column_resized(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no resized Grid column exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always -1.
int64_t rt_datagrid_get_resized_column(void *grid) {
    (void)grid;
    return -1;
}

/// @brief Stub: ignore Grid edit enablement when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param enabled Ignored boolean.
void rt_datagrid_set_editable(void *grid, int64_t enabled) {
    (void)grid;
    (void)enabled;
}

/// @brief Stub: no Grid cell edit can begin when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param row Ignored row.
/// @param col Ignored column.
/// @return Always zero.
int64_t rt_datagrid_begin_edit(void *grid, int64_t row, int64_t col) {
    (void)grid;
    (void)row;
    (void)col;
    return 0;
}

/// @brief Stub: no Grid cell edit can commit when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param text Ignored replacement text.
/// @return Always zero.
int64_t rt_datagrid_commit_edit(void *grid, rt_string text) {
    (void)grid;
    (void)text;
    return 0;
}

/// @brief Stub: ignore Grid edit cancellation when graphics is disabled.
/// @param grid Ignored Grid handle.
void rt_datagrid_cancel_edit(void *grid) {
    (void)grid;
}

/// @brief Stub: no Grid edit is active when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_is_editing(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no Grid cell-edit edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_cell_edited(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: ignore Grid scroll requests when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @param row Ignored logical row.
void rt_datagrid_scroll_to_row(void *grid, int64_t row) {
    (void)grid;
    (void)row;
}

/// @brief Stub: no Grid scroll offset exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_get_scroll_row(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no Grid change edge exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_was_changed(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Stub: no Grid revision exists when graphics is disabled.
/// @param grid Ignored Grid handle.
/// @return Always zero.
int64_t rt_datagrid_get_revision(void *grid) {
    (void)grid;
    return 0;
}

// --- PopupList stubs: graphics disabled — no popup list exists. ---
void *rt_popuplist_new(void *parent) {
    (void)parent;
    return NULL;
}

void rt_popuplist_add_item(void *list, rt_string text) {
    (void)list;
    (void)text;
}

void rt_popuplist_clear(void *list) {
    (void)list;
}

void rt_popuplist_set_filter(void *list, rt_string filter) {
    (void)list;
    (void)filter;
}

int64_t rt_popuplist_visible_count(void *list) {
    (void)list;
    return 0;
}

void rt_popuplist_navigate_up(void *list) {
    (void)list;
}

void rt_popuplist_navigate_down(void *list) {
    (void)list;
}

void rt_popuplist_set_selected_index(void *list, int64_t index) {
    (void)list;
    (void)index;
}

int64_t rt_popuplist_get_selected_index(void *list) {
    (void)list;
    return -1;
}

rt_string rt_popuplist_get_selected(void *list) {
    (void)list;
    return rt_str_empty();
}

void rt_popuplist_accept_selected(void *list) {
    (void)list;
}

int8_t rt_popuplist_was_accepted(void *list) {
    (void)list;
    return 0;
}

void rt_popuplist_anchor_at(void *list, double x, double y) {
    (void)list;
    (void)x;
    (void)y;
}

void rt_popuplist_set_width(void *list, double width) {
    (void)list;
    (void)width;
}

void rt_popuplist_set_max_rows(void *list, int64_t max_rows) {
    (void)list;
    (void)max_rows;
}

void rt_popuplist_set_font(void *list, void *font, double size) {
    (void)list;
    (void)font;
    (void)size;
}

void rt_popuplist_set_visible(void *list, int64_t visible) {
    (void)list;
    (void)visible;
}

int8_t rt_popuplist_is_visible(void *list) {
    (void)list;
    return 0;
}


#endif /* ZANNA_ENABLE_GRAPHICS */
