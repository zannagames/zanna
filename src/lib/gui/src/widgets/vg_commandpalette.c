//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_commandpalette.c
/// @brief Implements a scalable, fuzzy-search command-palette overlay.
///
/// @details The widget owns a master array of commands and rebuilds a separate
/// filtered pointer view whenever its query changes. The filtered view borrows
/// command pointers and never owns individual commands. Built-in matching is
/// UTF-8-aware with ASCII case folding, while client-filtered mode preserves
/// insertion order for application-ranked results.
///
/// Selection and the first visible row remain clamped to the current filtered
/// results and viewport capacity. Logical dimensions are scaled through the
/// active theme and constrained to the viewport. `is_visible` and the base
/// widget's visibility flag are updated together.
///
/// @see vg_ide_widgets.h
/// @see vg_widget.h
/// @see vg_theme.h
/// @see vg_event.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void commandpalette_destroy(vg_widget_t *widget);
static void commandpalette_measure(vg_widget_t *widget,
                                   float available_width,
                                   float available_height);
static void commandpalette_paint(vg_widget_t *widget, void *canvas);
static bool commandpalette_handle_event(vg_widget_t *widget, vg_event_t *event);
static void commandpalette_ensure_selection_visible(vg_commandpalette_t *palette);

static const float COMMANDPALETTE_SEARCH_HEIGHT = 36.0f;
static const float COMMANDPALETTE_VIEWPORT_MARGIN = 16.0f;

//=============================================================================
// CommandPalette VTable
//=============================================================================

static vg_widget_vtable_t g_commandpalette_vtable = {.destroy = commandpalette_destroy,
                                                     .measure = commandpalette_measure,
                                                     .arrange = NULL,
                                                     .paint = commandpalette_paint,
                                                     .handle_event = commandpalette_handle_event,
                                                     .can_focus = NULL,
                                                     .on_focus = NULL};

/// @brief Returns the current logical-to-framebuffer UI scale.
///
/// @return Positive theme scale, or `1.0` when no valid scale is active.
static float commandpalette_ui_scale(void) {
    vg_theme_t *theme = vg_theme_get_current();
    return theme && theme->ui_scale > 0.0f ? theme->ui_scale : 1.0f;
}

/// @brief Returns the physical search-row height for the active UI scale.
///
/// @return Scaled search-row height in framebuffer units.
static float commandpalette_search_height(void) {
    return COMMANDPALETTE_SEARCH_HEIGHT * commandpalette_ui_scale();
}

/// @brief Returns the physical result-row height for the active UI scale.
///
/// @param palette Palette whose logical item height is scaled.
/// @return Positive row height in framebuffer units, with `1.0` as the minimum
///         and null-palette fallback.
static float commandpalette_item_height(const vg_commandpalette_t *palette) {
    if (!palette)
        return 1.0f;
    float height = (float)palette->item_height * commandpalette_ui_scale();
    return height > 0.0f ? height : 1.0f;
}

/// @brief Returns the number of result rows that fit in the arranged palette.
///
/// @details Before first layout, the configured maximum keeps keyboard state
/// well-defined. Once arranged, viewport-constrained capacity drives painting,
/// hit testing, scrolling, and selection reveal.
///
/// @param palette Palette whose arranged height and row limit are inspected.
/// @return Non-negative visible-row capacity.
static int commandpalette_visible_capacity(const vg_commandpalette_t *palette) {
    if (!palette || palette->max_visible == 0)
        return 0;
    float height = palette->base.height;
    if (height <= 0.0f)
        return (int)palette->max_visible;
    float available = height - commandpalette_search_height();
    if (available <= 0.0f)
        return 0;
    int capacity = (int)(available / commandpalette_item_height(palette));
    if (capacity < 0)
        capacity = 0;
    if (capacity > (int)palette->max_visible)
        capacity = (int)palette->max_visible;
    return capacity;
}

//=============================================================================
// Fuzzy Matching
//=============================================================================

/// @brief Decodes and consumes the next UTF-8 code point.
///
/// @details Valid sequences advance @p cursor by their encoded length.
/// Malformed, overlong, surrogate, and out-of-range sequences produce U+FFFD;
/// malformed prefixes consume at least one byte so matching always progresses.
///
/// @param[in,out] cursor Address of the current UTF-8 byte pointer.
/// @return Decoded Unicode scalar, zero at the string terminator, or U+FFFD for
///         an invalid sequence.
static uint32_t commandpalette_decode_utf8(const char **cursor) {
    const unsigned char *s = (const unsigned char *)*cursor;
    if (!s || !*s)
        return 0;
    if (s[0] < 0x80) {
        *cursor += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && s[1] && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *cursor += 2;
        return cp >= 0x80 ? cp : 0xFFFD;
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2] && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
                      (uint32_t)(s[2] & 0x3F);
        *cursor += 3;
        return cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF) ? cp : 0xFFFD;
    }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3] && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                      ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *cursor += 4;
        return cp >= 0x10000 && cp <= 0x10FFFF ? cp : 0xFFFD;
    }
    *cursor += 1;
    return 0xFFFD;
}

