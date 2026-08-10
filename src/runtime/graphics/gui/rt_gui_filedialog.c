//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_filedialog.c
// Purpose: Cross-platform file-dialog runtime bindings, including sentinel-compatible and
//   Option/Seq one-shot helpers plus reusable synchronous/asynchronous picker controllers.
//
// Key invariants:
//   - Compatibility helpers block and retain their empty-string/escaped-list contracts.
//   - ShowAsync never polls recursively; ordinary app frames drive exactly one completion edge.
//   - Accepted path arrays are deep-copied atomically before lower-dialog storage can change.
//   - Reusable retained pickers share filtering/bookmark semantics across supported platforms.
//
// Ownership/Lifetime:
//   - rt_filedialog_data_t is GC-managed and owns its lower dialog plus copied result paths.
//   - Option/Seq results are independently managed and remain valid after controller destruction.
//
// Links: src/runtime/graphics/gui/rt_gui.h,
//        src/lib/gui/src/widgets/vg_filedialog.c,
//        docs/adr/0109-gui-dialog-media-scheduling-and-automation.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_filedialog.c
/// @brief Implements synchronous, asynchronous, and reusable file-dialog runtime bindings.
///
/// @details
/// The module preserves legacy sentinel and escaped-list APIs while also
/// exposing explicit Option and sequence results. Retained controller wrappers
/// deep-copy accepted paths and keep headless behavior ABI-compatible.

#include "rt_gui_internal.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_trap.h"

/// @brief Count paths in the escaped semicolon list returned by `OpenMultiple`.
/// @param escaped Runtime escaped path list.
/// @return Number of encoded paths, zero for empty input, or `INT64_MAX` on saturation.
int64_t rt_filedialog_path_list_count(rt_string escaped) {
    if (!escaped)
        return 0;
    int64_t len64 = rt_str_len(escaped);
    if (len64 <= 0)
        return 0;
    const char *bytes = rt_string_cstr(escaped);
    if (!bytes)
        return 0;

    size_t len = (size_t)len64;
    int64_t count = 1;
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\\' && i + 1 < len) {
            i++;
            continue;
        }
        if (bytes[i] == ';') {
            if (count == INT64_MAX)
                return INT64_MAX;
            count++;
        }
    }
    return count;
}

/// @brief Decode one path from the escaped semicolon list returned by `OpenMultiple`.
/// @param escaped Runtime escaped path list.
/// @param index Zero-based encoded path index.
/// @return Owned decoded path, or an empty runtime string for invalid input or allocation failure.
rt_string rt_filedialog_path_list_get(rt_string escaped, int64_t index) {
    if (!escaped || index < 0)
        return rt_str_empty();
    int64_t len64 = rt_str_len(escaped);
    if (len64 <= 0)
        return rt_str_empty();
    const char *bytes = rt_string_cstr(escaped);
    if (!bytes)
        return rt_str_empty();

    size_t len = (size_t)len64;
    int64_t current = 0;
    size_t segment_start = 0;
    size_t segment_end = len;
    int found = 0;
    for (size_t i = 0; i <= len; i++) {
        int at_separator = 0;
        if (i == len) {
            at_separator = 1;
        } else if (bytes[i] == '\\' && i + 1 < len) {
            i++;
            continue;
        } else if (bytes[i] == ';') {
            at_separator = 1;
        }
        if (at_separator) {
            if (current == index) {
                segment_end = i;
                found = 1;
                break;
            }
            if (current == INT64_MAX)
                return rt_str_empty();
            current++;
            segment_start = i + 1;
        }
    }
    if (!found || segment_end < segment_start)
        return rt_str_empty();

    size_t encoded_len = segment_end - segment_start;
    char *decoded = (char *)malloc(encoded_len + 1);
    if (!decoded)
        return rt_str_empty();
    size_t out = 0;
    for (size_t i = segment_start; i < segment_end; i++) {
        if (bytes[i] == '\\' && i + 1 < segment_end) {
            decoded[out++] = bytes[++i];
        } else {
            decoded[out++] = bytes[i];
        }
    }
    decoded[out] = '\0';
    rt_string result = rt_string_from_bytes(decoded, out);
    free(decoded);
    return result;
}

/// @brief Convert a compatibility path result into an explicit optional value.
/// @details File-system paths selected by the supported dialogs are non-empty. Consequently the
///          compatibility empty-string sentinel maps to `None`, while every selected path maps to
///          `Some(path)`. The Option retains the runtime string and this helper releases its local
///          reference before returning.
/// @param path Owned runtime string returned by a synchronous compatibility dialog.
/// @return Managed `Zanna.Option[str]`; `None` for cancellation, failure, or an empty path.
static void *rt_filedialog_path_option(rt_string path) {
    if (!path || rt_str_len(path) <= 0) {
        rt_string_unref(path);
        return rt_option_none();
    }
    void *option = rt_option_some_str(path);
    rt_string_unref(path);
    return option;
}

/// @brief Show the single-file picker and distinguish cancellation from selection.
/// @details This is the Option-returning companion to @ref rt_filedialog_open. It preserves the
///          same synchronous compatibility behavior and platform implementation but replaces the
///          ambiguous empty-string sentinel with `None`.
/// @param title Dialog title copied for the duration of the operation.
/// @param default_path Initial directory; embedded NUL input is rejected by the underlying API.
/// @param filter Semicolon-delimited glob filter such as `*.zia;*.bas`.
/// @return `Some(path)` after acceptance, otherwise `None` on cancel, unavailable graphics, or
///         construction failure.
void *rt_filedialog_open_option(rt_string title, rt_string default_path, rt_string filter) {
    return rt_filedialog_path_option(rt_filedialog_open(title, default_path, filter));
}

/// @brief Show the save picker and return its result without a sentinel collision.
/// @details Delegates to the existing synchronous save operation so native and retained dialogs
///          have identical filtering and extension behavior.
/// @param title Dialog title.
/// @param default_path Initial directory.
/// @param filter Semicolon-delimited glob filter.
/// @param default_name Initial filename shown in the save field.
/// @return `Some(path)` after acceptance, otherwise `None`.
void *rt_filedialog_save_option(rt_string title,
                                rt_string default_path,
                                rt_string filter,
                                rt_string default_name) {
    return rt_filedialog_path_option(rt_filedialog_save(title, default_path, filter, default_name));
}

/// @brief Show the folder picker and represent cancellation as `None`.
/// @param title Dialog title.
/// @param default_path Initial directory.
/// @return `Some(path)` for the selected folder, otherwise `None`.
void *rt_filedialog_select_folder_option(rt_string title, rt_string default_path) {
    return rt_filedialog_path_option(rt_filedialog_select_folder(title, default_path));
}

