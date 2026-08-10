//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_findbar.c
// Purpose: Find/replace bar widget runtime bindings for ZannaGUI. Provides
//   creation, editor binding, text/option configuration, and search/replace
//   operations for the vg_findreplacebar_t widget.
//
// Key invariants:
//   - The FindBar must be bound to a CodeEditor before search operations.
//   - rt_findbar_data_t (GC-managed) wraps the vg_findreplacebar_t pointer
//     and caches find/replace text for setter fallback.
//   - Option getters read the live vg widget state so user checkbox and
//     keyboard changes are visible to runtime callers.
//
// Ownership/Lifetime:
//   - rt_findbar_data_t is a GC heap object; the vg_findreplacebar_t is
//     manually freed on rt_findbar_destroy().
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/lib/gui/src/widgets/vg_findreplacebar.c (underlying widget)
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_findbar.c
/// @brief Implements managed FindBar bindings for CodeEditor search and replacement.
///
/// @details
/// The module validates GC-backed wrappers, tracks live editor bindings, mirrors
/// widget option state, and exposes both sentinel and Option search results.
/// Headless definitions retain the API with neutral behavior.

#include "rt_gui_internal.h"
#include "rt_option.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

//=============================================================================
// Phase 6: FindBar (Search & Replace)
//=============================================================================

// FindBar state tracking
#define RT_FINDBAR_DATA_MAGIC UINT64_C(0x525446494E444241)

typedef struct {
    uint64_t magic;
    vg_findreplacebar_t *bar;
    void *bound_editor;
    char *find_text;
    char *replace_text;
    int64_t case_sensitive;
    int64_t whole_word;
    int64_t regex;
    int64_t replace_mode;
} rt_findbar_data_t;

static const vg_widget_vtable_t *s_findbar_original_vtable = NULL;
static vg_widget_vtable_t s_findbar_runtime_vtable;
static rt_findbar_data_t **s_findbar_wrappers = NULL;
static size_t s_findbar_wrapper_count = 0;
static size_t s_findbar_wrapper_cap = 0;

static vg_codeeditor_t *rt_findbar_editor_checked(void *editor);
static void rt_findbar_sync_options_from_widget(rt_findbar_data_t *data);

/// @brief Record a wrapper in the global find-bar registry (idempotent).
/// @details The registry is the source of truth for handle validation: a checked
///          cast only trusts an opaque `void*` once it is found here, guarding
///          against forged or freed handles. Capacity doubles from 8 on demand.
/// @param data FindBar wrapper to register.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int rt_findbar_register_wrapper(rt_findbar_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_findbar_wrapper_count; i++) {
        if (s_findbar_wrappers[i] == data)
            return 1;
    }
    if (s_findbar_wrapper_count >= s_findbar_wrapper_cap) {
        size_t new_cap = 0;
        if (!rt_gui_next_collection_capacity(s_findbar_wrapper_cap,
                                             s_findbar_wrapper_count + 1u,
                                             8u,
                                             sizeof(rt_findbar_data_t *),
                                             &new_cap))
            return 0;
        void *p = realloc(s_findbar_wrappers, new_cap * sizeof(rt_findbar_data_t *));
        if (!p)
            return 0;
        s_findbar_wrappers = (rt_findbar_data_t **)p;
        s_findbar_wrapper_cap = new_cap;
    }
    s_findbar_wrappers[s_findbar_wrapper_count++] = data;
    return 1;
}

/// @brief Remove a wrapper from the find-bar registry, compacting the array. No-op if absent.
/// @param data Wrapper to remove.
static void rt_findbar_unregister_wrapper(rt_findbar_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < s_findbar_wrapper_count; i++) {
        if (s_findbar_wrappers[i] != data)
            continue;
        memmove(&s_findbar_wrappers[i],
                &s_findbar_wrappers[i + 1],
                (s_findbar_wrapper_count - i - 1) * sizeof(*s_findbar_wrappers));
        s_findbar_wrapper_count--;
        return;
    }
}

/// @brief True if @p data is a currently-registered wrapper; backs handle validation.
/// @param data Candidate wrapper pointer.
/// @return `1` when the exact pointer is registered; otherwise `0`.
static int rt_findbar_wrapper_is_registered(const rt_findbar_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_findbar_wrapper_count; i++) {
        if (s_findbar_wrappers[i] == data)
            return 1;
    }
    return 0;
}