/// @brief Applies ASCII case folding for case-insensitive matching.
///
/// @param cp Unicode code point to fold.
/// @return Lowercase ASCII equivalent for `A` through `Z`; otherwise @p cp.
static uint32_t commandpalette_fold_codepoint(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z')
        return cp + ('a' - 'A');
    return cp;
}

/// @brief Tests whether a code point delimits words for fuzzy-match bonuses.
///
/// @param cp Previous code point, or zero at the start of the candidate.
/// @return `true` for zero, space, underscore, or hyphen; otherwise `false`.
static bool commandpalette_is_word_boundary(uint32_t cp) {
    return cp == 0 || cp == ' ' || cp == '_' || cp == '-';
}

/// @brief Computes a fuzzy subsequence-match score for a pattern and candidate.
///
/// @details Matching decodes UTF-8, folds ASCII case, and rewards consecutive
/// and word-initial matches. A shorter candidate receives a small specificity
/// bonus. Every pattern code point must be found in order.
///
/// @param pattern UTF-8 search pattern.
/// @param text UTF-8 candidate label.
/// @return Positive score for a complete match, with larger values preferred;
///         zero for null inputs or an incomplete match.
static int fuzzy_match_score(const char *pattern, const char *text) {
    if (!pattern || !text)
        return 0;
    if (!pattern[0])
        return 1; // Empty pattern matches everything

    int score = 0;
    const char *p = pattern;
    const char *t = text;
    bool consecutive = true;
    int last_match_pos = -1;
    uint32_t prev_tc = 0;
    int text_pos = 0;

    while (*p && *t) {
        const char *p_next = p;
        const char *t_next = t;
        uint32_t pc = commandpalette_fold_codepoint(commandpalette_decode_utf8(&p_next));
        uint32_t tc = commandpalette_fold_codepoint(commandpalette_decode_utf8(&t_next));

        if (pc == tc) {
            // Found a match
            score += consecutive ? 10 : 5;

            // Bonus for matching at word boundaries
            if (t == text || commandpalette_is_word_boundary(prev_tc)) {
                score += 15;
            }

            // Bonus for consecutive matches
            if (text_pos == last_match_pos + 1) {
                score += 5;
            }

            last_match_pos = text_pos;
            p = p_next;
            consecutive = true;
        } else {
            consecutive = false;
        }
        prev_tc = tc;
        t = t_next;
        text_pos++;
    }

    // Pattern must be fully matched
    if (*p)
        return 0;

    // Bonus for shorter text (more specific match)
    int len = (int)strlen(text);
    if (len > 0) {
        score += 100 / len;
    }

    return score;
}

/// @brief Rebuilds the filtered command view from the current query.
///
/// @details Disabled commands are omitted. Built-in mode includes fuzzy
/// matches, while client-filtered mode includes enabled commands in insertion
/// order. The pointer array grows geometrically, selection is reset or
/// preserved according to mode, and scrolling returns to the first row.
///
/// @param palette Palette whose borrowed filtered view is rebuilt.
static void filter_commands(vg_commandpalette_t *palette) {
    if (!palette)
        return;

    int prev_selected = palette->selected_index;
    palette->filtered_count = 0;

    // Get query from search input
    const char *query = palette->current_query;

    for (size_t i = 0; i < palette->command_count; i++) {
        vg_command_t *cmd = palette->commands[i];
        if (!cmd || !cmd->enabled)
            continue;

        // Client-filtered mode: the application has already filtered and ranked
        // the commands (repopulating per keystroke), so show them in insertion
        // order without applying the built-in fuzzy filter.
        bool include;
        if (palette->client_filtered) {
            include = true;
        } else {
            int score = fuzzy_match_score(query, cmd->label);
            include = (score > 0 || !query || !query[0]);
        }
        if (include) {
            // Add to filtered list
            if (palette->filtered_count >= palette->filtered_capacity) {
                size_t new_cap = palette->filtered_capacity * 2;
                if (new_cap < 32)
                    new_cap = 32;
                if (new_cap <= palette->filtered_capacity ||
                    new_cap > SIZE_MAX / sizeof(vg_command_t *))
                    continue;
                vg_command_t **new_filtered =
                    realloc(palette->filtered, new_cap * sizeof(vg_command_t *));
                if (!new_filtered)
                    continue;
                palette->filtered = new_filtered;
                palette->filtered_capacity = new_cap;
            }
            palette->filtered[palette->filtered_count++] = cmd;
        }
    }

    // Selection: client-filtered mode preserves the row position across a
    // repopulation (so live re-adding does not flicker the highlight back to
    // the top); the built-in filter resets to the best match as before.
    if (palette->client_filtered && prev_selected >= 0 &&
        prev_selected < (int)palette->filtered_count) {
        palette->selected_index = prev_selected;
    } else {
        palette->selected_index = palette->filtered_count > 0 ? 0 : -1;
    }
    palette->first_visible_index = 0;
    palette->base.needs_paint = true;
}