/// @brief Show the multi-file picker and return an owned sequence of individual paths.
/// @details The legacy escaped-semicolon representation remains supported. This operation decodes
///          it immediately into an owned `Seq[str]`, including paths containing literal semicolons
///          or backslashes. Cancellation and failure return an empty sequence.
/// @param title Dialog title.
/// @param default_path Initial directory.
/// @param filter Semicolon-delimited glob filter.
/// @return Managed owned sequence of selected runtime strings; empty on cancellation/failure.
void *rt_filedialog_open_multiple_seq(rt_string title, rt_string default_path, rt_string filter) {
    void *paths = rt_seq_new_owned();
    if (!paths)
        return NULL;
    rt_string escaped = rt_filedialog_open_multiple(title, default_path, filter);
    int64_t count = rt_filedialog_path_list_count(escaped);
    for (int64_t index = 0; index < count; index++) {
        rt_string path = rt_filedialog_path_list_get(escaped, index);
        if (!path)
            continue;
        rt_seq_push(paths, path);
        rt_string_unref(path);
    }
    rt_string_unref(escaped);
    return paths;
}

#ifdef ZANNA_ENABLE_GRAPHICS

//=============================================================================
// Phase 5: FileDialog
//=============================================================================

/// @brief Return the active GUI app for file-dialog hosting.
/// @details Prefers the app returned by `rt_gui_get_active_app`; falls back to
///          the module-level `s_current_app` pointer so dialogs work even when
///          the "active" app is temporarily null during event dispatch.
/// @return Active or current application, or `NULL` when no GUI app is available.
static rt_gui_app_t *rt_filedialog_app(void) {
    rt_gui_app_t *app = rt_gui_get_active_app();
    return app ? app : s_current_app;
}

/// @brief Duplicate a NUL-terminated string using the runtime allocator contract.
/// @details Avoids platform-specific `strdup` variants in graphics code and
///          keeps ownership simple: the returned buffer is always allocated
///          with `malloc` and must be released with `free`.
/// @param text Source text to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_filedialog_strdup(const char *text) {
    return rt_gui_strdup_bounded(text);
}

/// @brief Join selected paths as a semicolon-delimited string with '\\' escaping.
/// @details Escapes literal ';' and '\\' so callers can unambiguously split the result.
/// @param paths Array of selected null-terminated paths.
/// @param count Number of entries in @p paths.
/// @param[out] out_len Optional destination for the encoded byte length.
/// @return Newly allocated escaped list, or `NULL` for empty input, overflow, or OOM.
static char *rt_filedialog_join_paths_escaped(char **paths, size_t count, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!paths || count == 0)
        return NULL;

    size_t needed = 1; // NUL
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            if (needed == SIZE_MAX)
                return NULL;
            needed++;
        }
        const char *path = paths[i] ? paths[i] : "";
        for (const char *p = path; *p; p++) {
            size_t add = (*p == ';' || *p == '\\') ? 2 : 1;
            if (needed > SIZE_MAX - add)
                return NULL;
            needed += add;
        }
    }

    char *joined = (char *)malloc(needed);
    if (!joined)
        return NULL;

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            joined[off++] = ';';
        const char *path = paths[i] ? paths[i] : "";
        for (const char *p = path; *p; p++) {
            if (*p == ';' || *p == '\\')
                joined[off++] = '\\';
            joined[off++] = *p;
        }
    }
    joined[off] = '\0';
    if (out_len)
        *out_len = off;
    return joined;
}

/// @brief Configure `dialog` for modal presentation and push it onto the app's dialog stack.
/// @details Ensures default font is applied, sets the dialog as the modal root over `app->root`,
///          shows it, and pushes it so the main loop blocks on it. Returns 0 if any pointer is
///          NULL.
/// @param app Application and window hosting the modal surface.
/// @param dialog Lower-toolkit file dialog to configure and push.
/// @return `1` when the dialog becomes the top modal dialog; otherwise `0`.
static int rt_filedialog_prepare_modal(rt_gui_app_t *app, vg_filedialog_t *dialog) {
    if (!app || !app->window || !app->root || !dialog)
        return 0;
    rt_gui_activate_app(app);
    rt_gui_ensure_default_font();
    rt_gui_apply_default_font(&dialog->base.base);
    vg_dialog_set_modal(&dialog->base, true, app->root);
    vg_filedialog_show(dialog);
    rt_gui_push_dialog(app, &dialog->base);
    return rt_gui_top_dialog(app) == &dialog->base;
}

/// @brief Run the GUI event loop until the dialog closes or the app signals shutdown.
/// @details Polls and renders the app in a tight loop; pops the dialog from the stack
///          and syncs the modal root on exit. Returns 1 if at least one file was selected.
/// @param app Application whose event loop drives the modal dialog.
/// @param dialog Open lower-toolkit file dialog.
/// @return `1` when at least one path was selected; otherwise `0`.
static int rt_filedialog_run_modal(rt_gui_app_t *app, vg_filedialog_t *dialog) {
    if (!app || !dialog)
        return 0;
    while (dialog->base.is_open && !app->should_close) {
        rt_gui_app_poll(app);
        rt_gui_app_render(app);
    }
    rt_gui_remove_dialog(app, &dialog->base);
    rt_gui_sync_modal_root(app);
    return dialog->selected_file_count > 0 ? 1 : 0;
}

/// @brief Prepare and then run a modal file dialog in one call.
/// @details Combines `rt_filedialog_prepare_modal` + `rt_filedialog_run_modal`.
/// @param app Application hosting the modal dialog.
/// @param dialog Lower-toolkit file dialog to show.
/// @return `1` when at least one path was selected; otherwise `0`.
#if !RT_PLATFORM_MACOS
/// @brief Prepare and synchronously run a modal file dialog.
/// @param app Application hosting the modal dialog.
/// @param dialog Lower-toolkit file dialog to show.
/// @return `1` when at least one path was selected; otherwise `0`.
static int rt_filedialog_show_modal(rt_gui_app_t *app, vg_filedialog_t *dialog) {
    if (!rt_filedialog_prepare_modal(app, dialog))
        return 0;
    return rt_filedialog_run_modal(app, dialog);
}
#endif

/// @brief One-shot "open file" dialog. Blocks the caller until the user picks a single file or
/// cancels when an active GUI app/window exists. Returns the absolute path on selection, or an
/// empty string on cancel or when no modal GUI window is active.
/// @param title Runtime dialog title.
/// @param default_path Runtime initial directory.
/// @param filter Runtime semicolon-delimited glob filter.
/// @return Owned selected path, or an empty runtime string on cancellation or failure.
rt_string rt_filedialog_open(rt_string title, rt_string default_path, rt_string filter) {
    RT_ASSERT_MAIN_THREAD();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cfilter = rt_string_to_cstr_no_nul(filter);
    char *cpath = rt_string_to_cstr_no_nul(default_path);

#if RT_PLATFORM_MACOS || RT_PLATFORM_WINDOWS
    // Native OS dialog (macOS panels; Windows IFileOpenDialog via COM). A
    // failed Windows COM probe routes to the drawn fallback below.
    char *result = NULL;
    int native_dialog_used = vg_native_dialogs_available();
    if (native_dialog_used)
        result = vg_native_open_file(ctitle, cpath, "Files", cfilter);
#else
    char *result = NULL;
    int native_dialog_used = 0;
#endif
#if !RT_PLATFORM_MACOS
    if (!native_dialog_used) {
        vg_filedialog_t *dlg = vg_filedialog_create(VG_FILEDIALOG_OPEN);
        if (dlg) {
            if (ctitle)
                vg_filedialog_set_title(dlg, ctitle);
            if (cpath)
                vg_filedialog_set_initial_path(dlg, cpath);
            if (cfilter && cfilter[0])
                vg_filedialog_add_filter(dlg, "Files", cfilter);
            vg_filedialog_add_default_bookmarks(dlg);
            if (rt_filedialog_show_modal(rt_filedialog_app(), dlg) &&
                dlg->selected_file_count > 0 && dlg->selected_files[0]) {
                result = rt_filedialog_strdup(dlg->selected_files[0]);
            }
            vg_filedialog_destroy(dlg);
        }
    }
#endif
    (void)native_dialog_used;

    if (ctitle)
        free(ctitle);
    if (cpath)
        free(cpath);
    if (cfilter)
        free(cfilter);

    if (result) {
        rt_string ret = rt_gui_string_from_cstr_bounded(result);
        free(result);
        return ret;
    }
    return rt_str_empty();
}