/// @brief Clear the wrapper's bound editor and drop the bar's search target (if live).
/// @param data FindBar wrapper to unbind.
static void rt_findbar_unbind_data(rt_findbar_data_t *data) {
    if (!data)
        return;
    data->bound_editor = NULL;
    if (data->bar && vg_widget_is_live(&data->bar->base))
        vg_findreplacebar_set_target(data->bar, NULL);
}

/// @brief True if the bar's bound editor is still a live code editor; lazily unbinds
///        and returns 0 when the editor has been destroyed out from under the bar.
/// @param data FindBar wrapper whose target is validated.
/// @return `1` when a live CodeEditor remains bound; otherwise `0`.
static int rt_findbar_has_live_editor(rt_findbar_data_t *data) {
    if (!data || !data->bound_editor)
        return 0;
    if (!rt_findbar_editor_checked(data->bound_editor)) {
        rt_findbar_unbind_data(data);
        return 0;
    }
    return 1;
}

/// @brief Unbind every FindBar targeting a CodeEditor inside a destroyed widget subtree.
/// @param subtree Root of the widget subtree being retired.
void rt_findbar_forget_editor_subtree(vg_widget_t *subtree) {
    if (!subtree)
        return;
    for (size_t i = 0; i < s_findbar_wrapper_count; i++) {
        rt_findbar_data_t *data = s_findbar_wrappers[i];
        vg_widget_t *target = data ? (vg_widget_t *)data->bound_editor : NULL;
        if (target && rt_gui_widget_tree_contains(subtree, target))
            rt_findbar_unbind_data(data);
    }
}

/// @brief Safe-cast an opaque handle to the find-bar wrapper by magic tag.
/// @param bar Candidate runtime FindBar handle.
/// @return Validated registered wrapper, or `NULL` for invalid input.
static rt_findbar_data_t *rt_findbar_wrapper_checked(void *bar) {
    rt_findbar_data_t *data = (rt_findbar_data_t *)bar;
    return rt_findbar_wrapper_is_registered(data) && data->magic == RT_FINDBAR_DATA_MAGIC ? data
                                                                                          : NULL;
}

/// @brief Safe-cast an opaque handle to the find-bar wrapper, validating its
///        backing widget is still live. Returns NULL otherwise.
/// @param bar Candidate runtime FindBar handle.
/// @return Validated wrapper with a live lower widget, or `NULL`.
static rt_findbar_data_t *rt_findbar_checked(void *bar) {
    rt_findbar_data_t *data = rt_findbar_wrapper_checked(bar);
    return data && data->bar && vg_widget_is_live(&data->bar->base) ? data : NULL;
}

/// @brief Safe-cast an opaque handle to the code editor the find-bar targets.
/// @param editor Candidate runtime widget handle.
/// @return The code editor widget, or NULL if @p editor is not one.
static vg_codeeditor_t *rt_findbar_editor_checked(void *editor) {
    return (vg_codeeditor_t *)rt_gui_widget_handle_checked_type(editor, VG_WIDGET_CODEEDITOR);
}

/// @brief Widget destroy override — clears the runtime wrapper back-pointer before chaining.
/// @param widget Lower FindBar widget being destroyed.
static void rt_findbar_widget_destroy(vg_widget_t *widget) {
    rt_findbar_data_t *data = widget ? (rt_findbar_data_t *)widget->user_data : NULL;
    if (data && data->bar == (vg_findreplacebar_t *)widget) {
        data->bar = NULL;
        data->bound_editor = NULL;
    }
    if (s_findbar_original_vtable && s_findbar_original_vtable->destroy)
        s_findbar_original_vtable->destroy(widget);
}

/// @brief Release the find/replace bar widget and free cached text buffers.
/// @param data Registered FindBar wrapper to dispose.
static void rt_findbar_dispose(rt_findbar_data_t *data) {
    if (!data)
        return;
    if (data->bar) {
        vg_findreplacebar_destroy(data->bar);
        data->bar = NULL;
    }
    if (data->find_text) {
        free(data->find_text);
        data->find_text = NULL;
    }
    if (data->replace_text) {
        free(data->replace_text);
        data->replace_text = NULL;
    }
    data->bound_editor = NULL;
    data->magic = 0;
    rt_findbar_unregister_wrapper(data);
}

/// @brief GC finalizer — delegates to `rt_findbar_dispose`.
/// @param bar Managed FindBar wrapper being finalized.
static void rt_findbar_finalize(void *bar) {
    rt_findbar_data_t *data = (rt_findbar_data_t *)bar;
    rt_findbar_dispose(data);
}

