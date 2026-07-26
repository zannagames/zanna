//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_system.c
// Purpose: System-level GUI services for the Zanna runtime: clipboard read/write,
//   keyboard shortcut registration and frame-based polling, window management
//   helpers (title, opacity, always-on-top, position, maximise/minimise), and
//   cursor style control. These are global services not tied to a specific widget.
//
// Key invariants:
//   - Shortcuts are stored per GUI app and the table grows dynamically.
//   - Shortcut trigger state is edge-triggered per frame on the active app: it is
//     set when polled and cleared on the next GUI app poll.
//   - shortcuts_global_enabled can disable all shortcut processing for the app
//     (e.g. when a text input widget has focus).
//   - Clipboard operations delegate directly to vgfx_clipboard_*; text is
//     converted to/from rt_string via rt_string_to_cstr / rt_string_from_bytes.
//   - Cursor style constants map 1:1 to VGFX_CURSOR_* enum values.
//
// Ownership/Lifetime:
//   - Shortcut id/keys/description strings are duplicated into the active app's
//     shortcut table with malloc-backed storage and freed by rt_shortcuts_clear().
//   - Clipboard text returned by vgfx_clipboard_get_text is malloc'd by the
//     platform; this file frees it after converting to rt_string.
//
// Links: src/runtime/graphics/gui/rt_gui_internal.h (internal types/globals),
//        src/lib/graphics/include/vgfx.h (clipboard and window management API),
//        src/runtime/rt_platform.h (platform detection)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements process- and window-level GUI system services.
 *
 * @details The bindings cover clipboard text, per-app keyboard shortcuts and
 *          two-stroke chords, window title/state/opacity/position controls,
 *          cursor selection, accessibility and platform preferences, and
 *          related GUI services that are not owned by one widget.
 */

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_internal.h"
#include "rt_platform.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Duplicate a NUL-terminated GUI string with malloc-backed ownership.
/// @details Keeps this runtime-facing module independent of platform-specific
///          `strdup` availability and makes all copied strings compatible with
///          the existing `free` cleanup paths.
/// @param text Source string to copy; NULL returns NULL.
/// @return Newly allocated copy, or NULL on invalid input, overflow, or OOM.
static char *rt_gui_system_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > SIZE_MAX - 1u)
        return NULL;
    char *copy = (char *)malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

// Clipboard Functions (Phase 1)
//=============================================================================

/// @brief Copy text to the system clipboard.
/// @details Converts the complete runtime display string to NUL-terminated GUI-safe UTF-8,
///          delegates synchronously to the platform clipboard, and releases the temporary copy.
///          Conversion failure leaves the clipboard unchanged.
/// @param text Runtime text to publish; embedded NUL bytes become replacement characters.
void rt_clipboard_set_text(rt_string text) {
    RT_ASSERT_MAIN_THREAD();
    char *ctext = rt_string_to_gui_cstr(text);
    if (ctext) {
        vgfx_clipboard_set_text(ctext);
        free(ctext);
    }
}

/// @brief Get the text of the clipboard.
/// @details Copies the platform-owned NUL-terminated text into a runtime string and frees the
///          platform allocation immediately; no clipboard storage is exposed to the caller.
/// @return Runtime clipboard text, or the canonical empty string when text is unavailable.
rt_string rt_clipboard_get_text(void) {
    RT_ASSERT_MAIN_THREAD();
    char *text = vgfx_clipboard_get_text();
    if (!text)
        return rt_str_empty();
    rt_string result = rt_string_from_bytes(text, strlen(text));
    free(text);
    return result;
}

/// @brief Has the text of the clipboard.
/// @return One when the platform reports the text clipboard format, otherwise zero.
int64_t rt_clipboard_has_text(void) {
    RT_ASSERT_MAIN_THREAD();
    return vgfx_clipboard_has_format(VGFX_CLIPBOARD_TEXT) ? 1 : 0;
}

/// @brief Remove all entries from the clipboard.
void rt_clipboard_clear(void) {
    RT_ASSERT_MAIN_THREAD();
    vgfx_clipboard_clear();
}

//=============================================================================
// Keyboard Shortcuts (Phase 1)
//=============================================================================

/// @brief Pick the app whose shortcut table to operate on.
/// @details Prefers the widget-construction/current-app context and falls back to the app whose
///          toolkit state is active. The pointer is borrowed and may be NULL.
/// @return Borrowed shortcut-owning app, or NULL when no GUI app context exists.
static rt_gui_app_t *rt_shortcuts_app(void) {
    return s_current_app ? s_current_app : rt_gui_get_active_app();
}

/// @brief Grow the shortcut table if it's full (doubles capacity, default 16).
/// @details Uses checked geometric growth and publishes the reallocated table only on success.
///          Invalid apps, available capacity, overflow, and allocation failure are no-ops.
/// @param app App whose owned shortcut array may need one additional slot.
static void rt_shortcuts_ensure_capacity(rt_gui_app_t *app) {
    if (!app || app->shortcut_count < app->shortcut_cap)
        return;
    if (app->shortcut_cap > INT_MAX / 2)
        return;
    int new_cap = app->shortcut_cap ? app->shortcut_cap * 2 : 16;
    if ((size_t)new_cap > SIZE_MAX / sizeof(*app->shortcuts))
        return;
    void *p = realloc(app->shortcuts, (size_t)new_cap * sizeof(*app->shortcuts));
    if (!p)
        return;
    app->shortcuts = p;
    app->shortcut_cap = new_cap;
}

/// @brief Release every copied identifier in an app's ordered trigger queue.
/// @details Frees the queue array and legacy last-triggered copy, then resets all counts,
///          capacities, and pointers without changing registered shortcuts or chord state.
/// @param app App whose transient trigger storage should be cleared; NULL is a no-op.
static void rt_shortcuts_free_triggered_queue(rt_gui_app_t *app) {
    if (!app)
        return;
    for (int i = 0; i < app->triggered_shortcut_count; i++) {
        free(app->triggered_shortcut_ids[i]);
    }
    free(app->triggered_shortcut_ids);
    app->triggered_shortcut_ids = NULL;
    app->triggered_shortcut_count = 0;
    app->triggered_shortcut_cap = 0;
    free(app->triggered_shortcut_id);
    app->triggered_shortcut_id = NULL;
}

/// @brief Append a copied shortcut identifier to the current frame's trigger queue.
/// @details Grows the queue geometrically with overflow checks, appends an owned copy in activation
///          order, and best-effort refreshes the legacy last-triggered identifier. A failure before
///          append leaves the queue unchanged; failure of only the legacy copy does not undo it.
/// @param app App owning transient shortcut state.
/// @param id Borrowed non-NULL registered identifier to copy.
/// @return One when the ordered queue accepted the identifier, otherwise zero.
static int rt_shortcuts_record_triggered(rt_gui_app_t *app, const char *id) {
    if (!app || !id)
        return 0;
    if (app->triggered_shortcut_count >= app->triggered_shortcut_cap) {
        if (app->triggered_shortcut_cap > INT_MAX / 2)
            return 0;
        int new_cap = app->triggered_shortcut_cap ? app->triggered_shortcut_cap * 2 : 4;
        if ((size_t)new_cap > SIZE_MAX / sizeof(*app->triggered_shortcut_ids))
            return 0;
        void *p = realloc(app->triggered_shortcut_ids,
                          (size_t)new_cap * sizeof(*app->triggered_shortcut_ids));
        if (!p)
            return 0;
        app->triggered_shortcut_ids = (char **)p;
        app->triggered_shortcut_cap = new_cap;
    }

    char *queued_id = rt_gui_system_strdup(id);
    if (!queued_id)
        return 0;
    app->triggered_shortcut_ids[app->triggered_shortcut_count++] = queued_id;

    char *last_id = rt_gui_system_strdup(id);
    if (last_id) {
        free(app->triggered_shortcut_id);
        app->triggered_shortcut_id = last_id;
    }
    return 1;
}

/// @brief Maximum interval between the two strokes of a shortcut chord.
enum { RT_SHORTCUT_CHORD_TIMEOUT_MS = 1500 };