/// @brief Open dialog with multi-select. Returns paths as an escaped semicolon-separated string.
/// Use `rt_filedialog_path_list_count` and `rt_filedialog_path_list_get` to decode it.
/// @param title Runtime dialog title.
/// @param default_path Runtime initial directory.
/// @param filter Runtime semicolon-delimited glob filter.
/// @return Owned escaped path list, or an empty runtime string on cancellation or failure.
rt_string rt_filedialog_open_multiple(rt_string title, rt_string default_path, rt_string filter) {
    RT_ASSERT_MAIN_THREAD();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cpath = rt_string_to_cstr_no_nul(default_path);
    char *cfilter = rt_string_to_cstr_no_nul(filter);

#if RT_PLATFORM_MACOS || RT_PLATFORM_WINDOWS
    if (vg_native_dialogs_available()) {
        size_t count = 0;
        char **paths = vg_native_open_files(ctitle, cpath, "Files", cfilter, &count);
        rt_string result = rt_str_empty();
        if (paths && count > 0) {
            size_t joined_len = 0;
            char *joined = rt_filedialog_join_paths_escaped(paths, count, &joined_len);
            if (joined) {
                result = rt_string_from_bytes(joined, joined_len);
                free(joined);
            }
        }
        vg_native_free_paths(paths, count);

        if (ctitle)
            free(ctitle);
        if (cpath)
            free(cpath);
        if (cfilter)
            free(cfilter);
        return result;
    }
#endif
#if !RT_PLATFORM_MACOS
    vg_filedialog_t *dlg = vg_filedialog_create(VG_FILEDIALOG_OPEN);
    if (!dlg) {
        if (ctitle)
            free(ctitle);
        if (cpath)
            free(cpath);
        if (cfilter)
            free(cfilter);
        return rt_str_empty();
    }

    if (ctitle)
        vg_filedialog_set_title(dlg, ctitle);
    if (cpath)
        vg_filedialog_set_initial_path(dlg, cpath);
    vg_filedialog_set_multi_select(dlg, true);
    if (cfilter && cfilter[0]) {
        vg_filedialog_add_filter(dlg, "Files", cfilter);
    }
    vg_filedialog_add_default_bookmarks(dlg);

    if (ctitle)
        free(ctitle);
    if (cpath)
        free(cpath);
    if (cfilter)
        free(cfilter);

    rt_filedialog_show_modal(rt_filedialog_app(), dlg);

    size_t count = 0;
    char **paths = vg_filedialog_get_selected_paths(dlg, &count);

    rt_string result = rt_str_empty();
    if (paths && count > 0) {
        size_t joined_len = 0;
        char *joined = rt_filedialog_join_paths_escaped(paths, count, &joined_len);
        if (joined) {
            result = rt_string_from_bytes(joined, joined_len);
            free(joined);
        }
    }

    vg_filedialog_destroy(dlg);
    return result;
#else
    return rt_str_empty(); /* unreachable: macOS native panels are always available */
#endif
}

/// @brief One-shot "save file" dialog. Returns the chosen path (with extension if user typed
/// one or accepted the default), or empty on cancel. Does not actually create the file — the
/// caller writes to the returned path.
/// @param title Runtime dialog title.
/// @param default_path Runtime initial directory.
/// @param filter Runtime semicolon-delimited glob filter.
/// @param default_name Runtime initial filename.
/// @return Owned selected path, or an empty runtime string on cancellation or failure.
rt_string rt_filedialog_save(rt_string title,
                             rt_string default_path,
                             rt_string filter,
                             rt_string default_name) {
    RT_ASSERT_MAIN_THREAD();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cfilter = rt_string_to_cstr_no_nul(filter);
    char *cname = rt_string_to_gui_cstr(default_name);
    char *cpath = rt_string_to_cstr_no_nul(default_path);

#if RT_PLATFORM_MACOS || RT_PLATFORM_WINDOWS
    // Native OS dialog with drawn fallback when Windows COM is unavailable.
    char *result = NULL;
    int native_dialog_used = vg_native_dialogs_available();
    if (native_dialog_used)
        result = vg_native_save_file(ctitle, cpath, cname, "Files", cfilter);
#else
    char *result = NULL;
    int native_dialog_used = 0;
#endif
#if !RT_PLATFORM_MACOS
    if (!native_dialog_used) {
        vg_filedialog_t *dlg = vg_filedialog_create(VG_FILEDIALOG_SAVE);
        if (dlg) {
            if (ctitle)
                vg_filedialog_set_title(dlg, ctitle);
            if (cpath)
                vg_filedialog_set_initial_path(dlg, cpath);
            if (cname)
                vg_filedialog_set_filename(dlg, cname);
            if (cfilter && cfilter[0])
                vg_filedialog_add_filter(dlg, "Files", cfilter);
            vg_filedialog_add_default_bookmarks(dlg);
            if (rt_filedialog_show_modal(rt_filedialog_app(), dlg) &&
                dlg->selected_file_count > 0 && dlg->selected_files[0]) {
                result = rt_filedialog_strdup(dlg->selected_files[0]);
            }
            vg_filedialog_destroy(dlg);
        }
    }
#endif
    (void)native_dialog_used;

    if (ctitle)
        free(ctitle);
    if (cpath)
        free(cpath);
    if (cfilter)
        free(cfilter);
    if (cname)
        free(cname);

    if (result) {
        rt_string ret = rt_gui_string_from_cstr_bounded(result);
        free(result);
        return ret;
    }
    return rt_str_empty();
}

