//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_filedialog.c
/// @brief Implements open, save, and folder-selection file dialogs.
///
/// @details The dialog combines a bookmark sidebar, filtered directory listing,
/// multi-selection support, and an inline UTF-8 filename editor for save mode.
/// Platform adapters provide path operations and directory enumeration, while
/// this file owns cross-platform sorting, filtering, navigation, rendering, and
/// interaction.
///
/// Loaded entries are sorted with directories first and ASCII
/// case-insensitive names second. File and bookmark scrolling use independently
/// clamped floating offsets. The filename cursor is a UTF-8 byte offset and
/// always moves across whole encoded sequences. Save confirmation appends the
/// configured default extension only when the filename has none.
///
/// The widget owns entries, filters, bookmarks, selected result paths, current
/// path, default filename, and default extension. It embeds `vg_dialog_t`; the
/// actual widget header is `base.base`.
///
/// @see vg_ide_widgets.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "vg_filedialog_platform.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void filedialog_destroy(vg_widget_t *widget);
static void filedialog_measure(vg_widget_t *widget, float available_width, float available_height);
static void filedialog_paint(vg_widget_t *widget, void *canvas);
static bool filedialog_handle_event(vg_widget_t *widget, vg_event_t *event);

#define FILEDIALOG_TITLE_HEIGHT 35.0f
#define FILEDIALOG_SIDEBAR_WIDTH 150.0f
#define FILEDIALOG_PATH_HEIGHT 30.0f
#define FILEDIALOG_ROW_HEIGHT 24.0f
#define FILEDIALOG_BOOKMARK_HEIGHT 25.0f
#define FILEDIALOG_BUTTON_WIDTH 80.0f
#define FILEDIALOG_BUTTON_HEIGHT 28.0f
#define FILEDIALOG_BUTTON_MARGIN 8.0f
#define FILEDIALOG_CLOSE_BUTTON_SIZE 20.0f
#define FILEDIALOG_FILENAME_HEIGHT 28.0f
#define FILEDIALOG_BOTTOM_HEIGHT 54.0f
#define FILEDIALOG_SAVE_EXTRA_HEIGHT 34.0f

static vg_filedialog_modal_runner_t g_modal_runner = NULL;
static void *g_modal_runner_user_data = NULL;

/// @brief Duplicates a null-terminated string for file-dialog ownership.
///
/// @param text Source string; may be null.
/// @return Owned copy, or null for null input or allocation failure.
static char *filedialog_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text);
    char *copy = malloc(len + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, text, len + 1u);
    return copy;
}

//=============================================================================
// FileDialog VTable
//=============================================================================

static vg_widget_vtable_t g_filedialog_vtable = {.destroy = filedialog_destroy,
                                                 .measure = filedialog_measure,
                                                 .arrange = NULL,
                                                 .paint = filedialog_paint,
                                                 .handle_event = filedialog_handle_event,
                                                 .can_focus = NULL,
                                                 .on_focus = NULL};

//=============================================================================
// Platform Abstraction Layer
//=============================================================================

/// @brief Case-insensitive glob match with `*` and `?` wildcards.
/// @details Used for file-extension filters on every platform so matching does
///          not depend on non-portable `fnmatch()` extension flags.  ASCII case
///          folding is sufficient for extension-style patterns such as
///          `*.png;*.jpg`.
/// @param pattern Glob pattern to evaluate.
/// @param filename Candidate filename.
/// @return true when @p filename matches @p pattern.
static bool filedialog_match_pattern_ci(const char *pattern, const char *filename) {
    const char *p = pattern;
    const char *f = filename;

    while (*p && *f) {
        if (*p == '*') {
            p++;
            if (!*p)
                return true; // Trailing * matches everything
            // Try to match rest of pattern at each position
            while (*f) {
                if (filedialog_match_pattern_ci(p, f))
                    return true;
                f++;
            }
            return false;
        } else if (*p == '?') {
            p++;
            f++;
        } else {
            // Case-insensitive compare
            char pc = *p, fc = *f;
            if (pc >= 'A' && pc <= 'Z')
                pc += 32;
            if (fc >= 'A' && fc <= 'Z')
                fc += 32;
            if (pc != fc)
                return false;
            p++;
            f++;
        }
    }

    while (*p == '*')
        p++; // Skip trailing asterisks
    return !*p && !*f;
}

/// @brief Returns the current user's home directory from the platform adapter.
///
/// @return Owned path string, or null when the location cannot be resolved.
static char *get_home_directory(void) {
    return vg_filedialog_platform_home_dir();
}

/// @brief Joins a directory and leaf name through the platform path adapter.
///
/// @param dir Directory path.
/// @param file Leaf filename or relative path.
/// @return Owned joined path, or null on invalid input or allocation failure.
static char *join_path(const char *dir, const char *file) {
    return vg_filedialog_platform_join_path(dir, file);
}

/// @brief Returns the parent directory of a path through the platform adapter.
///
/// @param path Path whose parent is requested.
/// @return Owned parent path, or null when no parent can be produced.
static char *get_parent_directory(const char *path) {
    return vg_filedialog_platform_parent_dir(path);
}

/// @brief Resolves the screen-space origin of a widget's parent.
///
/// @param widget Widget whose parent is queried; may be null.
/// @param[out] x Optional receiver for the screen X coordinate.
/// @param[out] y Optional receiver for the screen Y coordinate.
static void get_parent_screen_origin(vg_widget_t *widget, float *x, float *y) {
    float sx = 0.0f;
    float sy = 0.0f;
    if (widget && widget->parent) {
        vg_widget_get_screen_bounds(widget->parent, &sx, &sy, NULL, NULL);
    }
    if (x)
        *x = sx;
    if (y)
        *y = sy;
}

/// @brief Returns the action-area height for the current dialog mode.
///
/// @param dialog File dialog to inspect.
/// @return Base bottom height plus the filename-row height in save mode.
static float filedialog_bottom_height(const vg_filedialog_t *dialog) {
    return FILEDIALOG_BOTTOM_HEIGHT +
           (dialog && dialog->mode == VG_FILEDIALOG_SAVE ? FILEDIALOG_SAVE_EXTRA_HEIGHT : 0.0f);
}

/// @brief Returns the mode-specific accept-button label.
///
/// @param dialog File dialog whose mode is inspected.
/// @return Borrowed immutable label: `Open`, `Save`, `Select`, or `OK` for null.
static const char *filedialog_accept_label(const vg_filedialog_t *dialog) {
    if (!dialog)
        return "OK";
    switch (dialog->mode) {
        case VG_FILEDIALOG_SAVE:
            return "Save";
        case VG_FILEDIALOG_SELECT_FOLDER:
            return "Select";
        case VG_FILEDIALOG_OPEN:
        default:
            return "Open";
    }
}

/// @brief Tests whether a filename already contains an extension.
///
/// @param filename Filename or path to inspect.
/// @return `true` when a dot appears after the final separator and is not the
///         first character after that separator.
static bool filedialog_filename_has_extension(const char *filename) {
    if (!filename || !*filename)
        return false;

    const char *last_slash = NULL;
    for (const char *p = filename; *p; ++p) {
        if (vg_filedialog_platform_is_separator(*p))
            last_slash = p;
    }
    const char *last_dot = strrchr(filename, '.');
    return last_dot && (!last_slash || last_dot > last_slash + 1);
}

/// @brief Replaces the default filename and moves the cursor to its end.
///
/// @param dialog File dialog to update.
/// @param filename Filename to copy; may be null to clear.
static void filedialog_set_default_filename(vg_filedialog_t *dialog, const char *filename) {
    if (!dialog)
        return;
    char *copy = filedialog_strdup(filename);
    if (filename && !copy)
        return;
    free(dialog->default_filename);
    dialog->default_filename = copy;
    dialog->filename_cursor_pos = dialog->default_filename ? strlen(dialog->default_filename) : 0;
}

/// @brief Finds the UTF-8 code-point boundary before a byte cursor.
///
/// @param text Null-terminated UTF-8 text.
/// @param cursor Current byte offset, clamped to the text length.
/// @return Previous leading-byte offset, or zero at the beginning.
static size_t filedialog_prev_codepoint_boundary(const char *text, size_t cursor) {
    size_t len = 0;
    if (!text)
        return 0;
    len = strlen(text);
    if (cursor > len)
        cursor = len;
    if (cursor == 0)
        return 0;
    do {
        cursor--;
    } while (cursor > 0 && (((unsigned char)text[cursor] & 0xC0) == 0x80));
    return cursor;
}

/// @brief Finds the UTF-8 code-point boundary after a byte cursor.
///
/// @param text Null-terminated UTF-8 text.
/// @param cursor Current byte offset.
/// @return Next leading-byte offset, or the string length at the end.
static size_t filedialog_next_codepoint_boundary(const char *text, size_t cursor) {
    size_t len = 0;
    if (!text)
        return 0;
    len = strlen(text);
    if (cursor >= len)
        return len;
    cursor++;
    while (cursor < len && (((unsigned char)text[cursor] & 0xC0) == 0x80)) {
        cursor++;
    }
    return cursor;
}

/// @brief Clamps the filename cursor to the current byte length.
///
/// @param dialog File dialog whose editor cursor is normalized.
static void filedialog_sync_filename_cursor(vg_filedialog_t *dialog) {
    size_t len = 0;
    if (!dialog)
        return;
    len = dialog->default_filename ? strlen(dialog->default_filename) : 0;
    if (dialog->filename_cursor_pos > len)
        dialog->filename_cursor_pos = len;
}

/// @brief Deletes the UTF-8 sequence immediately before the filename cursor.
///
/// @param dialog Save dialog whose editable filename is modified.
static void filedialog_delete_last_codepoint(vg_filedialog_t *dialog) {
    char *text = NULL;
    size_t cursor = 0;
    size_t prev = 0;
    size_t len = 0;

    if (!dialog || !dialog->default_filename)
        return;

    text = dialog->default_filename;
    filedialog_sync_filename_cursor(dialog);
    cursor = dialog->filename_cursor_pos;
    if (cursor == 0)
        return;

    prev = filedialog_prev_codepoint_boundary(text, cursor);
    len = strlen(text);
    memmove(text + prev, text + cursor, len - cursor + 1);
    dialog->filename_cursor_pos = prev;
}

/// @brief Deletes the UTF-8 sequence at the filename cursor.
///
/// @param dialog Save dialog whose editable filename is modified.
static void filedialog_delete_codepoint_at_cursor(vg_filedialog_t *dialog) {
    char *text = NULL;
    size_t cursor = 0;
    size_t next = 0;
    size_t len = 0;

    if (!dialog || !dialog->default_filename)
        return;

    text = dialog->default_filename;
    filedialog_sync_filename_cursor(dialog);
    cursor = dialog->filename_cursor_pos;
    len = strlen(text);
    if (cursor >= len)
        return;

    next = filedialog_next_codepoint_boundary(text, cursor);
    memmove(text + cursor, text + next, len - next + 1);
}