/// @brief Clear a partially entered shortcut chord for one app.
/// @details Resets the pending flag, parsed prefix, and timeout origin without touching registered
///          shortcuts or current-frame trigger results.
/// @param app App whose chord state should be cancelled; NULL is a no-op.
static void rt_shortcuts_cancel_chord(rt_gui_app_t *app) {
    if (!app)
        return;
    app->shortcut_chord_pending = 0;
    memset(&app->shortcut_chord_prefix, 0, sizeof(app->shortcut_chord_prefix));
    app->shortcut_chord_started_ms = 0;
}

/// @brief Maps a case-insensitive textual key name to a toolkit key code.
typedef struct {
    const char *name; ///< Borrowed static key spelling.
    int key;          ///< Corresponding VG key code.
} rt_shortcut_named_key_t;

/// @brief Null-terminated lookup table for named non-character shortcut keys.
static const rt_shortcut_named_key_t RT_SHORTCUT_NAMED_KEYS[] = {
    {"Enter", VG_KEY_ENTER},
    {"Return", VG_KEY_ENTER},
    {"Escape", VG_KEY_ESCAPE},
    {"Esc", VG_KEY_ESCAPE},
    {"Space", VG_KEY_SPACE},
    {"Tab", VG_KEY_TAB},
    {"Backspace", VG_KEY_BACKSPACE},
    {"Delete", VG_KEY_DELETE},
    {"Del", VG_KEY_DELETE},
    {"Home", VG_KEY_HOME},
    {"End", VG_KEY_END},
    {"PageUp", VG_KEY_PAGE_UP},
    {"PgUp", VG_KEY_PAGE_UP},
    {"PageDown", VG_KEY_PAGE_DOWN},
    {"PgDn", VG_KEY_PAGE_DOWN},
    {"Left", VG_KEY_LEFT},
    {"Right", VG_KEY_RIGHT},
    {"Up", VG_KEY_UP},
    {"Down", VG_KEY_DOWN},
    {NULL, 0},
};

/// @brief Parse one `+`-separated keystroke into normalized modifier/key fields.
/// @param keys One stroke such as `Ctrl+Shift+S`.
/// @param stroke Destination, cleared before parsing.
/// @return 1 on success, 0 on invalid syntax or allocation failure.
static int parse_shortcut_stroke(const char *keys, rt_gui_shortcut_stroke_t *stroke) {
    if (!stroke)
        return 0;
    memset(stroke, 0, sizeof(*stroke));
    if (!keys)
        return 0;

    char *copy = rt_gui_system_strdup(keys);
    if (!copy)
        return 0;

    char *saveptr = NULL;
    char *token = rt_strtok_r(copy, "+", &saveptr);
    while (token) {
        int recognized = 0;
        while (isspace((unsigned char)*token))
            token++;
        char *end = token + strlen(token);
        while (end > token && isspace((unsigned char)end[-1]))
            *--end = '\0';

        if (*token == '\0') {
            free(copy);
            return 0;
        }

        if (rt_gui_ascii_casecmp(token, "Ctrl") == 0 ||
            rt_gui_ascii_casecmp(token, "Control") == 0) {
            stroke->ctrl = 1;
            recognized = 1;
        } else if (rt_gui_ascii_casecmp(token, "Shift") == 0) {
            stroke->shift = 1;
            recognized = 1;
        } else if (rt_gui_ascii_casecmp(token, "Alt") == 0) {
            stroke->alt = 1;
            recognized = 1;
        } else if (rt_gui_ascii_casecmp(token, "Cmd") == 0 ||
                   rt_gui_ascii_casecmp(token, "Command") == 0) {
            stroke->super = 1;
            recognized = 1;
        } else {
            int parsed_key = 0;
            if (strlen(token) == 1) {
                parsed_key = toupper((unsigned char)token[0]);
            } else if ((token[0] == 'F' || token[0] == 'f') && strlen(token) <= 3) {
                char *parse_end = NULL;
                long fnum = strtol(token + 1, &parse_end, 10);
                if (parse_end && *parse_end == '\0' && fnum >= 1 && fnum <= 12)
                    parsed_key = VG_KEY_F1 + ((int)fnum - 1);
            } else {
                for (const rt_shortcut_named_key_t *named = RT_SHORTCUT_NAMED_KEYS; named->name;
                     named++) {
                    if (rt_gui_ascii_casecmp(token, named->name) == 0) {
                        parsed_key = named->key;
                        break;
                    }
                }
            }
            if (parsed_key != 0 && stroke->key == 0) {
                stroke->key = parsed_key;
                recognized = 1;
            }
        }

        if (!recognized) {
            free(copy);
            return 0;
        }
        token = rt_strtok_r(NULL, "+", &saveptr);
    }

    free(copy);
    return stroke->key != 0;
}

/// @brief Find the whitespace separator between two shortcut strokes.
/// @details Whitespace immediately beside `+` remains part of a single stroke,
///          so user spellings such as `Ctrl + Shift + S` are still accepted.
/// @param text Mutable, trimmed shortcut text.
/// @return Start of the second stroke after terminating the first, or NULL.
static char *split_shortcut_chord(char *text) {
    for (char *cursor = text; cursor && *cursor;) {
        if (!isspace((unsigned char)*cursor)) {
            cursor++;
            continue;
        }
        char *run = cursor;
        while (isspace((unsigned char)*cursor))
            cursor++;
        if (run > text && run[-1] != '+' && *cursor != '\0' && *cursor != '+') {
            *run = '\0';
            return cursor;
        }
    }
    return NULL;
}

/// @brief Parse a one- or two-stroke shortcut binding.
/// @details Chord strokes are separated by whitespace, for example
///          `Ctrl+K Ctrl+S`. More than two strokes are rejected rather than
///          silently registering a partial binding.
/// @param keys Human-readable binding text.
/// @param first Parsed first stroke.
/// @param second Parsed second stroke, with key zero for a single-stroke binding.
/// @return 1 on success, 0 on invalid syntax or allocation failure.
static int parse_shortcut_keys(const char *keys,
                               rt_gui_shortcut_stroke_t *first,
                               rt_gui_shortcut_stroke_t *second) {
    if (!first || !second)
        return 0;
    memset(first, 0, sizeof(*first));
    memset(second, 0, sizeof(*second));
    if (!keys)
        return 0;

    char *copy = rt_gui_system_strdup(keys);
    if (!copy)
        return 0;
    char *start = copy;
    while (isspace((unsigned char)*start))
        start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1]))
        *--end = '\0';

    char *second_text = split_shortcut_chord(start);
    if (second_text && split_shortcut_chord(second_text)) {
        free(copy);
        return 0;
    }

    int valid = parse_shortcut_stroke(start, first);
    if (valid && second_text)
        valid = parse_shortcut_stroke(second_text, second);
    free(copy);
    return valid;
}

/// @brief Register (or update) a named keyboard shortcut on the active GUI app.
/// @details If a shortcut with the same `id` is already registered its key
///          binding and description are updated in-place and the pre-parsed
///          modifier/key fields are recomputed.  If it is new, the shortcut
///          table is grown as needed (doubling policy) before appending.
///          All three strings are duplicated into the table and freed by
///          `rt_shortcuts_clear` or `rt_shortcuts_unregister`.
/// @param id       Unique string identifier used to poll and toggle the shortcut.
/// @param keys     Human-readable key binding, e.g. "Ctrl+Shift+S".
/// @param description Optional description shown in a help/shortcut dialog.
void rt_shortcuts_register(rt_string id, rt_string keys, rt_string description) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app)
        return;
    if (rt_string_contains_nul(id) || rt_string_contains_nul(keys))
        return;

    char *cid = rt_string_to_cstr(id);
    char *ckeys = rt_string_to_cstr(keys);
    char *cdesc = rt_string_to_gui_cstr(description);

    if (!cid || !ckeys) {
        free(cid);
        free(ckeys);
        free(cdesc);
        return;
    }

    rt_gui_shortcut_stroke_t first;
    rt_gui_shortcut_stroke_t second;
    if (!parse_shortcut_keys(ckeys, &first, &second)) {
        free(cid);
        free(ckeys);
        free(cdesc);
        return;
    }

    // Check if already registered and update
    for (int i = 0; i < app->shortcut_count; i++) {
        if (app->shortcuts[i].id && strcmp(app->shortcuts[i].id, cid) == 0) {
            free(app->shortcuts[i].keys);
            free(app->shortcuts[i].description);
            app->shortcuts[i].keys = ckeys;
            app->shortcuts[i].description = cdesc;
            app->shortcuts[i].first = first;
            app->shortcuts[i].second = second;
            rt_shortcuts_cancel_chord(app);
            free(cid);
            return;
        }
    }

    // Add new shortcut
    rt_shortcuts_ensure_capacity(app);
    if (app->shortcut_count >= app->shortcut_cap) {
        free(cid);
        free(ckeys);
        free(cdesc);
        return;
    }
    rt_gui_shortcut_t *sc = &app->shortcuts[app->shortcut_count];
    sc->id = cid;
    sc->keys = ckeys;
    sc->description = cdesc;
    sc->enabled = 1;
    sc->triggered = 0;
    sc->first = first;
    sc->second = second;
    app->shortcut_count++;
    rt_shortcuts_cancel_chord(app);
}