/// @brief One-shot folder-picker dialog. Returns the absolute folder path or empty on cancel.
/// @param title Runtime dialog title.
/// @param default_path Runtime initial directory.
/// @return Owned selected folder path, or an empty runtime string on cancellation or failure.
rt_string rt_filedialog_select_folder(rt_string title, rt_string default_path) {
    RT_ASSERT_MAIN_THREAD();
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cpath = rt_string_to_cstr_no_nul(default_path);

#if RT_PLATFORM_MACOS || RT_PLATFORM_WINDOWS
    // Native OS dialog with drawn fallback when Windows COM is unavailable.
    char *result = NULL;
    int native_dialog_used = vg_native_dialogs_available();
    if (native_dialog_used)
        result = vg_native_select_folder(ctitle, cpath);
#else
    char *result = NULL;
    int native_dialog_used = 0;
#endif
#if !RT_PLATFORM_MACOS
    if (!native_dialog_used) {
        vg_filedialog_t *dlg = vg_filedialog_create(VG_FILEDIALOG_SELECT_FOLDER);
        if (dlg) {
            if (ctitle)
                vg_filedialog_set_title(dlg, ctitle);
            if (cpath)
                vg_filedialog_set_initial_path(dlg, cpath);
            vg_filedialog_add_default_bookmarks(dlg);
            if (rt_filedialog_show_modal(rt_filedialog_app(), dlg) &&
                dlg->selected_file_count > 0 && dlg->selected_files[0]) {
                result = rt_filedialog_strdup(dlg->selected_files[0]);
            }
            vg_filedialog_destroy(dlg);
        }
    }
#endif
    (void)native_dialog_used;

    if (ctitle)
        free(ctitle);
    if (cpath)
        free(cpath);

    if (result) {
        rt_string ret = rt_gui_string_from_cstr_bounded(result);
        free(result);
        return ret;
    }
    return rt_str_empty();
}

// Custom FileDialog structure
#define RT_FILEDIALOG_DATA_MAGIC UINT64_C(0x525446494C45444C)

typedef struct {
    uint64_t magic;
    rt_gui_app_t *owner_app;
    vg_filedialog_t *dialog;
    char **selected_paths;
    size_t selected_count;
    int64_t result;
    int64_t status;
    const char *error;
    uint64_t completed_edges;
} rt_filedialog_data_t;

static void rt_filedialog_clear_selected_paths(rt_filedialog_data_t *data);
static void rt_filedialog_record_completion(rt_filedialog_data_t *data);

static rt_filedialog_data_t **s_filedialog_wrappers = NULL;
static size_t s_filedialog_wrapper_count = 0;
static size_t s_filedialog_wrapper_cap = 0;

/// @brief Record a wrapper in the global file-dialog registry (idempotent).
/// @details The registry is the source of truth for handle validation: a checked
///          cast only trusts an opaque `void*` once it is found here (then verifies
///          the magic tag), guarding against forged/freed handles. Capacity doubles from 8.
/// @param data File-dialog wrapper to register.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int rt_filedialog_register_wrapper(rt_filedialog_data_t *data) {
    RT_ASSERT_MAIN_THREAD();
    if (!data)
        return 0;
    for (size_t i = 0; i < s_filedialog_wrapper_count; i++) {
        if (s_filedialog_wrappers[i] == data)
            return 1;
    }
    if (s_filedialog_wrapper_count >= s_filedialog_wrapper_cap) {
        size_t new_cap = 0;
        if (!rt_gui_next_collection_capacity(s_filedialog_wrapper_cap,
                                             s_filedialog_wrapper_count + 1u,
                                             8u,
                                             sizeof(rt_filedialog_data_t *),
                                             &new_cap))
            return 0;
        void *p = realloc(s_filedialog_wrappers, new_cap * sizeof(rt_filedialog_data_t *));
        if (!p)
            return 0;
        s_filedialog_wrappers = (rt_filedialog_data_t **)p;
        s_filedialog_wrapper_cap = new_cap;
    }
    s_filedialog_wrappers[s_filedialog_wrapper_count++] = data;
    return 1;
}

/// @brief Remove a wrapper from the file-dialog registry, compacting the array. No-op if absent.
/// @param data Wrapper to remove.
static void rt_filedialog_unregister_wrapper(rt_filedialog_data_t *data) {
    RT_ASSERT_MAIN_THREAD();
    if (!data)
        return;
    for (size_t i = 0; i < s_filedialog_wrapper_count; i++) {
        if (s_filedialog_wrappers[i] != data)
            continue;
        memmove(&s_filedialog_wrappers[i],
                &s_filedialog_wrappers[i + 1],
                (s_filedialog_wrapper_count - i - 1) * sizeof(*s_filedialog_wrappers));
        s_filedialog_wrapper_count--;
        return;
    }
}

/// @brief True if @p data is a currently-registered wrapper; backs handle validation.
/// @param data Candidate wrapper pointer.
/// @return `1` when the exact pointer is registered; otherwise `0`.
static int rt_filedialog_wrapper_is_registered(const rt_filedialog_data_t *data) {
    RT_ASSERT_MAIN_THREAD();
    if (!data)
        return 0;
    for (size_t i = 0; i < s_filedialog_wrapper_count; i++) {
        if (s_filedialog_wrappers[i] == data)
            return 1;
    }
    return 0;
}

/// @brief Invalidate any retained wrapper whose lower dialog was destroyed externally.
/// @param dialog Lower dialog pointer being invalidated.
void rt_filedialog_invalidate_dialog(vg_dialog_t *dialog) {
    RT_ASSERT_MAIN_THREAD();
    if (!dialog)
        return;
    for (size_t i = 0; i < s_filedialog_wrapper_count; i++) {
        rt_filedialog_data_t *data = s_filedialog_wrappers[i];
        if (data && data->dialog && &data->dialog->base == dialog) {
            if (data->status == RT_GUI_DIALOG_STATUS_OPEN) {
                data->status = RT_GUI_DIALOG_STATUS_FAILED;
                data->error = "No active GUI application is available";
                rt_filedialog_record_completion(data);
            }
            data->dialog = NULL;
            data->owner_app = NULL;
            data->result = 0;
        }
    }
}

/// @brief Safe-cast an opaque handle to the file-dialog wrapper by magic tag.
/// @param dialog Candidate runtime FileDialog handle.
/// @return Validated registered wrapper, or `NULL` for invalid input.
static rt_filedialog_data_t *rt_filedialog_wrapper_checked(void *dialog) {
    rt_filedialog_data_t *data = (rt_filedialog_data_t *)dialog;
    return rt_filedialog_wrapper_is_registered(data) && data->magic == RT_FILEDIALOG_DATA_MAGIC
               ? data
               : NULL;
}

/// @brief Safe-cast an opaque handle to a wrapper with a live backing dialog.
/// @param dialog Candidate runtime FileDialog handle.
/// @return Validated wrapper with a live lower widget, or `NULL`.
static rt_filedialog_data_t *rt_filedialog_data_checked(void *dialog) {
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    return data && data->dialog && vg_widget_is_live(&data->dialog->base.base) ? data : NULL;
}

/// @brief Free the selected-paths array and reset count to zero.
/// @param data File-dialog wrapper whose owned path snapshot is cleared.
static void rt_filedialog_clear_selected_paths(rt_filedialog_data_t *data) {
    if (!data || !data->selected_paths)
        return;
    for (size_t i = 0; i < data->selected_count; i++) {
        free(data->selected_paths[i]);
    }
    free(data->selected_paths);
    data->selected_paths = NULL;
    data->selected_count = 0;
}

/// @brief Record one completed asynchronous operation without losing unread edges.
/// @details Completion counters saturate instead of wrapping so a slow observer can consume every
///          distinct completion up to the representable limit. The status/error/result fields must
///          be populated before this helper is called.
/// @param data Live registered file-dialog wrapper receiving the completion edge.
static void rt_filedialog_record_completion(rt_filedialog_data_t *data) {
    if (data && data->completed_edges < UINT64_MAX)
        data->completed_edges++;
}