/// @brief Clamps scrolling so the selected result remains visible.
///
/// @param palette Palette whose `first_visible_index` is adjusted.
static void commandpalette_ensure_selection_visible(vg_commandpalette_t *palette) {
    if (!palette || palette->max_visible == 0)
        return;

    int max_visible = commandpalette_visible_capacity(palette);
    if (max_visible <= 0) {
        palette->first_visible_index = 0;
        return;
    }
    if (palette->selected_index < 0) {
        palette->first_visible_index = 0;
        return;
    }

    if (palette->first_visible_index < 0)
        palette->first_visible_index = 0;
    if (palette->selected_index < palette->first_visible_index)
        palette->first_visible_index = palette->selected_index;
    if (palette->selected_index >= palette->first_visible_index + max_visible) {
        palette->first_visible_index = palette->selected_index - max_visible + 1;
    }

    if (palette->filtered_count <= (size_t)max_visible) {
        palette->first_visible_index = 0;
    } else {
        int max_first = (int)palette->filtered_count - max_visible;
        if (palette->first_visible_index > max_first)
            palette->first_visible_index = max_first;
    }
}

/// @brief Encodes one Unicode scalar as UTF-8.
///
/// @param codepoint Unicode scalar to encode.
/// @param[out] out Four-byte buffer that receives the encoded bytes.
/// @return Encoded length from one through four, or zero for a surrogate or
///         out-of-range code point.
static size_t utf8_encode_codepoint(uint32_t codepoint, char out[4]) {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        return 0;
    if (codepoint <= 0x7F) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    if (codepoint <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

/// @brief Appends one Unicode code point to the query and refilters commands.
///
/// @details Control characters, invalid Unicode scalars, overflow, and
/// allocation failure leave the query unchanged. Successful mutation advances
/// the query generation.
///
/// @param palette Palette whose query is extended.
/// @param codepoint Unicode scalar to append as UTF-8.
static void append_query_char(vg_commandpalette_t *palette, uint32_t codepoint) {
    char encoded[4];
    size_t encoded_len = 0;
    if (!palette || codepoint < 0x20 || codepoint == 0x7F)
        return;
    encoded_len = utf8_encode_codepoint(codepoint, encoded);
    if (encoded_len == 0)
        return;
    size_t old_len = palette->current_query ? strlen(palette->current_query) : 0;
    if (old_len > SIZE_MAX - encoded_len - 1u)
        return;
    char *next = realloc(palette->current_query, old_len + encoded_len + 1);
    if (!next)
        return;
    memcpy(next + old_len, encoded, encoded_len);
    next[old_len + encoded_len] = '\0';
    palette->current_query = next;
    palette->query_generation++;
    filter_commands(palette);
}

/// @brief Removes the final UTF-8 sequence from the query and refilters commands.
///
/// @details Removing the final query character releases the query allocation
/// and restores the null representation of an empty query.
///
/// @param palette Palette whose query receives a backspace operation.
static void remove_query_char(vg_commandpalette_t *palette) {
    if (!palette || !palette->current_query)
        return;
    size_t len = strlen(palette->current_query);
    if (len == 0)
        return;
    size_t new_len = len;
    do {
        new_len--;
    } while (new_len > 0 && (((unsigned char)palette->current_query[new_len] & 0xC0) == 0x80));
    palette->current_query[new_len] = '\0';
    if (new_len == 0) {
        free(palette->current_query);
        palette->current_query = NULL;
    }
    palette->query_generation++;
    filter_commands(palette);
}

/// @brief Returns the current query text.
///
/// @param palette Palette to inspect.
/// @return Borrowed null-terminated query string, or an immutable empty string
///         when the palette or stored query is null.
const char *vg_commandpalette_get_query(vg_commandpalette_t *palette) {
    if (!palette || !palette->current_query)
        return "";
    return palette->current_query;
}

/// @brief Returns the query generation counter.
///
/// @param palette Palette to inspect.
/// @return Counter incremented on every query mutation, or zero for null.
uint64_t vg_commandpalette_get_query_generation(vg_commandpalette_t *palette) {
    return palette ? palette->query_generation : 0;
}

/// @brief Replaces the query text and reruns filtering.
///
/// @details Non-empty input is copied. Null or empty input clears the query.
/// The generation advances even when allocation failure leaves the stored query
/// empty.
///
/// @param palette Palette whose query is replaced.
/// @param text UTF-8 query to copy; may be null to clear.
void vg_commandpalette_set_query(vg_commandpalette_t *palette, const char *text) {
    if (!palette)
        return;
    free(palette->current_query);
    palette->current_query = NULL;
    if (text && text[0]) {
        size_t len = strlen(text);
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, text, len + 1);
            palette->current_query = copy;
        }
    }
    palette->query_generation++;
    filter_commands(palette);
}