/// @brief Create a new find/replace bar widget.
/// @details Allocates a GC-managed rt_findbar_data_t wrapper around a
///          vg_findreplacebar_t. The bar must be bound to a CodeEditor via
///          rt_findbar_bind_editor before search operations will work. Options
///          (case-sensitive, whole-word, regex) are cached locally and pushed
///          to the vg layer before each search.
/// @param parent Parent container or app handle.
/// @return Opaque find bar handle, or NULL on failure.
void *rt_findbar_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    vg_widget_t *parent_widget = rt_gui_widget_parent_container_from_handle(parent);
    if (parent && !parent_widget)
        return NULL;
    vg_findreplacebar_t *bar = vg_findreplacebar_create();
    if (!bar)
        return NULL;

    rt_findbar_data_t *data =
        (rt_findbar_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_findbar_data_t));
    if (!data) {
        vg_findreplacebar_destroy(bar);
        return NULL;
    }
    data->bar = bar;
    data->magic = RT_FINDBAR_DATA_MAGIC;
    data->bound_editor = NULL;
    data->find_text = NULL;
    data->replace_text = NULL;
    data->case_sensitive = 0;
    data->whole_word = 0;
    data->regex = 0;
    data->replace_mode = 0;
    if (!s_findbar_original_vtable && bar->base.vtable) {
        s_findbar_original_vtable = bar->base.vtable;
        s_findbar_runtime_vtable = *bar->base.vtable;
        s_findbar_runtime_vtable.destroy = rt_findbar_widget_destroy;
    }
    if (s_findbar_original_vtable) {
        bar->base.vtable = &s_findbar_runtime_vtable;
        bar->base.user_data = data;
    }
    if (!rt_findbar_register_wrapper(data)) {
        rt_findbar_dispose(data);
        return NULL;
    }
    rt_obj_set_finalizer(data, rt_findbar_finalize);
    if (parent_widget) {
        vg_widget_add_child(parent_widget, &bar->base);
    }
    if (app)
        rt_gui_activate_app(app);
    rt_gui_apply_default_font(&bar->base);
    return data;
}

/// @brief Destroy the find bar, freeing the vg widget and cached text.
/// @param bar Find bar handle (safe to pass NULL).
void rt_findbar_destroy(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_wrapper_checked(bar);
    if (!data)
        return;
    rt_findbar_dispose(data);
}

/// @brief Bind the find bar to a code editor for search/replace operations.
/// @details The bar needs a target editor to know which text to search. Without
///          binding, find/replace operations are no-ops.
/// @param bar    Find bar handle.
/// @param editor Code editor widget to bind to.
void rt_findbar_bind_editor(void *bar, void *editor) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    vg_codeeditor_t *target = NULL;
    if (editor) {
        target = rt_findbar_editor_checked(editor);
        if (!target)
            return;
    }
    data->bound_editor = target;
    vg_findreplacebar_set_target(data->bar, target);
}

/// @brief Unbind the find bar from its current code editor.
/// @param bar FindBar wrapper handle.
void rt_findbar_unbind_editor(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    rt_findbar_unbind_data(data);
}

/// @brief Toggle between find-only mode and find+replace mode.
/// @param bar FindBar wrapper handle.
/// @param replace Non-zero to expose replacement controls.
void rt_findbar_set_replace_mode(void *bar, int64_t replace) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    data->replace_mode = replace != 0 ? 1 : 0;
    vg_findreplacebar_set_show_replace(data->bar, data->replace_mode != 0);
}

/// @brief Check whether the find bar is in replace mode.
/// @param bar FindBar wrapper handle.
/// @return `1` when replacement controls are enabled; otherwise `0`.
int64_t rt_findbar_is_replace_mode(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    rt_findbar_sync_options_from_widget(data);
    return data->replace_mode;
}

/// @brief Set the search text for find operations.
/// @details Updates both the cached text and the underlying vg find input widget.
/// @param bar FindBar wrapper handle.
/// @param text Runtime search text copied into widget and wrapper state.
void rt_findbar_set_find_text(void *bar, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    char *new_text = text ? rt_string_to_gui_cstr(text) : NULL;
    if (text && !new_text)
        return;
    free(data->find_text);
    data->find_text = new_text;
    vg_findreplacebar_set_find_text(data->bar, data->find_text);
}