/// @brief Remove a registered shortcut by id, freeing its stored strings.
/// @details Scans the table linearly; on match frees id/keys/description,
///          then shifts the tail of the array down to keep it contiguous.
/// @param id Identifier passed to `rt_shortcuts_register`.
void rt_shortcuts_unregister(rt_string id) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app)
        return;
    if (rt_string_contains_nul(id))
        return;
    char *cid = rt_string_to_cstr(id);
    if (!cid)
        return;

    for (int i = 0; i < app->shortcut_count; i++) {
        if (app->shortcuts[i].id && strcmp(app->shortcuts[i].id, cid) == 0) {
            free(app->shortcuts[i].id);
            free(app->shortcuts[i].keys);
            free(app->shortcuts[i].description);

            // Shift remaining shortcuts down
            memmove(&app->shortcuts[i],
                    &app->shortcuts[i + 1],
                    (size_t)(app->shortcut_count - i - 1) * sizeof(*app->shortcuts));
            app->shortcut_count--;
            rt_shortcuts_cancel_chord(app);
            break;
        }
    }

    free(cid);
}

/// @brief Remove all entries from the shortcuts.
/// @details Frees every owned identifier, binding, and description plus both the registration and
///          trigger arrays, then cancels any pending chord. No app context is a no-op.
void rt_shortcuts_clear(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app)
        return;
    for (int i = 0; i < app->shortcut_count; i++) {
        free(app->shortcuts[i].id);
        free(app->shortcuts[i].keys);
        free(app->shortcuts[i].description);
    }
    free(app->shortcuts);
    app->shortcuts = NULL;
    app->shortcut_count = 0;
    app->shortcut_cap = 0;
    rt_shortcuts_cancel_chord(app);
    rt_shortcuts_free_triggered_queue(app);
}

/// @brief Check if any registered keyboard shortcut was triggered this frame.
/// @details Compares the runtime identifier by exact bytes against the ordered trigger queue and
///          legacy last-triggered copy without consuming either. Embedded NULs and invalid strings
///          are rejected so C-string registrations cannot match truncated input.
/// @param id Runtime shortcut identifier to query.
/// @return One when that identifier fired in the current frame, otherwise zero.
int64_t rt_shortcuts_was_triggered(rt_string id) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app || !id)
        return 0;
    if (rt_string_contains_nul(id))
        return 0;

    int64_t id_len64 = rt_str_len(id);
    if (id_len64 < 0)
        return 0;
    size_t id_len = (size_t)id_len64;
    const char *id_bytes = id_len > 0 ? rt_string_cstr(id) : "";
    if (!id_bytes)
        return 0;

    for (int i = 0; i < app->triggered_shortcut_count; i++) {
        const char *triggered = app->triggered_shortcut_ids[i];
        if (!triggered)
            continue;
        size_t triggered_len = strlen(triggered);
        if (id_len == triggered_len && memcmp(id_bytes, triggered, id_len) == 0)
            return 1;
    }

    if (app->triggered_shortcut_id) {
        size_t triggered_len = strlen(app->triggered_shortcut_id);
        if (id_len == triggered_len && memcmp(id_bytes, app->triggered_shortcut_id, id_len) == 0)
            return 1;
    }
    return 0;
}

/// @brief Reset all per-shortcut triggered flags and the cached triggered-id string.
/// @details Called at the start of each frame (from `rt_gui_app_poll`) before
///          processing new key events, so that `rt_shortcuts_was_triggered` reads
///          reflect only events in the current frame.
/// @param app App whose shortcut table is being reset; no-op if NULL.
void rt_shortcuts_clear_triggered(rt_gui_app_t *app) {
    RT_ASSERT_MAIN_THREAD();
    if (!app)
        return;
    for (int i = 0; i < app->shortcut_count; i++) {
        app->shortcuts[i].triggered = 0;
    }
    rt_shortcuts_free_triggered_queue(app);
}

/// @brief Compare two parsed strokes for exact key/modifier identity.
/// @param left First normalized stroke.
/// @param right Second normalized stroke.
/// @return One when both non-NULL strokes have identical key and modifier fields, otherwise zero.
static int rt_shortcut_strokes_equal(const rt_gui_shortcut_stroke_t *left,
                                     const rt_gui_shortcut_stroke_t *right) {
    return left && right && left->ctrl == right->ctrl && left->shift == right->shift &&
           left->alt == right->alt && left->super == right->super && left->key == right->key;
}

/// @brief Test one parsed stroke against a normalized input event.
/// @param stroke Parsed registered stroke; an empty key never matches.
/// @param key Normalized event key.
/// @param ctrl Normalized Control flag.
/// @param shift Normalized Shift flag.
/// @param alt Normalized Alt flag.
/// @param super Normalized Command/Super flag.
/// @return One for an exact key/modifier match, otherwise zero.
static int rt_shortcut_stroke_matches(
    const rt_gui_shortcut_stroke_t *stroke, int key, int ctrl, int shift, int alt, int super) {
    return stroke && stroke->key != 0 && stroke->key == key && stroke->ctrl == ctrl &&
           stroke->shift == shift && stroke->alt == alt && stroke->super == super;
}

/// @brief Return whether an app's pending chord exceeded its input window.
/// @details Missing timing state is not expired; a monotonic-clock reversal is treated as timeout
///          so stale prefix state cannot persist indefinitely.
/// @param app App whose pending prefix timestamps should be compared.
/// @return One when the chord is pending and older than 1.5 seconds, otherwise zero.
static int rt_shortcuts_chord_timed_out(const rt_gui_app_t *app) {
    if (!app || !app->shortcut_chord_pending || app->shortcut_chord_started_ms == 0 ||
        app->last_event_time_ms == 0)
        return 0;
    if (app->last_event_time_ms < app->shortcut_chord_started_ms)
        return 1;
    return app->last_event_time_ms - app->shortcut_chord_started_ms > RT_SHORTCUT_CHORD_TIMEOUT_MS;
}