/// @brief Deep-copy an explicit borrowed path array into a file-dialog wrapper atomically.
/// @details The old selection remains unchanged if any allocation fails. Empty arrays are treated
///          as no accepted selection and clear the previous snapshot only after validation.
/// @param data Live wrapper that will own the copied path array.
/// @param paths Borrowed UTF-8 path pointers owned by the lower dialog.
/// @param count Number of pointers in @p paths.
/// @return 1 after a complete copy; 0 for invalid/empty input, overflow, or allocation failure.
static int rt_filedialog_copy_paths(rt_filedialog_data_t *data, char **paths, size_t count) {
    if (!data || !paths || count == 0)
        return 0;
    if (count > RT_GUI_MAX_COLLECTION_ITEMS || count > SIZE_MAX / sizeof(char *))
        return 0;

    char **copy = (char **)calloc(count, sizeof(char *));
    if (!copy)
        return 0;

    size_t copied = 0;
    for (; copied < count; copied++) {
        copy[copied] = rt_filedialog_strdup(paths[copied] ? paths[copied] : "");
        if (!copy[copied])
            break;
    }
    if (copied != count) {
        for (size_t i = 0; i < copied; i++)
            free(copy[i]);
        free(copy);
        return 0;
    }

    rt_filedialog_clear_selected_paths(data);
    data->selected_paths = copy;
    data->selected_count = count;
    return 1;
}

/// @brief Copy the dialog's current selected-path list into the wrapper struct.
/// @details Fetches the borrowed path array owned by the VG backend, deep-copies every string, then
///          atomically replaces the wrapper's previous list. Partial copy failures clean
///          up fully and return 0 rather than leaving a corrupt partial list.
/// @param data Live wrapper whose lower-dialog selection is copied.
/// @return `1` after a complete copy; otherwise `0`.
static int rt_filedialog_copy_selected_paths(rt_filedialog_data_t *data) {
    if (!data || !data->dialog)
        return 0;

    size_t count = 0;
    char **paths = vg_filedialog_get_selected_paths(data->dialog, &count);
    if (!paths || count == 0) {
        rt_filedialog_clear_selected_paths(data);
        return 0;
    }
    return rt_filedialog_copy_paths(data, paths, count);
}

/// @brief Translate one lower-dialog close result into the runtime asynchronous state machine.
/// @details The callback never destroys the lower dialog because it runs inside
///          `vg_dialog_close`. It snapshots accepted paths, records exactly one completion edge,
///          and removes the now-closed modal entry from its owning app so subsequent events route
///          to the previous root immediately.
/// @param dialog Borrowed lower dialog that has just closed.
/// @param result Lower close result.
/// @param user_data Borrowed registered @ref rt_filedialog_data_t wrapper.
static void rt_filedialog_on_result(vg_dialog_t *dialog,
                                    vg_dialog_result_t result,
                                    void *user_data) {
    rt_filedialog_data_t *data = (rt_filedialog_data_t *)user_data;
    if (!rt_filedialog_wrapper_is_registered(data) || !data->dialog ||
        &data->dialog->base != dialog || data->status != RT_GUI_DIALOG_STATUS_OPEN) {
        return;
    }

    if (result == VG_DIALOG_RESULT_OK) {
        if (rt_filedialog_copy_selected_paths(data)) {
            data->result = 1;
            data->status = RT_GUI_DIALOG_STATUS_ACCEPTED;
            data->error = "";
        } else {
            data->result = 0;
            data->status = RT_GUI_DIALOG_STATUS_FAILED;
            data->error = "File dialog result could not be stored";
        }
    } else {
        rt_filedialog_clear_selected_paths(data);
        data->result = 0;
        data->status = RT_GUI_DIALOG_STATUS_CANCELLED;
        data->error = "";
    }
    rt_filedialog_record_completion(data);
    if (rt_gui_is_app_handle(data->owner_app))
        rt_gui_remove_dialog(data->owner_app, dialog);
}

/// @brief Release all resources owned by the file-dialog wrapper.
/// @param data Registered wrapper to dispose.
static void rt_filedialog_dispose(rt_filedialog_data_t *data) {
    if (!data)
        return;
    rt_filedialog_clear_selected_paths(data);
    if (data->dialog) {
        rt_gui_app_t *app =
            rt_gui_is_app_handle(data->owner_app) ? data->owner_app : rt_filedialog_app();
        if (app)
            rt_gui_remove_dialog(app, &data->dialog->base);
        vg_filedialog_destroy(data->dialog);
        data->dialog = NULL;
    }
    data->owner_app = NULL;
    data->result = 0;
    data->magic = 0;
    rt_filedialog_unregister_wrapper(data);
}

/// @brief GC finalizer — delegates to `rt_filedialog_dispose`.
/// @param dialog Managed FileDialog wrapper being finalized.
static void rt_filedialog_finalize(void *dialog) {
    rt_filedialog_dispose((rt_filedialog_data_t *)dialog);
}

/// @brief Construct a stateful FileDialog object — `type` is RT_FILEDIALOG_OPEN/SAVE/FOLDER.
/// Use the setters (`_set_title`, `_set_path`, `_add_filter`, ...) to configure, then `_show`
/// to display modally. Returns NULL on backend or allocation failure.
/// @param type Public open, save, or folder dialog type.
/// @return New managed FileDialog wrapper, or `NULL` for invalid type or allocation failure.
void *rt_filedialog_new(int64_t type) {
    RT_ASSERT_MAIN_THREAD();
    vg_filedialog_mode_t mode;
    switch (type) {
        case RT_FILEDIALOG_OPEN:
            mode = VG_FILEDIALOG_OPEN;
            break;
        case RT_FILEDIALOG_SAVE:
            mode = VG_FILEDIALOG_SAVE;
            break;
        case RT_FILEDIALOG_FOLDER:
            mode = VG_FILEDIALOG_SELECT_FOLDER;
            break;
        default:
            return NULL;
    }

    vg_filedialog_t *dlg = vg_filedialog_create(mode);
    if (!dlg)
        return NULL;
    vg_filedialog_add_default_bookmarks(dlg);

    rt_filedialog_data_t *data =
        (rt_filedialog_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_filedialog_data_t));
    if (!data) {
        vg_filedialog_destroy(dlg);
        return NULL;
    }
    data->magic = RT_FILEDIALOG_DATA_MAGIC;
    data->owner_app = rt_filedialog_app();
    data->dialog = dlg;
    data->selected_paths = NULL;
    data->selected_count = 0;
    data->result = 0;
    data->status = RT_GUI_DIALOG_STATUS_IDLE;
    data->error = "";
    data->completed_edges = 0;
    if (!rt_filedialog_register_wrapper(data)) {
        rt_filedialog_dispose(data);
        return NULL;
    }
    vg_dialog_set_on_result(&dlg->base, rt_filedialog_on_result, data);
    rt_obj_set_finalizer(data, rt_filedialog_finalize);

    return data;
}

/// @brief Convenience constructor for an Open-file dialog.
/// @return New managed open-file dialog, or `NULL` on failure.
void *rt_filedialog_new_open(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_filedialog_new(RT_FILEDIALOG_OPEN);
}

