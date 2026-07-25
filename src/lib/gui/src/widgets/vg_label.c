//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/widgets/vg_label.c
// Purpose: Label widget implementation — single/multi-line text display with
//          word-wrap, horizontal/vertical alignment, and optional font override.
// Key invariants:
//   - label->text is always a valid heap-allocated string (never NULL).
//   - The word-wrap line cache is invalidated whenever text or font changes.
// Ownership/Lifetime:
//   - vg_label_create copies the text string; label_destroy frees it.
//   - The line cache is heap-allocated per-layout and freed on text/font change.
// Links: lib/gui/include/vg_widgets.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_font.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements text labels with wrapping, ellipsis, alignment, optional
///        vector icons, and read-only UTF-8 selection.
/// @details The widget owns its source text and derived display caches. Wrapping
///          records source-byte spans for selection mapping, and all cache
///          invalidation is tied to changes in text, typography, or fitting
///          policy.
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_icon_vector.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Forward Declarations
//=============================================================================

static void label_destroy(vg_widget_t *widget);
static void label_measure(vg_widget_t *widget, float available_width, float available_height);
static void label_paint(vg_widget_t *widget, void *canvas);
static bool label_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool label_can_focus(vg_widget_t *widget);
/// @brief Quantize a wrap width to a stable quarter-pixel cache key.
static float label_quantized_wrap_width(float wrap_width);

//=============================================================================
// Label VTable
//=============================================================================

static vg_widget_vtable_t g_label_vtable = {.destroy = label_destroy,
                                            .measure = label_measure,
                                            .arrange = NULL, // Labels don't have children to layout
                                            .paint = label_paint,
                                            .handle_event = label_handle_event,
                                            .can_focus = label_can_focus,
                                            .on_focus = NULL};

//=============================================================================
// Text Wrapping Helpers
//=============================================================================

/// @brief Return the byte length of the UTF-8 unit beginning at @p text.
/// @details Invalid or truncated leading bytes are treated as one-byte units so
///          wrapping continues to make progress on malformed text.
/// @param text Pointer to a UTF-8 byte sequence.
/// @param remaining Number of available bytes at @p text.
/// @return Byte length in the range [1, remaining] when remaining > 0, otherwise 0.
static size_t label_utf8_unit_len(const char *text, size_t remaining) {
    if (!text || remaining == 0)
        return 0;
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80)
        return 1;
    if ((c & 0xE0u) == 0xC0u && remaining >= 2)
        return 2;
    if ((c & 0xF0u) == 0xE0u && remaining >= 3)
        return 3;
    if ((c & 0xF8u) == 0xF0u && remaining >= 4)
        return 4;
    return 1;
}

/// @brief Clamp @p len down to a UTF-8 unit boundary in @p text.
/// @details The returned length does not end immediately after a partial
///          continuation sequence unless the first byte itself is malformed.
/// @param text UTF-8 byte sequence.
/// @param len Candidate prefix byte length.
/// @return Prefix length ending at a UTF-8 unit boundary.
static size_t label_utf8_boundary_len(const char *text, size_t len) {
    if (!text || len == 0)
        return 0;
    while (len > 1 && (((unsigned char)text[len] & 0xC0u) == 0x80u))
        len--;
    return len;
}

/// @brief Measure a prefix of a word using the label's current font.
/// @param label Label whose font metrics are used.
/// @param word Source word segment.
/// @param len Prefix byte length to measure.
/// @param scratch Caller-provided buffer of at least len + 1 bytes.
/// @return Measured width in pixels.
static float label_measure_word_prefix(vg_label_t *label,
                                       const char *word,
                                       size_t len,
                                       char *scratch) {
    memcpy(scratch, word, len);
    scratch[len] = '\0';
    vg_text_metrics_t metrics;
    vg_font_measure_text(label->font, label->font_size, scratch, &metrics);
    return metrics.width;
}

/// @brief Find the longest word prefix that fits within @p max_width.
/// @details Uses binary search over byte lengths and clamps probes to UTF-8 unit
///          boundaries. If the first unit is wider than @p max_width, that unit
///          is returned so the caller always advances.
/// @param label Label whose font metrics are used.
/// @param word Start of the word segment.
/// @param word_len Available bytes in @p word.
/// @param max_width Available width in pixels.
/// @param scratch Caller-provided buffer of at least word_len + 1 bytes.
/// @return Prefix byte length to place on the current line.
static size_t label_fit_word_prefix(
    vg_label_t *label, const char *word, size_t word_len, float max_width, char *scratch) {
    if (!label || !word || !scratch || word_len == 0)
        return 0;
    if (max_width <= 0.0f)
        return label_utf8_unit_len(word, word_len);

    size_t low = 1;
    size_t high = word_len;
    size_t best = 0;
    while (low <= high) {
        size_t mid = low + (high - low) / 2u;
        mid = label_utf8_boundary_len(word, mid);
        if (mid == 0)
            mid = 1;
        float width = label_measure_word_prefix(label, word, mid, scratch);
        if (width <= max_width) {
            best = mid;
            if (mid == word_len)
                break;
            low = mid + 1u;
        } else {
            if (mid <= 1)
                break;
            high = mid - 1u;
        }
    }
    return best > 0 ? best : label_utf8_unit_len(word, word_len);
}

