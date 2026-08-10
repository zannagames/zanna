//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/rt_gui_features.c
// Purpose: Runtime bindings for advanced ZannaGUI feature widgets: CommandPalette
//   (fuzzy-searchable command list), Tooltip (hover annotation), Toast
//   (transient notification), Breadcrumb (navigation path), Minimap (scaled
//   document overview), and Drag & Drop. Each widget type wraps the corresponding
//   vg_* C widget with a GC-safe state struct that captures selection/event data
//   for polling by Zia code.
//
// Key invariants:
//   - rt_commandpalette_on_execute callback fires synchronously inside the vg
//     event loop; it duplicates the selected command id for later polling.
//   - Toast messages are transient: they auto-dismiss after the configured
//     duration; no explicit dismiss call is required.
//   - Minimap content is updated via rt_minimap_set_content and rendered at
//     reduced scale; the pixel buffer is owned by the vg_minimap_t widget.
//   - Drag & Drop uses VGFX drag events; drag data is stored as C strings
//     allocated by the platform and freed after the drop handler returns.
//   - Widget constructors accept parent handles through rt_gui_widget_parent_from_handle.
//
// Ownership/Lifetime:
//   - Wrapper state structs (e.g. rt_commandpalette_data_t) are allocated via
//     rt_obj_new_i64 (GC heap); their embedded vg_* pointers are manually freed
//     in the corresponding destroy functions.
//   - selected_command is duplicated on selection and freed on next selection or
//     at destroy time.
//
// Links: src/runtime/graphics/rt_gui_internal.h (internal types/globals),
//        src/lib/gui/include/vg.h (ZannaGUI C API),
//        src/runtime/rt_platform.h (platform detection helpers)
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_features.c
/// @brief Implements advanced GUI bindings for palettes, tooltips, toasts, and drag-and-drop.
///
/// @details
/// The graphics-enabled path manages GC-backed wrapper state, event snapshots,
/// scheduler timing, and runtime-string conversion for feature widgets.
/// Graphics-disabled definitions preserve the exported ABI with deterministic
/// inert behavior.

#include "rt_gui_internal.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Duplicate a feature-widget string with malloc ownership.
/// @details Command IDs, toast action labels, and dropped-file paths are stored
///          as heap strings released with `free`; this helper avoids relying on
///          platform-specific `strdup` declarations.
/// @param text Source string to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_gui_features_strdup(const char *text) {
    return rt_gui_strdup_bounded(text);
}

/// @brief Return the app scheduler's monotonic timer clock in milliseconds.
/// @details Once an app has rendered, this uses the same real-or-deterministic clock that drives
///          tooltip, notification, and dialog scheduling. Before the first frame, invalid clocks
///          fall back to the process monotonic clock. The conversion saturates and never exposes a
///          negative or non-finite timestamp.
/// @param app App whose scheduler clock is requested; NULL uses process time.
/// @return Monotonic millisecond timestamp suitable for lower GUI timer records.
static uint64_t rt_gui_feature_now_ms(rt_gui_app_t *app) {
    if (app && isfinite(app->scheduler_clock_ms) && app->scheduler_clock_ms > 0.0) {
        return app->scheduler_clock_ms >= (double)UINT64_MAX ? UINT64_MAX
                                                             : (uint64_t)app->scheduler_clock_ms;
    }
    return rt_gui_now_ms();
}

//=============================================================================
// Phase 6: CommandPalette
//=============================================================================

// CommandPalette state tracking
#define RT_COMMANDPALETTE_DATA_MAGIC UINT64_C(0x5254434D4450414C)

typedef struct {
    uint64_t magic;
    rt_gui_app_t *app;
    vg_commandpalette_t *palette;
    char *selected_command;
    int64_t was_selected;
} rt_commandpalette_data_t;

static rt_commandpalette_data_t **s_commandpalette_wrappers = NULL;
static size_t s_commandpalette_wrapper_count = 0;
static size_t s_commandpalette_wrapper_cap = 0;

/// @brief Record a wrapper in the global command-palette registry (idempotent).
/// @details The registry is the source of truth for handle validation: a checked
///          cast only trusts an opaque `void*` once it is found here (then verifies
///          the magic tag), guarding against forged/freed handles. Capacity doubles from 8.
/// @param data Command-palette wrapper to register.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int rt_commandpalette_register_wrapper(rt_commandpalette_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_commandpalette_wrapper_count; i++) {
        if (s_commandpalette_wrappers[i] == data)
            return 1;
    }
    if (s_commandpalette_wrapper_count >= s_commandpalette_wrapper_cap) {
        size_t new_cap = 0;
        if (!rt_gui_next_collection_capacity(s_commandpalette_wrapper_cap,
                                             s_commandpalette_wrapper_count + 1u,
                                             8u,
                                             sizeof(rt_commandpalette_data_t *),
                                             &new_cap))
            return 0;
        void *p = realloc(s_commandpalette_wrappers, new_cap * sizeof(rt_commandpalette_data_t *));
        if (!p)
            return 0;
        s_commandpalette_wrappers = (rt_commandpalette_data_t **)p;
        s_commandpalette_wrapper_cap = new_cap;
    }
    s_commandpalette_wrappers[s_commandpalette_wrapper_count++] = data;
    return 1;
}

/// @brief Remove a wrapper from the command-palette registry, compacting the array. No-op if
/// absent.
/// @param data Wrapper to remove.
static void rt_commandpalette_unregister_wrapper(rt_commandpalette_data_t *data) {
    if (!data)
        return;
    for (size_t i = 0; i < s_commandpalette_wrapper_count; i++) {
        if (s_commandpalette_wrappers[i] != data)
            continue;
        memmove(&s_commandpalette_wrappers[i],
                &s_commandpalette_wrappers[i + 1],
                (s_commandpalette_wrapper_count - i - 1) * sizeof(*s_commandpalette_wrappers));
        s_commandpalette_wrapper_count--;
        return;
    }
}

/// @brief True if @p data is a currently-registered wrapper; backs handle validation.
/// @param data Candidate wrapper pointer.
/// @return `1` when the exact pointer is registered; otherwise `0`.
static int rt_commandpalette_wrapper_is_registered(const rt_commandpalette_data_t *data) {
    if (!data)
        return 0;
    for (size_t i = 0; i < s_commandpalette_wrapper_count; i++) {
        if (s_commandpalette_wrappers[i] == data)
            return 1;
    }
    return 0;
}

/// @brief Free the cached selected-command string and reset the selection flag,
///        so a fresh palette session starts with no pending choice.
/// @param data Command-palette wrapper whose pending selection is cleared.
static void rt_commandpalette_clear_selection(rt_commandpalette_data_t *data) {
    if (!data)
        return;
    free(data->selected_command);
    data->selected_command = NULL;
    data->was_selected = 0;
}

/// @brief Authenticate a CommandPalette handle via its magic tag (NULL if not).
/// @param palette Candidate runtime CommandPalette handle.
/// @return Validated registered wrapper, or `NULL` for invalid input.
static rt_commandpalette_data_t *rt_commandpalette_checked(void *palette) {
    rt_commandpalette_data_t *data = (rt_commandpalette_data_t *)palette;
    return rt_commandpalette_wrapper_is_registered(data) &&
                   data->magic == RT_COMMANDPALETTE_DATA_MAGIC
               ? data
               : NULL;
}

/// @brief Release the command palette widget, unregister it from the app, and zero all fields.
/// @param data Registered wrapper to dispose; `NULL` is accepted.
static void rt_commandpalette_dispose(rt_commandpalette_data_t *data) {
    if (!data)
        return;
    if (data->palette) {
        rt_gui_unregister_command_palette(data->app, data->palette);
        if (data->palette->base.user_data == data)
            data->palette->base.user_data = NULL;
        vg_commandpalette_destroy(data->palette);
        data->palette = NULL;
    }
    free(data->selected_command);
    data->selected_command = NULL;
    data->was_selected = 0;
    data->app = NULL;
    data->magic = 0;
    rt_commandpalette_unregister_wrapper(data);
}