/// @brief Inserts one Unicode scalar at the filename cursor.
///
/// @details The code point is encoded as UTF-8 and inserted into the resized
/// filename buffer. Control characters, invalid scalars, and allocation failure
/// leave the value unchanged.
///
/// @param dialog Save dialog whose filename is modified.
/// @param codepoint Unicode scalar to insert.
static void filedialog_append_codepoint(vg_filedialog_t *dialog, uint32_t codepoint) {
    char encoded[5] = {0};
    size_t encoded_len = 0;
    size_t old_len = 0;
    size_t insert_at = 0;
    char *new_name = NULL;

    if (!dialog || codepoint < 0x20 || codepoint == 0x7F || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return;

    if (codepoint < 0x80) {
        encoded[0] = (char)codepoint;
        encoded_len = 1;
    } else if (codepoint < 0x800) {
        encoded[0] = (char)(0xC0 | (codepoint >> 6));
        encoded[1] = (char)(0x80 | (codepoint & 0x3F));
        encoded_len = 2;
    } else if (codepoint < 0x10000) {
        encoded[0] = (char)(0xE0 | (codepoint >> 12));
        encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[2] = (char)(0x80 | (codepoint & 0x3F));
        encoded_len = 3;
    } else {
        encoded[0] = (char)(0xF0 | (codepoint >> 18));
        encoded[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        encoded[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[3] = (char)(0x80 | (codepoint & 0x3F));
        encoded_len = 4;
    }

    old_len = dialog->default_filename ? strlen(dialog->default_filename) : 0;
    insert_at = dialog->filename_cursor_pos <= old_len ? dialog->filename_cursor_pos : old_len;
    new_name = realloc(dialog->default_filename, old_len + encoded_len + 1);
    if (!new_name)
        return;
    dialog->default_filename = new_name;
    memmove(dialog->default_filename + insert_at + encoded_len,
            dialog->default_filename + insert_at,
            old_len - insert_at + 1);
    memcpy(dialog->default_filename + insert_at, encoded, encoded_len);
    dialog->filename_cursor_pos = insert_at + encoded_len;
}

/// @brief Returns the usable list height after internal margins.
///
/// @param list_height Allocated list-region height.
/// @return Non-negative viewport height.
static float filedialog_list_view_height(float list_height) {
    float view_h = list_height - 10.0f;
    return view_h > 0.0f ? view_h : 0.0f;
}

/// @brief Computes the maximum vertical scroll offset for fixed-height rows.
///
/// @param item_count Number of content rows.
/// @param row_height Height of each row.
/// @param view_height Visible viewport height.
/// @return Non-negative maximum scroll position.
static float filedialog_max_scroll(size_t item_count, float row_height, float view_height) {
    float content_h = (float)item_count * row_height;
    float max_scroll = content_h - view_height;
    return max_scroll > 0.0f ? max_scroll : 0.0f;
}

/// @brief Clamps file-list and bookmark scroll offsets to valid ranges.
///
/// @param dialog File dialog whose scroll state is normalized.
/// @param list_height Shared allocated list-region height.
static void filedialog_clamp_scrolls(vg_filedialog_t *dialog, float list_height) {
    float view_h = filedialog_list_view_height(list_height);
    float max_file_scroll = 0.0f;
    float max_bookmark_scroll = 0.0f;

    if (!dialog)
        return;

    max_file_scroll = filedialog_max_scroll(dialog->entry_count, FILEDIALOG_ROW_HEIGHT, view_h);
    max_bookmark_scroll =
        filedialog_max_scroll(dialog->bookmark_count, FILEDIALOG_BOOKMARK_HEIGHT, view_h);

    if (dialog->file_scroll_y < 0.0f)
        dialog->file_scroll_y = 0.0f;
    if (dialog->file_scroll_y > max_file_scroll)
        dialog->file_scroll_y = max_file_scroll;
    if (dialog->bookmark_scroll_y < 0.0f)
        dialog->bookmark_scroll_y = 0.0f;
    if (dialog->bookmark_scroll_y > max_bookmark_scroll)
        dialog->bookmark_scroll_y = max_bookmark_scroll;
}

/// @brief Scrolls the first selected entry into the visible file list.
///
/// @param dialog File dialog whose file scroll offset may change.
/// @param list_height Allocated list-region height.
static void filedialog_scroll_selection_into_view(vg_filedialog_t *dialog, float list_height) {
    float view_h = filedialog_list_view_height(list_height);
    float item_top = 0.0f;
    float item_bottom = 0.0f;

    if (!dialog || dialog->selection_count == 0)
        return;

    item_top = (float)dialog->selected_indices[0] * FILEDIALOG_ROW_HEIGHT;
    item_bottom = item_top + FILEDIALOG_ROW_HEIGHT;
    if (item_top < dialog->file_scroll_y)
        dialog->file_scroll_y = item_top;
    else if (item_bottom > dialog->file_scroll_y + view_h)
        dialog->file_scroll_y = item_bottom - view_h;
    if (dialog->file_scroll_y < 0.0f)
        dialog->file_scroll_y = 0.0f;
}

/// @brief Computes a clipped text origin with optional end alignment.
///
/// @param font Font used to measure @p text.
/// @param font_size Font size used for measurement.
/// @param text Text to position.
/// @param base_x Normal left-aligned origin.
/// @param available_w Available clip width.
/// @param align_end Whether overflowing text should expose its trailing edge.
/// @return Horizontal text origin.
static float filedialog_text_origin(vg_font_t *font,
                                    float font_size,
                                    const char *text,
                                    float base_x,
                                    float available_w,
                                    bool align_end) {
    vg_text_metrics_t metrics = {0};

    if (!font || !text || available_w <= 0.0f)
        return base_x;

    vg_font_measure_text(font, font_size, text, &metrics);
    if (align_end && metrics.width > available_w)
        return base_x + available_w - metrics.width;
    return base_x;
}

/// @brief Draws text inside a temporary clipping rectangle.
///
/// @param canvas Destination drawing context.
/// @param font Font used for rendering.
/// @param font_size Font size.
/// @param clip_x Clip rectangle left edge.
/// @param clip_y Clip rectangle top edge.
/// @param clip_w Clip rectangle width.
/// @param clip_h Clip rectangle height.
/// @param text_x Text origin X coordinate.
/// @param text_y Text baseline Y coordinate.
/// @param text Null-terminated string to draw.
/// @param color Packed text color.
static void filedialog_draw_clipped_text(void *canvas,
                                         vg_font_t *font,
                                         float font_size,
                                         float clip_x,
                                         float clip_y,
                                         float clip_w,
                                         float clip_h,
                                         float text_x,
                                         float text_y,
                                         const char *text,
                                         uint32_t color) {
    vgfx_window_t win = (vgfx_window_t)canvas;
    if (!canvas || !font || !text || clip_w <= 0.0f || clip_h <= 0.0f)
        return;
    vgfx_set_clip(win, (int32_t)clip_x, (int32_t)clip_y, (int32_t)clip_w, (int32_t)clip_h);
    vg_font_draw_text(canvas, font, font_size, text_x, text_y, text, color);
    vgfx_clear_clip(win);
}

/// @brief Converts a local list coordinate and scroll offset to an item index.
///
/// @param local_y Vertical coordinate relative to the list viewport.
/// @param scroll_y Current content scroll offset.
/// @param row_height Fixed row height.
/// @param item_count Number of rows.
/// @return Zero-based item index, or `SIZE_MAX` outside the populated range.
static size_t filedialog_index_from_scroll(float local_y,
                                           float scroll_y,
                                           float row_height,
                                           size_t item_count) {
    size_t index = 0;
    if (local_y < 0.0f || row_height <= 0.0f)
        return SIZE_MAX;
    index = (size_t)((local_y + scroll_y) / row_height);
    return index < item_count ? index : SIZE_MAX;
}

/// @brief Tests whether a path is absolute under platform rules.
///
/// @param path Path to inspect.
/// @return `true` when the platform adapter recognizes an absolute path.
static bool filedialog_absolute_path(const char *path) {
    return vg_filedialog_platform_is_absolute_path(path);
}

/// @brief ASCII case-insensitive strcmp for stable file-dialog sorting.
/// @details The dialog only needs deterministic ASCII folding for names and
///          extensions; avoiding locale-specific collation keeps ordering
///          portable and removes platform C library branching from the widget.
/// @param a First string; NULL sorts before non-NULL strings.
/// @param b Second string; NULL sorts before non-NULL strings.
/// @return Negative, zero, or positive using strcmp-style ordering.
static int filedialog_ascii_casecmp(const char *a, const char *b) {
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return (int)ca - (int)cb;
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/// @brief Orders directory-entry pointers for display.
///
/// @param a Address of the first `vg_file_entry_t` pointer.
/// @param b Address of the second `vg_file_entry_t` pointer.
/// @return Negative, zero, or positive with directories before files and names
///         compared using stable ASCII case folding.
static int compare_entries(const void *a, const void *b) {
    const vg_file_entry_t *const *entry_a = (const vg_file_entry_t *const *)a;
    const vg_file_entry_t *const *entry_b = (const vg_file_entry_t *const *)b;
    const vg_file_entry_t *ea = *entry_a;
    const vg_file_entry_t *eb = *entry_b;

    // Directories first
    if (ea->is_directory && !eb->is_directory)
        return -1;
    if (!ea->is_directory && eb->is_directory)
        return 1;

    // Then alphabetically (case-insensitive)
    return filedialog_ascii_casecmp(ea->name, eb->name);
}

/// @brief Matches a filename against semicolon-separated filter patterns.
///
/// @details Empty, `*`, and `*.*` patterns match every name. Other patterns are
/// copied, split, trimmed, and evaluated with the portable wildcard matcher.
///
/// @param filename Candidate filename.
/// @param pattern Filter expression containing one or more glob patterns.
/// @return `true` when any pattern matches; otherwise `false`.
static bool match_filter(const char *filename, const char *pattern) {
    if (!pattern || !*pattern || strcmp(pattern, "*") == 0 || strcmp(pattern, "*.*") == 0) {
        return true;
    }

    // Pattern can be multiple patterns separated by semicolons
    char *patterns = filedialog_strdup(pattern);
    if (!patterns)
        return false;

    char *cursor = patterns;
    while (cursor) {
        char *token = cursor;
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
            cursor = next + 1;
        } else {
            cursor = NULL;
        }

        // Trim whitespace
        while (*token == ' ')
            token++;
        size_t token_len = strlen(token);
        while (token_len > 0 && token[token_len - 1] == ' ')
            token[--token_len] = '\0';
        if (token_len == 0)
            continue;

        if (filedialog_match_pattern_ci(token, filename)) {
            free(patterns);
            return true;
        }
    }

    free(patterns);
    return false;
}

/// @brief Releases one file entry and both strings it owns.
///
/// @param entry Entry to destroy; may be null.
static void free_entry(vg_file_entry_t *entry) {
    if (entry) {
        free(entry->name);
        free(entry->full_path);
        free(entry);
    }
}

/// @brief Releases all populated entries while retaining pointer-array capacity.
///
/// @param dialog File dialog whose directory entries are cleared.
static void clear_entries(vg_filedialog_t *dialog) {
    for (size_t i = 0; i < dialog->entry_count; i++) {
        free_entry(dialog->entries[i]);
    }
    dialog->entry_count = 0;
}

/// @brief Loads, filters, and sorts one directory into the dialog.
///
/// @details Platform entries are copied into widget-owned records. Hidden,
/// non-folder, and active-filter exclusions are applied as appropriate to the
/// current mode. Partial per-entry allocation failures skip those entries while
/// preserving successfully materialized results.
///
/// @param dialog File dialog to populate.
/// @param path Directory path to enumerate and copy.
static void load_directory(vg_filedialog_t *dialog, const char *path) {
    if (!dialog || !path)
        return;

    char *new_current_path = filedialog_strdup(path);
    if (!new_current_path)
        return;

    vg_filedialog_platform_entry_t *platform_entries = NULL;
    size_t platform_entry_count = 0;
    if (!vg_filedialog_platform_list_directory(path, &platform_entries, &platform_entry_count)) {
        free(new_current_path);
        return;
    }

    clear_entries(dialog);
    free(dialog->current_path);
    dialog->current_path = new_current_path;

    for (size_t i = 0; i < platform_entry_count; i++) {
        vg_filedialog_platform_entry_t *src = &platform_entries[i];
        if (!dialog->show_hidden && src->is_hidden)
            continue;

        // In folder select mode, only show directories
        if (dialog->mode == VG_FILEDIALOG_SELECT_FOLDER && !src->is_directory)
            continue;

        // Apply filter for non-directories
        if (!src->is_directory && dialog->filter_count > 0 &&
            dialog->active_filter < dialog->filter_count) {
            if (!match_filter(src->name, dialog->filters[dialog->active_filter].pattern))
                continue;
        }

        // Create entry
        vg_file_entry_t *fe = calloc(1, sizeof(vg_file_entry_t));
        if (!fe)
            continue;

        fe->name = filedialog_strdup(src->name);
        fe->full_path = filedialog_strdup(src->full_path);
        if (!fe->name || !fe->full_path) {
            free_entry(fe);
            continue;
        }
        fe->is_directory = src->is_directory;
        fe->size = (int64_t)src->size;
        fe->modified_time = src->modified_time;

        // Add to array
        if (dialog->entry_count >= dialog->entry_capacity) {
            if (dialog->entry_capacity > SIZE_MAX / (2u * sizeof(vg_file_entry_t *))) {
                free_entry(fe);
                continue;
            }
            size_t new_cap = dialog->entry_capacity == 0 ? 64 : dialog->entry_capacity * 2;
            vg_file_entry_t **new_entries =
                realloc(dialog->entries, new_cap * sizeof(vg_file_entry_t *));
            if (!new_entries) {
                free_entry(fe);
                continue;
            }
            dialog->entries = new_entries;
            dialog->entry_capacity = new_cap;
        }

        dialog->entries[dialog->entry_count++] = fe;
    }

    vg_filedialog_platform_free_entries(platform_entries, platform_entry_count);

    // Sort entries
    if (dialog->entry_count > 0) {
        qsort(dialog->entries, dialog->entry_count, sizeof(vg_file_entry_t *), compare_entries);
    }

    // Clear selection
    dialog->selection_count = 0;
    dialog->file_scroll_y = 0.0f;
    dialog->bookmark_scroll_y = 0.0f;
}

/// @brief Selects or toggles one entry according to multi-select mode.
///
/// @param dialog File dialog whose index array is updated.
/// @param index Zero-based entry index.
static void select_entry(vg_filedialog_t *dialog, size_t index) {
    if (index >= dialog->entry_count)
        return;
    if (index > (size_t)INT_MAX)
        return;

    if (!dialog->multi_select) {
        if (dialog->selection_capacity == 0) {
            dialog->selected_indices = malloc(sizeof(int));
            if (!dialog->selected_indices)
                return;
            dialog->selection_capacity = 1;
        }
        dialog->selection_count = 1;
        dialog->selected_indices[0] = (int)index;
    } else {
        // Toggle selection
        bool found = false;
        for (size_t i = 0; i < dialog->selection_count; i++) {
            if (dialog->selected_indices[i] == (int)index) {
                // Remove from selection
                for (size_t j = i; j < dialog->selection_count - 1; j++) {
                    dialog->selected_indices[j] = dialog->selected_indices[j + 1];
                }
                dialog->selection_count--;
                found = true;
                break;
            }
        }

        if (!found) {
            // Add to selection
            if (dialog->selection_count >= dialog->selection_capacity) {
                if (dialog->selection_capacity > SIZE_MAX / (2u * sizeof(int)))
                    return;
                size_t new_cap =
                    dialog->selection_capacity == 0 ? 8 : dialog->selection_capacity * 2;
                int *new_indices = realloc(dialog->selected_indices, new_cap * sizeof(int));
                if (!new_indices)
                    return;
                dialog->selected_indices = new_indices;
                dialog->selection_capacity = new_cap;
            }
            dialog->selected_indices[dialog->selection_count++] = (int)index;
        }
    }
}

/// @brief Tests whether an entry index belongs to the current selection.
///
/// @param dialog File dialog to inspect.
/// @param index Zero-based entry index.
/// @return `true` when the index is selected; otherwise `false`.
static bool is_selected(vg_filedialog_t *dialog, size_t index) {
    if (!dialog || index > (size_t)INT_MAX)
        return false;

    for (size_t i = 0; i < dialog->selection_count; i++) {
        if (dialog->selected_indices[i] == (int)index)
            return true;
    }
    return false;
}

/// @brief Validates and commits the current file-dialog selection.
///
/// @details Save mode resolves the filename and optional default extension.
/// Open mode navigates into a singly selected directory rather than returning
/// it. Successful confirmation rebuilds the owned result-path array, invokes
/// the selection callback, and closes the dialog.
///
/// @param dialog File dialog whose current selection is confirmed.
static void confirm_selection(vg_filedialog_t *dialog) {
    // Free previous results
    if (dialog->selected_files) {
        for (size_t i = 0; i < dialog->selected_file_count; i++) {
            free(dialog->selected_files[i]);
        }
        free(dialog->selected_files);
        dialog->selected_files = NULL;
        dialog->selected_file_count = 0;
    }

    if (dialog->mode == VG_FILEDIALOG_SAVE) {
        const char *filename = dialog->default_filename;
        if ((!filename || !filename[0]) && dialog->selection_count > 0) {
            int idx = dialog->selected_indices[0];
            if (idx >= 0 && (size_t)idx < dialog->entry_count &&
                !dialog->entries[idx]->is_directory)
                filename = dialog->entries[idx]->name;
        }

        if (!filename || !filename[0])
            return;

        char *save_name = NULL;
        save_name = filedialog_strdup(filename);
        if (!save_name)
            return;

        if (dialog->default_extension && dialog->default_extension[0] &&
            !filedialog_filename_has_extension(save_name)) {
            size_t name_len = strlen(save_name);
            size_t ext_len = strlen(dialog->default_extension);
            bool needs_dot = dialog->default_extension[0] != '.';
            size_t dot_len = needs_dot ? 1u : 0u;
            if (ext_len > SIZE_MAX - dot_len) {
                free(save_name);
                return;
            }
            size_t extra = ext_len + dot_len;
            if (name_len > SIZE_MAX - extra || name_len + extra > SIZE_MAX - 1u) {
                free(save_name);
                return;
            }
            char *with_ext = realloc(save_name, name_len + extra + 1u);
            if (!with_ext) {
                free(save_name);
                return;
            }
            save_name = with_ext;
            if (needs_dot)
                save_name[name_len++] = '.';
            memcpy(save_name + name_len, dialog->default_extension, ext_len + 1);
        }

        char **new_selected_files = malloc(sizeof(char *));
        if (!new_selected_files) {
            free(save_name);
            return;
        }

        if (filedialog_absolute_path(save_name)) {
            new_selected_files[0] = save_name;
        } else {
            new_selected_files[0] = join_path(dialog->current_path, save_name);
            free(save_name);
        }
        if (!new_selected_files[0]) {
            free(new_selected_files);
            return;
        }
        dialog->selected_files = new_selected_files;
        dialog->selected_file_count = 1;
    } else if (dialog->selection_count > 0) {
        int single_idx = dialog->selected_indices[0];
        if (dialog->mode == VG_FILEDIALOG_OPEN && dialog->selection_count == 1 && single_idx >= 0 &&
            (size_t)single_idx < dialog->entry_count && dialog->entries[single_idx]->is_directory) {
            load_directory(dialog, dialog->entries[single_idx]->full_path);
            dialog->base.base.needs_paint = true;
            return;
        }

        if (dialog->selection_count > SIZE_MAX / sizeof(char *))
            return;
        char **new_selected_files = calloc(dialog->selection_count, sizeof(char *));
        size_t new_selected_count = 0;
        if (new_selected_files) {
            for (size_t i = 0; i < dialog->selection_count; i++) {
                int idx = dialog->selected_indices[i];
                if (idx < 0 || (size_t)idx >= dialog->entry_count)
                    continue;
                if (dialog->mode == VG_FILEDIALOG_OPEN && dialog->entries[idx]->is_directory)
                    continue;
                char *path = NULL;
                path = filedialog_strdup(dialog->entries[idx]->full_path);
                if (!path) {
                    for (size_t j = 0; j < new_selected_count; j++)
                        free(new_selected_files[j]);
                    free(new_selected_files);
                    return;
                }
                new_selected_files[new_selected_count++] = path;
            }
            dialog->selected_files = new_selected_files;
            dialog->selected_file_count = new_selected_count;
        }
    } else if (dialog->mode == VG_FILEDIALOG_SELECT_FOLDER) {
        char **new_selected_files = malloc(sizeof(char *));
        if (new_selected_files) {
            new_selected_files[0] = filedialog_strdup(dialog->current_path);
            if (!new_selected_files[0]) {
                free(new_selected_files);
                return;
            }
            dialog->selected_files = new_selected_files;
            dialog->selected_file_count = 1;
        }
    }

    if (dialog->selected_file_count == 0)
        return;

    vg_dialog_close(&dialog->base, VG_DIALOG_RESULT_OK);

    if (dialog->on_select) {
        dialog->on_select(
            dialog, dialog->selected_files, dialog->selected_file_count, dialog->user_data);
    }
}

//=============================================================================
// FileDialog Implementation
//=============================================================================

/// @brief Creates a file dialog for an open, save, or folder-selection workflow.
///
/// @details The mode selects the default title and initial multi-selection
/// policy. The initial directory is the user's home directory with `.` as a
/// fallback. Theme colors initialize the embedded dialog, which starts closed
/// with a 700 by 500 logical-pixel size.
///
/// @param mode VG_FILEDIALOG_OPEN, VG_FILEDIALOG_SAVE, or VG_FILEDIALOG_SELECT_FOLDER.
/// @return Newly allocated file dialog, or null on allocation failure.
vg_filedialog_t *vg_filedialog_create(vg_filedialog_mode_t mode) {
    vg_filedialog_t *dialog = calloc(1, sizeof(vg_filedialog_t));
    if (!dialog)
        return NULL;

    // Initialize base dialog
    const char *title = "Open File";
    if (mode == VG_FILEDIALOG_SAVE)
        title = "Save File";
    else if (mode == VG_FILEDIALOG_SELECT_FOLDER)
        title = "Select Folder";

    // Initialize base widget
    vg_widget_init(&dialog->base.base, VG_WIDGET_DIALOG, &g_filedialog_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Initialize dialog fields
    dialog->base.title = filedialog_strdup(title);
    if (!dialog->base.title) {
        vg_widget_destroy(&dialog->base.base);
        return NULL;
    }
    dialog->base.show_close_button = true;
    dialog->base.draggable = true;
    dialog->base.modal = true;
    dialog->base.min_width = 600;
    dialog->base.min_height = 400;
    dialog->base.resizable = true;
    dialog->base.is_open = false;
    dialog->base.bg_color = theme->colors.bg_primary;
    dialog->base.title_bg_color = theme->colors.bg_secondary;
    dialog->base.title_text_color = theme->colors.fg_primary;
    dialog->base.text_color = theme->colors.fg_primary;
    dialog->base.button_bg_color = theme->colors.bg_tertiary;
    dialog->base.button_hover_color = theme->colors.bg_hover;
    dialog->base.font_size = theme->typography.size_normal;
    dialog->base.title_font_size = theme->typography.size_normal;
    dialog->base.button_preset = VG_DIALOG_BUTTONS_OK_CANCEL;

    // Initialize file dialog fields
    dialog->mode = mode;
    dialog->current_path = get_home_directory();
    if (!dialog->current_path) {
        dialog->current_path = filedialog_strdup(".");
    }
    if (!dialog->current_path) {
        vg_widget_destroy(&dialog->base.base);
        return NULL;
    }
    dialog->show_hidden = false;
    dialog->confirm_overwrite = true;
    dialog->multi_select = (mode == VG_FILEDIALOG_OPEN);

    // Set default size
    dialog->base.base.width = 700;
    dialog->base.base.height = 500;

    return dialog;
}

/// @brief Releases every resource and modal relationship owned by a file dialog.
///
/// @param widget File-dialog base widget being destroyed.
static void filedialog_destroy(vg_widget_t *widget) {
    vg_filedialog_t *dialog = (vg_filedialog_t *)widget;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    if (vg_widget_get_modal_root() == widget)
        vg_widget_set_modal_root(NULL);

    // Free entries
    clear_entries(dialog);
    free(dialog->entries);

    // Free selection
    free(dialog->selected_indices);

    // Free filters
    for (size_t i = 0; i < dialog->filter_count; i++) {
        free(dialog->filters[i].name);
        free(dialog->filters[i].pattern);
    }
    free(dialog->filters);

    // Free bookmarks
    for (size_t i = 0; i < dialog->bookmark_count; i++) {
        free(dialog->bookmarks[i].name);
        free(dialog->bookmarks[i].path);
    }
    free(dialog->bookmarks);

    // Free strings
    free(dialog->current_path);
    free(dialog->default_filename);
    free(dialog->default_extension);

    // Free results
    if (dialog->selected_files) {
        for (size_t i = 0; i < dialog->selected_file_count; i++) {
            free(dialog->selected_files[i]);
        }
        free(dialog->selected_files);
    }

    // Free base dialog fields
    free(dialog->base.title);
    free(dialog->base.message);
}

/// @brief Measures the file dialog from its configured minimum dimensions.
///
/// @param widget File-dialog base widget whose measured dimensions are updated.
/// @param available_width Available width; currently unused.
/// @param available_height Available height; currently unused.
static void filedialog_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_filedialog_t *dialog = (vg_filedialog_t *)widget;
    (void)available_width;
    (void)available_height;

    widget->measured_width = dialog->base.min_width;
    widget->measured_height = dialog->base.min_height;
}

/// @brief Paints the complete visible file-dialog interface.
///
/// @details Rendering covers the modal backdrop, dialog surface and title,
/// path row, scrollable bookmarks and entries, selection states, save filename
/// editor, filter selector, and accept/cancel controls.
///
/// @param widget File-dialog base widget to render.
/// @param canvas Destination drawing context.
static void filedialog_paint(vg_widget_t *widget, void *canvas) {
    vg_filedialog_t *dialog = (vg_filedialog_t *)widget;
    vg_theme_t *theme = vg_theme_get_current();

    if (!dialog->base.is_open)
        return;

    float x = widget->x;
    float y = widget->y;
    float w = widget->width;
    float h = widget->height;
    float sidebar_width = FILEDIALOG_SIDEBAR_WIDTH;
    float title_height = FILEDIALOG_TITLE_HEIGHT;
    float path_height = FILEDIALOG_PATH_HEIGHT;
    float bottom_height = filedialog_bottom_height(dialog);
    float list_x = x + sidebar_width;
    float list_y = y + title_height + path_height;
    float list_width = w - sidebar_width;
    float list_height = h - title_height - path_height - bottom_height;
    float button_y = y + h - FILEDIALOG_BUTTON_HEIGHT - 10.0f;
    float ok_x = x + w - FILEDIALOG_BUTTON_WIDTH - FILEDIALOG_BUTTON_MARGIN;
    float cancel_x = ok_x - FILEDIALOG_BUTTON_WIDTH - FILEDIALOG_BUTTON_MARGIN;

    vgfx_window_t win = (vgfx_window_t)canvas;
    float bookmark_view_h = filedialog_list_view_height(list_height);
    float file_view_h = filedialog_list_view_height(list_height);
    size_t first_bookmark = 0;
    size_t first_entry = 0;
    float bookmark_offset_y = 0.0f;
    float file_offset_y = 0.0f;

    filedialog_clamp_scrolls(dialog, list_height);
    first_bookmark = (size_t)(dialog->bookmark_scroll_y / FILEDIALOG_BOOKMARK_HEIGHT);
    bookmark_offset_y =
        dialog->bookmark_scroll_y - (float)first_bookmark * FILEDIALOG_BOOKMARK_HEIGHT;
    first_entry = (size_t)(dialog->file_scroll_y / FILEDIALOG_ROW_HEIGHT);
    file_offset_y = dialog->file_scroll_y - (float)first_entry * FILEDIALOG_ROW_HEIGHT;

    // Draw modal overlay (dark semi-transparent background behind dialog)
    int32_t win_w = 0, win_h = 0;
    if (vgfx_get_size(win, &win_w, &win_h)) {
        vgfx_fill_rect(win, 0, 0, win_w, win_h, 0x60101010u);
    }

    // Draw dialog background
    vgfx_fill_rect(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, theme->colors.bg_primary);
    vgfx_rect(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, theme->colors.border_primary);

    // Title bar
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)y,
                   (int32_t)w,
                   (int32_t)title_height,
                   dialog->base.title_bg_color);
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)(y + title_height - 1.0f),
                   (int32_t)w,
                   1,
                   theme->colors.border_primary);

    if (dialog->base.title && dialog->base.font) {
        vg_font_draw_text(canvas,
                          dialog->base.font,
                          dialog->base.title_font_size,
                          x + 12.0f,
                          y + title_height / 2.0f + dialog->base.title_font_size / 3.0f,
                          dialog->base.title,
                          dialog->base.title_text_color);
        vg_font_draw_text(canvas,
                          dialog->base.font,
                          dialog->base.font_size,
                          x + w - FILEDIALOG_CLOSE_BUTTON_SIZE - 8.0f,
                          y + title_height / 2.0f + dialog->base.font_size / 3.0f,
                          "X",
                          dialog->base.title_text_color);
    }

    // Path bar and sidebar chrome
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)(y + title_height),
                   (int32_t)w,
                   (int32_t)path_height,
                   theme->colors.bg_secondary);
    vgfx_fill_rect(win,
                   (int32_t)(x + sidebar_width - 1.0f),
                   (int32_t)(y + title_height),
                   1,
                   (int32_t)(h - title_height),
                   theme->colors.border_primary);
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)(y + title_height + path_height - 1.0f),
                   (int32_t)w,
                   1,
                   theme->colors.border_primary);

    // Draw path
    if (dialog->base.font && dialog->current_path) {
        vg_font_metrics_t path_metrics = {0};
        float path_clip_x = x + sidebar_width + 10.0f;
        float path_clip_y = y + title_height + 2.0f;
        float path_clip_w = w - sidebar_width - FILEDIALOG_CLOSE_BUTTON_SIZE - 30.0f;
        float path_clip_h = path_height - 4.0f;
        float path_baseline = 0.0f;
        float path_text_x = 0.0f;

        vg_font_get_metrics(dialog->base.font, dialog->base.font_size, &path_metrics);
        path_baseline =
            y + title_height + (path_height - dialog->base.font_size) * 0.5f + path_metrics.ascent;
        path_text_x = filedialog_text_origin(dialog->base.font,
                                             dialog->base.font_size,
                                             dialog->current_path,
                                             path_clip_x,
                                             path_clip_w,
                                             true);
        filedialog_draw_clipped_text(canvas,
                                     dialog->base.font,
                                     dialog->base.font_size,
                                     path_clip_x,
                                     path_clip_y,
                                     path_clip_w,
                                     path_clip_h,
                                     path_text_x,
                                     path_baseline,
                                     dialog->current_path,
                                     dialog->base.title_text_color);
    }

    // Draw bookmarks
    float bookmark_y = list_y + 5.0f - bookmark_offset_y;
    vgfx_set_clip(win,
                  (int32_t)x,
                  (int32_t)list_y,
                  (int32_t)sidebar_width,
                  (int32_t)(bookmark_view_h > 0.0f ? bookmark_view_h : 0.0f));
    for (size_t i = first_bookmark; i < dialog->bookmark_count && bookmark_y < list_y + list_height;
         i++) {
        if (dialog->base.font) {
            if (bookmark_y + FILEDIALOG_BOOKMARK_HEIGHT >= list_y) {
                filedialog_draw_clipped_text(canvas,
                                             dialog->base.font,
                                             dialog->base.font_size,
                                             x + 6.0f,
                                             bookmark_y,
                                             sidebar_width - 12.0f,
                                             FILEDIALOG_BOOKMARK_HEIGHT,
                                             x + 10.0f,
                                             bookmark_y + 18.0f,
                                             dialog->bookmarks[i].name,
                                             theme->colors.fg_primary);
            }
        }
        bookmark_y += FILEDIALOG_BOOKMARK_HEIGHT;
    }
    vgfx_clear_clip(win);

    // Draw file list
    float file_x = list_x + 10.0f;
    float file_y = list_y + 5.0f - file_offset_y;
    vgfx_set_clip(win,
                  (int32_t)list_x,
                  (int32_t)list_y,
                  (int32_t)list_width,
                  (int32_t)(list_height > 0.0f ? list_height : 0.0f));

    for (size_t i = first_entry; i < dialog->entry_count && file_y < list_y + list_height; i++) {
        vg_file_entry_t *entry = dialog->entries[i];

        // Highlight if selected
        uint32_t text_color = theme->colors.fg_primary;
        if (file_y + FILEDIALOG_ROW_HEIGHT < list_y) {
            file_y += FILEDIALOG_ROW_HEIGHT;
            continue;
        }
        if (is_selected(dialog, i)) {
            vgfx_fill_rect(win,
                           (int32_t)list_x,
                           (int32_t)file_y,
                           (int32_t)list_width,
                           (int32_t)FILEDIALOG_ROW_HEIGHT,
                           theme->colors.bg_selected);
        }

        // Draw icon indicator
        const char *icon = entry->is_directory ? "[D]" : "   ";
        if (dialog->base.font) {
            vg_font_draw_text(canvas,
                              dialog->base.font,
                              dialog->base.font_size,
                              file_x,
                              file_y + 18,
                              icon,
                              theme->colors.fg_secondary);

            filedialog_draw_clipped_text(canvas,
                                         dialog->base.font,
                                         dialog->base.font_size,
                                         file_x + 28.0f,
                                         file_y,
                                         list_width - 42.0f,
                                         FILEDIALOG_ROW_HEIGHT,
                                         file_x + 30.0f,
                                         file_y + 18.0f,
                                         entry->name,
                                         text_color);
        }

        file_y += FILEDIALOG_ROW_HEIGHT;
    }
    vgfx_clear_clip(win);

    if (dialog->entry_count > 0 && file_view_h > 0.0f) {
        float max_file_scroll =
            filedialog_max_scroll(dialog->entry_count, FILEDIALOG_ROW_HEIGHT, file_view_h);
        if (max_file_scroll > 0.0f) {
            float track_h = file_view_h;
            float content_h = (float)dialog->entry_count * FILEDIALOG_ROW_HEIGHT;
            float thumb_h = track_h * (file_view_h / content_h);
            float thumb_y =
                list_y + (track_h - thumb_h) * (dialog->file_scroll_y / max_file_scroll);
            if (thumb_h < 16.0f)
                thumb_h = 16.0f;
            vgfx_fill_rect(win,
                           (int32_t)(x + w - 12.0f),
                           (int32_t)list_y,
                           6,
                           (int32_t)track_h,
                           theme->colors.bg_tertiary);
            vgfx_fill_rect(win,
                           (int32_t)(x + w - 12.0f),
                           (int32_t)thumb_y,
                           6,
                           (int32_t)thumb_h,
                           theme->colors.accent_primary);
        }
    }

    if (dialog->bookmark_count > 0 && bookmark_view_h > 0.0f) {
        float max_bookmark_scroll = filedialog_max_scroll(
            dialog->bookmark_count, FILEDIALOG_BOOKMARK_HEIGHT, bookmark_view_h);
        if (max_bookmark_scroll > 0.0f) {
            float track_h = bookmark_view_h;
            float content_h = (float)dialog->bookmark_count * FILEDIALOG_BOOKMARK_HEIGHT;
            float thumb_h = track_h * (bookmark_view_h / content_h);
            float thumb_y =
                list_y + (track_h - thumb_h) * (dialog->bookmark_scroll_y / max_bookmark_scroll);
            if (thumb_h < 16.0f)
                thumb_h = 16.0f;
            vgfx_fill_rect(win,
                           (int32_t)(x + sidebar_width - 8.0f),
                           (int32_t)list_y,
                           4,
                           (int32_t)track_h,
                           theme->colors.bg_tertiary);
            vgfx_fill_rect(win,
                           (int32_t)(x + sidebar_width - 8.0f),
                           (int32_t)thumb_y,
                           4,
                           (int32_t)thumb_h,
                           theme->colors.border_focus);
        }
    }

    // Bottom action area
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)(y + h - bottom_height),
                   (int32_t)w,
                   (int32_t)bottom_height,
                   theme->colors.bg_secondary);
    vgfx_fill_rect(win,
                   (int32_t)x,
                   (int32_t)(y + h - bottom_height),
                   (int32_t)w,
                   1,
                   theme->colors.border_primary);

    if (dialog->mode == VG_FILEDIALOG_SAVE) {
        float field_x = list_x + 10.0f;
        float field_y = y + h - bottom_height + 8.0f;
        float field_w = w - sidebar_width - FILEDIALOG_BUTTON_WIDTH * 2.0f -
                        FILEDIALOG_BUTTON_MARGIN * 3.0f - 20.0f;
        if (field_w < 120.0f)
            field_w = 120.0f;

        vgfx_fill_rect(win,
                       (int32_t)field_x,
                       (int32_t)field_y,
                       (int32_t)field_w,
                       (int32_t)FILEDIALOG_FILENAME_HEIGHT,
                       theme->colors.bg_primary);
        vgfx_rect(win,
                  (int32_t)field_x,
                  (int32_t)field_y,
                  (int32_t)field_w,
                  (int32_t)FILEDIALOG_FILENAME_HEIGHT,
                  dialog->filename_active ? theme->colors.border_focus
                                          : theme->colors.border_primary);
        if (dialog->base.font) {
            vg_font_metrics_t name_metrics = {0};
            float clip_x = field_x + 6.0f;
            float clip_y = field_y + 2.0f;
            float clip_w = field_w - 12.0f;
            float clip_h = FILEDIALOG_FILENAME_HEIGHT - 4.0f;
            float baseline_y = 0.0f;
            float draw_x = field_x + 8.0f;
            const char *name_text = (dialog->default_filename && dialog->default_filename[0])
                                        ? dialog->default_filename
                                        : "File name";
            uint32_t name_color = (dialog->default_filename && dialog->default_filename[0])
                                      ? theme->colors.fg_primary
                                      : theme->colors.fg_placeholder;

            vg_font_get_metrics(dialog->base.font, dialog->base.font_size, &name_metrics);
            baseline_y = field_y + (FILEDIALOG_FILENAME_HEIGHT - dialog->base.font_size) * 0.5f +
                         name_metrics.ascent;

            if (dialog->default_filename && dialog->default_filename[0]) {
                vg_text_metrics_t prefix_metrics = {0};
                size_t cursor_len = dialog->filename_cursor_pos;
                char *prefix = NULL;
                filedialog_sync_filename_cursor(dialog);
                cursor_len = dialog->filename_cursor_pos;
                prefix = malloc(cursor_len + 1);
                if (prefix) {
                    memcpy(prefix, dialog->default_filename, cursor_len);
                    prefix[cursor_len] = '\0';
                    vg_font_measure_text(
                        dialog->base.font, dialog->base.font_size, prefix, &prefix_metrics);
                    free(prefix);
                    if (prefix_metrics.width > clip_w - 4.0f) {
                        draw_x -= prefix_metrics.width - (clip_w - 4.0f);
                    }
                }
            }

            filedialog_draw_clipped_text(canvas,
                                         dialog->base.font,
                                         dialog->base.font_size,
                                         clip_x,
                                         clip_y,
                                         clip_w,
                                         clip_h,
                                         draw_x,
                                         baseline_y,
                                         name_text,
                                         name_color);

            if (dialog->filename_active && dialog->default_filename) {
                vg_text_metrics_t cursor_metrics = {0};
                size_t cursor_len = dialog->filename_cursor_pos;
                char *prefix = malloc(cursor_len + 1);
                if (prefix) {
                    memcpy(prefix, dialog->default_filename, cursor_len);
                    prefix[cursor_len] = '\0';
                    vg_font_measure_text(
                        dialog->base.font, dialog->base.font_size, prefix, &cursor_metrics);
                    free(prefix);
                    vgfx_fill_rect(win,
                                   (int32_t)(draw_x + cursor_metrics.width),
                                   (int32_t)(field_y + 6.0f),
                                   1,
                                   (int32_t)(FILEDIALOG_FILENAME_HEIGHT - 12.0f),
                                   theme->colors.border_focus);
                }
            }
        }
    }

    // Draw OK/Cancel buttons at bottom right
    if (dialog->base.font) {
        uint32_t btn_bg = dialog->base.button_bg_color;
        uint32_t btn_border = theme->colors.border_primary;
        uint32_t btn_fg = dialog->base.title_text_color;
        const char *ok_label = filedialog_accept_label(dialog);

        // Cancel button
        vgfx_fill_rect(win,
                       (int32_t)cancel_x,
                       (int32_t)button_y,
                       (int32_t)FILEDIALOG_BUTTON_WIDTH,
                       (int32_t)FILEDIALOG_BUTTON_HEIGHT,
                       btn_bg);
        vgfx_rect(win,
                  (int32_t)cancel_x,
                  (int32_t)button_y,
                  (int32_t)FILEDIALOG_BUTTON_WIDTH,
                  (int32_t)FILEDIALOG_BUTTON_HEIGHT,
                  btn_border);
        vg_font_draw_text(canvas,
                          dialog->base.font,
                          dialog->base.font_size,
                          cancel_x + 16.0f,
                          button_y + 18.0f,
                          "Cancel",
                          btn_fg);

        // OK button
        vgfx_fill_rect(win,
                       (int32_t)ok_x,
                       (int32_t)button_y,
                       (int32_t)FILEDIALOG_BUTTON_WIDTH,
                       (int32_t)FILEDIALOG_BUTTON_HEIGHT,
                       theme->colors.accent_primary);
        vgfx_rect(win,
                  (int32_t)ok_x,
                  (int32_t)button_y,
                  (int32_t)FILEDIALOG_BUTTON_WIDTH,
                  (int32_t)FILEDIALOG_BUTTON_HEIGHT,
                  btn_border);
        vg_font_draw_text(canvas,
                          dialog->base.font,
                          dialog->base.font_size,
                          ok_x + 16.0f,
                          button_y + 18.0f,
                          ok_label,
                          btn_fg);
    }
}