/// @brief Create a UTF-8-safe copy fitted to @p max_width with a trailing ellipsis when needed.
/// @details The complete input is returned unchanged when it fits. Otherwise the longest measured
///          prefix that leaves room for U+2026 is copied, never ending within a UTF-8 unit.
///          Allocation failure returns NULL without altering label state.
/// @param label Label supplying font metrics.
/// @param text Null-terminated UTF-8 source text.
/// @param max_width Maximum rendered width in physical pixels.
/// @param force_ellipsis true to append U+2026 even when @p text alone fits.
/// @param out_source_bytes Receives source-prefix bytes represented before the ellipsis.
/// @return Newly allocated display string, or NULL on invalid input/allocation failure.
static char *label_make_ellipsized_text(vg_label_t *label,
                                        const char *text,
                                        float max_width,
                                        bool force_ellipsis,
                                        size_t *out_source_bytes) {
    if (out_source_bytes)
        *out_source_bytes = 0;
    if (!label || !text || !label->font)
        return NULL;

    size_t text_len = strlen(text);
    vg_text_metrics_t full_metrics;
    vg_font_measure_text(label->font, label->font_size, text, &full_metrics);
    if (!force_ellipsis && max_width > 0.0f && full_metrics.width <= max_width) {
        if (out_source_bytes)
            *out_source_bytes = text_len;
        return vg_strdup(text);
    }
    if (max_width <= 0.0f)
        return vg_strdup("");

    static const char ellipsis[] = "\xE2\x80\xA6";
    vg_text_metrics_t ellipsis_metrics;
    vg_font_measure_text(label->font, label->font_size, ellipsis, &ellipsis_metrics);
    float prefix_width = max_width - ellipsis_metrics.width;
    size_t prefix_len = force_ellipsis && full_metrics.width <= prefix_width ? text_len : 0;
    char *scratch = malloc(text_len + 1);
    if (!scratch)
        return NULL;
    if (prefix_len == 0 && prefix_width > 0.0f && text_len > 0) {
        prefix_len = label_fit_word_prefix(label, text, text_len, prefix_width, scratch);
        if (prefix_len > text_len)
            prefix_len = text_len;
        if (prefix_len > 0 &&
            label_measure_word_prefix(label, text, prefix_len, scratch) > prefix_width)
            prefix_len = 0;
    }
    free(scratch);

    if (prefix_len > SIZE_MAX - sizeof(ellipsis))
        return NULL;
    char *result = malloc(prefix_len + sizeof(ellipsis));
    if (!result)
        return NULL;
    memcpy(result, text, prefix_len);
    memcpy(result + prefix_len, ellipsis, sizeof(ellipsis));
    if (out_source_bytes)
        *out_source_bytes = prefix_len;
    return result;
}

/// @brief Return the cached single-line display string for the current arranged width.
/// @details Ellipsis-disabled labels borrow their original text. Ellipsis-enabled labels rebuild
///          a cache only when the quantized width changes. On allocation failure the complete
///          source remains visible rather than losing text.
/// @param label Label to inspect and, if needed, update.
/// @param width Available physical width.
/// @param out_source_bytes Receives source bytes represented before any ellipsis.
/// @return Borrowed display text owned by @p label.
static char *label_single_line_display(vg_label_t *label, float width, size_t *out_source_bytes) {
    size_t text_len = label && label->text ? strlen(label->text) : 0;
    if (out_source_bytes)
        *out_source_bytes = text_len;
    if (!label || !label->text || !label->ellipsis)
        return label ? label->text : NULL;

    float quantized = label_quantized_wrap_width(width);
    if (!label->ellipsis_cache || label->ellipsis_cached_w != quantized) {
        size_t source_bytes = 0;
        char *fitted =
            label_make_ellipsized_text(label, label->text, quantized, false, &source_bytes);
        if (fitted) {
            free(label->ellipsis_cache);
            label->ellipsis_cache = fitted;
            label->ellipsis_cached_w = quantized;
            label->ellipsis_source_bytes = source_bytes;
        }
    }
    if (!label->ellipsis_cache)
        return label->text;
    if (out_source_bytes)
        *out_source_bytes = label->ellipsis_source_bytes;
    return label->ellipsis_cache;
}

/// @brief Measure a mutable UTF-8 buffer prefix without allocating.
/// @details The byte at @p prefix_len is temporarily replaced with NUL for the synchronous font
///          measurement and restored before returning. GUI access is main-thread confined.
/// @param label Label supplying font metrics.
/// @param text Mutable null-terminated text buffer.
/// @param prefix_len Byte prefix to measure, clamped to strlen(text).
/// @return Rendered prefix width in pixels, or zero for invalid input/an empty prefix.
static float label_measure_mutable_prefix(vg_label_t *label, char *text, size_t prefix_len) {
    if (!label || !label->font || !text || prefix_len == 0)
        return 0.0f;
    size_t len = strlen(text);
    if (prefix_len > len)
        prefix_len = len;
    char saved = text[prefix_len];
    text[prefix_len] = '\0';
    vg_text_metrics_t metrics;
    vg_font_measure_text(label->font, label->font_size, text, &metrics);
    text[prefix_len] = saved;
    return metrics.width;
}

//=============================================================================
// Label Implementation
//=============================================================================