/// @brief GC finalizer — delegates to `rt_commandpalette_dispose`.
/// @param palette Managed CommandPalette wrapper being finalized.
static void rt_commandpalette_finalize(void *palette) {
    rt_commandpalette_dispose((rt_commandpalette_data_t *)palette);
}

/// @brief Callback fired by the command palette when the user activates a command.
///
/// Captures the selected command's ID into the per-palette data
/// struct so the next `rt_commandpalette_get_selected_id` call
/// returns it. Edge-triggered: each invocation overwrites the
/// previous selection.
/// @param palette Lower-toolkit palette that emitted the event.
/// @param cmd Activated command record.
/// @param user_data Registered runtime wrapper receiving the event snapshot.
static void rt_commandpalette_on_execute(vg_commandpalette_t *palette,
                                         vg_command_t *cmd,
                                         void *user_data) {
    rt_commandpalette_data_t *data = (rt_commandpalette_data_t *)user_data;
    if (rt_commandpalette_wrapper_is_registered(data) &&
        data->magic == RT_COMMANDPALETTE_DATA_MAGIC && cmd && cmd->id) {
        char *copy = rt_gui_features_strdup(cmd->id);
        if (!copy)
            return;
        free(data->selected_command);
        data->selected_command = copy;
        data->was_selected = 1;
    }
    (void)palette;
}

/// @brief Create a Ctrl+Shift+P-style command palette overlay.
///
/// The palette renders as a modal overlay above the active app's
/// window and supports fuzzy-search across registered commands.
/// Returns a small wrapper struct (`rt_commandpalette_data_t`)
/// rather than the bare `vg_commandpalette_t*` so callers can
/// poll the most recently activated command via
/// `rt_commandpalette_get_selected_id`.
/// @param parent App or widget handle used to resolve the owning application.
/// @return New managed CommandPalette wrapper, or `NULL` on invalid context or allocation failure.
void *rt_commandpalette_new(void *parent) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *parent_widget = parent ? rt_gui_widget_parent_from_handle(parent) : NULL;
    if (parent && !rt_gui_app_from_handle(parent) && !parent_widget)
        return NULL;
    rt_gui_app_t *app = rt_gui_app_from_handle(parent);
    if (!app && parent_widget)
        app = rt_gui_app_from_widget(parent_widget);
    if (!app)
        app = s_current_app;
    if (!app)
        return NULL;
    vg_commandpalette_t *palette = vg_commandpalette_create();
    if (!palette)
        return NULL;

    rt_commandpalette_data_t *data =
        (rt_commandpalette_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_commandpalette_data_t));
    if (!data) {
        vg_commandpalette_destroy(palette);
        return NULL;
    }
    data->app = app;
    data->magic = RT_COMMANDPALETTE_DATA_MAGIC;
    data->palette = palette;
    data->selected_command = NULL;
    data->was_selected = 0;
    palette->base.user_data = data;
    if (!rt_commandpalette_register_wrapper(data)) {
        rt_commandpalette_dispose(data);
        return NULL;
    }
    rt_obj_set_finalizer(data, rt_commandpalette_finalize);

    vg_commandpalette_set_callbacks(palette, rt_commandpalette_on_execute, NULL, data);
    if (app && app->default_font)
        vg_commandpalette_set_font(palette, app->default_font, rt_gui_app_effective_font_size(app));
    rt_gui_register_command_palette(app, palette);

    return data;
}

/// @brief Release resources and destroy the commandpalette.
/// @param palette CommandPalette wrapper to destroy; invalid handles are ignored.
void rt_commandpalette_destroy(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data)
        return;
    rt_commandpalette_dispose(data);
}

/// @brief Build a heap "[category] label" display string for a palette entry.
/// @details Length math is overflow-checked before the malloc. A NULL/empty
///          category yields NULL (caller falls back to the bare label); a NULL
///          label is treated as empty.
/// @param category Category text placed inside square brackets.
/// @param label Command label appended after the category.
/// @return Newly allocated string the caller must free, or NULL on bad input/OOM.
static char *rt_commandpalette_format_display_label(const char *category, const char *label) {
    if (!category || !category[0])
        return NULL;
    size_t category_len = strlen(category);
    size_t label_len = label ? strlen(label) : 0;
    if (category_len > SIZE_MAX - label_len || category_len + label_len > SIZE_MAX - 4)
        return NULL;
    size_t len = category_len + label_len + 4;
    char *display = (char *)malloc(len);
    if (!display)
        return NULL;
    snprintf(display, len, "[%s] %s", category, label ? label : "");
    return display;
}

/// @brief Register a command in the palette's fuzzy-searchable list.
/// @param palette CommandPalette wrapper handle.
/// @param id Runtime command identifier; embedded NUL bytes are rejected.
/// @param label Runtime display label.
/// @param category Optional runtime category prepended to the display label.
void rt_commandpalette_add_command(void *palette,
                                   rt_string id,
                                   rt_string label,
                                   rt_string category) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    if (rt_string_contains_nul(id))
        return;
    char *cid = rt_string_to_cstr(id);
    char *clabel = rt_string_to_gui_cstr(label);
    char *ccat = rt_string_to_gui_cstr(category);
    if (!cid || !clabel || !ccat) {
        free(ccat);
        free(cid);
        free(clabel);
        return;
    }

    // Prepend category to label if non-empty (e.g. "[File] Open")
    char *display_alloc = rt_commandpalette_format_display_label(ccat, clabel);
    const char *display = display_alloc ? display_alloc : clabel;

    vg_commandpalette_add_command(data->palette, cid, display, NULL, NULL, NULL);

    free(display_alloc);
    free(ccat);
    free(cid);
    free(clabel);
}

/// @brief Register a command in the palette with an associated keyboard shortcut.
///
/// As `rt_commandpalette_add_command` but the shortcut text (e.g.
/// `"Ctrl+S"`) is shown next to the entry. The shortcut is purely
/// informational — wiring it up to actually fire is up to the
/// caller's keyboard handler.
/// @param palette CommandPalette wrapper handle.
/// @param id Runtime command identifier; embedded NUL bytes are rejected.
/// @param label Runtime display label.
/// @param category Optional runtime category.
/// @param shortcut Runtime shortcut text displayed beside the command.
void rt_commandpalette_add_command_with_shortcut(
    void *palette, rt_string id, rt_string label, rt_string category, rt_string shortcut) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    if (rt_string_contains_nul(id))
        return;
    char *cid = rt_string_to_cstr(id);
    char *clabel = rt_string_to_gui_cstr(label);
    char *cshort = rt_string_to_gui_cstr(shortcut);
    char *ccat = rt_string_to_gui_cstr(category);
    if (!cid || !clabel || !cshort || !ccat) {
        free(ccat);
        free(cshort);
        free(cid);
        free(clabel);
        return;
    }

    // Prepend category to label if non-empty
    char *display_alloc = rt_commandpalette_format_display_label(ccat, clabel);
    const char *display = display_alloc ? display_alloc : clabel;

    vg_commandpalette_add_command(data->palette, cid, display, cshort, NULL, NULL);

    free(display_alloc);
    free(ccat);
    free(cid);
    free(clabel);
    free(cshort);
}

/// @brief Remove a command from the palette by its ID.
/// @param palette CommandPalette wrapper handle.
/// @param id Runtime command identifier; embedded NUL bytes are rejected.
void rt_commandpalette_remove_command(void *palette, rt_string id) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    if (rt_string_contains_nul(id))
        return;
    char *cid = rt_string_to_cstr(id);
    if (!cid)
        return;
    vg_commandpalette_remove_command(data->palette, cid);
    if (data->selected_command && strcmp(data->selected_command, cid) == 0)
        rt_commandpalette_clear_selection(data);
    free(cid);
}

/// @brief Remove all entries from the commandpalette.
/// @param palette CommandPalette wrapper handle.
void rt_commandpalette_clear(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    vg_commandpalette_clear(data->palette);
    rt_commandpalette_clear_selection(data);
}