/// @brief Handles file-dialog navigation, selection, editing, and dismissal.
///
/// @details Pointer events cover dragging, controls, bookmarks, list rows, and
/// scrolling. Keyboard input supports UTF-8 filename editing, list navigation,
/// parent traversal, confirmation, and cancellation. Unhandled input is consumed
/// while the embedded dialog is modal.
///
/// @param widget File-dialog base widget receiving the event.
/// @param event Event to inspect.
/// @return `true` when the event is handled or modal policy consumes it.
static bool filedialog_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_filedialog_t *dialog = (vg_filedialog_t *)widget;

    if (!dialog->base.is_open)
        return false;

    float title_height = FILEDIALOG_TITLE_HEIGHT;
    float sidebar_width = FILEDIALOG_SIDEBAR_WIDTH;
    float path_height = FILEDIALOG_PATH_HEIGHT;
    float bottom_height = filedialog_bottom_height(dialog);
    float list_y = title_height + path_height;
    float list_height = widget->height - title_height - path_height - bottom_height;
    float button_y = widget->height - FILEDIALOG_BUTTON_HEIGHT - 10.0f;
    float ok_x = widget->width - FILEDIALOG_BUTTON_WIDTH - FILEDIALOG_BUTTON_MARGIN;
    float cancel_x = ok_x - FILEDIALOG_BUTTON_WIDTH - FILEDIALOG_BUTTON_MARGIN;

    switch (event->type) {
        case VG_EVENT_MOUSE_MOVE:
            if (dialog->base.is_dragging) {
                float parent_sx = 0.0f;
                float parent_sy = 0.0f;
                get_parent_screen_origin(widget, &parent_sx, &parent_sy);
                widget->x = event->mouse.screen_x - parent_sx - (float)dialog->base.drag_offset_x;
                widget->y = event->mouse.screen_y - parent_sy - (float)dialog->base.drag_offset_y;
                widget->needs_paint = true;
                widget->needs_layout = true;
                return true;
            }
            return dialog->base.modal;

        case VG_EVENT_MOUSE_WHEEL: {
            float mx = event->wheel.screen_x - widget->x;
            float my = event->wheel.screen_y - widget->y;
            if (mx > sidebar_width && my > list_y && my < list_y + list_height) {
                dialog->file_scroll_y +=
                    FILEDIALOG_ROW_HEIGHT * (event->wheel.delta_y > 0.0f ? -3.0f : 3.0f);
                filedialog_clamp_scrolls(dialog, list_height);
                widget->needs_paint = true;
                return true;
            }
            if (mx >= 0.0f && mx < sidebar_width && my > list_y && my < list_y + list_height) {
                dialog->bookmark_scroll_y +=
                    FILEDIALOG_BOOKMARK_HEIGHT * (event->wheel.delta_y > 0.0f ? -3.0f : 3.0f);
                filedialog_clamp_scrolls(dialog, list_height);
                widget->needs_paint = true;
                return true;
            }
            return dialog->base.modal;
        }

        case VG_EVENT_MOUSE_DOWN: {
            float mx = event->mouse.x;
            float my = event->mouse.y;

            // Title bar close button
            if (dialog->base.show_close_button &&
                mx >= widget->width - FILEDIALOG_CLOSE_BUTTON_SIZE - 8.0f &&
                mx < widget->width - 8.0f &&
                my >= (title_height - FILEDIALOG_CLOSE_BUTTON_SIZE) / 2.0f &&
                my < (title_height + FILEDIALOG_CLOSE_BUTTON_SIZE) / 2.0f) {
                vg_dialog_close(&dialog->base, VG_DIALOG_RESULT_CANCEL);
                if (dialog->on_cancel)
                    dialog->on_cancel(dialog, dialog->user_data);
                return true;
            }

            if (dialog->base.draggable && my >= 0.0f && my < title_height) {
                float widget_sx = 0.0f;
                float widget_sy = 0.0f;
                vg_widget_get_screen_bounds(widget, &widget_sx, &widget_sy, NULL, NULL);
                dialog->base.is_dragging = true;
                dialog->base.drag_offset_x = (int)(event->mouse.screen_x - widget_sx);
                dialog->base.drag_offset_y = (int)(event->mouse.screen_y - widget_sy);
                vg_widget_set_input_capture(widget);
                return true;
            }

            if (mx >= cancel_x && mx < cancel_x + FILEDIALOG_BUTTON_WIDTH && my >= button_y &&
                my < button_y + FILEDIALOG_BUTTON_HEIGHT) {
                vg_dialog_close(&dialog->base, VG_DIALOG_RESULT_CANCEL);
                if (dialog->on_cancel)
                    dialog->on_cancel(dialog, dialog->user_data);
                return true;
            }

            if (mx >= ok_x && mx < ok_x + FILEDIALOG_BUTTON_WIDTH && my >= button_y &&
                my < button_y + FILEDIALOG_BUTTON_HEIGHT) {
                confirm_selection(dialog);
                return true;
            }

            if (dialog->mode == VG_FILEDIALOG_SAVE) {
                float field_x = sidebar_width + 10.0f;
                float field_y = widget->height - bottom_height + 8.0f;
                float field_w = widget->width - sidebar_width - FILEDIALOG_BUTTON_WIDTH * 2.0f -
                                FILEDIALOG_BUTTON_MARGIN * 3.0f - 20.0f;
                if (field_w < 120.0f)
                    field_w = 120.0f;
                dialog->filename_active = mx >= field_x && mx < field_x + field_w &&
                                          my >= field_y &&
                                          my < field_y + FILEDIALOG_FILENAME_HEIGHT;
                if (dialog->filename_active) {
                    filedialog_sync_filename_cursor(dialog);
                    dialog->filename_cursor_pos =
                        dialog->default_filename ? strlen(dialog->default_filename) : 0;
                }
                if (dialog->filename_active)
                    return true;
            } else {
                dialog->filename_active = false;
            }

            // Check if clicking in file list
            if (mx > sidebar_width && my > list_y && my < list_y + list_height) {
                size_t clicked_index = filedialog_index_from_scroll(my - list_y - 5.0f,
                                                                    dialog->file_scroll_y,
                                                                    FILEDIALOG_ROW_HEIGHT,
                                                                    dialog->entry_count);
                if (clicked_index < dialog->entry_count) {
                    select_entry(dialog, clicked_index);
                    if (dialog->mode == VG_FILEDIALOG_SAVE &&
                        !dialog->entries[clicked_index]->is_directory)
                        filedialog_set_default_filename(dialog,
                                                        dialog->entries[clicked_index]->name);
                    filedialog_scroll_selection_into_view(dialog, list_height);
                    widget->needs_paint = true;
                    return true;
                }
            }

            // Check if clicking in bookmarks
            if (mx < sidebar_width && my > list_y && my < list_y + list_height) {
                size_t clicked_bookmark = filedialog_index_from_scroll(my - list_y - 5.0f,
                                                                       dialog->bookmark_scroll_y,
                                                                       FILEDIALOG_BOOKMARK_HEIGHT,
                                                                       dialog->bookmark_count);
                if (clicked_bookmark < dialog->bookmark_count) {
                    load_directory(dialog, dialog->bookmarks[clicked_bookmark].path);
                    dialog->filename_active = false;
                    widget->needs_paint = true;
                    return true;
                }
            }

            return dialog->base.modal;
        }

        case VG_EVENT_DOUBLE_CLICK: {
            float mx = event->mouse.x;
            float my = event->mouse.y;

            // Check double-click in file list
            if (mx > sidebar_width && my > list_y && my < list_y + list_height) {
                size_t clicked_index = filedialog_index_from_scroll(my - list_y - 5.0f,
                                                                    dialog->file_scroll_y,
                                                                    FILEDIALOG_ROW_HEIGHT,
                                                                    dialog->entry_count);
                if (clicked_index < dialog->entry_count) {
                    vg_file_entry_t *entry = dialog->entries[clicked_index];
                    if (entry->is_directory) {
                        // Navigate into directory
                        load_directory(dialog, entry->full_path);
                        widget->needs_paint = true;
                    } else {
                        // Select file and confirm
                        select_entry(dialog, clicked_index);
                        if (dialog->mode == VG_FILEDIALOG_SAVE)
                            filedialog_set_default_filename(dialog, entry->name);
                        confirm_selection(dialog);
                    }
                    return true;
                }
            }
            return dialog->base.modal;
        }

        case VG_EVENT_MOUSE_UP:
            if (dialog->base.is_dragging) {
                dialog->base.is_dragging = false;
                if (vg_widget_get_input_capture() == widget)
                    vg_widget_release_input_capture();
                return true;
            }
            return dialog->base.modal;

        case VG_EVENT_KEY_DOWN: {
            if (event->key.key == VG_KEY_ESCAPE) {
                vg_dialog_close(&dialog->base, VG_DIALOG_RESULT_CANCEL);
                if (dialog->on_cancel) {
                    dialog->on_cancel(dialog, dialog->user_data);
                }
                return true;
            }

            if (dialog->mode == VG_FILEDIALOG_SAVE && dialog->filename_active) {
                if (event->key.key == VG_KEY_BACKSPACE) {
                    filedialog_delete_last_codepoint(dialog);
                    widget->needs_paint = true;
                    return true;
                }
                if (event->key.key == VG_KEY_DELETE) {
                    filedialog_delete_codepoint_at_cursor(dialog);
                    widget->needs_paint = true;
                    return true;
                }
                if (event->key.key == VG_KEY_LEFT) {
                    dialog->filename_cursor_pos = filedialog_prev_codepoint_boundary(
                        dialog->default_filename, dialog->filename_cursor_pos);
                    widget->needs_paint = true;
                    return true;
                }
                if (event->key.key == VG_KEY_RIGHT) {
                    dialog->filename_cursor_pos = filedialog_next_codepoint_boundary(
                        dialog->default_filename, dialog->filename_cursor_pos);
                    widget->needs_paint = true;
                    return true;
                }
                if (event->key.key == VG_KEY_HOME) {
                    dialog->filename_cursor_pos = 0;
                    widget->needs_paint = true;
                    return true;
                }
                if (event->key.key == VG_KEY_END) {
                    dialog->filename_cursor_pos =
                        dialog->default_filename ? strlen(dialog->default_filename) : 0;
                    widget->needs_paint = true;
                    return true;
                }
            }

            if ((event->key.key == VG_KEY_UP || event->key.key == VG_KEY_DOWN) &&
                dialog->entry_count > 0) {
                int current = dialog->selection_count > 0 ? dialog->selected_indices[0] : -1;
                int next = current;
                if (event->key.key == VG_KEY_UP) {
                    next = current <= 0 ? 0 : current - 1;
                } else {
                    next = current < 0 ? 0 : current + 1;
                    if (next >= (int)dialog->entry_count)
                        next = (int)dialog->entry_count - 1;
                }
                if (next >= 0) {
                    select_entry(dialog, (size_t)next);
                    filedialog_scroll_selection_into_view(dialog, list_height);
                    if (dialog->mode == VG_FILEDIALOG_SAVE &&
                        !dialog->entries[next]->is_directory) {
                        filedialog_set_default_filename(dialog, dialog->entries[next]->name);
                    }
                    widget->needs_paint = true;
                    return true;
                }
            }

            if (event->key.key == VG_KEY_ENTER) {
                if (dialog->selection_count > 0) {
                    // If selected item is directory, navigate
                    int idx = dialog->selected_indices[0];
                    if (idx >= 0 && (size_t)idx < dialog->entry_count) {
                        if (dialog->entries[idx]->is_directory) {
                            load_directory(dialog, dialog->entries[idx]->full_path);
                            widget->needs_paint = true;
                            return true;
                        }
                    }
                }
                // Confirm selection
                confirm_selection(dialog);
                return true;
            }

            // Backspace - go up
            if (event->key.key == VG_KEY_BACKSPACE) {
                char *parent = get_parent_directory(dialog->current_path);
                if (parent) {
                    load_directory(dialog, parent);
                    free(parent);
                    widget->needs_paint = true;
                }
                return true;
            }

            return dialog->base.modal;
        }

        case VG_EVENT_KEY_CHAR:
            if (dialog->mode == VG_FILEDIALOG_SAVE && dialog->filename_active) {
                filedialog_append_codepoint(dialog, event->key.codepoint);
                widget->needs_paint = true;
                return true;
            }
            return dialog->base.modal;

        default:
            break;
    }

    return dialog->base.modal;
}