/// @brief Create a label widget with the given text.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @param text   Display text; copied internally. An empty string is used if NULL.
/// @return Newly allocated vg_label_t, or NULL on allocation failure.
vg_label_t *vg_label_create(vg_widget_t *parent, const char *text) {
    vg_label_t *label = calloc(1, sizeof(vg_label_t));
    if (!label)
        return NULL;

    // Initialize base widget
    vg_widget_init(&label->base, VG_WIDGET_LABEL, &g_label_vtable);

    // Initialize label-specific fields
    label->text = text ? vg_strdup(text) : vg_strdup("");
    if (!label->text) {
        vg_widget_destroy(&label->base);
        return NULL;
    }
    vg_theme_t *theme = vg_theme_get_current();
    label->font = theme->typography.font_regular;
    label->font_size = theme->typography.size_normal;
    label->text_color = theme->colors.fg_primary;
    label->h_align = VG_ALIGN_H_LEFT;
    label->v_align = VG_ALIGN_V_CENTER;
    label->word_wrap = false;
    label->icon_vector_id = -1;
    label->max_lines = 0;
    label->ellipsis = false;
    label->selectable = false;
    label->selection_dragging = false;
    label->selection_anchor = 0;
    label->selection_start = 0;
    label->selection_end = 0;
    label->wrap_line_bufs = NULL;
    label->wrap_source_starts = NULL;
    label->wrap_source_ends = NULL;
    label->wrap_line_count = 0;
    label->wrap_cached_w = -1.0f;
    label->wrap_truncated = false;
    label->ellipsis_cache = NULL;
    label->ellipsis_cached_w = -1.0f;
    label->ellipsis_source_bytes = 0;

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &label->base);
    }

    return label;
}

/// @brief Release all wrapped-line and ellipsis display caches.
/// @details Source text and typography are retained. Cache metadata is reset so
///          the next measure or paint rebuilds display strings for its width.
/// @param label Label whose derived text storage is invalidated.
static void label_free_wrap_cache(vg_label_t *label) {
    if (label->wrap_line_bufs) {
        for (int i = 0; i < label->wrap_line_count; i++)
            free(label->wrap_line_bufs[i]);
        free(label->wrap_line_bufs);
        label->wrap_line_bufs = NULL;
    }
    free(label->wrap_source_starts);
    free(label->wrap_source_ends);
    label->wrap_source_starts = NULL;
    label->wrap_source_ends = NULL;
    label->wrap_line_count = 0;
    label->wrap_cached_w = -1.0f;
    label->wrap_truncated = false;
    free(label->ellipsis_cache);
    label->ellipsis_cache = NULL;
    label->ellipsis_cached_w = -1.0f;
    label->ellipsis_source_bytes = 0;
}

/// @brief Quantize a label wrap width for stable cache reuse.
/// @details Layout code can pass widths that differ by tiny floating-point
///          roundoff even though they resolve to the same pixel columns.  The
///          wrap cache only needs subpixel stability, so quarter-pixel buckets
///          prevent redundant cache rebuilds without visibly changing wrapping.
/// @param wrap_width Available text width in pixels.
/// @return Width rounded to the nearest quarter pixel, or 0 for invalid values.
static float label_quantized_wrap_width(float wrap_width) {
    if (wrap_width <= 0.0f)
        return 0.0f;
    if (wrap_width >= (float)INT_MAX / 4.0f)
        return (float)INT_MAX / 4.0f;
    int quarter_pixels = (int)(wrap_width * 4.0f + 0.5f);
    return (float)quarter_pixels / 4.0f;
}

/// @brief Release text, display caches, and any active input capture.
/// @param widget Label widget base being destroyed.
static void label_destroy(vg_widget_t *widget) {
    vg_label_t *label = (vg_label_t *)widget;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    if (label->text) {
        free(label->text);
        label->text = NULL;
    }
    label_free_wrap_cache(label);
}