/// @brief Match a key event against registered global shortcuts; trigger on hit.
/// @details Walks the shortcut table and handles both single strokes and one
///          pending two-stroke chord per app. A chord prefix is consumed without
///          triggering a command; one of its enabled suffixes must arrive within
///          1.5 seconds. An expired or mismatched suffix cancels the pending chord
///          before the current key is considered as a fresh shortcut. Several
///          normalisations apply before comparison:
///          - **Cmd/Ctrl identity.** Ctrl and Super/Cmd are compared as separate
///            modifiers on every platform so `Ctrl+X` and `Super+X` can coexist.
///          - **Modifier requirement.** Plain printable keys (no Ctrl, Super,
///            or Alt) are rejected, while non-text keys such as Escape and F1
///            remain valid. This prevents typing `S` in a text field from
///            accidentally firing a global command.
///          - **Case folding.** Lowercase letters are upper-cased for
///            comparison so the parsed-shortcut table and incoming events
///            match regardless of the user's caps-lock state.
///
///          On match, the shortcut's `triggered` flag is set and its id is
///          appended to the app's per-frame triggered queue. `Shortcuts.GetTriggered`
///          reads the first queued id for compatibility, while
///          `Shortcuts.WasTriggered(id)` can observe every shortcut fired this frame.
/// @param app App owning the shortcut table.
/// @param key Key code from the event.
/// @param mods Modifier bitmask (`VG_MOD_*`).
/// @return 1 if a shortcut fired or a chord prefix was consumed; 0 otherwise.
int8_t rt_shortcuts_check_key(rt_gui_app_t *app, int key, int mods) {
    RT_ASSERT_MAIN_THREAD();
    if (!app || !app->shortcuts_global_enabled)
        return 0;

    int has_ctrl = (mods & VG_MOD_CTRL) ? 1 : 0;
    int has_super = (mods & VG_MOD_SUPER) ? 1 : 0;
    int has_shift = (mods & VG_MOD_SHIFT) ? 1 : 0;
    int has_alt = (mods & VG_MOD_ALT) ? 1 : 0;
    int upper_key = (key >= 'a' && key <= 'z') ? key - ('a' - 'A') : key;

    if (app->shortcut_chord_pending) {
        if (!rt_shortcuts_chord_timed_out(app)) {
            for (int i = 0; i < app->shortcut_count; i++) {
                rt_gui_shortcut_t *shortcut = &app->shortcuts[i];
                if (!shortcut->enabled || shortcut->second.key == 0 ||
                    !rt_shortcut_strokes_equal(&shortcut->first, &app->shortcut_chord_prefix) ||
                    !rt_shortcut_stroke_matches(
                        &shortcut->second, upper_key, has_ctrl, has_shift, has_alt, has_super))
                    continue;

                rt_shortcuts_cancel_chord(app);
                if (!rt_shortcuts_record_triggered(app, shortcut->id))
                    return 0;
                shortcut->triggered = 1;
                return 1;
            }
        }
        rt_shortcuts_cancel_chord(app);
    }

    // Plain printable keys are intentionally excluded from global first
    // strokes so ordinary typing can never invoke a command. Non-text keys
    // such as Escape and function keys remain valid without modifiers.
    if (!has_ctrl && !has_super && !has_alt && upper_key >= VG_KEY_SPACE &&
        upper_key <= VG_KEY_GRAVE)
        return 0;

    // Chord prefixes win over an otherwise identical single-stroke binding.
    // This keeps shared prefixes deterministic instead of depending on catalog
    // registration order.
    for (int i = 0; i < app->shortcut_count; i++) {
        rt_gui_shortcut_t *shortcut = &app->shortcuts[i];
        if (!shortcut->enabled || shortcut->second.key == 0 ||
            !rt_shortcut_stroke_matches(
                &shortcut->first, upper_key, has_ctrl, has_shift, has_alt, has_super))
            continue;
        app->shortcut_chord_pending = 1;
        app->shortcut_chord_prefix = shortcut->first;
        app->shortcut_chord_started_ms = app->last_event_time_ms;
        return 1;
    }

    for (int i = 0; i < app->shortcut_count; i++) {
        rt_gui_shortcut_t *shortcut = &app->shortcuts[i];
        if (!shortcut->enabled || shortcut->second.key != 0 ||
            !rt_shortcut_stroke_matches(
                &shortcut->first, upper_key, has_ctrl, has_shift, has_alt, has_super))
            continue;
        if (!rt_shortcuts_record_triggered(app, shortcut->id))
            return 0;
        shortcut->triggered = 1;
        return 1;
    }
    return 0;
}

/// @brief Return the id of the first shortcut triggered this frame, or an empty string.
/// @details Reads the per-frame triggered queue populated by `rt_shortcuts_check_key`.
///          Valid only for the current frame; cleared at the start of the next poll by
///          `rt_shortcuts_clear_triggered`.
/// @return The shortcut id string, or an empty rt_string if no shortcut fired this frame.
rt_string rt_shortcuts_get_triggered(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (app && app->triggered_shortcut_count > 0 && app->triggered_shortcut_ids[0]) {
        const char *id = app->triggered_shortcut_ids[0];
        return rt_string_from_bytes(id, strlen(id));
    }
    if (app && app->triggered_shortcut_id) {
        return rt_string_from_bytes(app->triggered_shortcut_id, strlen(app->triggered_shortcut_id));
    }
    return rt_str_empty();
}

/// @brief Enable or disable one registered keyboard shortcut.
/// @details Performs an exact identifier lookup and normalizes @p enabled to Boolean. Invalid,
///          embedded-NUL, missing, or allocation-failed identifiers leave the table unchanged.
/// @param id Runtime identifier registered by @ref rt_shortcuts_register.
/// @param enabled Non-zero to enable matching, zero to suppress this binding.
void rt_shortcuts_set_enabled(rt_string id, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app)
        return;
    if (rt_string_contains_nul(id))
        return;
    char *cid = rt_string_to_cstr(id);
    if (!cid)
        return;

    for (int i = 0; i < app->shortcut_count; i++) {
        if (app->shortcuts[i].id && strcmp(app->shortcuts[i].id, cid) == 0) {
            app->shortcuts[i].enabled = enabled != 0;
            break;
        }
    }

    free(cid);
}

/// @brief Check whether one registered keyboard shortcut is enabled.
/// @param id Runtime identifier registered by @ref rt_shortcuts_register.
/// @return One when the exact binding exists and is enabled, otherwise zero.
int64_t rt_shortcuts_is_enabled(rt_string id) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app)
        return 0;
    if (rt_string_contains_nul(id))
        return 0;
    char *cid = rt_string_to_cstr(id);
    if (!cid)
        return 0;

    for (int i = 0; i < app->shortcut_count; i++) {
        if (app->shortcuts[i].id && strcmp(app->shortcuts[i].id, cid) == 0) {
            free(cid);
            return app->shortcuts[i].enabled ? 1 : 0;
        }
    }

    free(cid);
    return 0;
}

/// @brief Enable or disable all shortcut processing for the active app at once.
/// @details When the global flag is false, `rt_shortcuts_check_key` returns 0
///          without scanning the table, silencing all shortcuts (e.g. while a
///          text input has focus so typing does not fire editor commands).
/// @param enabled Non-zero to enable shortcut processing; 0 to suppress it.
void rt_shortcuts_set_global_enabled(int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (app) {
        app->shortcuts_global_enabled = enabled != 0;
        if (!app->shortcuts_global_enabled)
            rt_shortcuts_cancel_chord(app);
    }
}

/// @brief Return whether global shortcut processing is currently active for the app.
/// @return 1 if shortcuts are enabled globally, 0 if suppressed.
int64_t rt_shortcuts_get_global_enabled(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    return app && app->shortcuts_global_enabled ? 1 : 0;
}

//=============================================================================
// Window Management (Phase 1)
//=============================================================================

/// @brief Validate `app` as a live app handle and return it, or NULL if invalid/destroyed.
/// @details Thin wrapper around rt_gui_app_handle_checked that provides a
///          concise name for the window-management helpers in this file.
/// @param app Candidate opaque runtime handle.
/// @return Borrowed live app, or NULL for invalid, foreign, or destroyed input.
static rt_gui_app_t *rt_app_checked(void *app) {
    return rt_gui_app_handle_checked(app);
}

/// @brief Return the window's HiDPI scale factor, defaulting to 1.0 for NULL or non-finite results.
/// @details Used by window-size helpers to convert between logical and physical
///          pixels. Guards against zero/NaN scale values from unusual display
///          configurations.
/// @param app Borrowed validated app; may be NULL or lack a window.
/// @return Positive finite platform scale, with 1.0 as the fallback.
static float rt_app_window_scale(rt_gui_app_t *app) {
    float scale = (app && app->window) ? vgfx_window_get_scale(app->window) : 1.0f;
    return (!isfinite(scale) || scale <= 0.0f) ? 1.0f : scale;
}