//=============================================================================
// FileDialog API
//=============================================================================

/// @brief Destroy the file dialog, freeing all entries, filters, bookmarks, and strings.
///
/// @param dialog The file dialog to destroy; may be NULL.
void vg_filedialog_destroy(vg_filedialog_t *dialog) {
    if (dialog)
        vg_widget_destroy(&dialog->base.base);
}

/// @brief Replace the file dialog title bar text.
///
/// @param dialog The dialog to update; may be NULL.
/// @param title  New title text; copied internally.
void vg_filedialog_set_title(vg_filedialog_t *dialog, const char *title) {
    if (!dialog)
        return;
    char *new_title = title ? filedialog_strdup(title) : NULL;
    if (title && !new_title)
        return;
    free(dialog->base.title);
    dialog->base.title = new_title;
    dialog->base.base.needs_paint = true;
}

/// @brief Set the directory the dialog opens at; defaults to home if NULL.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param path   Absolute directory path; copied internally.  NULL → home directory.
void vg_filedialog_set_initial_path(vg_filedialog_t *dialog, const char *path) {
    if (!dialog)
        return;
    char *new_path = path ? filedialog_strdup(path) : get_home_directory();
    if (!new_path)
        return;
    if (dialog->base.is_open || dialog->base.base.visible) {
        load_directory(dialog, new_path);
        free(new_path);
    } else {
        free(dialog->current_path);
        dialog->current_path = new_path;
    }
    dialog->base.base.needs_layout = true;
    dialog->base.base.needs_paint = true;
}