/// @brief Build or reuse the greedy word-wrap cache and measure its height.
/// @details Each cached line owns a null-terminated display string plus the
///          corresponding half-open source-byte range. Long words are split only
///          at UTF-8 unit boundaries. The maximum-line and ellipsis policies are
///          applied during construction, and a matching quantized width reuses
///          the existing cache.
/// @param label Label supplying source text, typography, and wrap policy.
/// @param wrap_width Available physical width, quantized for stable caching.
/// @param line_height Height assigned to each produced line.
/// @param out_total_height Optional destination for line count times line height.
/// @return Number of lines produced. Allocation failure reports one fallback
///         line without installing a partial cache.
static int label_measure_wrapped(vg_label_t *label,
                                 float wrap_width,
                                 float line_height,
                                 float *out_total_height) {
    wrap_width = label_quantized_wrap_width(wrap_width);

    /* Cache hit: same width as last measure — reuse stored lines. */
    if (label->wrap_line_bufs && label->wrap_cached_w == wrap_width) {
        if (out_total_height)
            *out_total_height = label->wrap_line_count * line_height;
        return label->wrap_line_count;
    }

    /* Cache miss: invalidate old cache and rebuild. */
    label_free_wrap_cache(label);

    size_t text_len = strlen(label->text);
    char *word_buf = malloc(text_len + 1);
    char *line_buf = malloc(text_len + 1);
    if (!word_buf || !line_buf) {
        free(word_buf);
        free(line_buf);
        if (out_total_height)
            *out_total_height = line_height;
        return 1;
    }

    /* Measure space width once */
    vg_text_metrics_t sm;
    vg_font_measure_text(label->font, label->font_size, " ", &sm);
    float space_w = sm.width;

    /* Dynamic cache array */
    int cache_cap = 8;
    char **cache = malloc((size_t)cache_cap * sizeof(char *));
    size_t *source_starts = malloc((size_t)cache_cap * sizeof(size_t));
    size_t *source_ends = malloc((size_t)cache_cap * sizeof(size_t));
    int line_count = 0;
    if (!cache || !source_starts || !source_ends) {
        free(cache);
        free(source_starts);
        free(source_ends);
        free(word_buf);
        free(line_buf);
        if (out_total_height)
            *out_total_height = line_height;
        return 1;
    }

    const char *p = label->text;
    size_t line_pos = 0;
    float line_w = 0.0f;
    size_t line_source_start = SIZE_MAX;
    size_t line_source_end = 0;

    /* Helper lambda (inline): flush current line_buf to cache */
#define FLUSH_LINE()                                                                               \
    do {                                                                                           \
        if (line_count == cache_cap) {                                                             \
            if (cache_cap > INT_MAX / 2)                                                           \
                goto wrap_oom;                                                                     \
            int new_cap = cache_cap * 2;                                                           \
            if ((size_t)new_cap > SIZE_MAX / sizeof(char *))                                       \
                goto wrap_oom;                                                                     \
            char **tmp = realloc(cache, (size_t)new_cap * sizeof(char *));                         \
            if (!tmp)                                                                              \
                goto wrap_oom;                                                                     \
            cache = tmp;                                                                           \
            size_t *tmp_starts = realloc(source_starts, (size_t)new_cap * sizeof(size_t));         \
            if (!tmp_starts)                                                                       \
                goto wrap_oom;                                                                     \
            source_starts = tmp_starts;                                                            \
            size_t *tmp_ends = realloc(source_ends, (size_t)new_cap * sizeof(size_t));             \
            if (!tmp_ends)                                                                         \
                goto wrap_oom;                                                                     \
            source_ends = tmp_ends;                                                                \
            cache_cap = new_cap;                                                                   \
        }                                                                                          \
        line_buf[line_pos] = '\0';                                                                 \
        cache[line_count] = vg_strdup(line_buf);                                                   \
        if (!cache[line_count])                                                                    \
            goto wrap_oom;                                                                         \
        source_starts[line_count] =                                                                \
            line_source_start == SIZE_MAX ? (size_t)(p - label->text) : line_source_start;         \
        source_ends[line_count] =                                                                  \
            line_source_start == SIZE_MAX ? source_starts[line_count] : line_source_end;           \
        line_count++;                                                                              \
        line_pos = 0;                                                                              \
        line_w = 0.0f;                                                                             \
        line_source_start = SIZE_MAX;                                                              \
        line_source_end = 0;                                                                       \
    } while (0)

    while (*p) {
        /* Collect next word */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\n')
            p++;
        size_t word_len = (size_t)(p - start);

        if (word_len > 0) {
            const char *segment = start;
            size_t remaining = word_len;
            bool first_segment = true;
            while (remaining > 0) {
                float sep_w = (line_w > 0.0f && first_segment) ? space_w : 0.0f;
                size_t fit_len = remaining;
                float available = wrap_width - line_w - sep_w;
                if (available < 0.0f)
                    available = 0.0f;
                float segment_w = label_measure_word_prefix(label, segment, fit_len, word_buf);

                if (line_w > 0.0f && sep_w + segment_w > wrap_width - line_w) {
                    FLUSH_LINE();
                    if (label->max_lines > 0 && line_count >= label->max_lines) {
                        label->wrap_truncated = true;
                        goto wrap_done;
                    }
                    sep_w = 0.0f;
                    available = wrap_width;
                    segment_w = label_measure_word_prefix(label, segment, fit_len, word_buf);
                }

                if (segment_w > available && wrap_width > 0.0f) {
                    fit_len = label_fit_word_prefix(label, segment, remaining, available, word_buf);
                    segment_w = label_measure_word_prefix(label, segment, fit_len, word_buf);
                }

                if (line_w > 0.0f && first_segment) {
                    line_buf[line_pos++] = ' ';
                    line_w += sep_w;
                }
                size_t segment_offset = (size_t)(segment - label->text);
                if (line_source_start == SIZE_MAX)
                    line_source_start = segment_offset;
                line_source_end = segment_offset + fit_len;
                memcpy(line_buf + line_pos, segment, fit_len);
                line_pos += fit_len;
                line_w += segment_w;
                segment += fit_len;
                remaining -= fit_len;
                first_segment = false;

                if (remaining > 0) {
                    FLUSH_LINE();
                    if (label->max_lines > 0 && line_count >= label->max_lines) {
                        label->wrap_truncated = true;
                        goto wrap_done;
                    }
                }
            }
        }

        /* Skip spaces */
        while (*p == ' ')
            p++;
        if (*p == '\n') {
            FLUSH_LINE();
            if (label->max_lines > 0 && line_count >= label->max_lines && p[1] != '\0') {
                label->wrap_truncated = true;
                goto wrap_done;
            }
            p++;
        }
    }
    /* Flush last line if non-empty */
    if (line_pos > 0 || line_count == 0)
        FLUSH_LINE();

wrap_done:
#undef FLUSH_LINE
    free(word_buf);
    free(line_buf);

    if (label->ellipsis && label->wrap_truncated && line_count > 0) {
        size_t represented = 0;
        char *fitted = label_make_ellipsized_text(
            label, cache[line_count - 1], wrap_width, true, &represented);
        if (fitted) {
            free(cache[line_count - 1]);
            cache[line_count - 1] = fitted;
            size_t mapped_end = source_starts[line_count - 1] + represented;
            if (mapped_end < source_ends[line_count - 1])
                source_ends[line_count - 1] = mapped_end;
        }
    }

    label->wrap_line_bufs = cache;
    label->wrap_source_starts = source_starts;
    label->wrap_source_ends = source_ends;
    label->wrap_line_count = line_count;
    label->wrap_cached_w = wrap_width;

    if (out_total_height)
        *out_total_height = line_count * line_height;
    return line_count;

wrap_oom:
    free(word_buf);
    free(line_buf);
    for (int k = 0; k < line_count; k++)
        free(cache[k]);
    free(cache);
    free(source_starts);
    free(source_ends);
    if (out_total_height)
        *out_total_height = line_height;
    return 1;
}