/// @brief Show the commandpalette.
/// @param palette CommandPalette wrapper handle.
void rt_commandpalette_show(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    rt_commandpalette_clear_selection(data);
    vg_commandpalette_show(data->palette);
}

/// @brief Hide the commandpalette.
/// @param palette CommandPalette wrapper handle.
void rt_commandpalette_hide(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    vg_commandpalette_hide(data->palette);
}

/// @brief Check whether the command palette is currently visible.
/// @param palette CommandPalette wrapper handle.
/// @return `1` when its overlay is visible; otherwise `0`.
int64_t rt_commandpalette_is_visible(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return 0;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return 0;
    return data->palette->base.visible ? 1 : 0;
}

/// @brief Set the placeholder of the commandpalette.
/// @param palette CommandPalette wrapper handle.
/// @param text Runtime placeholder text copied into palette storage.
void rt_commandpalette_set_placeholder(void *palette, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_commandpalette_set_placeholder(data->palette, ctext);
    if (ctext)
        free(ctext);
}

/// @brief Get the selected command of the commandpalette.
/// @param palette CommandPalette wrapper handle.
/// @return Owned selected command identifier, or an empty runtime string when absent.
rt_string rt_commandpalette_get_selected_command(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return rt_str_empty();
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (data && data->selected_command) {
        return rt_string_from_bytes(data->selected_command, strlen(data->selected_command));
    }
    return rt_str_empty();
}

/// @brief Check if a command was selected since the last call (edge-triggered, resets).
/// @param palette CommandPalette wrapper handle.
/// @return `1` once after command activation; otherwise `0`.
int64_t rt_commandpalette_was_command_selected(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return 0;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data)
        return 0;
    int64_t result = data->was_selected;
    data->was_selected = 0; // Reset after checking
    return result;
}

/// @brief `CommandPalette.GetQuery` — current live query text.
/// @param palette CommandPalette wrapper handle.
/// @return Owned query text, or an empty runtime string for invalid input.
rt_string rt_commandpalette_get_query(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return rt_str_empty();
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return rt_str_empty();
    const char *q = vg_commandpalette_get_query(data->palette);
    return q ? rt_string_from_bytes(q, strlen(q)) : rt_str_empty();
}

/// @brief `CommandPalette.GetQueryGeneration` — bumped on every query change.
/// @param palette CommandPalette wrapper handle.
/// @return Current query generation, or zero for invalid input.
int64_t rt_commandpalette_get_query_generation(void *palette) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return 0;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return 0;
    return rt_gui_saturating_u64_to_i64(vg_commandpalette_get_query_generation(data->palette));
}

/// @brief `CommandPalette.SetQuery` — prefill the query and re-filter.
/// @param palette CommandPalette wrapper handle.
/// @param text Runtime query text copied into palette storage.
void rt_commandpalette_set_query(void *palette, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_commandpalette_set_query(data->palette, ctext);
    if (ctext)
        free(ctext);
}

/// @brief `CommandPalette.SetClientFiltered` — toggle application-driven filtering.
/// @param palette CommandPalette wrapper handle.
/// @param enabled Non-zero to let the application supply filtered results.
void rt_commandpalette_set_client_filtered(void *palette, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    if (!palette)
        return;
    rt_commandpalette_data_t *data = rt_commandpalette_checked(palette);
    if (!data || !data->palette)
        return;
    vg_commandpalette_set_client_filtered(data->palette, enabled != 0);
}

//=============================================================================
// Phase 7: Tooltip Implementation
//=============================================================================

/// @brief Allocate "title\nbody" with overflow checks.
/// @param title Optional title text.
/// @param body Optional body text.
/// @return Newly allocated joined string, or `NULL` on overflow or allocation failure.
static char *rt_gui_join_title_body(const char *title, const char *body) {
    const char *t = title ? title : "";
    const char *b = body ? body : "";
    size_t title_len = strlen(t);
    size_t body_len = strlen(b);
    if (body_len > SIZE_MAX - 2 || title_len > SIZE_MAX - body_len - 2)
        return NULL;
    size_t needed = title_len + body_len + 2;
    char *combined = (char *)malloc(needed);
    if (!combined)
        return NULL;
    snprintf(combined, needed, "%s\n%s", t, b);
    return combined;
}

/// @brief Show the tooltip.
/// @param text Runtime tooltip text.
/// @param x Screen-space horizontal coordinate.
/// @param y Screen-space vertical coordinate.
void rt_tooltip_show(rt_string text, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (!app)
        return;
    char *ctext = rt_string_to_gui_cstr(text);

    // Create tooltip if needed
    if (!app->manual_tooltip) {
        app->manual_tooltip = vg_tooltip_create();
    }

    if (app->manual_tooltip && ctext) {
        if (app->default_font) {
            app->manual_tooltip->font = app->default_font;
            app->manual_tooltip->font_size = rt_gui_app_effective_font_size(app);
        }
        vg_tooltip_set_timing(app->manual_tooltip, app->manual_tooltip_delay_ms, 100, 0);
        vg_tooltip_set_text(app->manual_tooltip, ctext);
        vg_tooltip_show_at(app->manual_tooltip,
                           rt_gui_clamp_i64_to_i32(x, INT32_MIN, INT32_MAX),
                           rt_gui_clamp_i64_to_i32(y, INT32_MIN, INT32_MAX));
    }

    if (ctext)
        free(ctext);
}

/// @brief Show a rich tooltip with a title and body at a specific screen position.
/// @param title Runtime title text.
/// @param body Runtime body text.
/// @param x Screen-space horizontal coordinate.
/// @param y Screen-space vertical coordinate.
void rt_tooltip_show_rich(rt_string title, rt_string body, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (!app)
        return;
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cbody = rt_string_to_gui_cstr(body);

    // Create tooltip if needed
    if (!app->manual_tooltip) {
        app->manual_tooltip = vg_tooltip_create();
    }

    if (app->manual_tooltip) {
        // Combines title and body as plain text separated by newline.
        // Rich formatting (bold, colors) would require vg_tooltip_t enhancements.
        char *combined = rt_gui_join_title_body(ctitle, cbody);
        if (!combined)
            goto tooltip_rich_done;
        if (app->default_font) {
            app->manual_tooltip->font = app->default_font;
            app->manual_tooltip->font_size = rt_gui_app_effective_font_size(app);
        }
        vg_tooltip_set_timing(app->manual_tooltip, app->manual_tooltip_delay_ms, 100, 0);
        vg_tooltip_set_text(app->manual_tooltip, combined);
        free(combined);
        vg_tooltip_show_at(app->manual_tooltip,
                           rt_gui_clamp_i64_to_i32(x, INT32_MIN, INT32_MAX),
                           rt_gui_clamp_i64_to_i32(y, INT32_MIN, INT32_MAX));
    }

tooltip_rich_done:
    if (ctitle)
        free(ctitle);
    if (cbody)
        free(cbody);
}

/// @brief Hide the tooltip.
void rt_tooltip_hide(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (app && app->manual_tooltip) {
        vg_tooltip_hide(app->manual_tooltip);
    }
}

/// @brief Set the delay of the tooltip.
/// @param delay_ms Non-negative show delay in milliseconds, clamped to `UINT32_MAX`.
void rt_tooltip_set_delay(int64_t delay_ms) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (!app)
        return;
    if (delay_ms < 0)
        delay_ms = 0;
    if (delay_ms > UINT32_MAX)
        delay_ms = UINT32_MAX;
    app->manual_tooltip_delay_ms = (uint32_t)delay_ms;
    if (app->manual_tooltip) {
        vg_tooltip_set_timing(app->manual_tooltip, app->manual_tooltip_delay_ms, 100, 0);
    }
    if (vg_tooltip_manager_get()->active_tooltip) {
        vg_tooltip_set_timing(
            vg_tooltip_manager_get()->active_tooltip, app->manual_tooltip_delay_ms, 100, 0);
    }
}