/// @brief Set the default filename pre-filled in the save-mode text field.
///
/// @param dialog   The dialog to configure; may be NULL.
/// @param filename Initial filename string; copied internally.
void vg_filedialog_set_filename(vg_filedialog_t *dialog, const char *filename) {
    if (!dialog)
        return;
    filedialog_set_default_filename(dialog, filename);
    dialog->base.base.needs_paint = true;
}

/// @brief Enable or disable multi-file selection in the dialog.
///
/// @details In multi-select mode, clicking a file row without a modifier toggles
///          its selection while retaining other selections. When false (the default),
///          each click replaces the current selection with a single entry and the
///          confirm button remains labelled for a single file.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param multi  true to allow multiple files to be selected simultaneously.
void vg_filedialog_set_multi_select(vg_filedialog_t *dialog, bool multi) {
    if (dialog)
        dialog->multi_select = multi;
}

/// @brief Control whether hidden files and directories appear in the file list.
///
/// @details On POSIX, entries whose names begin with '.' are hidden unless show
///          is true. On Windows, entries carrying FILE_ATTRIBUTE_HIDDEN are treated
///          equivalently. Changing this flag takes effect the next time the directory
///          is (re-)loaded via load_directory.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param show   true to include hidden entries in the file list.
void vg_filedialog_set_show_hidden(vg_filedialog_t *dialog, bool show) {
    if (dialog)
        dialog->show_hidden = show;
}