/// @brief Get the current search text.
/// @param bar FindBar wrapper handle.
/// @return Owned live or cached search text, or an empty runtime string.
rt_string rt_findbar_get_find_text(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return rt_str_empty();
    if (data->bar && data->bar->find_input) {
        const char *text = vg_textinput_get_text((vg_textinput_t *)data->bar->find_input);
        if (text)
            return rt_string_from_bytes(text, strlen(text));
    }
    if (data->find_text)
        return rt_string_from_bytes(data->find_text, strlen(data->find_text));
    return rt_str_empty();
}

/// @brief Set the replacement text for replace operations.
/// @details Updates the cached text and pushes it to the underlying vg replace
///          input widget so replace/replace_all use the correct value.
/// @param bar FindBar wrapper handle.
/// @param text Runtime replacement text copied into widget and wrapper state.
void rt_findbar_set_replace_text(void *bar, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    char *new_text = text ? rt_string_to_gui_cstr(text) : NULL;
    if (text && !new_text)
        return;
    free(data->replace_text);
    data->replace_text = new_text;
    // Apply to the underlying replace text input so replace/replace_all
    // operations use the correct replacement text.
    if (data->bar && data->bar->replace_input)
        vg_textinput_set_text((vg_textinput_t *)data->bar->replace_input, data->replace_text);
}

/// @brief Get the current replacement text.
/// @param bar FindBar wrapper handle.
/// @return Owned live or cached replacement text, or an empty runtime string.
rt_string rt_findbar_get_replace_text(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return rt_str_empty();
    if (data->bar && data->bar->replace_input) {
        const char *text = vg_textinput_get_text((vg_textinput_t *)data->bar->replace_input);
        if (text)
            return rt_string_from_bytes(text, strlen(text));
    }
    if (data->replace_text)
        return rt_string_from_bytes(data->replace_text, strlen(data->replace_text));
    return rt_str_empty();
}

/// @brief Push cached search options to the vg find/replace bar widget.
/// @details Called internally after any option change (case-sensitive, whole-word,
///          regex) so the underlying search engine picks up the new settings.
/// @param data FindBar wrapper whose cached options are applied.
static void rt_findbar_update_options(rt_findbar_data_t *data) {
    if (!data || !data->bar)
        return;
    vg_search_options_t opts = {.case_sensitive = data->case_sensitive != 0,
                                .whole_word = data->whole_word != 0,
                                .use_regex = data->regex != 0,
                                .in_selection = false,
                                .wrap_around = true};
    vg_findreplacebar_set_options(data->bar, &opts);
}

/// @brief Copy the bar widget's current search options (case/whole-word/regex/replace
///        mode) back into the wrapper's cached flags after the user toggles them.
/// @param data FindBar wrapper whose cache is synchronized.
static void rt_findbar_sync_options_from_widget(rt_findbar_data_t *data) {
    if (!data || !data->bar)
        return;
    data->case_sensitive = data->bar->options.case_sensitive ? 1 : 0;
    data->whole_word = data->bar->options.whole_word ? 1 : 0;
    data->regex = data->bar->options.use_regex ? 1 : 0;
    data->replace_mode = data->bar->show_replace ? 1 : 0;
}

/// @brief Enable or disable case-sensitive matching.
/// @param bar FindBar wrapper handle.
/// @param sensitive Non-zero for case-sensitive matching.
void rt_findbar_set_case_sensitive(void *bar, int64_t sensitive) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    data->case_sensitive = sensitive != 0 ? 1 : 0;
    rt_findbar_update_options(data);
}

/// @brief Check whether case-sensitive matching is enabled.
/// @param bar FindBar wrapper handle.
/// @return `1` when case-sensitive matching is enabled; otherwise `0`.
int64_t rt_findbar_is_case_sensitive(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    rt_findbar_sync_options_from_widget(data);
    return data->case_sensitive;
}

/// @brief Enable or disable whole-word matching.
/// @param bar FindBar wrapper handle.
/// @param whole Non-zero to require whole-word matches.
void rt_findbar_set_whole_word(void *bar, int64_t whole) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    data->whole_word = whole != 0 ? 1 : 0;
    rt_findbar_update_options(data);
}

/// @brief Check whether whole-word matching is enabled.
/// @param bar FindBar wrapper handle.
/// @return `1` when whole-word matching is enabled; otherwise `0`.
int64_t rt_findbar_is_whole_word(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    rt_findbar_sync_options_from_widget(data);
    return data->whole_word;
}

/// @brief Enable or disable regex-based pattern matching.
/// @param bar FindBar wrapper handle.
/// @param regex Non-zero to interpret the search text as a regular expression.
void rt_findbar_set_regex(void *bar, int64_t regex) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    data->regex = regex != 0 ? 1 : 0;
    rt_findbar_update_options(data);
}