/// @brief Enables or disables application-managed filtering and ranking.
///
/// @details When enabled, every enabled command is shown in insertion order.
/// Changing the mode immediately rebuilds the filtered view.
///
/// @param palette Palette to configure.
/// @param enabled `true` to bypass built-in fuzzy filtering.
void vg_commandpalette_set_client_filtered(vg_commandpalette_t *palette, bool enabled) {
    if (!palette)
        return;
    palette->client_filtered = enabled;
    filter_commands(palette);
}

//=============================================================================
// CommandPalette Implementation
//=============================================================================

/// @brief Creates an initially hidden command palette with default styling.
///
/// @details The palette begins empty, uses a ten-row maximum, and owns a
/// default search placeholder. It is not parented automatically.
///
/// @return Newly allocated palette, or null on allocation failure.
vg_commandpalette_t *vg_commandpalette_create(void) {
    vg_commandpalette_t *palette = calloc(1, sizeof(vg_commandpalette_t));
    if (!palette)
        return NULL;

    vg_widget_init(&palette->base, VG_WIDGET_CUSTOM, &g_commandpalette_vtable);

    vg_theme_t *theme = vg_theme_get_current();

    // Defaults
    palette->item_height = 32;
    palette->max_visible = 10;
    palette->width = 500;
    palette->bg_color = 0xFF252526;
    palette->selected_bg = 0xFF094771;
    palette->text_color = 0xFFCCCCCC;
    palette->shortcut_color = 0xFF808080;

    palette->font_size = theme->typography.size_normal;
    palette->is_visible = false;
    palette->selected_index = -1;
    palette->hovered_index = -1;
    palette->first_visible_index = 0;
    palette->placeholder_text = vg_strdup("Type to search...");
    if (!palette->placeholder_text) {
        vg_widget_destroy(&palette->base);
        return NULL;
    }

    return palette;
}

/// @brief Releases a command and all strings it owns.
///
/// @param cmd Command to release; may be null.
static void free_command(vg_command_t *cmd) {
    if (!cmd)
        return;
    free(cmd->id);
    free(cmd->label);
    free(cmd->description);
    free(cmd->shortcut);
    free(cmd->category);
    free(cmd);
}

/// @brief Releases all resources owned by the command palette.
///
/// @details Each registered command is destroyed before the master and filtered
/// arrays, placeholder, and current query are released.
///
/// @param widget Palette base widget being destroyed.
static void commandpalette_destroy(vg_widget_t *widget) {
    vg_commandpalette_t *palette = (vg_commandpalette_t *)widget;

    for (size_t i = 0; i < palette->command_count; i++) {
        free_command(palette->commands[i]);
    }
    free(palette->commands);
    free(palette->filtered);
    free(palette->placeholder_text);
    free(palette->current_query);
}

/// @brief Destroys the palette and all registered commands.
///
/// @param palette Palette to destroy; may be null.
void vg_commandpalette_destroy(vg_commandpalette_t *palette) {
    if (!palette)
        return;
    vg_widget_destroy(&palette->base);
}

/// @brief Measure a scale-aware palette that remains inside the current viewport.
///
/// @details Width, search height, and row height are stored in logical units.
/// The scaled width clamps to 16-point side margins. Row capacity shrinks when
/// needed so the palette's 15-percent-from-top runtime placement retains the
/// same bottom margin.
///
/// @param widget Palette base widget whose measured dimensions are updated.
/// @param available_width Current viewport width in framebuffer units.
/// @param available_height Current viewport height in framebuffer units.
static void commandpalette_measure(vg_widget_t *widget,
                                   float available_width,
                                   float available_height) {
    vg_commandpalette_t *palette = (vg_commandpalette_t *)widget;
    float scale = commandpalette_ui_scale();
    float margin = COMMANDPALETTE_VIEWPORT_MARGIN * scale;
    float search_height = COMMANDPALETTE_SEARCH_HEIGHT * scale;
    float item_height = commandpalette_item_height(palette);

    float width = palette->width * scale;
    if (available_width > 0.0f) {
        float max_width = available_width - margin * 2.0f;
        if (max_width < 1.0f)
            max_width = available_width > 1.0f ? available_width : 1.0f;
        if (width > max_width)
            width = max_width;
    }
    if (width < 1.0f)
        width = 1.0f;

    size_t visible = palette->filtered_count;
    if (visible > palette->max_visible)
        visible = palette->max_visible;

    float max_height = available_height * 0.85f - margin;
    if (available_height <= 0.0f)
        max_height = search_height + (float)visible * item_height;
    if (max_height < 1.0f)
        max_height = available_height > 1.0f ? available_height : 1.0f;
    size_t height_capacity = 0;
    if (max_height > search_height)
        height_capacity = (size_t)((max_height - search_height) / item_height);
    if (visible > height_capacity)
        visible = height_capacity;

    float height = search_height + (float)visible * item_height;
    if (height > max_height)
        height = max_height;
    if (height < 1.0f)
        height = 1.0f;

    widget->measured_width = width;
    widget->measured_height = height;
}