/// @brief Enable or disable an overwrite-confirmation prompt in save mode.
///
/// @details When true and the dialog is in VG_FILEDIALOG_SAVE mode,
///          confirm_selection checks whether the chosen filename already exists
///          and presents a confirmation prompt before populating selected_files[].
///          Has no effect in OPEN or SELECT_FOLDER modes.
///
/// @param dialog   The dialog to configure; may be NULL.
/// @param confirm  true to prompt before overwriting an existing file.
void vg_filedialog_set_confirm_overwrite(vg_filedialog_t *dialog, bool confirm) {
    if (dialog)
        dialog->confirm_overwrite = confirm;
}

/// @brief Append a named file-type filter to the dialog's filter list.
///
/// @details Filters are (name, pattern) pairs where pattern is a semicolon-separated
///          list of glob patterns (e.g., "*.c;*.h"). The active filter determines
///          which entries are visible in the file list. Both name and pattern are
///          copied internally. The backing array doubles in capacity starting at 4;
///          if realloc fails the filter is silently dropped.
///
/// @param dialog   The dialog to configure; may be NULL.
/// @param name     Display label for the filter (e.g., "C Source Files").
/// @param pattern  Semicolon-separated glob pattern (e.g., "*.c;*.h").
void vg_filedialog_add_filter(vg_filedialog_t *dialog, const char *name, const char *pattern) {
    if (!dialog || !name || !pattern)
        return;

    if (dialog->filter_count >= dialog->filter_capacity) {
        size_t new_cap = dialog->filter_capacity == 0 ? 4 : dialog->filter_capacity;
        while (new_cap <= dialog->filter_count) {
            if (new_cap > SIZE_MAX / 2)
                return;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(vg_file_filter_t))
            return;
        vg_file_filter_t *new_filters =
            realloc(dialog->filters, new_cap * sizeof(vg_file_filter_t));
        if (!new_filters)
            return;
        dialog->filters = new_filters;
        dialog->filter_capacity = new_cap;
    }

    char *name_copy = filedialog_strdup(name);
    char *pattern_copy = filedialog_strdup(pattern);
    if (!name_copy || !pattern_copy) {
        free(name_copy);
        free(pattern_copy);
        return;
    }
    dialog->filters[dialog->filter_count].name = name_copy;
    dialog->filters[dialog->filter_count].pattern = pattern_copy;
    dialog->filter_count++;
}