/// @brief Check whether regex matching is enabled.
/// @param bar FindBar wrapper handle.
/// @return `1` when regular-expression matching is enabled; otherwise `0`.
int64_t rt_findbar_is_regex(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    rt_findbar_sync_options_from_widget(data);
    return data->regex;
}

/// @brief Advance to the next match in the bound editor.
/// @param bar FindBar wrapper handle.
/// @return 1 if at least one match exists, 0 otherwise.
int64_t rt_findbar_find_next(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data || !rt_findbar_has_live_editor(data))
        return 0;
    vg_findreplacebar_find_next(data->bar);
    return vg_findreplacebar_get_match_count(data->bar) > 0 ? 1 : 0;
}

/// @brief Advance to the next match and return its current 1-based match index.
/// @param bar FindBar runtime wrapper.
/// @return Zanna.Option.SomeI64(index) when a match exists, otherwise Zanna.Option.None().
void *rt_findbar_find_next_option(void *bar) {
    if (!rt_findbar_find_next(bar))
        return rt_option_none();
    int64_t current = rt_findbar_get_current_match(bar);
    return current > 0 ? rt_option_some_i64(current) : rt_option_none();
}

/// @brief Move to the previous match in the bound editor.
/// @param bar FindBar wrapper handle.
/// @return 1 if at least one match exists, 0 otherwise.
int64_t rt_findbar_find_previous(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data || !rt_findbar_has_live_editor(data))
        return 0;
    vg_findreplacebar_find_prev(data->bar);
    return vg_findreplacebar_get_match_count(data->bar) > 0 ? 1 : 0;
}

/// @brief Move to the previous match and return its current 1-based match index.
/// @param bar FindBar runtime wrapper.
/// @return Zanna.Option.SomeI64(index) when a match exists, otherwise Zanna.Option.None().
void *rt_findbar_find_previous_option(void *bar) {
    if (!rt_findbar_find_previous(bar))
        return rt_option_none();
    int64_t current = rt_findbar_get_current_match(bar);
    return current > 0 ? rt_option_some_i64(current) : rt_option_none();
}

/// @brief Replace the current match with the replacement text.
/// @param bar FindBar wrapper handle.
/// @return 1 when text was replaced, 0 otherwise.
int64_t rt_findbar_replace(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data || !rt_findbar_has_live_editor(data))
        return 0;
    size_t count_before = vg_findreplacebar_get_match_count(data->bar);
    if (count_before == 0)
        return 0;
    return vg_findreplacebar_replace_current(data->bar) ? 1 : 0;
}

/// @brief Replace all matches in the bound editor with the replacement text.
/// @param bar FindBar wrapper handle.
/// @return The number of matches that existed before replacement.
int64_t rt_findbar_replace_all(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data || !rt_findbar_has_live_editor(data))
        return 0;
    size_t count_before = vg_findreplacebar_get_match_count(data->bar);
    size_t replaced = vg_findreplacebar_replace_all(data->bar);
    return rt_gui_saturating_size_to_i64(replaced <= count_before ? replaced : count_before);
}

/// @brief Get the total number of matches for the current search text.
/// @param bar FindBar wrapper handle.
/// @return Current match count, or zero for invalid input.
int64_t rt_findbar_get_match_count(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    return rt_gui_saturating_size_to_i64(vg_findreplacebar_get_match_count(data->bar));
}

/// @brief Get the 1-based index of the currently highlighted match.
/// @param bar FindBar wrapper handle.
/// @return One-based current match index, or zero when no match is active.
int64_t rt_findbar_get_current_match(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    if (vg_findreplacebar_get_match_count(data->bar) == 0)
        return 0;
    size_t current = vg_findreplacebar_get_current_match(data->bar);
    return current >= (size_t)INT64_MAX ? INT64_MAX : (int64_t)current + 1;
}

/// @brief Show or hide the find bar widget.
/// @param bar FindBar wrapper handle.
/// @param visible Non-zero to show the bar.
void rt_findbar_set_visible(void *bar, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    vg_widget_set_visible(&data->bar->base, visible != 0);
}

/// @brief Check whether the find bar is currently visible.
/// @param bar FindBar wrapper handle.
/// @return `1` when visible; otherwise `0`.
int64_t rt_findbar_is_visible(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return 0;
    return data->bar->base.visible ? 1 : 0;
}