/// @brief Resize the platform window after clamping `w`/`h` to the valid vgfx range.
/// @details Scales the maximum allowed size by the current HiDPI factor so
///          the logical-pixel cap is respected even on high-DPI displays. Does
///          NOT set a fixed size on the root widget — root sizing is driven by
///          the layout engine in rt_gui_app_render().
/// @param app Borrowed validated app whose native window should be resized.
/// @param w Requested logical width.
/// @param h Requested logical height.
static void rt_app_set_window_size_checked(rt_gui_app_t *app, int64_t w, int64_t h) {
    if (!app || !app->window)
        return;

    float scale = rt_app_window_scale(app);
    double max_w_d = (double)VGFX_MAX_WIDTH / (double)scale;
    double max_h_d = (double)VGFX_MAX_HEIGHT / (double)scale;
    int32_t max_w = max_w_d > (double)INT32_MAX ? INT32_MAX : (int32_t)max_w_d;
    int32_t max_h = max_h_d > (double)INT32_MAX ? INT32_MAX : (int32_t)max_h_d;
    if (max_w < 1)
        max_w = 1;
    if (max_h < 1)
        max_h = 1;

    int32_t clamped_w = rt_gui_clamp_i64_to_i32(w, 1, max_w);
    int32_t clamped_h = rt_gui_clamp_i64_to_i32(h, 1, max_h);
    vgfx_set_window_size(app->window, clamped_w, clamped_h);
    // Root sizing is handled by vg_widget_layout(root, phys_w, phys_h) in
    // rt_gui_app_render; setting a root fixed size here would corrupt layout.
}

/// @brief Publish a logical native-window minimum after clamping to vgfx limits.
/// @details Computes the largest representable logical dimensions from the current platform scale
///          and clamps each request to the inclusive supported range.
/// @param app Borrowed validated app whose native minimum should change.
/// @param w Requested logical minimum width.
/// @param h Requested logical minimum height.
static void rt_app_set_minimum_size_checked(rt_gui_app_t *app, int64_t w, int64_t h) {
    if (!app || !app->window)
        return;

    float scale = rt_app_window_scale(app);
    double max_w_d = (double)VGFX_MAX_WIDTH / (double)scale;
    double max_h_d = (double)VGFX_MAX_HEIGHT / (double)scale;
    int32_t max_w = max_w_d > (double)INT32_MAX ? INT32_MAX : (int32_t)max_w_d;
    int32_t max_h = max_h_d > (double)INT32_MAX ? INT32_MAX : (int32_t)max_h_d;
    if (max_w < 1)
        max_w = 1;
    if (max_h < 1)
        max_h = 1;

    int32_t clamped_w = rt_gui_clamp_i64_to_i32(w, 1, max_w);
    int32_t clamped_h = rt_gui_clamp_i64_to_i32(h, 1, max_h);
    vgfx_set_window_min_size(app->window, clamped_w, clamped_h);
}

/// @brief Set the title of the app.
/// @details Allocation-first conversion preserves the old title on failure. Success updates the
///          native title, query copy, root accessible name, accessibility bridge, and native menu.
/// @param app Candidate live GUI app handle.
/// @param title Runtime display title; embedded NULs become replacement characters.
void rt_app_set_title(void *app, rt_string title) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (!gui_app->window)
        return;
    char *cstr = rt_string_to_gui_cstr(title);
    if (!cstr)
        return;
    char *stored = rt_gui_system_strdup(cstr);
    if (!stored) {
        free(cstr);
        return;
    }
    vgfx_set_title(gui_app->window, cstr);
    // Store a copy for rt_app_get_title (no vgfx_get_title API exists)
    free(gui_app->title);
    gui_app->title = stored;
    if (gui_app->root) {
        vg_widget_set_accessible_name(gui_app->root, stored);
        rt_gui_accessibility_platform_notify(gui_app->window, gui_app->root);
    }
    rt_gui_macos_menu_sync_app(gui_app);
    free(cstr);
}

/// @brief Get the title of the app.
/// @param app Candidate live GUI app handle.
/// @return Runtime copy of the stored title, or the canonical empty string when invalid/unset.
rt_string rt_app_get_title(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return rt_str_empty();
    if (gui_app->title)
        return rt_string_from_bytes(gui_app->title, strlen(gui_app->title));
    return rt_str_empty();
}

/// @brief Resize the app window to the requested logical-pixel dimensions.
/// @param app    App handle.
/// @param width  New window width in logical pixels.
/// @param height New window height in logical pixels.
void rt_app_set_size(void *app, int64_t width, int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    rt_app_set_window_size_checked(rt_app_checked(app), width, height);
}

/// @brief Get the width of the app.
/// @param app Candidate live GUI app handle.
/// @return Current native window width in platform pixels, or zero when invalid.
int64_t rt_app_get_width(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_size(gui_app->window, &w, &h);
    return w;
}

/// @brief Get the height of the app.
/// @param app Candidate live GUI app handle.
/// @return Current native window height in platform pixels, or zero when invalid.
int64_t rt_app_get_height(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_size(gui_app->window, &w, &h);
    return h;
}

/// @brief Get the window's HiDPI backing scale factor (>= 1.0).
/// @details Window width/height are reported in physical pixels; dividing by this
///          factor recovers the logical (point) dimensions used for layout
///          breakpoints. Mirrors the scaling already applied by the monitor getters.
/// @param app Candidate live GUI app handle.
/// @return Positive finite platform backing scale, or 1.0 when invalid.
double rt_app_get_scale(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 1.0;
    return (double)rt_app_window_scale(gui_app);
}

/// @brief Get the window width in logical (point) units (physical width / scale).
/// @param app Candidate live GUI app handle.
/// @return DPI-rounded logical width, or zero when invalid.
int64_t rt_app_get_logical_width(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app || !gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_size(gui_app->window, &w, &h);
    return rt_gui_dpi_to_logical(w, (double)rt_app_window_scale(gui_app));
}

/// @brief Get the window height in logical (point) units (physical height / scale).
/// @param app Candidate live GUI app handle.
/// @return DPI-rounded logical height, or zero when invalid.
int64_t rt_app_get_logical_height(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app || !gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_size(gui_app->window, &w, &h);
    return rt_gui_dpi_to_logical(h, (double)rt_app_window_scale(gui_app));
}

/// @brief Convert a physical-pixel value to logical units using the app's current scale.
/// @param app Candidate live GUI app handle.
/// @param physical Signed physical-pixel value to convert.
/// @return DPI-rounded logical value, or @p physical unchanged for an invalid app.
int64_t rt_app_to_logical(void *app, int64_t physical) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return physical;
    return rt_gui_dpi_to_logical(physical, (double)rt_app_window_scale(gui_app));
}

/// @brief Convert a logical value to physical pixels using the app's current scale.
/// @param app Candidate live GUI app handle.
/// @param logical Signed logical value to convert.
/// @return DPI-rounded physical value, or @p logical unchanged for an invalid app.
int64_t rt_app_to_physical(void *app, int64_t logical) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return logical;
    return rt_gui_dpi_to_physical(logical, (double)rt_app_window_scale(gui_app));
}

/// @brief Enable or disable inertial smooth scrolling for the app (ADR 0137).
/// @details Applies process-wide to scroll views and code editors; themes that
///          request reduced motion suppress easing regardless of this flag.
/// @param app Reserved app handle; ignored because the setting is process-global.
/// @param enabled Non-zero to request smooth scrolling, zero for immediate motion.
void rt_app_set_smooth_scroll(void *app, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    (void)app;
    vg_set_smooth_scroll_enabled(enabled != 0);
}

/// @brief Return whether inertial smooth scrolling is requested (ADR 0137).
/// @param app Reserved app handle; ignored because the setting is process-global.
/// @return One when smooth scrolling is requested, otherwise zero.
int64_t rt_app_get_smooth_scroll(void *app) {
    RT_ASSERT_MAIN_THREAD();
    (void)app;
    return vg_smooth_scroll_enabled() ? 1 : 0;
}

/// @brief Set the user UI zoom multiplier applied on top of the HiDPI scale.
/// @details Non-positive and NaN input becomes 1.0, then the value is clamped to [0.5, 3.0].
///          A change refreshes theme/font metrics and invalidates dependent layout and paint.
/// @param app Candidate live GUI app handle.
/// @param scale Requested user zoom multiplier.
void rt_app_set_ui_scale(void *app, double scale) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (!(scale > 0.0))
        scale = 1.0;
    if (scale < 0.5)
        scale = 0.5;
    if (scale > 3.0)
        scale = 3.0;
    if (gui_app->user_scale == (float)scale)
        return;
    gui_app->user_scale = (float)scale;
    rt_gui_refresh_theme(gui_app);
}