/// @brief Set the tooltip of the widget.
/// @param widget Live widget handle.
/// @param text Runtime tooltip text copied into widget storage.
void rt_widget_set_tooltip(void *widget, rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    if (!rt_gui_is_widget_handle(widget))
        return;
    char *ctext = rt_string_to_gui_cstr(text);
    vg_widget_set_tooltip_text((vg_widget_t *)widget, ctext);
    if (ctext)
        free(ctext);
}

/// @brief Attach a rich tooltip (title + body) to a widget for hover display.
/// @param widget Live widget handle.
/// @param title Runtime title text.
/// @param body Runtime body text.
void rt_widget_set_tooltip_rich(void *widget, rt_string title, rt_string body) {
    RT_ASSERT_MAIN_THREAD();
    if (!rt_gui_is_widget_handle(widget))
        return;
    // Combines title and body as plain text. Rich formatting would require
    // vg_tooltip_t enhancements.
    char *ctitle = rt_string_to_gui_cstr(title);
    char *cbody = rt_string_to_gui_cstr(body);

    char *combined = rt_gui_join_title_body(ctitle, cbody);
    if (combined) {
        vg_widget_set_tooltip_text((vg_widget_t *)widget, combined);
        free(combined);
    }

    if (ctitle)
        free(ctitle);
    if (cbody)
        free(cbody);
}

/// @brief Clear the tooltip of the widget.
/// @param widget Live widget handle.
void rt_widget_clear_tooltip(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    if (!rt_gui_is_widget_handle(widget))
        return;
    vg_widget_set_tooltip_text((vg_widget_t *)widget, NULL);
}

//=============================================================================
// Phase 7: Toast/Notifications Implementation
//=============================================================================

// Wrapper to track toast state
#define RT_TOAST_DATA_MAGIC UINT64_C(0x5254544F41535431)

typedef struct rt_toast_data {
    uint64_t magic;
    rt_gui_app_t *app;
    uint32_t id;
    int64_t was_action_clicked;
    int64_t was_dismissed;
    int64_t dismissal_reported;
    char *action_label; ///< Optional action button label (owned, may be NULL)
} rt_toast_data_t;

/// @brief Authenticate a Toast handle via its magic tag (NULL if not).
/// @param toast Candidate managed Toast wrapper.
/// @return Validated wrapper, or `NULL` when the magic tag does not match.
static rt_toast_data_t *rt_toast_checked(void *toast) {
    rt_toast_data_t *data = (rt_toast_data_t *)toast;
    return data && data->magic == RT_TOAST_DATA_MAGIC ? data : NULL;
}

/// @brief Toast action-button callback — flips an edge-trigger when the user clicks "Undo" /
/// "Retry" etc.
/// @param id Notification identifier supplied by the manager.
/// @param user_data Toast wrapper associated with the notification.
static void rt_toast_on_action(uint32_t id, void *user_data) {
    rt_toast_data_t *data = (rt_toast_data_t *)user_data;
    if (!data || data->magic != RT_TOAST_DATA_MAGIC || data->id != id)
        return;
    data->was_action_clicked = 1;
}

/// @brief Return the toast's live app, rejecting stale app handles without dereferencing them.
/// @param data Toast wrapper whose owning application is validated.
/// @return Live application pointer, or `NULL` when absent or stale.
static rt_gui_app_t *rt_toast_live_app(rt_toast_data_t *data) {
    return data && data->app && rt_gui_is_app_handle(data->app) ? data->app : NULL;
}

/// @brief Detach notification callbacks that point at this toast wrapper and free owned state.
/// @param data Toast wrapper to dispose.
static void rt_toast_dispose(rt_toast_data_t *data) {
    if (!data)
        return;
    rt_gui_app_t *app = rt_toast_live_app(data);
    vg_notification_manager_t *mgr = app ? app->notification_manager : NULL;
    if (mgr) {
        for (size_t i = 0; i < mgr->notification_count; i++) {
            vg_notification_t *notif = mgr->notifications[i];
            if (notif && notif->action_user_data == data) {
                notif->action_user_data = NULL;
                notif->action_callback = NULL;
            }
        }
    }
    free(data->action_label);
    data->action_label = NULL;
    data->app = NULL;
    data->magic = 0;
}

/// @brief GC finalizer for toast handles.
/// @param toast Managed Toast wrapper being finalized.
static void rt_toast_finalize(void *toast) {
    rt_toast_dispose((rt_toast_data_t *)toast);
}

/// @brief Return (and lazily create) the per-app notification manager.
///
/// Notifications stack on the active app's overlay; each app gets
/// its own manager so background apps don't show toasts on the
/// foreground window.
/// @param app Application that owns the notification overlay.
/// @return Existing or newly created manager, or `NULL` for invalid input or allocation failure.
static vg_notification_manager_t *rt_get_notification_manager(rt_gui_app_t *app) {
    if (!app)
        return NULL;
    if (!app->notification_manager) {
        app->notification_manager = vg_notification_manager_create();
        if (app->notification_manager && app->default_font) {
            vg_notification_manager_set_font(
                app->notification_manager, app->default_font, rt_gui_app_effective_font_size(app));
        }
    }
    return app->notification_manager;
}

/// @brief Map a public `RT_TOAST_*` enum to the internal `VG_NOTIFICATION_*` enum.
/// Defaults to INFO for any unknown value.
/// @param type Public toast type value.
/// @return Corresponding widget-layer notification type.
static vg_notification_type_t rt_toast_type_to_vg(int64_t type) {
    switch (type) {
        case RT_TOAST_INFO:
            return VG_NOTIFICATION_INFO;
        case RT_TOAST_SUCCESS:
            return VG_NOTIFICATION_SUCCESS;
        case RT_TOAST_WARNING:
            return VG_NOTIFICATION_WARNING;
        case RT_TOAST_ERROR:
            return VG_NOTIFICATION_ERROR;
        default:
            return VG_NOTIFICATION_INFO;
    }
}

/// @brief Map a public `RT_TOAST_POSITION_*` enum to the internal `VG_NOTIFICATION_*` corner.
/// Defaults to TOP_RIGHT for unknown positions.
/// @param position Public toast-position value.
/// @return Corresponding widget-layer notification position.
static vg_notification_position_t rt_toast_position_to_vg(int64_t position) {
    switch (position) {
        case RT_TOAST_POSITION_TOP_RIGHT:
            return VG_NOTIFICATION_TOP_RIGHT;
        case RT_TOAST_POSITION_TOP_LEFT:
            return VG_NOTIFICATION_TOP_LEFT;
        case RT_TOAST_POSITION_BOTTOM_RIGHT:
            return VG_NOTIFICATION_BOTTOM_RIGHT;
        case RT_TOAST_POSITION_BOTTOM_LEFT:
            return VG_NOTIFICATION_BOTTOM_LEFT;
        case RT_TOAST_POSITION_TOP_CENTER:
            return VG_NOTIFICATION_TOP_CENTER;
        case RT_TOAST_POSITION_BOTTOM_CENTER:
            return VG_NOTIFICATION_BOTTOM_CENTER;
        default:
            return VG_NOTIFICATION_TOP_RIGHT;
    }
}

/// @brief Compare optional notification strings without treating two NULL values as unequal.
/// @param left First optional string.
/// @param right Second optional string.
/// @return `true` when both are null or their contents match; otherwise `false`.
static bool rt_toast_text_equal(const char *left, const char *right) {
    if (left == right)
        return true;
    if (!left || !right)
        return false;
    return strcmp(left, right) == 0;
}

