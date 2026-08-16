//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_table.c
/// @file
/// @brief Implements the fixed-width text table builder.
///
// Purpose: See rt_table.h.
//
// Key invariants:
//   - All widths are byte widths, matching Zanna.String.PadLeft/PadRight, so a
//     table and a hand-padded line agree for ASCII content.
//   - Auto-width columns resolve at render time over the header plus every
//     cell, so a row added after the column still counts.
//   - Trailing padding on the last column is trimmed: an aligned report should
//     not carry invisible whitespace to the line end.
//
// Ownership/Lifetime:
//   - Headers, cells, and the gutter are copied into malloc'd storage owned by
//     the table and released by its finalizer.
//   - Rendered strings are freshly allocated and owned by the caller.
//
// Links: src/runtime/text/rt_table.h, docs/adr/0252-text-table-layout.md
//
//===----------------------------------------------------------------------===//

#include "rt_table.h"

#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_internal.h"
#include "rt_trap.h"

#include <stdlib.h>
#include <string.h>

/// @brief Upper bound on columns; far beyond any legible table.
#define RT_TABLE_MAX_COLUMNS 256

/// @brief One declared column.
typedef struct {
    char *header;   ///< Owned heading text.
    int64_t width;  ///< Declared byte width; 0 means auto.
    int64_t align;  ///< RT_TABLE_ALIGN_*.
    int8_t is_auto; ///< 1 when the width resolves from content.
    int8_t truncate; ///< 1 to clip cells wider than the column.
} rt_table_column;

/// @brief Mutable state owned by a Table runtime object.
struct rt_table_impl {
    rt_table_column *columns; ///< Column declarations.
    int64_t column_count;     ///< Live column count.
    int64_t column_cap;       ///< Allocated column slots.
    char **cells;             ///< Row-major cell storage, column_cap wide.
    int64_t row_count;        ///< Live row count.
    int64_t row_cap;          ///< Allocated row slots.
    char *gutter;             ///< Owned inter-column separator.
};

/// @brief Safe-cast a handle to the Table impl, trapping on a class mismatch.
/// @param obj Borrowed candidate Table handle.
/// @param api Trap message identifying the calling API.
/// @return Typed pointer, or NULL after trapping.
static struct rt_table_impl *table_checked(void *obj, const char *api) {
    if (!rt_obj_is_instance(obj, RT_TABLE_CLASS_ID, sizeof(struct rt_table_impl))) {
        rt_trap(api);
        return NULL;
    }
    return (struct rt_table_impl *)obj;
}

/// @brief Duplicate a runtime string's bytes into malloc'd storage.
/// @param s Borrowed runtime string, or NULL.
/// @return Owned NUL-terminated copy, or NULL on allocation failure.
static char *dup_rt_string(rt_string s) {
    const char *src = s ? rt_string_cstr(s) : NULL;
    if (!src)
        src = "";
    size_t len = strlen(src);
    char *out = (char *)malloc(len + 1u);
    if (!out)
        return NULL;
    memcpy(out, src, len + 1u);
    return out;
}

/// @brief Resolve a column's effective byte width.
/// @details An auto column measures its header and every cell; a fixed column
///          is never narrower than its own header, so a heading cannot be
///          clipped by a too-small declaration.
/// @param t Validated table.
/// @param col Zero-based column index.
/// @return Effective width in bytes.
static size_t effective_width(struct rt_table_impl *t, int64_t col) {
    size_t width = strlen(t->columns[col].header);
    if (!t->columns[col].is_auto) {
        size_t declared = (size_t)(t->columns[col].width < 0 ? 0 : t->columns[col].width);
        return declared > width ? declared : width;
    }
    for (int64_t r = 0; r < t->row_count; r++) {
        const char *cell = t->cells[(size_t)r * (size_t)t->column_cap + (size_t)col];
        if (!cell)
            continue;
        size_t len = strlen(cell);
        if (len > width)
            width = len;
    }
    return width;
}

/// @brief Append one padded field to a growing line buffer.
/// @param dst Destination buffer.
/// @param cap Destination capacity in bytes.
/// @param len In/out current length.
/// @param text Field text.
/// @param width Target byte width.
/// @param align RT_TABLE_ALIGN_*.
/// @param truncate Non-zero to clip a too-wide field.
static void append_field(char *dst, size_t cap, size_t *len, const char *text, size_t width,
                         int64_t align, int8_t truncate) {
    size_t tlen = strlen(text);
    if (truncate && tlen > width)
        tlen = width;
    size_t pad = tlen >= width ? 0u : width - tlen;
    size_t lead = 0u;
    size_t trail = 0u;
    if (align == RT_TABLE_ALIGN_RIGHT) {
        lead = pad;
    } else if (align == RT_TABLE_ALIGN_CENTER) {
        lead = pad / 2u;
        trail = pad - lead;
    } else {
        trail = pad;
    }
    for (size_t i = 0; i < lead && *len + 1u < cap; i++)
        dst[(*len)++] = ' ';
    for (size_t i = 0; i < tlen && *len + 1u < cap; i++)
        dst[(*len)++] = text[i];
    for (size_t i = 0; i < trail && *len + 1u < cap; i++)
        dst[(*len)++] = ' ';
    dst[*len] = '\0';
}