/// @brief `App.SetWheelSpeed` — set the global mouse-wheel scroll sensitivity
///        used by the code editor, list box, and output pane. The value is
///        clamped inside the gui library. The app handle is accepted for API
///        symmetry but the setting is process-global.
/// @param app Reserved app handle; ignored.
/// @param speed Requested wheel sensitivity forwarded to the toolkit sanitizer.
void rt_app_set_wheel_speed(void *app, double speed) {
    RT_ASSERT_MAIN_THREAD();
    (void)app;
    vg_set_wheel_speed((float)speed);
}

/// @brief `App.GetWheelSpeed` — return the global mouse-wheel scroll sensitivity.
/// @param app Reserved app handle; ignored.
/// @return Current process-global wheel sensitivity.
double rt_app_get_wheel_speed(void *app) {
    RT_ASSERT_MAIN_THREAD();
    (void)app;
    return (double)vg_get_wheel_speed();
}

/// @brief Get the current user UI zoom multiplier (1.0 = default).
/// @param app Candidate live GUI app handle.
/// @return Positive stored user multiplier, or 1.0 for invalid/non-positive state.
double rt_app_get_ui_scale(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 1.0;
    if (!(gui_app->user_scale > 0.0f))
        return 1.0;
    return (double)gui_app->user_scale;
}

/// @brief Return the app's effective window-scale times user-zoom factor.
/// @details The value converts logical GUI metrics and font points to retained framebuffer units.
///          Invalid app handles return the identity scale and are never dereferenced.
/// @param app GUI application handle.
/// @return Positive finite effective scale, or 1.0 for an invalid app.
double rt_app_get_effective_scale(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    return (double)rt_gui_app_effective_scale(gui_app);
}

/// @brief Move the app window to a specific screen position.
/// @details Saturates each coordinate to the signed 32-bit platform domain.
/// @param app Candidate live GUI app handle.
/// @param x Requested screen-space X coordinate.
/// @param y Requested screen-space Y coordinate.
void rt_app_set_position(void *app, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_set_position(gui_app->window,
                          rt_gui_clamp_i64_to_i32(x, INT32_MIN, INT32_MAX),
                          rt_gui_clamp_i64_to_i32(y, INT32_MIN, INT32_MAX));
}

/// @brief Return the x screen coordinate of the app window's top-left corner.
/// @param app Candidate live GUI app handle.
/// @return X position in screen pixels, or 0 if the app or window is invalid.
int64_t rt_app_get_x(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t x = 0, y = 0;
    vgfx_get_position(gui_app->window, &x, &y);
    return x;
}

/// @brief Return the y screen coordinate of the app window's top-left corner.
/// @param app Candidate live GUI app handle.
/// @return Y position in screen pixels, or 0 if the app or window is invalid.
int64_t rt_app_get_y(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t x = 0, y = 0;
    vgfx_get_position(gui_app->window, &x, &y);
    return y;
}

/// @brief Minimize the app.
/// @param app Candidate live GUI app handle; invalid/windowless handles are ignored.
void rt_app_minimize(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_minimize(gui_app->window);
}

/// @brief Maximize the app.
/// @param app Candidate live GUI app handle; invalid/windowless handles are ignored.
void rt_app_maximize(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_maximize(gui_app->window);
}

/// @brief Restore the app.
/// @param app Candidate live GUI app handle; invalid/windowless handles are ignored.
void rt_app_restore(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_restore(gui_app->window);
}

/// @brief Check whether the app window is currently minimized.
/// @param app Candidate live GUI app handle.
/// @return Platform minimized state, or zero when invalid/windowless.
int64_t rt_app_is_minimized(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    return gui_app->window ? vgfx_is_minimized(gui_app->window) : 0;
}

/// @brief Check whether the app window is currently maximized.
/// @param app Candidate live GUI app handle.
/// @return Platform maximized state, or zero when invalid/windowless.
int64_t rt_app_is_maximized(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    return gui_app->window ? vgfx_is_maximized(gui_app->window) : 0;
}

/// @brief Enter or exit fullscreen mode for the app window.
/// @param app        App handle.
/// @param fullscreen Non-zero to enter fullscreen, 0 to restore windowed mode.
void rt_app_set_fullscreen(void *app, int64_t fullscreen) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_set_fullscreen(gui_app->window, (int32_t)(fullscreen != 0));
}

/// @brief Check whether the app window is in fullscreen mode.
/// @param app Candidate live GUI app handle.
/// @return Platform fullscreen state, or zero when invalid/windowless.
int64_t rt_app_is_fullscreen(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    return gui_app->window ? vgfx_is_fullscreen(gui_app->window) : 0;
}

/// @brief Focus the app.
/// @details Requests OS foreground activation without changing the runtime active-app pointer.
/// @param app Candidate live GUI app handle.
void rt_app_focus(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_request_foreground(gui_app->window);
}

/// @brief Activate the app as the foreground OS application/window.
/// @param app Candidate live GUI app handle forwarded to @ref rt_app_focus.
void rt_app_activate(void *app) {
    rt_app_focus(app);
}

/// @brief Check whether the app window currently has OS-level focus.
/// @param app Candidate live GUI app handle.
/// @return One when the native window is focused, otherwise zero.
int64_t rt_app_is_focused(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    return gui_app->window ? vgfx_is_focused(gui_app->window) : 0;
}

/// @brief Count of frames that took the full-window repaint path (plan 07).
/// @param app Candidate live GUI app handle.
/// @return Monotonic full-repaint frame count, or zero when invalid.
int64_t rt_app_get_paint_frames_full(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    return gui_app ? (int64_t)gui_app->frames_full : 0;
}

/// @brief Count of frames that took the damage-region (partial) repaint path.
/// @param app Candidate live GUI app handle.
/// @return Monotonic partial-repaint frame count, or zero when invalid.
int64_t rt_app_get_paint_frames_partial(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    return gui_app ? (int64_t)gui_app->frames_partial : 0;
}

/// @brief Enable or disable damage-region rendering at runtime (kill switch).
/// @details Complements the ZANNA_GUI_FULL_REPAINT=1 env var. Passing 0 forces
///          every dirty frame through the full-window repaint path.
/// @param app Candidate live GUI app handle.
/// @param enabled Non-zero to permit partial repainting, zero to force full repaint.
void rt_app_set_partial_paint(void *app, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (gui_app)
        gui_app->partial_paint_enabled = (enabled != 0) ? 1 : 0;
}

/// @brief Control whether the OS close button is allowed to close the window.
/// @details When `prevent` is non-zero the platform close request is intercepted
///          and `rt_app_was_close_requested` returns 1 instead, letting the app
///          show a "Save before quitting?" dialog before deciding to exit.
/// @param app     App handle.
/// @param prevent Non-zero to block automatic close; 0 to allow it.
void rt_app_set_prevent_close(void *app, int64_t prevent) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    if (gui_app->window)
        vgfx_set_prevent_close(gui_app->window, (int32_t)(prevent != 0));
    gui_app->prevent_close = (prevent != 0);
}

/// @brief Check whether the window close was requested this frame.
/// @details Consumes the app's close-request edge so one platform request is reported once.
/// @param app Candidate live GUI app handle.
/// @return One when a pending request was consumed, otherwise zero.
int64_t rt_app_was_close_requested(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    int64_t requested = gui_app->close_requested ? 1 : 0;
    gui_app->close_requested = 0;
    return requested;
}

/// @brief Get the monitor width of the app.
/// @param app Candidate live GUI app handle.
/// @return Monitor width in logical units, or zero when invalid/windowless.
int64_t rt_app_get_monitor_width(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_monitor_size(gui_app->window, &w, &h);
    float scale = rt_app_window_scale(gui_app);
    return (int64_t)((double)w / (double)scale);
}

/// @brief Get the monitor height of the app.
/// @param app Candidate live GUI app handle.
/// @return Monitor height in logical units, or zero when invalid/windowless.
int64_t rt_app_get_monitor_height(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 0;
    if (!gui_app->window)
        return 0;
    int32_t w = 0, h = 0;
    vgfx_get_monitor_size(gui_app->window, &w, &h);
    float scale = rt_app_window_scale(gui_app);
    return (int64_t)((double)h / (double)scale);
}