/// @brief Show or refresh one fire-and-forget notification.
/// @details Repeated background events often report the same condition on consecutive frames.
///          Coalescing an exact active match keeps those events from obscuring newer, unrelated
///          feedback. Configurable Toast handles remain distinct because callers use their IDs
///          for action and dismissal polling.
/// @param app Application supplying the scheduler clock.
/// @param mgr Notification manager receiving or refreshing the toast.
/// @param type Notification severity.
/// @param title Optional title text.
/// @param message Optional message text.
/// @param duration_ms Auto-dismiss duration, or zero for persistent display.
/// @return Existing or newly assigned notification identifier, or zero on failure.
static uint32_t rt_toast_show_shortcut(rt_gui_app_t *app,
                                       vg_notification_manager_t *mgr,
                                       vg_notification_type_t type,
                                       const char *title,
                                       const char *message,
                                       uint32_t duration_ms) {
    if (!app || !mgr)
        return 0;

    uint64_t now_ms = rt_gui_feature_now_ms(app);
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        if (!notif || notif->dismissed || notif->action_label || notif->action_callback)
            continue;
        if (notif->type != type || !rt_toast_text_equal(notif->title, title) ||
            !rt_toast_text_equal(notif->message, message)) {
            continue;
        }
        notif->duration_ms = duration_ms;
        notif->created_at = now_ms;
        mgr->base.needs_paint = true;
        return notif->id;
    }

    uint32_t id = vg_notification_show(mgr, type, title, message, duration_ms);
    for (size_t i = 0; i < mgr->notification_count; i++) {
        if (mgr->notifications[i] && mgr->notifications[i]->id == id) {
            mgr->notifications[i]->created_at = now_ms;
            break;
        }
    }
    return id;
}

/// @brief Show an informational toast notification (auto-dismisses after 3 seconds).
/// @param message Runtime notification message.
void rt_toast_info(rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    vg_notification_manager_t *mgr = rt_get_notification_manager(app);
    if (!mgr)
        return;

    char *cmsg = rt_string_to_gui_cstr(message);
    rt_toast_show_shortcut(app, mgr, VG_NOTIFICATION_INFO, "Info", cmsg, 3000);
    if (cmsg)
        free(cmsg);
}

/// @brief Show a success toast notification (auto-dismisses after 3 seconds).
/// @param message Runtime notification message.
void rt_toast_success(rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    vg_notification_manager_t *mgr = rt_get_notification_manager(app);
    if (!mgr)
        return;

    char *cmsg = rt_string_to_gui_cstr(message);
    rt_toast_show_shortcut(app, mgr, VG_NOTIFICATION_SUCCESS, "Success", cmsg, 3000);
    if (cmsg)
        free(cmsg);
}

/// @brief Show a warning toast notification (auto-dismisses after 5 seconds).
/// @param message Runtime notification message.
void rt_toast_warning(rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    vg_notification_manager_t *mgr = rt_get_notification_manager(app);
    if (!mgr)
        return;

    char *cmsg = rt_string_to_gui_cstr(message);
    rt_toast_show_shortcut(app, mgr, VG_NOTIFICATION_WARNING, "Warning", cmsg, 5000);
    if (cmsg)
        free(cmsg);
}

/// @brief Show an error toast notification (does not auto-dismiss; user must close).
/// @param message Runtime notification message.
void rt_toast_error(rt_string message) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    vg_notification_manager_t *mgr = rt_get_notification_manager(app);
    if (!mgr)
        return;

    char *cmsg = rt_string_to_gui_cstr(message);
    rt_toast_show_shortcut(app, mgr, VG_NOTIFICATION_ERROR, "Error", cmsg, 0);
    if (cmsg)
        free(cmsg);
}

/// @brief Create a configurable toast and return a handle for action / dismissal polling.
///
/// Unlike the `rt_toast_info/success/warning/error` shortcuts which
/// fire-and-forget, this version returns a wrapper struct that
/// callers can poll via `rt_toast_was_action_clicked` to detect
/// user interaction. `duration_ms == 0` means no auto-dismiss.
/// @param message Runtime notification message.
/// @param type Public toast severity value.
/// @param duration_ms Auto-dismiss delay in milliseconds; non-positive values persist.
/// @return New managed Toast wrapper, or `NULL` when no active app exists or creation fails.
void *rt_toast_new(rt_string message, int64_t type, int64_t duration_ms) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    vg_notification_manager_t *mgr = rt_get_notification_manager(app);
    if (!mgr)
        return NULL;

    char *cmsg = rt_string_to_gui_cstr(message);

    rt_toast_data_t *data = (rt_toast_data_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_toast_data_t));
    if (!data) {
        free(cmsg);
        return NULL;
    }
    data->app = app;
    data->magic = RT_TOAST_DATA_MAGIC;
    rt_obj_set_finalizer(data, rt_toast_finalize);

    uint32_t clamped_duration = 0;
    if (duration_ms > 0)
        clamped_duration = duration_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_ms;

    data->id = vg_notification_show(mgr, rt_toast_type_to_vg(type), NULL, cmsg, clamped_duration);
    if (data->id == 0) {
        free(cmsg);
        data->magic = 0;
        return NULL;
    }
    data->was_action_clicked = 0;
    data->was_dismissed = 0;
    data->dismissal_reported = 0;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        if (mgr->notifications[i] && mgr->notifications[i]->id == data->id) {
            mgr->notifications[i]->created_at = rt_gui_feature_now_ms(app);
            break;
        }
    }

    if (cmsg)
        free(cmsg);
    return data;
}

/// @brief Set the action of the toast.
/// @param toast Managed Toast wrapper.
/// @param label Non-empty runtime label for the action button.
void rt_toast_set_action(void *toast, rt_string label) {
    RT_ASSERT_MAIN_THREAD();
    rt_toast_data_t *data = rt_toast_checked(toast);
    if (!data)
        return;
    char *new_label = rt_string_to_gui_cstr(label);
    if (!new_label || !new_label[0]) {
        free(new_label);
        return;
    }
    vg_notification_manager_t *mgr = rt_get_notification_manager(rt_toast_live_app(data));
    if (!mgr) {
        free(new_label);
        return;
    }
    for (size_t i = 0; i < mgr->notification_count; i++) {
        vg_notification_t *notif = mgr->notifications[i];
        if (!notif || notif->id != data->id)
            continue;
        char *notif_label = rt_gui_features_strdup(new_label);
        if (!notif_label) {
            free(new_label);
            return;
        }
        free(data->action_label);
        data->action_label = new_label;
        free(notif->action_label);
        notif->action_label = notif_label;
        notif->action_callback = rt_toast_on_action;
        notif->action_user_data = data;
        mgr->base.needs_paint = true;
        return;
    }
    free(new_label);
}

/// @brief Check if the toast's action button was clicked (edge-triggered).
/// @param toast Managed Toast wrapper.
/// @return `1` once after an action click; otherwise `0`.
int64_t rt_toast_was_action_clicked(void *toast) {
    RT_ASSERT_MAIN_THREAD();
    rt_toast_data_t *data = rt_toast_checked(toast);
    if (!data)
        return 0;
    int64_t result = data->was_action_clicked;
    data->was_action_clicked = 0;
    return result;
}

/// @brief Check if the toast was dismissed (expired or manually closed).
/// @param toast Managed Toast wrapper.
/// @return `1` once after dismissal or removal from the manager; otherwise `0`.
int64_t rt_toast_was_dismissed(void *toast) {
    RT_ASSERT_MAIN_THREAD();
    rt_toast_data_t *data = rt_toast_checked(toast);
    if (!data)
        return 0;

    if (data->dismissal_reported)
        return 0;

    if (data->was_dismissed) {
        data->dismissal_reported = 1;
        return 1;
    }

    // Check with the notification manager for auto-timeout dismissal
    rt_gui_app_t *app = rt_toast_live_app(data);
    vg_notification_manager_t *mgr = app ? app->notification_manager : NULL;
    if (!mgr) {
        data->was_dismissed = 1;
        data->dismissal_reported = 1;
        return 1;
    }
    bool found = false;
    for (size_t i = 0; i < mgr->notification_count; i++) {
        if (mgr->notifications[i] && mgr->notifications[i]->id == data->id) {
            found = true;
            if (mgr->notifications[i]->dismissed) {
                data->was_dismissed = 1;
                data->dismissal_reported = 1;
                return 1;
            }
            break;
        }
    }
    // If notification is no longer tracked by the manager, it was dismissed
    if (!found) {
        data->was_dismissed = 1;
        data->dismissal_reported = 1;
        return 1;
    }
    return 0;
}