/// @brief Measure wrapped or single-line label content and apply constraints.
/// @details Wrapped labels occupy the offered width and derive height from the
///          line cache. Single-line labels use font metrics, include optional
///          icon space, and clamp ellipsized content to the offered width.
/// @param widget Label widget base to measure.
/// @param available_width Width offered by the parent layout.
/// @param available_height Height offered by the parent; currently unused
///                         before widget constraints are applied.
static void label_measure(vg_widget_t *widget, float available_width, float available_height) {
    vg_label_t *label = (vg_label_t *)widget;
    (void)available_height;

    if (!label->text || !label->font) {
        // No text or font - use minimum size
        widget->measured_width = widget->constraints.min_width;
        widget->measured_height = widget->constraints.min_height;
        return;
    }

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(label->font, label->font_size, &font_metrics);
    float line_height = (float)font_metrics.line_height;

    if (label->word_wrap && available_width > 0.0f) {
        float wrap_w = available_width;
        if (widget->constraints.max_width > 0.0f && widget->constraints.max_width < wrap_w)
            wrap_w = widget->constraints.max_width;

        float total_h = 0.0f;
        label_measure_wrapped(label, wrap_w, line_height, &total_h);
        widget->measured_width = wrap_w;
        widget->measured_height = total_h;
    } else {
        // Measure text
        vg_text_metrics_t metrics;
        vg_font_measure_text(label->font, label->font_size, label->text, &metrics);

        widget->measured_width = metrics.width;
        if (label->icon_vector_id >= 0)
            widget->measured_width += label->font_size + 7.0f; // icon box + gap
        if (label->ellipsis && available_width > 0.0f && widget->measured_width > available_width)
            widget->measured_width = available_width;
        widget->measured_height = metrics.height;
    }

    vg_widget_apply_constraints(widget);
}

/// @brief Return the local horizontal origin for one rendered label line.
/// @param label Label supplying alignment and font metrics.
/// @param line_text Null-terminated line to measure.
/// @param width Available widget width.
/// @return Local X coordinate for left, center, or right alignment.
static float label_line_origin_x(vg_label_t *label, const char *line_text, float width) {
    if (!label || !line_text || !label->font)
        return 0.0f;
    if (label->h_align == VG_ALIGN_H_LEFT)
        return 0.0f;
    vg_text_metrics_t metrics;
    vg_font_measure_text(label->font, label->font_size, line_text, &metrics);
    return label->h_align == VG_ALIGN_H_CENTER ? (width - metrics.width) * 0.5f
                                               : width - metrics.width;
}

/// @brief Return the local top coordinate for a block of label lines.
/// @param label Label supplying vertical alignment.
/// @param content_height Total rendered block height.
/// @return Local Y coordinate honoring top, center, bottom, and baseline modes.
static float label_block_origin_y(vg_label_t *label, float content_height) {
    if (!label)
        return 0.0f;
    if (label->v_align == VG_ALIGN_V_CENTER)
        return (label->base.height - content_height) * 0.5f;
    if (label->v_align == VG_ALIGN_V_BOTTOM)
        return label->base.height - content_height;
    return 0.0f;
}

/// @brief Convert a local X position into a UTF-8 byte boundary within rendered text.
/// @details Each unit is compared at the midpoint between measured prefix advances, yielding the
///          nearest caret boundary without splitting a UTF-8 unit.
/// @param label Label supplying font metrics.
/// @param text Mutable rendered text buffer.
/// @param local_x X coordinate relative to the rendered line origin.
/// @return Byte boundary in the inclusive range [0, strlen(text)].
static size_t label_hit_test_line(vg_label_t *label, char *text, float local_x) {
    if (!label || !text || local_x <= 0.0f)
        return 0;
    size_t len = strlen(text);
    size_t offset = 0;
    float previous_width = 0.0f;
    while (offset < len) {
        size_t unit = label_utf8_unit_len(text + offset, len - offset);
        size_t next = offset + (unit ? unit : 1);
        float next_width = label_measure_mutable_prefix(label, text, next);
        if (local_x < (previous_width + next_width) * 0.5f)
            return offset;
        previous_width = next_width;
        offset = next;
    }
    return len;
}

/// @brief Map a pointer position to a source-text UTF-8 byte boundary.
/// @details Wrapped-line source ranges retain the original source span. Normalized runs of spaces
///          can make intra-line mapping approximate, but endpoints remain clamped and UTF-8 safe.
/// @param label Selectable label to hit-test.
/// @param local_x Pointer X relative to the label.
/// @param local_y Pointer Y relative to the label.
/// @return Clamped byte offset in label->text.
static size_t label_source_offset_at(vg_label_t *label, float local_x, float local_y) {
    if (!label || !label->text || !label->font)
        return 0;
    size_t text_len = strlen(label->text);
    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(label->font, label->font_size, &font_metrics);
    float line_height = (float)font_metrics.line_height;

    if (label->word_wrap && label->base.width > 0.0f) {
        label_measure_wrapped(label, label->base.width, line_height, NULL);
        if (label->wrap_line_count <= 0)
            return 0;
        float block_height = (float)label->wrap_line_count * line_height;
        float top = label_block_origin_y(label, block_height);
        int line = local_y <= top ? 0 : (int)((local_y - top) / line_height);
        if (line >= label->wrap_line_count)
            line = label->wrap_line_count - 1;
        char *line_text = label->wrap_line_bufs[line];
        float origin_x = label_line_origin_x(label, line_text, label->base.width);
        size_t display_offset = label_hit_test_line(label, line_text, local_x - origin_x);
        size_t source_start = label->wrap_source_starts[line];
        size_t source_end = label->wrap_source_ends[line];
        size_t source_span = source_end >= source_start ? source_end - source_start : 0;
        if (display_offset > source_span)
            display_offset = source_span;
        size_t result = source_start + display_offset;
        return result <= text_len ? result : text_len;
    }

    size_t represented = text_len;
    char *display = label_single_line_display(label, label->base.width, &represented);
    if (!display)
        return 0;
    float origin_x = label_line_origin_x(label, display, label->base.width);
    size_t display_offset = label_hit_test_line(label, display, local_x - origin_x);
    if (display_offset > represented)
        display_offset = represented;
    return display_offset <= text_len ? display_offset : text_len;
}