/// @brief Convenience constructor for a Save-file dialog.
/// @return New managed save-file dialog, or `NULL` on failure.
void *rt_filedialog_new_save(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_filedialog_new(RT_FILEDIALOG_SAVE);
}

/// @brief Convenience constructor for a Select-folder dialog.
/// @return New managed folder dialog, or `NULL` on failure.
void *rt_filedialog_new_folder(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_filedialog_new(RT_FILEDIALOG_FOLDER);
}

/// @brief Set the dialog's titlebar text. No-op if `dialog` is NULL.
/// @param dialog FileDialog wrapper handle.
/// @param title Runtime title copied into lower-dialog storage.
void rt_filedialog_set_title(void *dialog, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *ctitle = rt_string_to_gui_cstr(title);
    vg_filedialog_set_title(data->dialog, ctitle);
    if (ctitle)
        free(ctitle);
}

/// @brief Set the directory the dialog opens in. Subsequent navigation may move elsewhere; the
/// returned selection is always an absolute path.
/// @param dialog FileDialog wrapper handle.
/// @param path Runtime initial directory; embedded NUL bytes are rejected.
void rt_filedialog_set_path(void *dialog, rt_string path) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *cpath = rt_string_to_cstr_no_nul(path);
    if (path && !cpath)
        return;
    vg_filedialog_set_initial_path(data->dialog, cpath);
    if (cpath)
        free(cpath);
}

/// @brief Replace all filename filters with a single (`name`, `pattern`) entry. `name` is the
/// human label shown in the dialog (e.g., "Image files"), `pattern` is the glob (e.g., "*.png").
/// @param dialog FileDialog wrapper handle.
/// @param name Optional runtime display name; defaults to `Files`.
/// @param pattern Required runtime glob pattern without embedded NUL bytes.
void rt_filedialog_set_filter(void *dialog, rt_string name, rt_string pattern) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *cname = name ? rt_string_to_gui_cstr(name) : NULL;
    char *cpattern = pattern ? rt_string_to_cstr_no_nul(pattern) : NULL;
    if (!pattern || (name && !cname) || !cpattern) {
        free(cname);
        free(cpattern);
        return;
    }
    vg_filedialog_clear_filters(data->dialog);
    vg_filedialog_add_filter(data->dialog, cname ? cname : "Files", cpattern);
    free(cname);
    free(cpattern);
}

/// @brief Append an additional (`name`, `pattern`) filter without clearing existing ones. The
/// dialog typically shows them in a dropdown; users can switch between filters at picking time.
/// @param dialog FileDialog wrapper handle.
/// @param name Optional runtime display name; defaults to `Files`.
/// @param pattern Required runtime glob pattern without embedded NUL bytes.
void rt_filedialog_add_filter(void *dialog, rt_string name, rt_string pattern) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *cname = name ? rt_string_to_gui_cstr(name) : NULL;
    char *cpattern = pattern ? rt_string_to_cstr_no_nul(pattern) : NULL;
    if (!pattern || (name && !cname) || !cpattern) {
        free(cname);
        free(cpattern);
        return;
    }
    vg_filedialog_add_filter(data->dialog, cname ? cname : "Files", cpattern);
    free(cname);
    free(cpattern);
}

/// @brief Pre-fill the filename field (Save dialogs primarily). User can edit before confirming.
/// @param dialog FileDialog wrapper handle.
/// @param name Runtime initial filename copied into lower-dialog storage.
void rt_filedialog_set_default_name(void *dialog, rt_string name) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *cname = rt_string_to_gui_cstr(name);
    vg_filedialog_set_filename(data->dialog, cname);
    if (cname)
        free(cname);
}

/// @brief Toggle multi-select. After `_show`, retrieve count via `_get_path_count` and individual
/// paths via `_get_path_at(i)`. Has no effect on Save/Folder dialogs.
/// @param dialog FileDialog wrapper handle.
/// @param multiple Non-zero to enable multiple selection.
void rt_filedialog_set_multiple(void *dialog, int64_t multiple) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    vg_filedialog_set_multi_select(data->dialog, multiple != 0);
}

/// @brief Control whether hidden directory entries are visible in this picker.
/// @details The retained cross-platform picker reloads the current directory immediately when the
///          value changes. Invalid, destroyed, or currently unavailable handles are ignored.
/// @param dialog Live FileDialog wrapper.
/// @param show_hidden Non-zero to include platform-hidden entries; zero to filter them out.
void rt_filedialog_set_show_hidden(void *dialog, int64_t show_hidden) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (data)
        vg_filedialog_set_show_hidden(data->dialog, show_hidden != 0);
}

/// @brief Configure save-mode overwrite confirmation.
/// @details This setting is ignored by open/folder modes and is implemented by the same retained
///          dialog on macOS, Windows, and Linux. Invalid handles are ignored.
/// @param dialog Live FileDialog wrapper.
/// @param confirm Non-zero to require confirmation before replacing an existing path.
void rt_filedialog_set_confirm_overwrite(void *dialog, int64_t confirm) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (data)
        vg_filedialog_set_confirm_overwrite(data->dialog, confirm != 0);
}

/// @brief Set the extension appended to extensionless save filenames.
/// @details The lower picker copies the value and accepts forms with or without a leading period.
///          Embedded NUL input is rejected atomically. The setting affects save mode only.
/// @param dialog Live FileDialog wrapper.
/// @param extension UTF-8 extension such as `.zia`; an empty string disables automatic append.
void rt_filedialog_set_default_extension(void *dialog, rt_string extension) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *value = rt_string_to_cstr_no_nul(extension);
    if (extension && !value)
        return;
    vg_filedialog_set_default_extension(data->dialog, value ? value : "");
    free(value);
}

/// @brief Add one quick-access path to the file-dialog bookmark sidebar.
/// @details The complete path is used as both the stable label and destination, avoiding locale-
///          dependent basename parsing at the runtime boundary. The lower platform adapter copies
///          the string. Embedded NUL input and invalid handles leave the bookmark list unchanged.
/// @param dialog Live FileDialog wrapper.
/// @param path Absolute UTF-8 directory path.
void rt_filedialog_add_bookmark(void *dialog, rt_string path) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return;
    char *value = rt_string_to_cstr_no_nul(path);
    if (!value)
        return;
    vg_filedialog_add_bookmark(data->dialog, value, value);
    free(value);
}

/// @brief Remove every quick-access bookmark from a file dialog.
/// @details This includes default bookmarks installed by construction. Call AddBookmark afterward
///          to build an application-specific list. Invalid or destroyed handles are ignored.
/// @param dialog Live FileDialog wrapper.
void rt_filedialog_clear_bookmarks(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (data)
        vg_filedialog_clear_bookmarks(data->dialog);
}