/// @brief Dismiss the toast.
/// @param toast Managed Toast wrapper to dismiss.
void rt_toast_dismiss(void *toast) {
    RT_ASSERT_MAIN_THREAD();
    rt_toast_data_t *data = rt_toast_checked(toast);
    if (!data)
        return;
    vg_notification_manager_t *mgr = rt_get_notification_manager(rt_toast_live_app(data));
    if (mgr) {
        vg_notification_dismiss(mgr, data->id);
        data->was_dismissed = 1;
        data->dismissal_reported = 0;
    }
}

/// @brief Set the position of the toast.
/// @param position Public notification-stack position value.
void rt_toast_set_position(int64_t position) {
    RT_ASSERT_MAIN_THREAD();
    vg_notification_manager_t *mgr = rt_get_notification_manager(rt_gui_get_active_app());
    if (mgr) {
        vg_notification_manager_set_position(mgr, rt_toast_position_to_vg(position));
    }
}

/// @brief Set the max visible of the toast.
/// @param count Maximum simultaneously visible notifications, clamped to `[1,100]`.
void rt_toast_set_max_visible(int64_t count) {
    RT_ASSERT_MAIN_THREAD();
    if (count < 1)
        count = 1;
    if (count > 100)
        count = 100;
    vg_notification_manager_t *mgr = rt_get_notification_manager(rt_gui_get_active_app());
    if (mgr) {
        mgr->max_visible = (uint32_t)count;
    }
}

/// @brief Dismiss the all of the toast.
void rt_toast_dismiss_all(void) {
    RT_ASSERT_MAIN_THREAD();
    vg_notification_manager_t *mgr = rt_get_notification_manager(rt_gui_get_active_app());
    if (mgr) {
        vg_notification_dismiss_all(mgr);
    }
}

//=============================================================================
// Phase 8: Drag and Drop Implementation
//=============================================================================

/// @brief Borrowed snapshot of a widget's drag/drop configuration and runtime flags.
/// @details The fields mirror the drag/drop state stored directly on `vg_widget_t`.
///          String pointers are borrowed from the widget and must not be freed or
///          retained after the widget may be destroyed or mutated. The snapshot
///          keeps public drag/drop accessors consistent without duplicating
///          ownership rules across each getter.
typedef struct rt_drag_drop_data {
    int64_t is_draggable;
    const char *drag_type;
    const char *drag_data;
    int64_t is_drop_target;
    const char *accepted_types;
    int64_t is_being_dragged;
    int64_t is_drag_over;
    int64_t was_dropped;
    const char *drop_type;
    const char *drop_data;
} rt_drag_drop_data_t;

/// @brief Capture all drag/drop state from @p widget into a value object.
/// @details Returns zero/default fields for NULL widgets. The snapshot borrows
///          all pointer fields from the widget; callers should use it only for
///          immediate reads on the GUI thread.
/// @param widget Widget whose drag/drop state should be observed.
/// @return Borrowed drag/drop state snapshot.
static rt_drag_drop_data_t rt_widget_drag_drop_snapshot(const vg_widget_t *widget) {
    rt_drag_drop_data_t data = {0};
    if (!widget)
        return data;
    data.is_draggable = widget->draggable ? 1 : 0;
    data.drag_type = widget->drag_type;
    data.drag_data = widget->drag_data;
    data.is_drop_target = widget->is_drop_target ? 1 : 0;
    data.accepted_types = widget->accepted_drop_types;
    data.is_being_dragged = widget->_is_being_dragged ? 1 : 0;
    data.is_drag_over = widget->_is_drag_over ? 1 : 0;
    data.was_dropped = widget->_was_dropped ? 1 : 0;
    data.drop_type = widget->_drop_received_type;
    data.drop_data = widget->_drop_received_data;
    return data;
}

/// @brief Set the draggable of the widget.
/// @param widget Live widget handle.
/// @param draggable Non-zero to allow initiating drags.
void rt_widget_set_draggable(void *widget, int64_t draggable) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    w->draggable = draggable != 0;
}

/// @brief Set the drag data of the widget.
/// @param widget Live widget handle.
/// @param type Runtime drag payload type; embedded NUL bytes are rejected.
/// @param data Runtime drag payload data; embedded NUL bytes are rejected.
void rt_widget_set_drag_data(void *widget, rt_string type, rt_string data) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    if (rt_string_contains_nul(type) || rt_string_contains_nul(data))
        return;
    char *new_type = type ? rt_string_to_cstr(type) : NULL;
    char *new_data = data ? rt_string_to_cstr(data) : NULL;
    if ((type && !new_type) || (data && !new_data)) {
        free(new_type);
        free(new_data);
        return;
    }
    free(w->drag_type);
    free(w->drag_data);
    w->drag_type = new_type;
    w->drag_data = new_data;
}

/// @brief Check whether a widget is currently being dragged.
/// @param widget Live widget handle.
/// @return `1` while the widget is the active drag source; otherwise `0`.
int64_t rt_widget_is_being_dragged(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    rt_drag_drop_data_t data = rt_widget_drag_drop_snapshot(w);
    return data.is_being_dragged;
}

/// @brief Get a value from the widget.
/// @param widget Live widget handle.
/// @param target Non-zero to accept drops on the widget.
void rt_widget_set_drop_target(void *widget, int64_t target) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    w->is_drop_target = target != 0;
}

/// @brief Set the accepted drop types of the widget.
/// @param widget Live widget handle.
/// @param types Runtime accepted-type specification; embedded NUL bytes are rejected.
void rt_widget_set_accepted_drop_types(void *widget, rt_string types) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return;
    if (rt_string_contains_nul(types))
        return;
    char *new_types = types ? rt_string_to_cstr(types) : NULL;
    if (types && !new_types)
        return;
    free(w->accepted_drop_types);
    w->accepted_drop_types = new_types;
}

/// @brief Check whether a dragged item is hovering over this drop target.
/// @param widget Live widget handle.
/// @return `1` while a compatible drag is over the widget; otherwise `0`.
int64_t rt_widget_is_drag_over(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    rt_drag_drop_data_t data = rt_widget_drag_drop_snapshot(w);
    return data.is_drag_over;
}

/// @brief Check whether a drop was completed on this widget this frame.
/// @param widget Live widget handle.
/// @return `1` once after a completed drop; otherwise `0`.
int64_t rt_widget_was_dropped(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return 0;
    rt_drag_drop_data_t data = rt_widget_drag_drop_snapshot(w);
    int64_t result = data.was_dropped;
    w->_was_dropped = false; // Clear after read (edge-triggered)
    return result;
}

/// @brief Get the drop type of the widget.
/// @param widget Live widget handle.
/// @return Owned type of the most recent drop, or an empty runtime string.
rt_string rt_widget_get_drop_type(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return rt_str_empty();
    rt_drag_drop_data_t data = rt_widget_drag_drop_snapshot(w);
    if (data.drop_type)
        return rt_string_from_bytes(data.drop_type, strlen(data.drop_type));
    return rt_str_empty();
}

/// @brief Get the drop data of the widget.
/// @param widget Live widget handle.
/// @return Owned data from the most recent drop, or an empty runtime string.
rt_string rt_widget_get_drop_data(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    if (!w)
        return rt_str_empty();
    rt_drag_drop_data_t data = rt_widget_drag_drop_snapshot(w);
    if (data.drop_data)
        return rt_string_from_bytes(data.drop_data, strlen(data.drop_data));
    return rt_str_empty();
}

/// @brief The pointer x recorded when the last drop landed on this widget.
/// @param widget Live widget handle.
/// @return Recorded horizontal coordinate, or `0.0` for an invalid handle.
double rt_widget_get_drop_x(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? (double)w->_drop_received_x : 0.0;
}

/// @brief The pointer y recorded when the last drop landed on this widget.
/// @param widget Live widget handle.
/// @return Recorded vertical coordinate, or `0.0` for an invalid handle.
double rt_widget_get_drop_y(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    vg_widget_t *w = rt_gui_widget_handle_checked(widget);
    return w ? (double)w->_drop_received_y : 0.0;
}