/// @brief Total bytes a rendered line can occupy, including the terminator.
/// @param t Validated table.
/// @return Conservative upper bound.
static size_t line_capacity(struct rt_table_impl *t) {
    size_t total = 1u;
    size_t gutter = strlen(t->gutter);
    for (int64_t c = 0; c < t->column_count; c++) {
        size_t w = effective_width(t, c);
        // A non-truncating column may exceed its declared width.
        for (int64_t r = 0; r < t->row_count; r++) {
            const char *cell = t->cells[(size_t)r * (size_t)t->column_cap + (size_t)c];
            if (cell) {
                size_t len = strlen(cell);
                if (len > w)
                    w = len;
            }
        }
        total += w + gutter;
    }
    return total;
}

/// @brief Trim trailing spaces from a rendered line in place.
/// @param dst Line buffer.
/// @param len In/out length.
static void trim_trailing(char *dst, size_t *len) {
    while (*len > 0 && dst[*len - 1u] == ' ')
        (*len)--;
    dst[*len] = '\0';
}

/// @brief Release every heap allocation owned by a Table.
/// @details Installed with rt_obj_set_finalizer at construction: the table's
///          headers, cells, and gutter are malloc'd rather than runtime
///          strings, so the GC cannot reclaim them on its own.
/// @param obj Table payload being reclaimed.
static void table_finalize(void *obj) {
    struct rt_table_impl *t = (struct rt_table_impl *)obj;
    if (!t)
        return;
    if (t->columns) {
        for (int64_t c = 0; c < t->column_count; c++)
            free(t->columns[c].header);
        free(t->columns);
        t->columns = NULL;
    }
    if (t->cells) {
        for (int64_t r = 0; r < t->row_cap; r++)
            for (int64_t c = 0; c < t->column_cap; c++)
                free(t->cells[(size_t)r * (size_t)t->column_cap + (size_t)c]);
        free(t->cells);
        t->cells = NULL;
    }
    free(t->gutter);
    t->gutter = NULL;
    t->column_count = 0;
    t->column_cap = 0;
    t->row_count = 0;
    t->row_cap = 0;
}

void *rt_table_new(void) {
    struct rt_table_impl *t = (struct rt_table_impl *)rt_obj_new_i64(
        RT_TABLE_CLASS_ID, (int64_t)sizeof(struct rt_table_impl));
    if (!t) {
        rt_trap("Text.Table.New: memory allocation failed");
        return NULL;
    }
    t->columns = NULL;
    t->column_count = 0;
    t->column_cap = 0;
    t->cells = NULL;
    t->row_count = 0;
    t->row_cap = 0;
    t->gutter = (char *)malloc(2u);
    if (t->gutter) {
        t->gutter[0] = ' ';
        t->gutter[1] = '\0';
    }
    rt_obj_set_finalizer(t, table_finalize);
    return t;
}

/// @brief Grow the column array to hold at least one more entry.
/// @param t Validated table.
/// @return Non-zero on success.
static int ensure_column_slot(struct rt_table_impl *t) {
    if (t->column_count < t->column_cap)
        return 1;
    if (t->row_count > 0) {
        // Reallocating the row stride after rows exist would have to reshape
        // every row; declare all columns before adding rows.
        rt_trap("Text.Table.AddColumn: declare every column before adding rows");
        return 0;
    }
    int64_t cap = t->column_cap ? t->column_cap * 2 : 8;
    if (cap > RT_TABLE_MAX_COLUMNS)
        cap = RT_TABLE_MAX_COLUMNS;
    if (t->column_count >= cap) {
        rt_trap("Text.Table.AddColumn: too many columns");
        return 0;
    }
    rt_table_column *grown =
        (rt_table_column *)realloc(t->columns, (size_t)cap * sizeof(rt_table_column));
    if (!grown)
        return 0;
    t->columns = grown;
    t->column_cap = cap;
    return 1;
}