/// @brief Alias for `rt_app_set_size` — resize the app window in logical pixels.
/// @param app App handle.
/// @param w   New window width in logical pixels.
/// @param h   New window height in logical pixels.
void rt_app_set_window_size(void *app, int64_t w, int64_t h) {
    RT_ASSERT_MAIN_THREAD();
    rt_app_set_window_size_checked(rt_app_checked(app), w, h);
}

/// @brief Set the app window's persistent minimum logical dimensions.
/// @param app Candidate live GUI app handle.
/// @param w Requested logical minimum width.
/// @param h Requested logical minimum height.
void rt_app_set_minimum_size(void *app, int64_t w, int64_t h) {
    RT_ASSERT_MAIN_THREAD();
    rt_app_set_minimum_size_checked(rt_app_checked(app), w, h);
}

/// @brief Return the default font size for the app window in logical points.
/// @param app Candidate live GUI app handle.
/// @return Font size in logical points, defaulting to 14.0 if the app is invalid.
double rt_app_get_font_size(void *app) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return 14.0;
    return (double)gui_app->default_font_size;
}

/// @brief Return the app's stored default font size in logical points.
/// @details Unlike the internal effective pixel size, this value is stable across monitor-DPI and
///          user-zoom changes. It aliases the compatibility `GetFontSize` behavior explicitly.
/// @param app GUI application handle.
/// @return Logical font size, or 14.0 for an invalid app.
double rt_app_get_logical_font_size(void *app) {
    return rt_app_get_font_size(app);
}

/// @brief Set the default font size for the app window in logical points.
/// @details Clamps `size` to [6, 72] pt before storing. The window/canvas
///          backend owns HiDPI coordinate scaling.
/// @param app  App handle.
/// @param size Desired font size in logical points.
void rt_app_set_font_size(void *app, double size) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *gui_app = rt_app_checked(app);
    if (!gui_app)
        return;
    size = rt_gui_sanitize_font_size(size, rt_app_get_font_size(app));
    if (size < 6.0)
        size = 6.0;
    if (size > 72.0)
        size = 72.0;
    gui_app->default_font_size = (float)size;
    rt_gui_reapply_default_font(gui_app);
}

//=============================================================================
// Cursor Styles (Phase 1)
//=============================================================================

/// @brief Change the mouse cursor shape for the active app window.
/// @param type A `VGFX_CURSOR_*` constant (e.g. VGFX_CURSOR_DEFAULT, VGFX_CURSOR_HAND).
void rt_cursor_set(int64_t type) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (!app || !app->window)
        return;
    if (type < VGFX_CURSOR_DEFAULT || type > VGFX_CURSOR_NOT_ALLOWED)
        type = VGFX_CURSOR_DEFAULT;
    vgfx_set_cursor(app->window, (int32_t)type);
}

/// @brief Restore the mouse cursor to the default arrow shape.
void rt_cursor_reset(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_cursor_set(0); /* VGFX_CURSOR_DEFAULT */
}

/// @brief Show or hide the mouse cursor.
/// @param visible Non-zero to show the active app cursor, zero to hide it.
void rt_cursor_set_visible(int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_shortcuts_app();
    if (app && app->window)
        vgfx_set_cursor_visible(app->window, (int32_t)visible);
}

/// @brief Set the process-global cursor while accepting a widget-shaped API.
/// @details Cursor selection is currently active-app-wide; @p widget is reserved for future
///          per-widget cursor policy.
/// @param widget Reserved candidate widget handle; currently ignored.
/// @param type Public cursor kind forwarded to @ref rt_cursor_set.
void rt_widget_set_cursor(void *widget, int64_t type) {
    RT_ASSERT_MAIN_THREAD();
    (void)widget;
    rt_cursor_set(type);
}

/// @brief Reset the process-global cursor while accepting a widget-shaped API.
/// @param widget Reserved candidate widget handle; currently ignored.
void rt_widget_reset_cursor(void *widget) {
    RT_ASSERT_MAIN_THREAD();
    (void)widget;
    rt_cursor_reset();
}

#else /* !ZANNA_ENABLE_GRAPHICS */

/// @brief Stub: ignore clipboard writes when graphics is disabled.
/// @param text Ignored runtime text.
void rt_clipboard_set_text(rt_string text) {
    (void)text;
}

/// @brief Stub: report empty clipboard text when graphics is disabled.
/// @return Canonical empty runtime string.
rt_string rt_clipboard_get_text(void) {
    return rt_str_empty();
}

/// @brief Stub: report no clipboard text when graphics is disabled.
/// @return Always zero.
int64_t rt_clipboard_has_text(void) {
    return 0;
}

/// @brief Remove all entries from the clipboard.
void rt_clipboard_clear(void) {}

/// @brief Stub: graphics disabled — no-op; shortcut registration is silently ignored.
/// @param id Ignored runtime identifier.
/// @param keys Ignored binding text.
/// @param description Ignored display description.
void rt_shortcuts_register(rt_string id, rt_string keys, rt_string description) {
    (void)id;
    (void)keys;
    (void)description;
}

/// @brief Stub: graphics disabled — no-op; no shortcut table exists to remove from.
/// @param id Ignored runtime identifier.
void rt_shortcuts_unregister(rt_string id) {
    (void)id;
}

/// @brief Remove all entries from the shortcuts.
void rt_shortcuts_clear(void) {}