/// @brief Open a file dialog without entering a nested polling loop.
/// @details Presentation is registered with the owning app and completion is advanced by ordinary
///          App.Poll/RunFrame dispatch. Calling this while the same object is open traps with the
///          stable `GUI dialog is already open` error and preserves its current state. A failed
///          start records status Failed, an error string, and one completion edge.
/// @param dialog Live stateful FileDialog wrapper.
/// @return 1 when presentation started, otherwise 0 for an invalid handle/app or setup failure.
int64_t rt_filedialog_show_async(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return 0;
    if (data->status == RT_GUI_DIALOG_STATUS_OPEN || vg_dialog_is_open(&data->dialog->base)) {
        rt_trap("GUI dialog is already open");
        return 0;
    }

    rt_gui_app_t *app =
        rt_gui_is_app_handle(data->owner_app) ? data->owner_app : rt_filedialog_app();
    rt_filedialog_clear_selected_paths(data);
    data->result = 0;
    data->error = "";
    if (!app || !app->window || !app->root) {
        data->status = RT_GUI_DIALOG_STATUS_FAILED;
        data->error = "No active GUI application is available";
        rt_filedialog_record_completion(data);
        return 0;
    }

    data->owner_app = app;
    data->status = RT_GUI_DIALOG_STATUS_OPEN;
    if (!rt_filedialog_prepare_modal(app, data->dialog)) {
        vg_dialog_hide(&data->dialog->base);
        data->status = RT_GUI_DIALOG_STATUS_FAILED;
        data->error = "The requested file dialog operation is not available";
        rt_filedialog_record_completion(data);
        return 0;
    }
    return 1;
}

/// @brief Show the dialog modally. Blocks the caller until the user dismisses it. Returns 1 if
/// the user confirmed at least one selection, 0 if cancelled or if no active GUI window exists.
/// Replaces any prior selection snapshot (calling `_show` twice on the same handle is allowed).
/// @param dialog Live stateful FileDialog wrapper.
/// @return `1` after an accepted selection; otherwise `0`.
int64_t rt_filedialog_show(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data || !rt_filedialog_show_async(dialog))
        return 0;
    rt_gui_app_t *app = data->owner_app;
    rt_filedialog_run_modal(app, data->dialog);
    if (data->status == RT_GUI_DIALOG_STATUS_OPEN)
        vg_dialog_close(&data->dialog->base, VG_DIALOG_RESULT_CANCEL);
    return data->status == RT_GUI_DIALOG_STATUS_ACCEPTED ? 1 : 0;
}

/// @brief Query whether a stateful file dialog is currently presented.
/// @param dialog FileDialog wrapper; invalid or destroyed handles are treated as closed.
/// @return 1 only while the lower dialog is open and status is Open, otherwise 0.
int64_t rt_filedialog_is_open(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    return data && data->status == RT_GUI_DIALOG_STATUS_OPEN &&
                   vg_dialog_is_open(&data->dialog->base)
               ? 1
               : 0;
}

/// @brief Consume one file-dialog completion edge.
/// @details Selection, cancellation, and start/runtime failure each produce exactly one edge. The
///          status and result data remain readable after consumption and by multiple observers.
/// @param dialog Registered FileDialog wrapper; a missing/destroyed wrapper has no edge.
/// @return 1 when an unread completion exists, otherwise 0.
int64_t rt_filedialog_was_completed(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    if (!data || data->completed_edges == 0)
        return 0;
    data->completed_edges--;
    return 1;
}

/// @brief Return the current file-dialog state-machine status.
/// @param dialog Registered FileDialog wrapper.
/// @return One of `RT_GUI_DIALOG_STATUS_*`; invalid/destroyed handles report Failed.
int64_t rt_filedialog_get_status(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    return data ? data->status : RT_GUI_DIALOG_STATUS_FAILED;
}

/// @brief Return a stable diagnostic for the most recent file-dialog failure.
/// @details Successful, idle, open, and cancelled states return an empty string. The returned
///          runtime string is independently managed and remains valid after the dialog is reused.
/// @param dialog Registered FileDialog wrapper.
/// @return Owned runtime error string, or empty for no recorded error/invalid handles.
rt_string rt_filedialog_get_error(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    const char *error = data && data->error ? data->error : "";
    return rt_gui_string_from_cstr_bounded(error);
}

/// @brief Snapshot every accepted path as an owned runtime sequence.
/// @details The wrapper's native path snapshot is copied into managed strings; callers may retain
///          or mutate the sequence independently of later dialog reuse or destruction.
/// @param dialog Registered FileDialog wrapper.
/// @return Owned `Seq[str]`, empty before acceptance/cancel/failure or for invalid handles.
void *rt_filedialog_get_paths(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    void *paths = rt_seq_new_owned();
    if (!paths)
        return NULL;
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    if (!data)
        return paths;
    for (size_t index = 0; index < data->selected_count; index++) {
        const char *path = data->selected_paths[index] ? data->selected_paths[index] : "";
        rt_string value = rt_gui_string_from_cstr_bounded(path);
        if (!value)
            continue;
        rt_seq_push(paths, value);
        rt_string_unref(value);
    }
    return paths;
}

/// @brief Return the first selected path from the most recent `_show`. Empty if no selection.
/// @param dialog Live FileDialog wrapper.
/// @return Owned first selected path, or an empty runtime string when absent or invalid.
rt_string rt_filedialog_get_path(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return rt_str_empty();
    if (data->selected_paths && data->selected_count > 0) {
        return rt_gui_string_from_cstr_bounded(data->selected_paths[0]);
    }
    return rt_str_empty();
}

/// @brief Number of paths selected by the most recent `_show` (0 if cancelled or pre-show).
/// @param dialog Live FileDialog wrapper.
/// @return Selected path count saturated at `INT64_MAX`, or zero for invalid input.
int64_t rt_filedialog_get_path_count(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return 0;
    if (data->selected_count > (size_t)INT64_MAX)
        return INT64_MAX;
    return rt_gui_saturating_size_to_i64(data->selected_count);
}

/// @brief Return the i-th selected path (0-based) from a multi-select dialog. Empty if `index`
/// is out of range. Use `_get_path_count` first to bound the iteration.
/// @param dialog Live FileDialog wrapper.
/// @param index Zero-based selected-path index.
/// @return Owned selected path, or an empty runtime string for invalid input.
rt_string rt_filedialog_get_path_at(void *dialog, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_data_checked(dialog);
    if (!data)
        return rt_str_empty();
    if (data->selected_paths && index >= 0 && (uintmax_t)index <= (uintmax_t)SIZE_MAX) {
        size_t idx = (size_t)index;
        if (idx < data->selected_count) {
            return rt_gui_string_from_cstr_bounded(data->selected_paths[idx]);
        }
    }
    return rt_str_empty();
}