/// @brief Update a label's selection endpoints and invalidate paint when they change.
/// @param label Label to update.
/// @param start First source byte endpoint.
/// @param end Second source byte endpoint.
static void label_set_selection(vg_label_t *label, size_t start, size_t end) {
    if (!label || !label->text)
        return;
    size_t len = strlen(label->text);
    if (start > len)
        start = len;
    if (end > len)
        end = len;
    if (label->selection_start == start && label->selection_end == end)
        return;
    label->selection_start = start;
    label->selection_end = end;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Copy the selected source bytes into a null-terminated clipboard buffer.
/// @param label Label whose ordered selection is copied.
/// @return Newly allocated selection, or NULL when empty/allocation fails.
static char *label_copy_selection(const vg_label_t *label) {
    size_t length = 0;
    const char *selection = vg_label_get_selected_text(label, &length);
    if (!selection || length == 0)
        return NULL;
    char *copy = malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, selection, length);
    copy[length] = '\0';
    return copy;
}

/// @brief Render fitted text, an optional icon, and selection highlights.
/// @details Wrapped text is painted line by line from cached display strings and
///          source mappings. Single-line text optionally uses a width-keyed
///          ellipsis cache. Disabled colors, horizontal and vertical alignment,
///          and read-only selection backgrounds are applied in both paths.
/// @param widget Arranged label widget base to paint.
/// @param canvas Backend canvas passed to font, vector-icon, and fill routines.
static void label_paint(vg_widget_t *widget, void *canvas) {
    vg_label_t *label = (vg_label_t *)widget;
    if (!label->font)
        return;
    if (!label->text || !label->text[0]) {
        if (label->icon_vector_id >= 0 && !label->word_wrap) {
            vg_theme_t *icon_theme = vg_theme_get_current();
            uint32_t icon_color = (widget->state & VG_STATE_DISABLED)
                                      ? icon_theme->colors.fg_disabled
                                      : label->text_color;
            float icon_sz = label->font_size + 2.0f;
            float icon_y = widget->y + (widget->height - icon_sz) / 2.0f;
            vg_icon_vector_draw((vgfx_window_t)canvas,
                                label->icon_vector_id,
                                (int32_t)(widget->x + 0.5f),
                                (int32_t)(icon_y + 0.5f),
                                (int32_t)(icon_sz + 0.5f),
                                icon_color);
        }
        return;
    }

    vg_font_metrics_t font_metrics;
    vg_font_get_metrics(label->font, label->font_size, &font_metrics);
    float line_height = (float)font_metrics.line_height;
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t color =
        (widget->state & VG_STATE_DISABLED) ? theme->colors.fg_disabled : label->text_color;
    size_t selection_start = label->selection_start < label->selection_end ? label->selection_start
                                                                           : label->selection_end;
    size_t selection_end = label->selection_start < label->selection_end ? label->selection_end
                                                                         : label->selection_start;

    if (label->word_wrap && widget->width > 0.0f) {
        label_measure_wrapped(label, widget->width, line_height, NULL);
        float block_height = (float)label->wrap_line_count * line_height;
        float top = label_block_origin_y(label, block_height);
        for (int line = 0; line < label->wrap_line_count; line++) {
            char *line_text = label->wrap_line_bufs[line];
            float local_x = label_line_origin_x(label, line_text, widget->width);
            float baseline = top + (float)line * line_height + font_metrics.ascent;
            size_t source_start = label->wrap_source_starts[line];
            size_t source_end = label->wrap_source_ends[line];
            size_t overlap_start = selection_start > source_start ? selection_start : source_start;
            size_t overlap_end = selection_end < source_end ? selection_end : source_end;
            if (label->selectable && overlap_end > overlap_start) {
                size_t local_start = overlap_start - source_start;
                size_t local_end = overlap_end - source_start;
                size_t display_len = strlen(line_text);
                if (local_start > display_len)
                    local_start = display_len;
                if (local_end > display_len)
                    local_end = display_len;
                float x1 = label_measure_mutable_prefix(label, line_text, local_start);
                float x2 = label_measure_mutable_prefix(label, line_text, local_end);
                vgfx_fill_rect((vgfx_window_t)canvas,
                               (int32_t)(widget->x + local_x + x1),
                               (int32_t)(widget->y + baseline - font_metrics.ascent),
                               (int32_t)(x2 - x1 + 0.5f),
                               (int32_t)(line_height + 0.5f),
                               theme->colors.bg_selected);
            }
            vg_font_draw_text(canvas,
                              label->font,
                              label->font_size,
                              widget->x + local_x,
                              widget->y + baseline,
                              line_text,
                              color);
        }
        return;
    }

    size_t represented = 0;
    char *display = label_single_line_display(label, widget->width, &represented);
    if (!display)
        return;
    vg_text_metrics_t metrics;
    vg_font_measure_text(label->font, label->font_size, display, &metrics);
    float local_x = label_line_origin_x(label, display, widget->width);
    if (label->icon_vector_id >= 0) {
        float icon_sz = label->font_size + 2.0f;
        float icon_y = widget->y + (widget->height - icon_sz) / 2.0f;
        vg_icon_vector_draw((vgfx_window_t)canvas,
                            label->icon_vector_id,
                            (int32_t)(widget->x + local_x + 0.5f),
                            (int32_t)(icon_y + 0.5f),
                            (int32_t)(icon_sz + 0.5f),
                            color);
        local_x += icon_sz + 5.0f;
    }
    float top = label_block_origin_y(label, metrics.height);
    float baseline = label->v_align == VG_ALIGN_V_BOTTOM ? widget->height - font_metrics.descent
                                                         : top + font_metrics.ascent;
    size_t visible_selection_start = selection_start < represented ? selection_start : represented;
    size_t visible_selection_end = selection_end < represented ? selection_end : represented;
    if (label->selectable && visible_selection_end > visible_selection_start) {
        float x1 = label_measure_mutable_prefix(label, label->text, visible_selection_start);
        float x2 = label_measure_mutable_prefix(label, label->text, visible_selection_end);
        vgfx_fill_rect((vgfx_window_t)canvas,
                       (int32_t)(widget->x + local_x + x1),
                       (int32_t)(widget->y + baseline - font_metrics.ascent),
                       (int32_t)(x2 - x1 + 0.5f),
                       (int32_t)(line_height + 0.5f),
                       theme->colors.bg_selected);
    }
    vg_font_draw_text(canvas,
                      label->font,
                      label->font_size,
                      widget->x + local_x,
                      widget->y + baseline,
                      display,
                      color);
}