/// @brief Shared body of the fixed and auto column constructors.
/// @param obj Table receiver.
/// @param header Column heading.
/// @param width Declared width, or 0 for auto.
/// @param align RT_TABLE_ALIGN_*.
/// @param is_auto Non-zero for a content-sized column.
/// @param api Trap message identifying the caller.
/// @return Column index, or -1.
static int64_t add_column_impl(
    void *obj, rt_string header, int64_t width, int64_t align, int8_t is_auto, const char *api) {
    struct rt_table_impl *t = table_checked(obj, api);
    if (!t)
        return -1;
    if (align != RT_TABLE_ALIGN_RIGHT && align != RT_TABLE_ALIGN_CENTER)
        align = RT_TABLE_ALIGN_LEFT;
    if (!ensure_column_slot(t))
        return -1;
    char *copy = dup_rt_string(header);
    if (!copy)
        return -1;
    rt_table_column *col = &t->columns[t->column_count];
    col->header = copy;
    col->width = width < 0 ? 0 : width;
    col->align = align;
    col->is_auto = is_auto;
    col->truncate = 0;
    return t->column_count++;
}

int64_t rt_table_add_column(void *obj, rt_string header, int64_t width, int64_t align) {
    return add_column_impl(obj, header, width, align, 0, "Text.Table.AddColumn: expected a Table");
}

int64_t rt_table_add_column_auto(void *obj, rt_string header, int64_t align) {
    return add_column_impl(
        obj, header, 0, align, 1, "Text.Table.AddColumnAuto: expected a Table");
}

void rt_table_set_truncate(void *obj, int64_t column, int8_t on) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.SetTruncate: expected a Table");
    if (!t || column < 0 || column >= t->column_count)
        return;
    t->columns[column].truncate = on ? 1 : 0;
}

int64_t rt_table_column_count(void *obj) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.ColumnCount: expected a Table");
    return t ? t->column_count : 0;
}

int64_t rt_table_add_row(void *obj) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.AddRow: expected a Table");
    if (!t)
        return -1;
    if (t->column_count == 0) {
        rt_trap("Text.Table.AddRow: declare at least one column first");
        return -1;
    }
    if (t->row_count >= t->row_cap) {
        int64_t cap = t->row_cap ? t->row_cap * 2 : 16;
        char **grown = (char **)realloc(
            t->cells, (size_t)cap * (size_t)t->column_cap * sizeof(char *));
        if (!grown)
            return -1;
        t->cells = grown;
        for (int64_t r = t->row_cap; r < cap; r++)
            for (int64_t c = 0; c < t->column_cap; c++)
                t->cells[(size_t)r * (size_t)t->column_cap + (size_t)c] = NULL;
        t->row_cap = cap;
    }
    for (int64_t c = 0; c < t->column_cap; c++)
        t->cells[(size_t)t->row_count * (size_t)t->column_cap + (size_t)c] = NULL;
    return t->row_count++;
}

void rt_table_set_cell(void *obj, int64_t row, int64_t column, rt_string text) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.SetCell: expected a Table");
    if (!t || row < 0 || row >= t->row_count || column < 0 || column >= t->column_count)
        return;
    size_t idx = (size_t)row * (size_t)t->column_cap + (size_t)column;
    char *copy = dup_rt_string(text);
    if (!copy)
        return;
    free(t->cells[idx]);
    t->cells[idx] = copy;
}

rt_string rt_table_get_cell(void *obj, int64_t row, int64_t column) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.GetCell: expected a Table");
    if (!t || row < 0 || row >= t->row_count || column < 0 || column >= t->column_count)
        return rt_empty_string();
    const char *cell = t->cells[(size_t)row * (size_t)t->column_cap + (size_t)column];
    if (!cell)
        return rt_empty_string();
    return rt_string_from_bytes(cell, strlen(cell));
}

int64_t rt_table_row_count(void *obj) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.RowCount: expected a Table");
    return t ? t->row_count : 0;
}

void rt_table_clear_rows(void *obj) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.ClearRows: expected a Table");
    if (!t)
        return;
    for (int64_t r = 0; r < t->row_count; r++)
        for (int64_t c = 0; c < t->column_cap; c++) {
            size_t idx = (size_t)r * (size_t)t->column_cap + (size_t)c;
            free(t->cells[idx]);
            t->cells[idx] = NULL;
        }
    t->row_count = 0;
}

void rt_table_set_gutter(void *obj, rt_string gutter) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.SetGutter: expected a Table");
    if (!t)
        return;
    char *copy = dup_rt_string(gutter);
    if (!copy)
        return;
    free(t->gutter);
    t->gutter = copy;
}

rt_string rt_table_render_header(void *obj) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.RenderHeader: expected a Table");
    if (!t || t->column_count == 0)
        return rt_empty_string();
    size_t cap = line_capacity(t);
    char *line = (char *)malloc(cap);
    if (!line)
        return rt_empty_string();
    size_t len = 0;
    line[0] = '\0';
    for (int64_t c = 0; c < t->column_count; c++) {
        if (c > 0)
            append_field(line, cap, &len, t->gutter, strlen(t->gutter), RT_TABLE_ALIGN_LEFT, 0);
        append_field(line, cap, &len, t->columns[c].header, effective_width(t, c),
                     t->columns[c].align, 0);
    }
    trim_trailing(line, &len);
    rt_string out = rt_string_from_bytes(line, len);
    free(line);
    return out;
}