/// @brief Paints the search row and visible filtered command results.
///
/// @details Painting clips to the palette rectangle, restores any prior clip,
/// highlights the selected row, and reserves trailing space for shortcuts when
/// they fit. The placeholder uses the shortcut color when no query is present.
///
/// @param widget Palette base widget to render.
/// @param canvas Destination drawing context.
static void commandpalette_paint(vg_widget_t *widget, void *canvas) {
    vg_commandpalette_t *palette = (vg_commandpalette_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;

    if (!palette->is_visible)
        return;

    float scale = commandpalette_ui_scale();
    float search_height = COMMANDPALETTE_SEARCH_HEIGHT * scale;
    float item_height = commandpalette_item_height(palette);
    float text_inset = 12.0f * scale;
    float shortcut_inset = 16.0f * scale;
    float shortcut_gap = 12.0f * scale;
    float x = widget->x;
    float y = widget->y;
    float w = widget->width > 0.0f ? widget->width : palette->width * scale;
    float h = widget->height;

    vgfx_fill_rect(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, palette->bg_color);
    vgfx_rect(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, 0xFF3C3C3C);
    vgfx_fill_rect(win, (int32_t)x, (int32_t)(y + search_height - 1.0f), (int32_t)w, 1, 0xFF3C3C3C);

    int32_t prior_clip_x = 0;
    int32_t prior_clip_y = 0;
    int32_t prior_clip_w = 0;
    int32_t prior_clip_h = 0;
    int had_prior_clip =
        vgfx_get_clip(win, &prior_clip_x, &prior_clip_y, &prior_clip_w, &prior_clip_h);
    vgfx_set_clip(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);

    if (palette->font && w > text_inset * 2.0f) {
        const char *query =
            (palette->current_query && palette->current_query[0])
                ? palette->current_query
                : (palette->placeholder_text ? palette->placeholder_text : "Type to search...");
        uint32_t query_color = (palette->current_query && palette->current_query[0])
                                   ? palette->text_color
                                   : palette->shortcut_color;
        vg_font_draw_text(canvas,
                          palette->font,
                          palette->font_size,
                          x + text_inset,
                          y + 24.0f * scale,
                          query,
                          query_color);
    }

    // Draw filtered results
    float item_y = widget->y + search_height;
    size_t visible = palette->filtered_count;
    int visible_capacity = commandpalette_visible_capacity(palette);
    if (visible > (size_t)visible_capacity)
        visible = (size_t)visible_capacity;

    commandpalette_ensure_selection_visible(palette);

    for (size_t i = 0; i < visible; i++) {
        size_t filtered_index = (size_t)palette->first_visible_index + i;
        vg_command_t *cmd =
            filtered_index < palette->filtered_count ? palette->filtered[filtered_index] : NULL;
        if (!cmd)
            continue;

        // Draw item background
        if ((int)filtered_index == palette->selected_index) {
            vgfx_fill_rect(win,
                           (int32_t)(x + 1.0f),
                           (int32_t)item_y,
                           (int32_t)(w - 2.0f),
                           (int32_t)item_height,
                           palette->selected_bg);
        }

        // Draw label and shortcut
        if (palette->font && cmd->label) {
            float shortcut_x = x + w - shortcut_inset;
            float label_right = x + w - text_inset;
            bool draw_shortcut = false;
            vg_text_metrics_t shortcut_metrics = {0};
            if (cmd->shortcut) {
                vg_font_measure_text(
                    palette->font, palette->font_size, cmd->shortcut, &shortcut_metrics);
                shortcut_x -= shortcut_metrics.width;
                if (shortcut_x > x + text_inset) {
                    draw_shortcut = true;
                    label_right = shortcut_x - shortcut_gap;
                }
            }
            int32_t label_clip_width = (int32_t)(label_right - (x + text_inset));
            if (label_clip_width > 0) {
                vgfx_set_clip(win,
                              (int32_t)(x + text_inset),
                              (int32_t)item_y,
                              label_clip_width,
                              (int32_t)item_height);
                vg_font_draw_text(canvas,
                                  palette->font,
                                  palette->font_size,
                                  x + text_inset,
                                  item_y + 22.0f * scale,
                                  cmd->label,
                                  palette->text_color);
                vgfx_set_clip(win, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
            }

            if (draw_shortcut) {
                vg_font_draw_text(canvas,
                                  palette->font,
                                  palette->font_size,
                                  shortcut_x,
                                  item_y + 22.0f * scale,
                                  cmd->shortcut,
                                  palette->shortcut_color);
            }
        }

        item_y += item_height;
    }

    if (had_prior_clip)
        vgfx_set_clip(win, prior_clip_x, prior_clip_y, prior_clip_w, prior_clip_h);
    else
        vgfx_clear_clip(win);
}

/// @brief Handles query input, navigation, scrolling, and result activation.
///
/// @details Escape dismisses the palette, Enter executes the selection, arrow
/// keys move selection, and Backspace or character events edit the UTF-8 query.
/// Pointer motion selects hovered results, the wheel scrolls the visible
/// window, and clicks execute hit rows.
///
/// @param widget Palette base widget receiving the event.
/// @param event Event to inspect.
/// @return `true` when a visible palette consumes the event; otherwise `false`.
static bool commandpalette_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_commandpalette_t *palette = (vg_commandpalette_t *)widget;

    if (!palette->is_visible)
        return false;

    if (event->type == VG_EVENT_KEY_DOWN) {
        switch (event->key.key) {
            case VG_KEY_ESCAPE:
                vg_commandpalette_hide(palette);
                return true;

            case VG_KEY_ENTER:
                vg_commandpalette_execute_selected(palette);
                return true;

            case VG_KEY_UP:
                if (palette->selected_index > 0) {
                    palette->selected_index--;
                    commandpalette_ensure_selection_visible(palette);
                    palette->base.needs_paint = true;
                }
                return true;

            case VG_KEY_DOWN:
                if (palette->selected_index < (int)palette->filtered_count - 1) {
                    palette->selected_index++;
                    commandpalette_ensure_selection_visible(palette);
                    palette->base.needs_paint = true;
                }
                return true;

            case VG_KEY_BACKSPACE:
                remove_query_char(palette);
                return true;

            default:
                break;
        }
    }

    if (event->type == VG_EVENT_KEY_CHAR) {
        append_query_char(palette, event->key.codepoint);
        return true;
    }

    int visible_capacity = commandpalette_visible_capacity(palette);
    float search_height = commandpalette_search_height();
    float item_height = commandpalette_item_height(palette);

    if (event->type == VG_EVENT_MOUSE_WHEEL) {
        int direction = 0;
        if (event->wheel.delta_y > 0.0f)
            direction = -1;
        else if (event->wheel.delta_y < 0.0f)
            direction = 1;

        if (direction != 0 && visible_capacity > 0 &&
            palette->filtered_count > (size_t)visible_capacity) {
            int max_first = (int)palette->filtered_count - visible_capacity;
            palette->first_visible_index += direction;
            if (palette->first_visible_index < 0)
                palette->first_visible_index = 0;
            if (palette->first_visible_index > max_first)
                palette->first_visible_index = max_first;
            if (palette->selected_index < palette->first_visible_index)
                palette->selected_index = palette->first_visible_index;
            if (palette->selected_index >= palette->first_visible_index + visible_capacity) {
                palette->selected_index = palette->first_visible_index + visible_capacity - 1;
            }
            palette->hovered_index = -1;
            palette->base.needs_paint = true;
            return true;
        }
    }

    if (event->type == VG_EVENT_MOUSE_MOVE) {
        float local_y = event->mouse.y - search_height;
        if (local_y >= 0.0f) {
            int hovered = palette->first_visible_index + (int)(local_y / item_height);
            if (hovered >= 0 && hovered < (int)palette->filtered_count &&
                hovered < palette->first_visible_index + visible_capacity) {
                if (palette->hovered_index != hovered) {
                    palette->hovered_index = hovered;
                    palette->selected_index = hovered;
                    commandpalette_ensure_selection_visible(palette);
                    palette->base.needs_paint = true;
                }
                return true;
            }
        }
    }

    if (event->type == VG_EVENT_MOUSE_DOWN || event->type == VG_EVENT_CLICK) {
        float local_y = event->mouse.y - search_height;
        if (local_y >= 0.0f) {
            int clicked = palette->first_visible_index + (int)(local_y / item_height);
            if (clicked >= 0 && clicked < (int)palette->filtered_count &&
                clicked < palette->first_visible_index + visible_capacity) {
                palette->selected_index = clicked;
                commandpalette_ensure_selection_visible(palette);
                palette->base.needs_paint = true;
                if (event->type == VG_EVENT_CLICK)
                    vg_commandpalette_execute_selected(palette);
                return true;
            }
        }
    }

    return false;
}

/// @brief Registers a new command with the palette.
///
/// @details The identifier, label, and optional shortcut are copied into a
/// newly owned command. The master array grows geometrically, and a visible
/// palette is immediately refiltered. This function does not enforce identifier
/// uniqueness.
///
/// @param palette Palette to update; may be null.
/// @param id Required command identifier to copy.
/// @param label Required display label to copy.
/// @param shortcut Optional shortcut label to copy; may be null.
/// @param action Callback invoked when the command executes; may be null.
/// @param user_data Opaque pointer forwarded to action.
/// @return Owned command record, or null for invalid arguments or allocation
///         failure. The returned pointer remains owned by @p palette.
vg_command_t *vg_commandpalette_add_command(vg_commandpalette_t *palette,
                                            const char *id,
                                            const char *label,
                                            const char *shortcut,
                                            void (*action)(vg_command_t *, void *),
                                            void *user_data) {
    if (!palette || !id || !label)
        return NULL;

    vg_command_t *cmd = calloc(1, sizeof(vg_command_t));
    if (!cmd)
        return NULL;

    cmd->id = vg_strdup(id);
    cmd->label = vg_strdup(label);
    if (shortcut)
        cmd->shortcut = vg_strdup(shortcut);
    if (!cmd->id || !cmd->label || (shortcut && !cmd->shortcut)) {
        free_command(cmd);
        return NULL;
    }
    cmd->action = action;
    cmd->user_data = user_data;
    cmd->enabled = true;

    // Add to array
    if (palette->command_count >= palette->command_capacity) {
        size_t new_cap = palette->command_capacity * 2;
        if (new_cap < 32)
            new_cap = 32;
        if (new_cap <= palette->command_capacity || new_cap > SIZE_MAX / sizeof(vg_command_t *)) {
            free_command(cmd);
            return NULL;
        }
        vg_command_t **new_cmds = realloc(palette->commands, new_cap * sizeof(vg_command_t *));
        if (!new_cmds) {
            free_command(cmd);
            return NULL;
        }
        palette->commands = new_cmds;
        palette->command_capacity = new_cap;
    }

    palette->commands[palette->command_count++] = cmd;

    // Re-filter if visible
    if (palette->is_visible) {
        filter_commands(palette);
    }

    return cmd;
}

/// @brief Removes the first registered command with a matching identifier.
///
/// @details The command is destroyed, later master-array entries shift left,
/// and a visible palette is refiltered. A missing identifier leaves state
/// unchanged.
///
/// @param palette Palette to modify; may be null.
/// @param id Identifier of the command to remove; may be null.
void vg_commandpalette_remove_command(vg_commandpalette_t *palette, const char *id) {
    if (!palette || !id)
        return;

    for (size_t i = 0; i < palette->command_count; i++) {
        if (palette->commands[i] && strcmp(palette->commands[i]->id, id) == 0) {
            free_command(palette->commands[i]);
            // Shift remaining
            for (size_t j = i; j < palette->command_count - 1; j++) {
                palette->commands[j] = palette->commands[j + 1];
            }
            palette->command_count--;

            if (palette->is_visible) {
                filter_commands(palette);
            }
            return;
        }
    }
}

/// @brief Looks up the first registered command with a matching identifier.
///
/// @param palette Palette to search; may be null.
/// @param id Identifier to match; may be null.
/// @return Borrowed command pointer, or null when no match exists.
vg_command_t *vg_commandpalette_get_command(vg_commandpalette_t *palette, const char *id) {
    if (!palette || !id)
        return NULL;

    for (size_t i = 0; i < palette->command_count; i++) {
        if (palette->commands[i] && strcmp(palette->commands[i]->id, id) == 0) {
            return palette->commands[i];
        }
    }
    return NULL;
}

/// @brief Shows the palette with a cleared query and rebuilt result list.
///
/// @details Both visibility flags are enabled, the query generation advances,
/// selection and scrolling are reset by filtering, and layout plus paint are
/// scheduled.
///
/// @param palette Palette to show; may be null.
void vg_commandpalette_show(vg_commandpalette_t *palette) {
    if (!palette)
        return;

    palette->is_visible = true;
    palette->base.visible = true;

    // Clear search and filter all
    free(palette->current_query);
    palette->current_query = NULL;
    palette->query_generation++;
    filter_commands(palette);
    palette->first_visible_index = 0;

    palette->base.needs_paint = true;
    palette->base.needs_layout = true;
}

/// @brief Hides the palette and invokes its dismissal callback.
///
/// @details Both palette-specific and base-widget visibility are cleared before
/// the callback runs.
///
/// @param palette Palette to hide; may be null.
void vg_commandpalette_hide(vg_commandpalette_t *palette) {
    if (!palette)
        return;

    palette->is_visible = false;
    palette->base.visible = false;

    if (palette->on_dismiss) {
        palette->on_dismiss(palette, palette->user_data);
    }
}

/// @brief Toggles palette visibility through the normal show and hide paths.
///
/// @param palette Palette to toggle; may be null.
void vg_commandpalette_toggle(vg_commandpalette_t *palette) {
    if (!palette)
        return;

    if (palette->is_visible) {
        vg_commandpalette_hide(palette);
    } else {
        vg_commandpalette_show(palette);
    }
}

/// @brief Executes the selected command and then hides the palette.
///
/// @details The selected command is snapshotted, including copies of its owned
/// strings, before callbacks run. This permits callbacks to mutate or destroy
/// command registrations safely. The command action runs first, followed by the
/// palette execution callback and dismissal when the palette remains live.
///
/// @param palette Palette whose selection to execute; may be null.
void vg_commandpalette_execute_selected(vg_commandpalette_t *palette) {
    if (!palette)
        return;
    if (palette->selected_index < 0 || palette->selected_index >= (int)palette->filtered_count)
        return;

    vg_command_t *cmd = palette->filtered[palette->selected_index];
    if (!cmd || !cmd->enabled)
        return;

    vg_command_t snapshot = *cmd;
    snapshot.id = cmd->id ? vg_strdup(cmd->id) : NULL;
    snapshot.label = cmd->label ? vg_strdup(cmd->label) : NULL;
    snapshot.description = cmd->description ? vg_strdup(cmd->description) : NULL;
    snapshot.shortcut = cmd->shortcut ? vg_strdup(cmd->shortcut) : NULL;
    snapshot.category = cmd->category ? vg_strdup(cmd->category) : NULL;
    if ((cmd->id && !snapshot.id) || (cmd->label && !snapshot.label) ||
        (cmd->description && !snapshot.description) || (cmd->category && !snapshot.category) ||
        (cmd->shortcut && !snapshot.shortcut)) {
        free(snapshot.id);
        free(snapshot.label);
        free(snapshot.description);
        free(snapshot.shortcut);
        free(snapshot.category);
        return;
    }
    void (*action)(vg_command_t *, void *) = cmd->action;
    void *action_user_data = cmd->user_data;
    void (*on_execute)(vg_commandpalette_t *, vg_command_t *, void *) = palette->on_execute;
    void *on_execute_user_data = palette->user_data;

    // Execute action
    if (action) {
        action(&snapshot, action_user_data);
    }

    // Notify callback
    if (vg_widget_is_live(&palette->base) && on_execute) {
        on_execute(palette, &snapshot, on_execute_user_data);
    }

    // Hide palette
    if (vg_widget_is_live(&palette->base))
        vg_commandpalette_hide(palette);
    free(snapshot.id);
    free(snapshot.label);
    free(snapshot.description);
    free(snapshot.shortcut);
    free(snapshot.category);
}

/// @brief Registers command-execution and palette-dismissal callbacks.
///
/// @param palette Palette to configure; may be null.
/// @param on_execute Called after a command action and before hiding; may be
///                   null.
/// @param on_dismiss Called after visibility is cleared; may be null.
/// @param user_data Opaque pointer forwarded to both callbacks.
void vg_commandpalette_set_callbacks(vg_commandpalette_t *palette,
                                     void (*on_execute)(vg_commandpalette_t *,
                                                        vg_command_t *,
                                                        void *),
                                     void (*on_dismiss)(vg_commandpalette_t *, void *),
                                     void *user_data) {
    if (!palette)
        return;

    palette->on_execute = on_execute;
    palette->on_dismiss = on_dismiss;
    palette->user_data = user_data;
}

/// @brief Sets the font and size used for all command-palette text.
///
/// @details The font is borrowed and must remain valid while the palette uses
/// it. A changed pair schedules repaint.
///
/// @param palette Palette to configure; may be null.
/// @param font Borrowed font for query, placeholder, labels, and shortcuts.
/// @param size Font size passed to text measurement and rendering.
void vg_commandpalette_set_font(vg_commandpalette_t *palette, vg_font_t *font, float size) {
    if (!palette)
        return;
    if (palette->font == font && palette->font_size == size)
        return;

    palette->font = font;
    palette->font_size = size;
    palette->base.needs_paint = true;
}

/// @brief Sets the placeholder shown while the query is empty.
///
/// @details The input is copied before the previous placeholder is released.
/// Allocation failure preserves the prior value.
///
/// @param palette Palette to configure; may be null.
/// @param text Placeholder string to copy; may be null to clear.
void vg_commandpalette_set_placeholder(vg_commandpalette_t *palette, const char *text) {
    if (!palette)
        return;
    char *copy = text ? vg_strdup(text) : NULL;
    if (text && !copy)
        return;
    free(palette->placeholder_text);
    palette->placeholder_text = copy;
    palette->base.needs_paint = true;
}

/// @brief Clears all commands and result-selection state.
///
/// @details Individual command objects are destroyed, while the master and
/// filtered pointer-array allocations are retained for reuse.
///
/// @param palette Palette to clear; may be null.
void vg_commandpalette_clear(vg_commandpalette_t *palette) {
    if (!palette)
        return;

    for (size_t i = 0; i < palette->command_count; i++)
        free_command(palette->commands[i]);

    palette->command_count = 0;
    /* Keep the allocation; clear filtered list */
    palette->filtered_count = 0;
    palette->selected_index = -1;
    palette->hovered_index = -1;
    palette->first_visible_index = 0;
    palette->base.needs_paint = true;
}