/// @brief Remove all file-type filters, freeing their name and pattern strings.
///
/// @details The backing filters[] array is retained at its current capacity;
///          only per-entry strings are freed and filter_count reset to zero.
///          After clearing, no filter drop-down is shown and all files are visible.
///
/// @param dialog The dialog to modify; may be NULL.
void vg_filedialog_clear_filters(vg_filedialog_t *dialog) {
    if (!dialog)
        return;

    for (size_t i = 0; i < dialog->filter_count; i++) {
        free(dialog->filters[i].name);
        free(dialog->filters[i].pattern);
    }
    dialog->filter_count = 0;
}

/// @brief Set the extension auto-appended in save mode when the filename lacks one.
///
/// @details In VG_FILEDIALOG_SAVE mode, confirm_selection checks whether the typed
///          filename contains a dot after the last path separator; if none is found
///          the default_extension is appended with a leading '.' unless ext itself
///          already starts with '.'. Passing NULL clears the default extension.
///          Has no effect in OPEN or SELECT_FOLDER modes.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param ext    Extension with or without leading dot (e.g., "txt" or ".txt"),
///               or NULL to disable automatic extension appending.
void vg_filedialog_set_default_extension(vg_filedialog_t *dialog, const char *ext) {
    if (!dialog)
        return;
    char *copy = filedialog_strdup(ext);
    if (ext && !copy)
        return;
    free(dialog->default_extension);
    dialog->default_extension = copy;
}

/// @brief Append a named shortcut to the sidebar bookmark list.
///
/// @details Bookmarks appear in the left sidebar; clicking one navigates the file
///          list to the bookmark's path via load_directory. Both name and path are
///          copied internally. The backing array doubles in capacity starting at 8;
///          if realloc fails the bookmark is silently dropped. The icon field is
///          initialised to VG_ICON_NONE.
///
/// @param dialog The dialog to configure; may be NULL.
/// @param name   Display label shown in the sidebar (e.g., "Projects").
/// @param path   Absolute directory path to navigate to when clicked.
void vg_filedialog_add_bookmark(vg_filedialog_t *dialog, const char *name, const char *path) {
    if (!dialog || !name || !path)
        return;

    if (dialog->bookmark_count >= dialog->bookmark_capacity) {
        if (dialog->bookmark_capacity > SIZE_MAX / (2u * sizeof(vg_bookmark_t)))
            return;
        size_t new_cap = dialog->bookmark_capacity == 0 ? 8 : dialog->bookmark_capacity * 2;
        vg_bookmark_t *new_bookmarks = realloc(dialog->bookmarks, new_cap * sizeof(vg_bookmark_t));
        if (!new_bookmarks)
            return;
        dialog->bookmarks = new_bookmarks;
        dialog->bookmark_capacity = new_cap;
    }

    char *name_copy = filedialog_strdup(name);
    char *path_copy = filedialog_strdup(path);
    if (!name_copy || !path_copy) {
        free(name_copy);
        free(path_copy);
        return;
    }
    dialog->bookmarks[dialog->bookmark_count].name = name_copy;
    dialog->bookmarks[dialog->bookmark_count].path = path_copy;
    dialog->bookmarks[dialog->bookmark_count].icon.type = VG_ICON_NONE;
    dialog->bookmark_count++;
}

/// @brief Populate the sidebar with standard OS locations.
///
/// @details On all platforms adds "Home" (the user's home directory). Any of
///          Desktop, Documents, and Downloads that exist as subdirectories of home
///          are stat-checked and added only if they are directories. Finally,
///          "Computer" is added pointing to '/' on POSIX or 'C:\' on Windows.
///          The home string from get_home_directory is freed internally after use.
///
/// @param dialog The dialog to populate; may be NULL.
void vg_filedialog_add_default_bookmarks(vg_filedialog_t *dialog) {
    if (!dialog)
        return;

    char *home = get_home_directory();
    if (home) {
        vg_filedialog_add_bookmark(dialog, "Home", home);

        char *desktop = join_path(home, "Desktop");
        if (desktop) {
            if (vg_filedialog_platform_path_is_dir(desktop)) {
                vg_filedialog_add_bookmark(dialog, "Desktop", desktop);
            }
            free(desktop);
        }

        char *documents = join_path(home, "Documents");
        if (documents) {
            if (vg_filedialog_platform_path_is_dir(documents)) {
                vg_filedialog_add_bookmark(dialog, "Documents", documents);
            }
            free(documents);
        }

        char *downloads = join_path(home, "Downloads");
        if (downloads) {
            if (vg_filedialog_platform_path_is_dir(downloads)) {
                vg_filedialog_add_bookmark(dialog, "Downloads", downloads);
            }
            free(downloads);
        }

        free(home);
    }

    vg_filedialog_add_bookmark(dialog, "Computer", vg_filedialog_platform_root_path());
}