/// @brief Manually free dialog resources (paths, backend handle). The GC finalizer also calls
/// this, so explicit destruction is optional — useful for early cleanup before GC catches up.
/// @param dialog Registered FileDialog wrapper to destroy.
void rt_filedialog_destroy(void *dialog) {
    RT_ASSERT_MAIN_THREAD();
    rt_filedialog_data_t *data = rt_filedialog_wrapper_checked(dialog);
    if (!data)
        return;
    rt_filedialog_dispose(data);
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: returns empty string — file open dialog requires graphics.
/// @param title Ignored dialog title.
/// @param default_path Ignored initial directory.
/// @param filter Ignored glob filter.
/// @return Empty runtime string.
rt_string rt_filedialog_open(rt_string title, rt_string default_path, rt_string filter) {
    (void)title;
    (void)default_path;
    (void)filter;
    return rt_str_empty();
}

/// @brief Stub: returns empty string — multi-select open dialog requires graphics.
/// @param title Ignored dialog title.
/// @param default_path Ignored initial directory.
/// @param filter Ignored glob filter.
/// @return Empty runtime string.
rt_string rt_filedialog_open_multiple(rt_string title, rt_string default_path, rt_string filter) {
    (void)title;
    (void)default_path;
    (void)filter;
    return rt_str_empty();
}

/// @brief Stub: returns empty string — save dialog requires graphics.
/// @param title Ignored dialog title.
/// @param default_path Ignored initial directory.
/// @param filter Ignored glob filter.
/// @param default_name Ignored initial filename.
/// @return Empty runtime string.
rt_string rt_filedialog_save(rt_string title,
                             rt_string default_path,
                             rt_string filter,
                             rt_string default_name) {
    (void)title;
    (void)default_path;
    (void)filter;
    (void)default_name;
    return rt_str_empty();
}

/// @brief Stub: returns empty string — folder picker requires graphics.
/// @param title Ignored dialog title.
/// @param default_path Ignored initial directory.
/// @return Empty runtime string.
rt_string rt_filedialog_select_folder(rt_string title, rt_string default_path) {
    (void)title;
    (void)default_path;
    return rt_str_empty();
}

/// @brief Stub: returns NULL — file dialog object requires graphics.
/// @param type Ignored dialog type.
/// @return Always `NULL`.
void *rt_filedialog_new(int64_t type) {
    (void)type;
    return NULL;
}

/// @brief Stub: returns NULL — open-file dialog requires graphics.
/// @return Always `NULL`.
void *rt_filedialog_new_open(void) {
    return NULL;
}

/// @brief Stub: returns NULL — save-file dialog requires graphics.
/// @return Always `NULL`.
void *rt_filedialog_new_save(void) {
    return NULL;
}

/// @brief Stub: returns NULL — folder-picker dialog requires graphics.
/// @return Always `NULL`.
void *rt_filedialog_new_folder(void) {
    return NULL;
}

/// @brief Stub: `FileDialog.SetTitle` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param title Ignored title.
void rt_filedialog_set_title(void *dialog, rt_string title) {
    (void)dialog;
    (void)title;
}

/// @brief Stub: `FileDialog.SetPath` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param path Ignored initial directory.
void rt_filedialog_set_path(void *dialog, rt_string path) {
    (void)dialog;
    (void)path;
}

/// @brief Stub: `FileDialog.SetFilter` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param name Ignored filter name.
/// @param pattern Ignored glob pattern.
void rt_filedialog_set_filter(void *dialog, rt_string name, rt_string pattern) {
    (void)dialog;
    (void)name;
    (void)pattern;
}

/// @brief Stub: `FileDialog.AddFilter` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param name Ignored filter name.
/// @param pattern Ignored glob pattern.
void rt_filedialog_add_filter(void *dialog, rt_string name, rt_string pattern) {
    (void)dialog;
    (void)name;
    (void)pattern;
}

/// @brief Stub: `FileDialog.SetDefaultName` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param name Ignored filename.
void rt_filedialog_set_default_name(void *dialog, rt_string name) {
    (void)dialog;
    (void)name;
}

/// @brief Stub: `FileDialog.SetMultiple` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param multiple Ignored multi-select flag.
void rt_filedialog_set_multiple(void *dialog, int64_t multiple) {
    (void)dialog;
    (void)multiple;
}

/// @brief Stub: hidden-file configuration is unavailable without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param show_hidden Ignored visibility flag.
void rt_filedialog_set_show_hidden(void *dialog, int64_t show_hidden) {
    (void)dialog;
    (void)show_hidden;
}

/// @brief Stub: overwrite confirmation is unavailable without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param confirm Ignored confirmation flag.
void rt_filedialog_set_confirm_overwrite(void *dialog, int64_t confirm) {
    (void)dialog;
    (void)confirm;
}

/// @brief Stub: default-extension configuration is unavailable without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param extension Ignored extension.
void rt_filedialog_set_default_extension(void *dialog, rt_string extension) {
    (void)dialog;
    (void)extension;
}

/// @brief Stub: bookmark configuration is unavailable without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param path Ignored bookmark path.
void rt_filedialog_add_bookmark(void *dialog, rt_string path) {
    (void)dialog;
    (void)path;
}

/// @brief Stub: no bookmark state exists without graphics.
/// @param dialog Ignored FileDialog handle.
void rt_filedialog_clear_bookmarks(void *dialog) {
    (void)dialog;
}

/// @brief Stub: asynchronous presentation cannot start without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Always `0`.
int64_t rt_filedialog_show_async(void *dialog) {
    (void)dialog;
    return 0;
}

/// @brief Stub: returns 0 — dialog cannot be shown without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Always `0`.
int64_t rt_filedialog_show(void *dialog) {
    (void)dialog;
    return 0;
}

/// @brief Stub: no file dialog is open without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Always `0`.
int64_t rt_filedialog_is_open(void *dialog) {
    (void)dialog;
    return 0;
}

/// @brief Stub: no stateful wrapper can complete without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Always `0`.
int64_t rt_filedialog_was_completed(void *dialog) {
    (void)dialog;
    return 0;
}

/// @brief Stub: absent file-dialog handles report Failed.
/// @param dialog Ignored FileDialog handle.
/// @return `RT_GUI_DIALOG_STATUS_FAILED`.
int64_t rt_filedialog_get_status(void *dialog) {
    (void)dialog;
    return RT_GUI_DIALOG_STATUS_FAILED;
}

/// @brief Stub: return the stable graphics-disabled capability diagnostic.
/// @param dialog Ignored FileDialog handle.
/// @return Runtime string describing unavailable GUI support.
rt_string rt_filedialog_get_error(void *dialog) {
    (void)dialog;
    return rt_const_cstr("GUI support is not available in this build");
}

/// @brief Stub: return an empty owned path sequence without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return New empty owned sequence, or `NULL` on allocation failure.
void *rt_filedialog_get_paths(void *dialog) {
    (void)dialog;
    return rt_seq_new_owned();
}

/// @brief Stub: returns empty string — no path available without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Empty runtime string.
rt_string rt_filedialog_get_path(void *dialog) {
    (void)dialog;
    return rt_str_empty();
}

/// @brief Stub: returns 0 — no paths available without graphics.
/// @param dialog Ignored FileDialog handle.
/// @return Always `0`.
int64_t rt_filedialog_get_path_count(void *dialog) {
    (void)dialog;
    return 0;
}

/// @brief Stub: returns empty string — no path at index without graphics.
/// @param dialog Ignored FileDialog handle.
/// @param index Ignored path index.
/// @return Empty runtime string.
rt_string rt_filedialog_get_path_at(void *dialog, int64_t index) {
    (void)dialog;
    (void)index;
    return rt_str_empty();
}

/// @brief Stub: `FileDialog.Destroy` is a no-op without graphics.
/// @param dialog Ignored FileDialog handle.
void rt_filedialog_destroy(void *dialog) {
    (void)dialog;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