rt_string rt_table_render_rule(void *obj, rt_string fill) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.RenderRule: expected a Table");
    if (!t || t->column_count == 0)
        return rt_empty_string();
    const char *f = fill ? rt_string_cstr(fill) : NULL;
    char ch = (f && f[0]) ? f[0] : '-';
    size_t total = 0;
    size_t gutter = strlen(t->gutter);
    for (int64_t c = 0; c < t->column_count; c++) {
        if (c > 0)
            total += gutter;
        total += effective_width(t, c);
    }
    char *line = (char *)malloc(total + 1u);
    if (!line)
        return rt_empty_string();
    memset(line, ch, total);
    line[total] = '\0';
    rt_string out = rt_string_from_bytes(line, total);
    free(line);
    return out;
}

rt_string rt_table_render_row(void *obj, int64_t row) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.RenderRow: expected a Table");
    if (!t || row < 0 || row >= t->row_count || t->column_count == 0)
        return rt_empty_string();
    size_t cap = line_capacity(t);
    char *line = (char *)malloc(cap);
    if (!line)
        return rt_empty_string();
    size_t len = 0;
    line[0] = '\0';
    for (int64_t c = 0; c < t->column_count; c++) {
        if (c > 0)
            append_field(line, cap, &len, t->gutter, strlen(t->gutter), RT_TABLE_ALIGN_LEFT, 0);
        const char *cell = t->cells[(size_t)row * (size_t)t->column_cap + (size_t)c];
        append_field(line, cap, &len, cell ? cell : "", effective_width(t, c), t->columns[c].align,
                     t->columns[c].truncate);
    }
    trim_trailing(line, &len);
    rt_string out = rt_string_from_bytes(line, len);
    free(line);
    return out;
}

rt_string rt_table_render(void *obj, int8_t with_header, int8_t with_rule) {
    struct rt_table_impl *t = table_checked(obj, "Text.Table.Render: expected a Table");
    if (!t || t->column_count == 0)
        return rt_empty_string();

    size_t cap = line_capacity(t);
    size_t total = (cap + 1u) * (size_t)(t->row_count + 2);
    char *buf = (char *)malloc(total);
    if (!buf)
        return rt_empty_string();
    size_t len = 0;
    buf[0] = '\0';

    /// Append one owned rendered line and release it.
    /// (Inlined rather than a helper so the ownership is visible at each site.)
    if (with_header) {
        rt_string h = rt_table_render_header(obj);
        const char *hs = rt_string_cstr(h);
        if (hs) {
            size_t hl = strlen(hs);
            if (len + hl + 1u < total) {
                memcpy(buf + len, hs, hl);
                len += hl;
            }
        }
        rt_string_unref(h);
        if (len + 1u < total)
            buf[len++] = '\n';
    }
    if (with_rule) {
        rt_string r = rt_table_render_rule(obj, NULL);
        const char *rs = rt_string_cstr(r);
        if (rs) {
            size_t rl = strlen(rs);
            if (len + rl + 1u < total) {
                memcpy(buf + len, rs, rl);
                len += rl;
            }
        }
        rt_string_unref(r);
        if (len + 1u < total)
            buf[len++] = '\n';
    }
    for (int64_t i = 0; i < t->row_count; i++) {
        rt_string line = rt_table_render_row(obj, i);
        const char *ls = rt_string_cstr(line);
        if (ls) {
            size_t ll = strlen(ls);
            if (len + ll + 1u < total) {
                memcpy(buf + len, ls, ll);
                len += ll;
            }
        }
        rt_string_unref(line);
        if (i + 1 < t->row_count && len + 1u < total)
            buf[len++] = '\n';
    }
    buf[len] = '\0';
    rt_string out = rt_string_from_bytes(buf, len);
    free(buf);
    return out;
}

/// @brief Alignment constant for left-aligned columns.
/// @return `RT_TABLE_ALIGN_LEFT`.
int64_t rt_table_align_left(void) {
    return RT_TABLE_ALIGN_LEFT;
}

/// @brief Alignment constant for right-aligned columns.
/// @return `RT_TABLE_ALIGN_RIGHT`.
int64_t rt_table_align_right(void) {
    return RT_TABLE_ALIGN_RIGHT;
}

/// @brief Alignment constant for centred columns.
/// @return `RT_TABLE_ALIGN_CENTER`.
int64_t rt_table_align_center(void) {
    return RT_TABLE_ALIGN_CENTER;
}