/// @brief Handle pointer selection and keyboard commands for a selectable label.
/// @details Primary-button dragging updates UTF-8 source-byte endpoints under
///          input capture. Ctrl/Super+A selects all, Ctrl/Super+C copies the
///          ordered selection, and Escape clears it.
/// @param widget Label widget receiving the event.
/// @param event Mutable event whose handled flag is set when consumed.
/// @return `true` when the selectable-label interaction consumed the event.
static bool label_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_label_t *label = (vg_label_t *)widget;
    if (!label->selectable || !widget->enabled || !event)
        return false;

    switch (event->type) {
        case VG_EVENT_MOUSE_DOWN: {
            if (event->mouse.button != VG_MOUSE_LEFT)
                return false;
            size_t offset = label_source_offset_at(label, event->mouse.x, event->mouse.y);
            vg_widget_set_focus(widget);
            if ((event->modifiers & VG_MOD_SHIFT) == 0) {
                label->selection_anchor = offset;
                label_set_selection(label, offset, offset);
            } else {
                label_set_selection(label, label->selection_anchor, offset);
            }
            label->selection_dragging = true;
            vg_widget_set_input_capture(widget);
            event->handled = true;
            return true;
        }
        case VG_EVENT_MOUSE_MOVE:
            if (!label->selection_dragging || vg_widget_get_input_capture() != widget)
                return false;
            label_set_selection(label,
                                label->selection_anchor,
                                label_source_offset_at(label, event->mouse.x, event->mouse.y));
            event->handled = true;
            return true;
        case VG_EVENT_MOUSE_UP:
            if (!label->selection_dragging)
                return false;
            label->selection_dragging = false;
            if (vg_widget_get_input_capture() == widget)
                vg_widget_release_input_capture();
            event->handled = true;
            return true;
        case VG_EVENT_KEY_DOWN: {
            bool command = (event->modifiers & (VG_MOD_CTRL | VG_MOD_SUPER)) != 0;
            if (command && event->key.key == VG_KEY_A) {
                size_t len = strlen(label->text);
                label->selection_anchor = 0;
                label_set_selection(label, 0, len);
                event->handled = true;
                return true;
            }
            if (command && event->key.key == VG_KEY_C) {
                char *selection = label_copy_selection(label);
                if (selection) {
                    vgfx_clipboard_set_text(selection);
                    free(selection);
                }
                event->handled = true;
                return true;
            }
            if (event->key.key == VG_KEY_ESCAPE) {
                label_set_selection(label, 0, 0);
                label->selection_anchor = 0;
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

/// @brief VTable focus predicate for selectable labels.
/// @param widget Candidate label widget.
/// @return true only while selection is enabled and the widget is enabled and visible.
static bool label_can_focus(vg_widget_t *widget) {
    vg_label_t *label = (vg_label_t *)widget;
    return label->selectable && widget->enabled && widget->visible;
}

//=============================================================================
// Label API
//=============================================================================

/// @brief Replace the label's display text and invalidate the wrap cache.
///
/// @param label The label to update.
/// @param text  New text (copied); NULL is treated as an empty string.
void vg_label_set_text(vg_label_t *label, const char *text) {
    if (!label)
        return;

    const char *new_text = text ? text : "";
    if (label->text && strcmp(label->text, new_text) == 0)
        return;

    char *copy = vg_strdup(new_text);
    if (!copy)
        return;

    free(label->text);
    label->text = copy;
    label->selection_anchor = 0;
    label->selection_start = 0;
    label->selection_end = 0;
    label_free_wrap_cache(label); /* text changed — cache stale */
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Enable or disable width-constrained word wrapping.
/// @details A mode change discards fitted-text caches and schedules both layout
///          and paint. Wrapped labels report a multi-line measured height.
/// @param label Label to configure; `NULL` is ignored.
/// @param enabled `true` to wrap source text to the arranged width.
void vg_label_set_word_wrap(vg_label_t *label, bool enabled) {
    if (!label || label->word_wrap == enabled)
        return;
    label->word_wrap = enabled;
    label_free_wrap_cache(label); /* wrap mode changed — cache stale */
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Return the label's current display text.
///
/// @param label The label to query.
/// @return Internal string pointer, or NULL if label is NULL.
const char *vg_label_get_text(vg_label_t *label) {
    return label ? label->text : NULL;
}

/// @brief Override the label's font and size; invalidates the wrap cache.
///
/// @param label The label to configure.
/// @param font  Font to use; NULL keeps the existing font.
/// @param size  Font size in pixels; <= 0 defaults to 13.0.
void vg_label_set_font(vg_label_t *label, vg_font_t *font, float size) {
    if (!label)
        return;

    float font_size = size > 0 ? size : 13.0f;
    if (label->font == font && label->font_size == font_size)
        return;
    label->font = font;
    label->font_size = font_size;
    label_free_wrap_cache(label); /* font metrics changed — cache stale */
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Set or clear the vector icon drawn before single-line label text.
/// @details Negative identifiers normalize to -1, which disables the icon.
///          A changed icon affects intrinsic width and schedules layout and paint.
/// @param label Label to configure; `NULL` is ignored.
/// @param vector_id Registered vector icon identifier, or a negative value for none.
void vg_label_set_vector_icon(vg_label_t *label, int32_t vector_id) {
    if (!label)
        return;
    if (vector_id < 0)
        vector_id = -1;
    if (label->icon_vector_id == vector_id)
        return;
    label->icon_vector_id = vector_id;
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Set the label's text and vector-icon color.
/// @param label Label to configure; `NULL` is ignored.
/// @param color Packed color value in the GUI renderer's RGBA representation.
void vg_label_set_color(vg_label_t *label, uint32_t color) {
    if (!label)
        return;
    if (label->text_color == color)
        return;

    label->text_color = color;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Set the label's text alignment within its bounding box.
///
/// @param label   The label to configure.
/// @param h_align Horizontal alignment (VG_ALIGN_LEFT, CENTER, or RIGHT).
/// @param v_align Vertical alignment (VG_ALIGN_TOP, MIDDLE, or BOTTOM).
void vg_label_set_alignment(vg_label_t *label, vg_h_align_t h_align, vg_v_align_t v_align) {
    if (!label)
        return;
    if (label->h_align == h_align && label->v_align == v_align)
        return;

    label->h_align = h_align;
    label->v_align = v_align;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Return a label's horizontal alignment.
/// @param label Label widget to inspect.
/// @return Left, center, or right; NULL defaults to left.
vg_h_align_t vg_label_get_alignment(const vg_label_t *label) {
    return label ? label->h_align : VG_ALIGN_H_LEFT;
}

/// @brief Enable or disable UTF-8-safe ellipsis rendering for truncated text.
/// @param label Label widget to update; NULL is ignored.
/// @param enabled true to fit truncated text with U+2026.
void vg_label_set_ellipsis(vg_label_t *label, bool enabled) {
    if (!label || label->ellipsis == enabled)
        return;
    label->ellipsis = enabled;
    label_free_wrap_cache(label);
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Set the maximum number of visible wrapped lines.
/// @details Negative values normalize to zero, which means unlimited.
/// @param label Label widget to update; NULL is ignored.
/// @param count Maximum line count or zero for unlimited.
void vg_label_set_max_lines(vg_label_t *label, int count) {
    if (!label)
        return;
    int normalized = count > 0 ? count : 0;
    if (label->max_lines == normalized)
        return;
    label->max_lines = normalized;
    label_free_wrap_cache(label);
    label->base.needs_layout = true;
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Enable or disable read-only selection interaction.
/// @details Disabling selection clears endpoints, input capture, and label focus so stale input
///          state cannot survive a mode transition.
/// @param label Label widget to update; NULL is ignored.
/// @param enabled true to make the label focusable/selectable.
void vg_label_set_selectable(vg_label_t *label, bool enabled) {
    if (!label || label->selectable == enabled)
        return;
    label->selectable = enabled;
    if (!enabled) {
        label->selection_dragging = false;
        label->selection_anchor = 0;
        label->selection_start = 0;
        label->selection_end = 0;
        if (vg_widget_get_input_capture() == &label->base)
            vg_widget_release_input_capture();
        if (vg_widget_get_focused(&label->base) == &label->base)
            vg_widget_set_focus(NULL);
    }
    label->base.needs_paint = true;
    vg_widget_note_revision(&label->base);
}

/// @brief Return the label's ordered selected source-byte slice.
/// @param label Label widget to inspect.
/// @param out_length Receives the selected UTF-8 byte count when non-NULL.
/// @return Borrowed pointer into label text, or NULL for an empty/invalid selection.
const char *vg_label_get_selected_text(const vg_label_t *label, size_t *out_length) {
    if (out_length)
        *out_length = 0;
    if (!label || !label->text)
        return NULL;
    size_t text_len = strlen(label->text);
    size_t start = label->selection_start < label->selection_end ? label->selection_start
                                                                 : label->selection_end;
    size_t end = label->selection_start < label->selection_end ? label->selection_end
                                                               : label->selection_start;
    if (start > text_len)
        start = text_len;
    if (end > text_len)
        end = text_len;
    if (end <= start)
        return NULL;
    if (out_length)
        *out_length = end - start;
    return label->text + start;
}