/// @brief Give keyboard focus to the find bar's search input field.
/// @param bar FindBar wrapper handle.
void rt_findbar_focus(void *bar) {
    RT_ASSERT_MAIN_THREAD();
    rt_findbar_data_t *data = rt_findbar_checked(bar);
    if (!data)
        return;
    vg_findreplacebar_focus(data->bar);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

// Graphics-disabled stubs — every public entry-point returns a benign zero/no-op so callers
// link cleanly without graphics. Behavioral docs live on the real implementations above.

/// @brief Stub — graphics disabled at build time. Returns NULL.
/// @param parent Ignored parent handle.
/// @return Always `NULL`.
void *rt_findbar_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Stub: `FindBar.Destroy` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
void rt_findbar_destroy(void *bar) {
    (void)bar;
}

/// @brief Stub: `FindBar.BindEditor` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param editor Ignored CodeEditor handle.
void rt_findbar_bind_editor(void *bar, void *editor) {
    (void)bar;
    (void)editor;
}

/// @brief Stub: `FindBar.UnbindEditor` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
void rt_findbar_unbind_editor(void *bar) {
    (void)bar;
}

/// @brief Stub: `FindBar.SetReplaceMode` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param replace Ignored replacement-mode flag.
void rt_findbar_set_replace_mode(void *bar, int64_t replace) {
    (void)bar;
    (void)replace;
}

/// @brief Stub: returns 0 (replace mode always off without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_is_replace_mode(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: `FindBar.SetFindText` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param text Ignored search text.
void rt_findbar_set_find_text(void *bar, rt_string text) {
    (void)bar;
    (void)text;
}

/// @brief Stub: returns empty string (no find text without graphics).
/// @param bar Ignored FindBar handle.
/// @return Empty runtime string.
rt_string rt_findbar_get_find_text(void *bar) {
    (void)bar;
    return rt_str_empty();
}

/// @brief Stub: `FindBar.SetReplaceText` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param text Ignored replacement text.
void rt_findbar_set_replace_text(void *bar, rt_string text) {
    (void)bar;
    (void)text;
}

/// @brief Stub: returns empty string (no replace text without graphics).
/// @param bar Ignored FindBar handle.
/// @return Empty runtime string.
rt_string rt_findbar_get_replace_text(void *bar) {
    (void)bar;
    return rt_str_empty();
}

/// @brief Stub: `FindBar.SetCaseSensitive` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param sensitive Ignored matching flag.
void rt_findbar_set_case_sensitive(void *bar, int64_t sensitive) {
    (void)bar;
    (void)sensitive;
}

/// @brief Stub: returns 0 (case-insensitive by default without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_is_case_sensitive(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: `FindBar.SetWholeWord` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param whole Ignored matching flag.
void rt_findbar_set_whole_word(void *bar, int64_t whole) {
    (void)bar;
    (void)whole;
}

/// @brief Stub: returns 0 (whole-word off without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_is_whole_word(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: `FindBar.SetRegex` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param regex Ignored matching flag.
void rt_findbar_set_regex(void *bar, int64_t regex) {
    (void)bar;
    (void)regex;
}

/// @brief Stub: returns 0 (regex off without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_is_regex(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns 0 (no search without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_find_next(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns None (no search without graphics).
/// @param bar Ignored FindBar handle.
/// @return Managed empty Option.
void *rt_findbar_find_next_option(void *bar) {
    (void)bar;
    return rt_option_none();
}

/// @brief Stub: returns 0 (no search without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_find_previous(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns None (no search without graphics).
/// @param bar Ignored FindBar handle.
/// @return Managed empty Option.
void *rt_findbar_find_previous_option(void *bar) {
    (void)bar;
    return rt_option_none();
}

/// @brief Stub: returns 0 (no replace without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_replace(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns 0 (no replace-all without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_replace_all(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns 0 (no match count without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_get_match_count(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: returns 0 (no current match without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_get_current_match(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: `FindBar.SetVisible` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
/// @param visible Ignored visibility flag.
void rt_findbar_set_visible(void *bar, int64_t visible) {
    (void)bar;
    (void)visible;
}

/// @brief Stub: returns 0 (always hidden without graphics).
/// @param bar Ignored FindBar handle.
/// @return Always `0`.
int64_t rt_findbar_is_visible(void *bar) {
    (void)bar;
    return 0;
}

/// @brief Stub: `FindBar.Focus` is a no-op without graphics.
/// @param bar Ignored FindBar handle.
void rt_findbar_focus(void *bar) {
    (void)bar;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