/// @brief Check whether files were dropped onto the app window this frame.
/// @param app Runtime application handle.
/// @return `1` once for a pending file-drop batch; otherwise `0`.
int64_t rt_app_was_file_dropped(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_gui_app_handle_checked(app);
    if (!gui_app)
        return 0;
    int64_t result = gui_app->file_drop.was_dropped;
    gui_app->file_drop.was_dropped = 0;
    return result;
}

/// @brief Get the number of files dropped onto the app window.
/// @param app Runtime application handle.
/// @return Number of paths in the current drop batch, or zero for invalid input.
int64_t rt_app_get_dropped_file_count(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_gui_app_handle_checked(app);
    return gui_app ? gui_app->file_drop.file_count : 0;
}

/// @brief Get the dropped file of the app.
/// @param app Runtime application handle.
/// @param index Zero-based path index in the current drop batch.
/// @return Owned path string, or an empty runtime string for invalid input.
rt_string rt_app_get_dropped_file(void *app, int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_gui_app_handle_checked(app);
    if (gui_app && index >= 0 && index < gui_app->file_drop.file_count &&
        gui_app->file_drop.files) {
        char *file = gui_app->file_drop.files[index];
        if (file) {
            return rt_string_from_bytes(file, strlen(file));
        }
    }
    return rt_str_empty();
}

/// @brief Add an element to the file.
/// @param app Application receiving the platform file-drop event.
/// @param path Null-terminated path copied into the current batch.
void rt_gui_file_drop_add(rt_gui_app_t *app, const char *path) {
    if (!app || !path)
        return;
    if (app->file_drop.was_dropped &&
        (app->file_drop.file_count < 0 ||
         (uint64_t)app->file_drop.file_count >= RT_GUI_MAX_COLLECTION_ITEMS))
        return;
    char *path_copy = rt_gui_features_strdup(path);
    if (!path_copy)
        return;

    // Free old data on first file of a new batch
    if (!app->file_drop.was_dropped) {
        for (int64_t i = 0; i < app->file_drop.file_count; i++)
            free(app->file_drop.files[i]);
        free(app->file_drop.files);
        app->file_drop.files = NULL;
        app->file_drop.file_count = 0;
    }

    // Grow array
    if (app->file_drop.file_count < 0 ||
        (uint64_t)app->file_drop.file_count >= RT_GUI_MAX_COLLECTION_ITEMS ||
        (size_t)app->file_drop.file_count >= SIZE_MAX / sizeof(char *) - 1) {
        free(path_copy);
        return;
    }
    char **new_files = (char **)realloc(app->file_drop.files,
                                        (size_t)(app->file_drop.file_count + 1) * sizeof(char *));
    if (!new_files) {
        free(path_copy);
        return;
    }
    app->file_drop.files = new_files;
    app->file_drop.files[app->file_drop.file_count++] = path_copy;
    app->file_drop.was_dropped = 1;
}

/// @brief Cleanup the features.
/// @param app Application whose feature widgets and file-drop state are released.
void rt_gui_features_cleanup(rt_gui_app_t *app) {
    if (!app)
        return;
    if (app->manual_tooltip) {
        vg_tooltip_destroy(app->manual_tooltip);
        app->manual_tooltip = NULL;
    }
    if (app->notification_manager) {
        vg_notification_manager_destroy(app->notification_manager);
        app->notification_manager = NULL;
    }
    for (int i = 0; i < app->command_palette_count; i++) {
        if (app->command_palettes[i]) {
            rt_commandpalette_data_t *data =
                (rt_commandpalette_data_t *)app->command_palettes[i]->base.user_data;
            if (data && data->palette == app->command_palettes[i]) {
                rt_commandpalette_clear_selection(data);
                data->palette = NULL;
                data->app = NULL;
            }
            vg_commandpalette_destroy(app->command_palettes[i]);
            app->command_palettes[i] = NULL;
        }
    }
    for (int64_t i = 0; i < app->file_drop.file_count; i++)
        free(app->file_drop.files[i]);
    free(app->file_drop.files);
    app->file_drop = (rt_gui_file_drop_data_t){0};
}

#else /* !ZANNA_ENABLE_GRAPHICS */

// ===========================================================================
// Headless stubs — same prototypes as the real implementations above so
// non-graphical builds (server / CLI) link without pulling in
// the GUI. Each stub no-ops or returns a sentinel; doc comments are
// inherited from the real functions above by virtue of identical names.
// ===========================================================================

/// @brief Stub: returns NULL — command palette requires graphics.
/// @param parent Ignored parent handle.
/// @return Always `NULL`.
void *rt_commandpalette_new(void *parent) {
    (void)parent;
    return NULL;
}

/// @brief Release resources and destroy the commandpalette.
/// @param palette Ignored CommandPalette handle.
void rt_commandpalette_destroy(void *palette) {
    (void)palette;
}

/// @brief Register a command in the palette's fuzzy-searchable list.
/// @param palette Ignored CommandPalette handle.
/// @param id Ignored command identifier.
/// @param label Ignored display label.
/// @param category Ignored category.
void rt_commandpalette_add_command(void *palette,
                                   rt_string id,
                                   rt_string label,
                                   rt_string category) {
    (void)palette;
    (void)id;
    (void)label;
    (void)category;
}

/// @brief Stub: `CommandPalette.AddCommandWithShortcut` is a no-op without graphics.
/// @param palette Ignored CommandPalette handle.
/// @param id Ignored command identifier.
/// @param label Ignored display label.
/// @param category Ignored category.
/// @param shortcut Ignored shortcut text.
void rt_commandpalette_add_command_with_shortcut(
    void *palette, rt_string id, rt_string label, rt_string category, rt_string shortcut) {
    (void)palette;
    (void)id;
    (void)label;
    (void)category;
    (void)shortcut;
}

/// @brief Remove a command from the palette by its ID.
/// @param palette Ignored CommandPalette handle.
/// @param id Ignored command identifier.
void rt_commandpalette_remove_command(void *palette, rt_string id) {
    (void)palette;
    (void)id;
}

/// @brief Remove all entries from the commandpalette.
/// @param palette Ignored CommandPalette handle.
void rt_commandpalette_clear(void *palette) {
    (void)palette;
}

/// @brief Show the commandpalette.
/// @param palette Ignored CommandPalette handle.
void rt_commandpalette_show(void *palette) {
    (void)palette;
}

/// @brief Hide the commandpalette.
/// @param palette Ignored CommandPalette handle.
void rt_commandpalette_hide(void *palette) {
    (void)palette;
}