/// @brief Stub: report no shortcut trigger when graphics is disabled.
/// @param id Ignored runtime identifier.
/// @return Always zero.
int64_t rt_shortcuts_was_triggered(rt_string id) {
    (void)id;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; no triggered shortcut state to clear.
/// @param app Ignored app pointer.
void rt_shortcuts_clear_triggered(rt_gui_app_t *app) {
    (void)app;
}

/// @brief Stub: graphics disabled — returns 0; no shortcut can be triggered without graphics.
/// @param app Ignored app pointer.
/// @param key Ignored key code.
/// @param mods Ignored modifier mask.
/// @return Always zero.
int8_t rt_shortcuts_check_key(rt_gui_app_t *app, int key, int mods) {
    (void)app;
    (void)key;
    (void)mods;
    return 0;
}

/// @brief Stub: graphics disabled — returns empty string; no shortcut was triggered.
/// @return Canonical empty runtime string.
rt_string rt_shortcuts_get_triggered(void) {
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no-op; no shortcut to enable or disable.
/// @param id Ignored runtime identifier.
/// @param enabled Ignored enabled flag.
void rt_shortcuts_set_enabled(rt_string id, int64_t enabled) {
    (void)id;
    (void)enabled;
}

/// @brief Stub: graphics disabled — returns 0; no shortcuts exist to query.
/// @param id Ignored runtime identifier.
/// @return Always zero.
int64_t rt_shortcuts_is_enabled(rt_string id) {
    (void)id;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; global shortcut flag has no effect.
/// @param enabled Ignored global enabled flag.
void rt_shortcuts_set_global_enabled(int64_t enabled) {
    (void)enabled;
}

/// @brief Stub: graphics disabled — returns 0; global shortcuts are always inactive.
/// @return Always zero.
int64_t rt_shortcuts_get_global_enabled(void) {
    return 0;
}

/// @brief Stub: ignore app title changes when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @param title Ignored runtime title.
void rt_app_set_title(void *app, rt_string title) {
    (void)app;
    (void)title;
}

/// @brief Stub: return an empty app title when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Canonical empty runtime string.
rt_string rt_app_get_title(void *app) {
    (void)app;
    return rt_str_empty();
}

/// @brief Stub: graphics disabled — no-op; no window exists to resize.
/// @param app Ignored candidate app handle.
/// @param width Ignored logical width.
/// @param height Ignored logical height.
void rt_app_set_size(void *app, int64_t width, int64_t height) {
    (void)app;
    (void)width;
    (void)height;
}

/// @brief Stub: report zero app width when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_width(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: report zero app height when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_height(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no HiDPI scaling, returns 1.0.
/// @param app Ignored candidate app handle.
/// @return Always 1.0.
double rt_app_get_scale(void *app) {
    (void)app;
    return 1.0;
}

/// @brief Stub: graphics disabled — no window.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_logical_width(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no window.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_logical_height(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — scale 1.0, value passes through.
/// @param app Ignored candidate app handle.
/// @param physical Physical value returned unchanged.
/// @return @p physical unchanged.
int64_t rt_app_to_logical(void *app, int64_t physical) {
    (void)app;
    return physical;
}

/// @brief Stub: graphics disabled — scale 1.0, value passes through.
/// @param app Ignored candidate app handle.
/// @param logical Logical value returned unchanged.
/// @return @p logical unchanged.
int64_t rt_app_to_physical(void *app, int64_t logical) {
    (void)app;
    return logical;
}

/// @brief Graphics-disabled smooth-scroll setter stub.
/// @param app Ignored candidate app handle.
/// @param enabled Ignored enabled flag.
void rt_app_set_smooth_scroll(void *app, int64_t enabled) {
    (void)app;
    (void)enabled;
}

/// @brief Graphics-disabled smooth-scroll getter stub.
/// @param app Ignored candidate app handle.
/// @return Always one, preserving the default requested policy.
int64_t rt_app_get_smooth_scroll(void *app) {
    (void)app;
    return 1;
}

/// @brief Stub: ignore UI-scale changes when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @param scale Ignored user zoom multiplier.
void rt_app_set_ui_scale(void *app, double scale) {
    (void)app;
    (void)scale;
}

/// @brief Stub: graphics disabled — returns 1.0.
/// @param app Ignored candidate app handle.
/// @return Always 1.0.
double rt_app_get_ui_scale(void *app) {
    (void)app;
    return 1.0;
}

/// @brief Stub: graphics-disabled builds use the identity effective scale.
/// @details The app handle is intentionally ignored because no platform window or user-scaled
///          retained theme exists in this build configuration.
/// @param app Ignored GUI application handle.
/// @return Always 1.0.
double rt_app_get_effective_scale(void *app) {
    (void)app;
    return 1.0;
}

/// @brief Stub: `App.SetWheelSpeed` is a no-op without graphics.
/// @param app Ignored candidate app handle.
/// @param speed Ignored wheel sensitivity.
void rt_app_set_wheel_speed(void *app, double speed) {
    (void)app;
    (void)speed;
}

/// @brief Stub: graphics disabled — returns 1.0.
/// @param app Ignored candidate app handle.
/// @return Always 1.0.
double rt_app_get_wheel_speed(void *app) {
    (void)app;
    return 1.0;
}

/// @brief Stub: ignore window movement when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
void rt_app_set_position(void *app, int64_t x, int64_t y) {
    (void)app;
    (void)x;
    (void)y;
}

/// @brief Stub: graphics disabled — returns 0; no window position exists.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_x(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — returns 0; no window position exists.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_y(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: ignore minimization when graphics is disabled.
/// @param app Ignored candidate app handle.
void rt_app_minimize(void *app) {
    (void)app;
}

/// @brief Stub: ignore maximization when graphics is disabled.
/// @param app Ignored candidate app handle.
void rt_app_maximize(void *app) {
    (void)app;
}

/// @brief Stub: ignore window restoration when graphics is disabled.
/// @param app Ignored candidate app handle.
void rt_app_restore(void *app) {
    (void)app;
}

/// @brief Stub: report window non-minimized when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_is_minimized(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: report window non-maximized when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_is_maximized(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; no window to enter or exit fullscreen.
/// @param app Ignored candidate app handle.
/// @param fullscreen Ignored fullscreen flag.
void rt_app_set_fullscreen(void *app, int64_t fullscreen) {
    (void)app;
    (void)fullscreen;
}

/// @brief Stub: report window non-fullscreen when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_is_fullscreen(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: ignore focus requests when graphics is disabled.
/// @param app Ignored candidate app handle.
void rt_app_focus(void *app) {
    (void)app;
}

/// @brief Stub: graphics disabled — no-op; no window to activate.
/// @param app Ignored candidate app handle.
void rt_app_activate(void *app) {
    (void)app;
}

/// @brief Stub: report app unfocused when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_is_focused(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no frames are ever painted.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_paint_frames_full(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no frames are ever painted.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_paint_frames_partial(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; there is no render path to toggle.
/// @param app Ignored candidate app handle.
/// @param enabled Ignored partial-paint flag.
void rt_app_set_partial_paint(void *app, int64_t enabled) {
    (void)app;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no-op; no window close event can be intercepted.
/// @param app Ignored candidate app handle.
/// @param prevent Ignored close-prevention flag.
void rt_app_set_prevent_close(void *app, int64_t prevent) {
    (void)app;
    (void)prevent;
}

/// @brief Stub: report no close request when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_was_close_requested(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: report zero monitor width when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_monitor_width(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: report zero monitor height when graphics is disabled.
/// @param app Ignored candidate app handle.
/// @return Always zero.
int64_t rt_app_get_monitor_height(void *app) {
    (void)app;
    return 0;
}

/// @brief Stub: graphics disabled — no-op; no window exists to resize.
/// @param app Ignored candidate app handle.
/// @param w Ignored logical width.
/// @param h Ignored logical height.
void rt_app_set_window_size(void *app, int64_t w, int64_t h) {
    (void)app;
    (void)w;
    (void)h;
}

/// @brief Stub: graphics disabled — no-op; there is no native window to constrain.
/// @param app Ignored candidate app handle.
/// @param w Ignored logical minimum width.
/// @param h Ignored logical minimum height.
void rt_app_set_minimum_size(void *app, int64_t w, int64_t h) {
    (void)app;
    (void)w;
    (void)h;
}

/// @brief Stub: graphics disabled — returns default 14.0 pt; no real app window exists.
/// @param app Ignored candidate app handle.
/// @return Always 14.0 logical points.
double rt_app_get_font_size(void *app) {
    (void)app;
    return 14.0;
}

/// @brief Stub: return the graphics-disabled logical default font size.
/// @details This mirrors `GetFontSize` and provides a stable logical-unit contract even when GUI
///          construction is unavailable.
/// @param app Ignored GUI application handle.
/// @return Always 14.0 logical points.
double rt_app_get_logical_font_size(void *app) {
    (void)app;
    return 14.0;
}

/// @brief Stub: graphics disabled — no-op; no app window font size to set.
/// @param app Ignored candidate app handle.
/// @param size Ignored logical font size.
void rt_app_set_font_size(void *app, double size) {
    (void)app;
    (void)size;
}

/// @brief Stub: graphics disabled — no-op; no window cursor to change.
/// @param type Ignored cursor kind.
void rt_cursor_set(int64_t type) {
    (void)type;
}

/// @brief Stub: graphics disabled — no-op; no window cursor to reset.
void rt_cursor_reset(void) {}

/// @brief Stub: ignore cursor visibility when graphics is disabled.
/// @param visible Ignored visibility flag.
void rt_cursor_set_visible(int64_t visible) {
    (void)visible;
}

/// @brief Stub: graphics disabled — no-op; no widget or cursor exists to change.
/// @param widget Ignored candidate widget handle.
/// @param type Ignored cursor kind.
void rt_widget_set_cursor(void *widget, int64_t type) {
    (void)widget;
    (void)type;
}

/// @brief Stub: graphics disabled — no-op; no widget or cursor exists to reset.
/// @param widget Ignored candidate widget handle.
void rt_widget_reset_cursor(void *widget) {
    (void)widget;
}

#endif /* ZANNA_ENABLE_GRAPHICS */

//=============================================================================
// Zanna.System.Clipboard compatibility surface
//=============================================================================

/// @brief Copy text to the system clipboard via the canonical GUI clipboard backend.
/// @param text Runtime text forwarded unchanged to @ref rt_clipboard_set_text.
void rt_system_clipboard_set(rt_string text) {
    rt_clipboard_set_text(text);
}

/// @brief Read UTF-8 text from the system clipboard via the canonical GUI clipboard backend.
/// @return Runtime clipboard text or the backend's canonical empty fallback.
rt_string rt_system_clipboard_get(void) {
    return rt_clipboard_get_text();
}

/// @brief Check whether the system clipboard currently exposes text.
/// @return One when the canonical backend reports text, otherwise zero.
int64_t rt_system_clipboard_has_text(void) {
    return rt_clipboard_has_text() ? 1 : 0;
}