/// @brief Remove all sidebar bookmarks, freeing their name and path strings.
///
/// @details The backing bookmarks[] array is retained at its current capacity;
///          only per-entry strings are freed and bookmark_count reset to zero.
///
/// @param dialog The dialog to modify; may be NULL.
void vg_filedialog_clear_bookmarks(vg_filedialog_t *dialog) {
    if (!dialog)
        return;

    for (size_t i = 0; i < dialog->bookmark_count; i++) {
        free(dialog->bookmarks[i].name);
        free(dialog->bookmarks[i].path);
    }
    dialog->bookmark_count = 0;
}

/// @brief Show the dialog by loading the current directory and making it visible.
///
/// @details Calls load_directory to populate entries[] from current_path, resets
///          drag/result state, marks the widget visible, and forces layout and paint
///          passes. In SAVE mode activates the filename text field immediately
///          (filename_active = true). If the dialog is modal, registers it as the
///          modal root via vg_widget_set_modal_root so pointer events outside it
///          are blocked.
///
/// @param dialog The dialog to display; may be NULL.
void vg_filedialog_show(vg_filedialog_t *dialog) {
    if (!dialog)
        return;

    // Load current directory
    load_directory(dialog, dialog->current_path);

    // Show dialog
    dialog->base.is_open = true;
    dialog->base.is_dragging = false;
    dialog->base.result = VG_DIALOG_RESULT_NONE;
    dialog->base.base.visible = true;
    dialog->filename_active = dialog->mode == VG_FILEDIALOG_SAVE;
    dialog->base.base.needs_layout = true;
    dialog->base.base.needs_paint = true;
    if (dialog->base.modal)
        vg_widget_set_modal_root(&dialog->base.base);
}

/// @brief Return the array of selected file paths populated after the dialog confirms.
///
/// @details selected_files[] is owned by the dialog and remains valid until the
///          next confirm_selection call or vg_filedialog_destroy. Callers that need
///          paths to outlive the dialog must copy each string. The count written to
///          *count is zero until the user confirms a selection.
///
/// @param dialog The dialog to query; may be NULL (returns NULL, sets *count to 0).
/// @param count  Out-parameter receiving the number of entries; may be NULL.
/// @return       Pointer to the internal selected_files[] array, or NULL on failure.
char **vg_filedialog_get_selected_paths(vg_filedialog_t *dialog, size_t *count) {
    if (!dialog) {
        if (count)
            *count = 0;
        return NULL;
    }

    if (count)
        *count = dialog->selected_file_count;
    return dialog->selected_files;
}

/// @brief Return the first (or only) selected path after the dialog confirms.
///
/// @details Convenience accessor equivalent to indexing vg_filedialog_get_selected_paths[0].
///          The returned pointer is owned by the dialog; copy it before destroying
///          the dialog.
///
/// @param dialog The dialog to query; may be NULL.
/// @return       selected_files[0], or NULL if no selection has been confirmed.
char *vg_filedialog_get_selected_path(vg_filedialog_t *dialog) {
    if (!dialog || dialog->selected_file_count == 0)
        return NULL;
    return dialog->selected_files[0];
}

/// @brief Register the callback invoked when the user confirms a selection.
///
/// @details The callback receives the dialog pointer, the selected_files[] array,
///          the selection count, and user_data. The selected_files[] array and
///          each path string are owned by the dialog and remain valid only until
///          the next confirmation or dialog destruction; callbacks that retain
///          paths must copy them. Passing NULL removes the callback.
///          user_data is shared with the on_cancel callback (both read from
///          dialog->user_data).
///
/// @param dialog    The dialog to configure; may be NULL.
/// @param callback  Function called on selection confirmation, or NULL to clear.
/// @param user_data Opaque pointer forwarded to the callback unchanged.
void vg_filedialog_set_on_select(vg_filedialog_t *dialog,
                                 void (*callback)(vg_filedialog_t *, char **, size_t, void *),
                                 void *user_data) {
    if (!dialog)
        return;
    dialog->on_select = callback;
    dialog->user_data = user_data;
}

/// @brief Register the callback invoked when the user cancels the dialog.
///
/// @details Fired when the close/cancel button is activated. The callback receives
///          the dialog pointer and user_data. Passing NULL removes the callback.
///          user_data is shared with the on_select callback (stored in
///          dialog->user_data).
///
/// @param dialog    The dialog to configure; may be NULL.
/// @param callback  Function called on cancellation, or NULL to clear.
/// @param user_data Opaque pointer forwarded to the callback unchanged.
void vg_filedialog_set_on_cancel(vg_filedialog_t *dialog,
                                 void (*callback)(vg_filedialog_t *, void *),
                                 void *user_data) {
    if (!dialog)
        return;
    dialog->on_cancel = callback;
    dialog->user_data = user_data;
}

/// @brief Installs the process-wide modal runner used by convenience dialogs.
///
/// @param runner Function that drives a shown dialog until completion; may be
///               null to disable external modal driving.
/// @param user_data Opaque context forwarded to @p runner.
void vg_filedialog_set_modal_runner(vg_filedialog_modal_runner_t runner, void *user_data) {
    g_modal_runner = runner;
    g_modal_runner_user_data = user_data;
}

/// @brief Shows a dialog and delegates modal event driving when configured.
///
/// @param dialog File dialog to show and run.
/// @return `true` when the modal runner succeeds and a selection exists, or
///         when a runnerless show already has a selection.
static bool filedialog_run_modal(vg_filedialog_t *dialog) {
    if (!dialog)
        return false;
    vg_filedialog_show(dialog);
    if (!g_modal_runner)
        return dialog->selected_file_count > 0;
    return g_modal_runner(dialog, g_modal_runner_user_data) && dialog->selected_file_count > 0;
}

//=============================================================================
// Convenience Functions
//=============================================================================

/// @brief Convenience: create, configure, show, and destroy an open-file dialog.
///
/// @details Creates a VG_FILEDIALOG_OPEN dialog, optionally sets a title, initial
///          path, and one filter, then adds default bookmarks and runs the
///          installed modal runner until the dialog closes.
///
/// @param title          Dialog window title, or NULL for the default.
/// @param initial_path   Directory to open at, or NULL for the home directory.
/// @param filter_name    Display name for the single filter, or NULL for none.
/// @param filter_pattern Glob pattern for the filter (e.g., "*.c;*.h"), or NULL.
/// @return  Heap-allocated selected path (caller must free), or NULL if cancelled.
char *vg_filedialog_open_file(const char *title,
                              const char *initial_path,
                              const char *filter_name,
                              const char *filter_pattern) {
    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_OPEN);
    if (!dialog)
        return NULL;

    if (title)
        vg_filedialog_set_title(dialog, title);
    if (initial_path)
        vg_filedialog_set_initial_path(dialog, initial_path);
    if (filter_name && filter_pattern) {
        vg_filedialog_add_filter(dialog, filter_name, filter_pattern);
    }
    vg_filedialog_add_default_bookmarks(dialog);

    char *result = NULL;
    if (filedialog_run_modal(dialog) && dialog->selected_file_count > 0 &&
        dialog->selected_files[0]) {
        result = filedialog_strdup(dialog->selected_files[0]);
    }

    vg_filedialog_destroy(dialog);
    return result;
}

/// @brief Convenience: create, configure, show, and destroy a save-file dialog.
///
/// @details Creates a VG_FILEDIALOG_SAVE dialog, optionally sets a title, initial
///          path, a default filename, and one filter, then adds default bookmarks
///          and runs the installed modal runner until the dialog closes. Returns
///          a heap-allocated copy of the first selected path (caller must free)
///          or NULL if cancelled.
///
/// @param title          Dialog window title, or NULL for the default.
/// @param initial_path   Directory to open at, or NULL for the home directory.
/// @param default_name   Pre-filled filename in the save text field, or NULL.
/// @param filter_name    Display name for the single filter, or NULL for none.
/// @param filter_pattern Glob pattern for the filter (e.g., "*.txt"), or NULL.
/// @return  Heap-allocated selected path (caller must free), or NULL if cancelled.
char *vg_filedialog_save_file(const char *title,
                              const char *initial_path,
                              const char *default_name,
                              const char *filter_name,
                              const char *filter_pattern) {
    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_SAVE);
    if (!dialog)
        return NULL;

    if (title)
        vg_filedialog_set_title(dialog, title);
    if (initial_path)
        vg_filedialog_set_initial_path(dialog, initial_path);
    if (default_name)
        vg_filedialog_set_filename(dialog, default_name);
    if (filter_name && filter_pattern) {
        vg_filedialog_add_filter(dialog, filter_name, filter_pattern);
    }
    vg_filedialog_add_default_bookmarks(dialog);

    char *result = NULL;
    if (filedialog_run_modal(dialog) && dialog->selected_file_count > 0 &&
        dialog->selected_files[0]) {
        result = filedialog_strdup(dialog->selected_files[0]);
    }

    vg_filedialog_destroy(dialog);
    return result;
}

/// @brief Convenience: create, configure, show, and destroy a folder-select dialog.
///
/// @details Creates a VG_FILEDIALOG_SELECT_FOLDER dialog, optionally sets a title
///          and initial path, adds default bookmarks, and shows the dialog. Returns
///          a heap-allocated copy of the selected folder path (caller must free),
///          or NULL if cancelled.
///
/// @param title        Dialog window title, or NULL for the default.
/// @param initial_path Directory to open at, or NULL for the home directory.
/// @return  Heap-allocated selected folder path (caller must free), or NULL if cancelled.
char *vg_filedialog_select_folder(const char *title, const char *initial_path) {
    vg_filedialog_t *dialog = vg_filedialog_create(VG_FILEDIALOG_SELECT_FOLDER);
    if (!dialog)
        return NULL;

    if (title)
        vg_filedialog_set_title(dialog, title);
    if (initial_path)
        vg_filedialog_set_initial_path(dialog, initial_path);
    vg_filedialog_add_default_bookmarks(dialog);

    char *result = NULL;
    if (filedialog_run_modal(dialog) && dialog->selected_file_count > 0 &&
        dialog->selected_files[0]) {
        result = filedialog_strdup(dialog->selected_files[0]);
    }

    vg_filedialog_destroy(dialog);
    return result;
}