/// @brief Check whether the command palette is currently visible.
/// @param palette Ignored CommandPalette handle.
/// @return Always `0`.
int64_t rt_commandpalette_is_visible(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Set the placeholder of the commandpalette.
/// @param palette Ignored CommandPalette handle.
/// @param text Ignored placeholder text.
void rt_commandpalette_set_placeholder(void *palette, rt_string text) {
    (void)palette;
    (void)text;
}

/// @brief Get the selected command of the commandpalette.
/// @param palette Ignored CommandPalette handle.
/// @return Empty runtime string.
rt_string rt_commandpalette_get_selected_command(void *palette) {
    (void)palette;
    return rt_str_empty();
}

/// @brief Check if a command was selected since the last call (edge-triggered, resets).
/// @param palette Ignored CommandPalette handle.
/// @return Always `0`.
int64_t rt_commandpalette_was_command_selected(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Stub: no query text without graphics.
/// @param palette Ignored CommandPalette handle.
/// @return Empty runtime string.
rt_string rt_commandpalette_get_query(void *palette) {
    (void)palette;
    return rt_str_empty();
}

/// @brief Return the neutral query generation without graphics.
/// @param palette Ignored CommandPalette handle.
/// @return Always `0`.
int64_t rt_commandpalette_get_query_generation(void *palette) {
    (void)palette;
    return 0;
}

/// @brief Ignore query assignment without graphics.
/// @param palette Ignored CommandPalette handle.
/// @param text Ignored query text.
void rt_commandpalette_set_query(void *palette, rt_string text) {
    (void)palette;
    (void)text;
}

/// @brief Ignore client-filter mode assignment without graphics.
/// @param palette Ignored CommandPalette handle.
/// @param enabled Ignored filtering flag.
void rt_commandpalette_set_client_filtered(void *palette, int64_t enabled) {
    (void)palette;
    (void)enabled;
}

/// @brief Show the tooltip.
/// @param text Ignored tooltip text.
/// @param x Ignored horizontal coordinate.
/// @param y Ignored vertical coordinate.
void rt_tooltip_show(rt_string text, int64_t x, int64_t y) {
    (void)text;
    (void)x;
    (void)y;
}

/// @brief Show a rich tooltip with a title and body at a specific screen position.
/// @param title Ignored title text.
/// @param body Ignored body text.
/// @param x Ignored horizontal coordinate.
/// @param y Ignored vertical coordinate.
void rt_tooltip_show_rich(rt_string title, rt_string body, int64_t x, int64_t y) {
    (void)title;
    (void)body;
    (void)x;
    (void)y;
}

/// @brief Hide the tooltip.
void rt_tooltip_hide(void) {}

/// @brief Set the delay of the tooltip.
/// @param delay_ms Ignored tooltip delay.
void rt_tooltip_set_delay(int64_t delay_ms) {
    (void)delay_ms;
}

/// @brief Set the tooltip of the widget.
/// @param widget Ignored widget handle.
/// @param text Ignored tooltip text.
void rt_widget_set_tooltip(void *widget, rt_string text) {
    (void)widget;
    (void)text;
}

/// @brief Attach a rich tooltip (title + body) to a widget for hover display.
/// @param widget Ignored widget handle.
/// @param title Ignored title text.
/// @param body Ignored body text.
void rt_widget_set_tooltip_rich(void *widget, rt_string title, rt_string body) {
    (void)widget;
    (void)title;
    (void)body;
}

/// @brief Clear the tooltip of the widget.
/// @param widget Ignored widget handle.
void rt_widget_clear_tooltip(void *widget) {
    (void)widget;
}

/// @brief Show an informational toast notification (auto-dismisses after 3 seconds).
/// @param message Ignored notification message.
void rt_toast_info(rt_string message) {
    (void)message;
}

/// @brief Show a success toast notification (auto-dismisses after 3 seconds).
/// @param message Ignored notification message.
void rt_toast_success(rt_string message) {
    (void)message;
}

/// @brief Show a warning toast notification (auto-dismisses after 5 seconds).
/// @param message Ignored notification message.
void rt_toast_warning(rt_string message) {
    (void)message;
}

/// @brief Show an error toast notification (does not auto-dismiss; user must close).
/// @param message Ignored notification message.
void rt_toast_error(rt_string message) {
    (void)message;
}

/// @brief Stub: returns NULL — toast notifications require graphics.
/// @param message Ignored notification message.
/// @param type Ignored toast severity.
/// @param duration_ms Ignored auto-dismiss duration.
/// @return Always `NULL`.
void *rt_toast_new(rt_string message, int64_t type, int64_t duration_ms) {
    (void)message;
    (void)type;
    (void)duration_ms;
    return NULL;
}

/// @brief Set the action of the toast.
/// @param toast Ignored Toast handle.
/// @param label Ignored action label.
void rt_toast_set_action(void *toast, rt_string label) {
    (void)toast;
    (void)label;
}

/// @brief Check if the toast's action button was clicked (edge-triggered).
/// @param toast Ignored Toast handle.
/// @return Always `0`.
int64_t rt_toast_was_action_clicked(void *toast) {
    (void)toast;
    return 0;
}

/// @brief Check if the toast was dismissed (expired or manually closed).
/// @param toast Ignored Toast handle.
/// @return Always `0`.
int64_t rt_toast_was_dismissed(void *toast) {
    (void)toast;
    return 0;
}

/// @brief Dismiss the toast.
/// @param toast Ignored Toast handle.
void rt_toast_dismiss(void *toast) {
    (void)toast;
}

/// @brief Set the position of the toast.
/// @param position Ignored notification-stack position.
void rt_toast_set_position(int64_t position) {
    (void)position;
}

/// @brief Set the max visible of the toast.
/// @param count Ignored maximum visible count.
void rt_toast_set_max_visible(int64_t count) {
    (void)count;
}

/// @brief Dismiss the all of the toast.
void rt_toast_dismiss_all(void) {}

/// @brief Set the draggable of the widget.
/// @param widget Ignored widget handle.
/// @param draggable Ignored draggable flag.
void rt_widget_set_draggable(void *widget, int64_t draggable) {
    (void)widget;
    (void)draggable;
}

/// @brief Set the drag data of the widget.
/// @param widget Ignored widget handle.
/// @param type Ignored payload type.
/// @param data Ignored payload data.
void rt_widget_set_drag_data(void *widget, rt_string type, rt_string data) {
    (void)widget;
    (void)type;
    (void)data;
}

/// @brief Check whether a widget is currently being dragged.
/// @param widget Ignored widget handle.
/// @return Always `0`.
int64_t rt_widget_is_being_dragged(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get a value from the widget.
/// @param widget Ignored widget handle.
/// @param target Ignored drop-target flag.
void rt_widget_set_drop_target(void *widget, int64_t target) {
    (void)widget;
    (void)target;
}

/// @brief Set the accepted drop types of the widget.
/// @param widget Ignored widget handle.
/// @param types Ignored accepted-type specification.
void rt_widget_set_accepted_drop_types(void *widget, rt_string types) {
    (void)widget;
    (void)types;
}

/// @brief Check whether a dragged item is hovering over this drop target.
/// @param widget Ignored widget handle.
/// @return Always `0`.
int64_t rt_widget_is_drag_over(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Check whether a drop was completed on this widget this frame.
/// @param widget Ignored widget handle.
/// @return Always `0`.
int64_t rt_widget_was_dropped(void *widget) {
    (void)widget;
    return 0;
}

/// @brief Get the drop type of the widget.
/// @param widget Ignored widget handle.
/// @return Empty runtime string.
rt_string rt_widget_get_drop_type(void *widget) {
    (void)widget;
    return rt_str_empty();
}

/// @brief Get the drop data of the widget.
/// @param widget Ignored widget handle.
/// @return Empty runtime string.
rt_string rt_widget_get_drop_data(void *widget) {
    (void)widget;
    return rt_str_empty();
}

/// @brief Stub: no drop position exists when graphics is disabled.
/// @param widget Ignored widget handle.
/// @return Always `0.0`.
double rt_widget_get_drop_x(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Stub: no drop position exists when graphics is disabled.
/// @param widget Ignored widget handle.
/// @return Always `0.0`.
double rt_widget_get_drop_y(void *widget) {
    (void)widget;
    return 0.0;
}

/// @brief Check whether files were dropped onto the app window this frame.
/// @param app Ignored application handle.
/// @return Always `0`.
int64_t rt_app_was_file_dropped(void *app) {
    (void)app;
    return 0;
}

/// @brief Get the number of files dropped onto the app window.
/// @param app Ignored application handle.
/// @return Always `0`.
int64_t rt_app_get_dropped_file_count(void *app) {
    (void)app;
    return 0;
}

/// @brief Get the dropped file of the app.
/// @param app Ignored application handle.
/// @param index Ignored path index.
/// @return Empty runtime string.
rt_string rt_app_get_dropped_file(void *app, int64_t index) {
    (void)app;
    (void)index;
    return rt_str_empty();
}

/// @brief Add an element to the file.
/// @param app Ignored application pointer.
/// @param path Ignored file path.
void rt_gui_file_drop_add(rt_gui_app_t *app, const char *path) {
    (void)app;
    (void)path;
}

/// @brief Cleanup the features.
/// @param app Ignored application pointer.
void rt_gui_features_cleanup(rt_gui_app_t *app) {
    (void)app;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
