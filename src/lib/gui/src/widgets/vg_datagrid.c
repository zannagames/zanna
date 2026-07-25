//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file vg_datagrid.c
/// @brief Implements the interactive, viewport-aware data-grid widget.
///
/// @details The grid supports dense compatibility storage and a sparse virtual
/// mode for very large logical row counts. Dense cells occupy a flat
/// `row_capacity * col_count` array, while virtual cells are sorted by row and
/// column so lookup and insertion use binary search. Painting visits only the
/// current viewport slice.
///
/// The grid owns all headers, cell strings, sparse entries, and column metadata.
/// Effective automatic widths are cached outside painting; an explicit width
/// of zero selects the cached automatic width. Selection, sorting, resizing,
/// and editing expose independent edge counters for external controllers.
///
/// @see vg_ide_widgets_panels.h
/// @see vg_font.h
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_event.h"
#include "../../include/vg_font.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "../../include/vg_widgets.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_DATAGRID_MIN_COLUMN_WIDTH 16.0f
#define VG_DATAGRID_MAX_COLUMN_WIDTH 1000000.0f
#define VG_DATAGRID_RESIZE_HIT_SLOP 4.0f

/// @brief One sorted sparse virtual-grid cell.
struct vg_datagrid_virtual_cell {
    size_t row; ///< Logical row index.
    int col;    ///< Logical column index.
    char *text; ///< Owned non-empty UTF-8 text.
};

//=============================================================================
// Forward declarations
//=============================================================================

static void datagrid_measure(vg_widget_t *widget, float avail_w, float avail_h);
static void datagrid_arrange(vg_widget_t *widget, float x, float y, float w, float h);
static void datagrid_paint(vg_widget_t *widget, void *canvas);
static void datagrid_destroy(vg_widget_t *widget);
static bool datagrid_handle_event(vg_widget_t *widget, vg_event_t *event);
static bool datagrid_can_focus(vg_widget_t *widget);

static vg_widget_vtable_t g_datagrid_vtable = {
    .destroy = datagrid_destroy,
    .measure = datagrid_measure,
    .arrange = datagrid_arrange,
    .paint = datagrid_paint,
    .handle_event = datagrid_handle_event,
    .can_focus = datagrid_can_focus,
    .on_focus = NULL,
};

//=============================================================================
// Internal helpers
//=============================================================================

/// @brief Heap-copy @p text, returning NULL for NULL/empty input.
/// @param text NUL-terminated input text.
/// @return Owned copy, or NULL for empty input/allocation failure.
static char *datagrid_dup(const char *text) {
    if (!text || !text[0])
        return NULL;
    size_t n = strlen(text);
    char *copy = (char *)malloc(n + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, n + 1);
    return copy;
}

/// @brief Compare nullable grid text values after normalizing empty strings to NULL.
/// @param lhs First borrowed text.
/// @param rhs Second borrowed text.
/// @return true when both normalized values are byte-identical.
static bool datagrid_text_equal(const char *lhs, const char *rhs) {
    if (lhs && lhs[0] == '\0')
        lhs = NULL;
    if (rhs && rhs[0] == '\0')
        rhs = NULL;
    return (!lhs && !rhs) || (lhs && rhs && strcmp(lhs, rhs) == 0);
}

/// @brief Return the grid's logical dense-or-virtual row count.
/// @param grid Grid to inspect.
/// @return Logical row count, or zero for NULL.
static size_t datagrid_total_rows(const vg_datagrid_t *grid) {
    if (!grid)
        return 0;
    return grid->virtual_mode ? grid->virtual_row_count : (size_t)grid->row_count;
}

/// @brief Free the dense cell array and reset dense row counters.
/// @param grid Grid whose dense storage should be cleared.
static void datagrid_free_cells(vg_datagrid_t *grid) {
    if (grid->cells) {
        size_t total = (size_t)grid->row_capacity * (size_t)grid->col_count;
        for (size_t i = 0; i < total; i++)
            free(grid->cells[i]);
        free(grid->cells);
        grid->cells = NULL;
    }
    grid->row_count = 0;
    grid->row_capacity = 0;
}

/// @brief Free every materialized sparse virtual cell and reset virtual storage.
/// @param grid Grid whose sparse storage should be cleared.
static void datagrid_free_virtual_cells(vg_datagrid_t *grid) {
    if (!grid)
        return;
    for (size_t i = 0; i < grid->virtual_cell_count; ++i)
        free(grid->virtual_cells[i].text);
    free(grid->virtual_cells);
    grid->virtual_cells = NULL;
    grid->virtual_cell_count = 0;
    grid->virtual_cell_capacity = 0;
}

/// @brief Free the header array.
/// @param grid Grid whose headers should be cleared.
static void datagrid_free_headers(vg_datagrid_t *grid) {
    if (grid->headers) {
        for (int c = 0; c < grid->col_count; c++)
            free(grid->headers[c]);
        free(grid->headers);
        grid->headers = NULL;
    }
    grid->has_headers = false;
}

/// @brief Free all per-column interactive and width-cache arrays.
/// @param grid Grid whose column metadata should be cleared.
static void datagrid_free_column_metadata(vg_datagrid_t *grid) {
    if (!grid)
        return;
    free(grid->sortable_columns);
    free(grid->column_widths);
    free(grid->auto_column_widths);
    free(grid->resizable_columns);
    grid->sortable_columns = NULL;
    grid->column_widths = NULL;
    grid->auto_column_widths = NULL;
    grid->resizable_columns = NULL;
}

/// @brief Grow dense storage to at least @p min_rows rows.
/// @param grid Grid to grow.
/// @param min_rows Required dense row capacity.
/// @return true on success, false on overflow/allocation failure.
static bool datagrid_ensure_rows(vg_datagrid_t *grid, int min_rows) {
    if (grid->col_count <= 0)
        return false;
    if (min_rows <= grid->row_capacity)
        return true;
    int new_cap = 8;
    if (grid->row_capacity > 0) {
        new_cap = grid->row_capacity > INT_MAX / 2 ? INT_MAX : grid->row_capacity * 2;
    }
    if (new_cap < min_rows)
        new_cap = min_rows;
    if (new_cap > INT_MAX / grid->col_count)
        return false;
    char **new_cells = (char **)calloc((size_t)new_cap * grid->col_count, sizeof(char *));
    if (!new_cells)
        return false;
    if (grid->cells) {
        memcpy(
            new_cells, grid->cells, (size_t)grid->row_capacity * grid->col_count * sizeof(char *));
        free(grid->cells);
    }
    grid->cells = new_cells;
    grid->row_capacity = new_cap;
    return true;
}

/// @brief Compare one sparse-cell key with a requested row/column key.
/// @param cell Sparse entry to compare.
/// @param row Requested row.
/// @param col Requested column.
/// @return Negative, zero, or positive according to key ordering.
static int datagrid_virtual_key_compare(const vg_datagrid_virtual_cell_t *cell,
                                        size_t row,
                                        int col) {
    if (cell->row < row)
        return -1;
    if (cell->row > row)
        return 1;
    if (cell->col < col)
        return -1;
    if (cell->col > col)
        return 1;
    return 0;
}

/// @brief Binary-search the sorted sparse-cell array.
/// @param grid Grid to search.
/// @param row Requested row.
/// @param col Requested column.
/// @param found Optional output set true for an exact key.
/// @return Exact index or insertion position in `[0, virtual_cell_count]`.
static size_t datagrid_virtual_lower_bound(const vg_datagrid_t *grid,
                                           size_t row,
                                           int col,
                                           bool *found) {
    size_t lo = 0;
    size_t hi = grid ? grid->virtual_cell_count : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = datagrid_virtual_key_compare(&grid->virtual_cells[mid], row, col);
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    bool exact = grid && lo < grid->virtual_cell_count &&
                 datagrid_virtual_key_compare(&grid->virtual_cells[lo], row, col) == 0;
    if (found)
        *found = exact;
    return lo;
}

/// @brief Return one borrowed sparse virtual-cell value.
/// @param grid Grid to inspect.
/// @param row Logical row.
/// @param col Logical column.
/// @return Borrowed text, or NULL when the sparse entry is absent.
static const char *datagrid_get_virtual_cell(const vg_datagrid_t *grid, size_t row, int col) {
    bool found = false;
    size_t index = datagrid_virtual_lower_bound(grid, row, col, &found);
    return found ? grid->virtual_cells[index].text : NULL;
}

/// @brief Return one borrowed dense-or-sparse cell value using a size_t row.
/// @param grid Grid to inspect.
/// @param row Logical row.
/// @param col Logical column.
/// @return Borrowed text, or NULL when empty/out of range.
static const char *datagrid_get_cell_at(const vg_datagrid_t *grid, size_t row, int col) {
    if (!grid || col < 0 || col >= grid->col_count || row >= datagrid_total_rows(grid))
        return NULL;
    if (grid->virtual_mode)
        return datagrid_get_virtual_cell(grid, row, col);
    if (!grid->cells)
        return NULL;
    return grid->cells[row * (size_t)grid->col_count + (size_t)col];
}

/// @brief Measure one text value including horizontal cell padding.
/// @param grid Grid supplying font and padding.
/// @param text Borrowed text; NULL/empty measures as padding only.
/// @return Rounded physical width, clamped to at least one pixel when a column exists.
static int datagrid_measure_text_width(const vg_datagrid_t *grid, const char *text) {
    if (!grid || !grid->font || !vg_font_is_live(grid->font) || grid->font_size <= 0.0f)
        return 0;
    float width = grid->cell_padding * 2.0f;
    if (text && text[0]) {
        vg_text_metrics_t metrics = {0};
        vg_font_measure_text(grid->font, grid->font_size, text, &metrics);
        width += metrics.width;
    }
    if (width < 1.0f)
        width = 1.0f;
    if (width > VG_DATAGRID_MAX_COLUMN_WIDTH)
        width = VG_DATAGRID_MAX_COLUMN_WIDTH;
    return (int)(width + 0.5f);
}

/// @brief Recompute one cached automatic column width outside the paint path.
/// @param grid Grid to update.
/// @param col Valid zero-based column.
static void datagrid_recompute_auto_width(vg_datagrid_t *grid, int col) {
    if (!grid || !grid->auto_column_widths || col < 0 || col >= grid->col_count)
        return;
    int width = datagrid_measure_text_width(grid, grid->headers ? grid->headers[col] : NULL);
    if (grid->virtual_mode) {
        for (size_t i = 0; i < grid->virtual_cell_count; ++i) {
            if (grid->virtual_cells[i].col != col)
                continue;
            int candidate = datagrid_measure_text_width(grid, grid->virtual_cells[i].text);
            if (candidate > width)
                width = candidate;
        }
    } else if (grid->cells) {
        for (int row = 0; row < grid->row_count; ++row) {
            const char *text = grid->cells[(size_t)row * (size_t)grid->col_count + (size_t)col];
            int candidate = datagrid_measure_text_width(grid, text);
            if (candidate > width)
                width = candidate;
        }
    }
    grid->auto_column_widths[col] = width;
}

/// @brief Recompute every cached automatic column width outside paint.
/// @param grid Grid to update.
static void datagrid_recompute_all_auto_widths(vg_datagrid_t *grid) {
    if (!grid)
        return;
    for (int col = 0; col < grid->col_count; ++col)
        datagrid_recompute_auto_width(grid, col);
}

/// @brief Update one cached automatic width after replacing one contributing text value.
/// @details Widening is O(1). A full materialized-column rescan occurs only when the removed value
///          may have supplied the previous maximum, keeping incremental virtual population linear
///          for the common append/materialize path.
/// @param grid Grid whose cache should be updated after storage already contains the new value.
/// @param col Valid changed column.
/// @param old_width Measured width of the replaced/removed value before mutation.
/// @param new_text Borrowed replacement text now present in storage, or NULL when removed.
static void datagrid_update_auto_width_after_text_change(vg_datagrid_t *grid,
                                                         int col,
                                                         int old_width,
                                                         const char *new_text) {
    if (!grid || !grid->auto_column_widths || col < 0 || col >= grid->col_count)
        return;
    int new_width = datagrid_measure_text_width(grid, new_text);
    int cached = grid->auto_column_widths[col];
    if (new_width >= cached) {
        grid->auto_column_widths[col] = new_width;
    } else if (old_width >= cached) {
        datagrid_recompute_auto_width(grid, col);
    }
}

/// @brief Saturating increment for independent grid event counters.
/// @param version Counter to advance; may be NULL.
static void datagrid_increment_version(uint64_t *version) {
    if (version && *version < UINT64_MAX)
        (*version)++;
}

/// @brief Refresh the semantic value describing grid selection.
/// @param grid Grid whose accessible value should be updated.
static void datagrid_update_accessible_value(vg_datagrid_t *grid) {
    if (!grid)
        return;
    if (grid->selected_row == SIZE_MAX || grid->selected_col < 0) {
        vg_widget_set_accessible_value(&grid->base, "");
        return;
    }
    char value[80];
    (void)snprintf(value,
                   sizeof(value),
                   "row %llu column %d",
                   (unsigned long long)(grid->selected_row + 1u),
                   grid->selected_col + 1);
    vg_widget_set_accessible_value(&grid->base, value);
}

/// @brief Record one independent and common selection transition.
/// @param grid Grid whose selection changed.
static void datagrid_note_selection(vg_datagrid_t *grid) {
    if (!grid)
        return;
    datagrid_increment_version(&grid->selection_version);
    datagrid_update_accessible_value(grid);
    vg_widget_note_change(&grid->base);
}

/// @brief Record one sort transition and general revision.
/// @param grid Grid whose sort request changed.
static void datagrid_note_sort(vg_datagrid_t *grid) {
    if (!grid)
        return;
    datagrid_increment_version(&grid->sort_version);
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Record one effective column-width transition and general revision.
/// @param grid Grid whose width changed.
/// @param col Resized column.
static void datagrid_note_resize(vg_datagrid_t *grid, int col) {
    if (!grid)
        return;
    grid->resized_column = col;
    datagrid_increment_version(&grid->resize_version);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Return whether row/column identify a logical grid cell.
/// @param grid Grid to inspect.
/// @param row Logical row.
/// @param col Logical column.
/// @return true for a valid cell.
static bool datagrid_cell_is_valid(const vg_datagrid_t *grid, size_t row, int col) {
    return grid && col >= 0 && col < grid->col_count && row < datagrid_total_rows(grid);
}

/// @brief Clamp viewport and selection/edit state after row-count changes.
/// @param grid Grid to normalize.
static void datagrid_clamp_row_state(vg_datagrid_t *grid) {
    if (!grid)
        return;
    size_t rows = datagrid_total_rows(grid);
    if (rows == 0) {
        grid->viewport_first_row = 0;
        grid->scroll_row = 0;
    } else {
        if (grid->viewport_first_row >= rows)
            grid->viewport_first_row = rows - 1;
        if (grid->scroll_row >= rows)
            grid->scroll_row = rows - 1;
    }
    if (grid->selected_row != SIZE_MAX && grid->selected_row >= rows) {
        grid->selected_row = SIZE_MAX;
        grid->selected_col = -1;
        datagrid_note_selection(grid);
    }
    if (grid->editing && grid->editing_row >= rows) {
        grid->editing = false;
        grid->editing_row = SIZE_MAX;
        grid->editing_col = -1;
        vg_widget_note_revision(&grid->base);
    }
}

//=============================================================================
// VTable implementations
//=============================================================================

/// @brief Compute the automatic number of data rows that fit the arranged viewport.
/// @param grid Grid to inspect.
/// @return At least one row when height is positive, otherwise zero.
static size_t datagrid_height_row_capacity(const vg_datagrid_t *grid) {
    if (!grid)
        return 0;
    float height = grid->base.height;
    float line_height = grid->line_height > 0.0f ? grid->line_height : 18.0f;
    if (grid->has_headers)
        height -= line_height;
    if (height <= 0.0f)
        return 0;
    double capacity = ceil((double)height / (double)line_height);
    if (capacity >= (double)SIZE_MAX)
        return SIZE_MAX;
    return (size_t)capacity;
}

/// @brief Resolve the clamped first/count slice visited by paint and hit testing.
/// @param grid Grid to inspect.
/// @param out_first Receives the first logical row.
/// @param out_count Receives the number of logical rows in the slice.
static void datagrid_viewport_bounds(const vg_datagrid_t *grid,
                                     size_t *out_first,
                                     size_t *out_count) {
    size_t rows = datagrid_total_rows(grid);
    size_t first = grid ? grid->scroll_row : 0;
    if (rows == 0)
        first = 0;
    else if (first >= rows)
        first = rows - 1;
    size_t count = grid && grid->viewport_row_count > 0 ? grid->viewport_row_count
                                                        : datagrid_height_row_capacity(grid);
    if (count > rows - first)
        count = rows - first;
    if (out_first)
        *out_first = first;
    if (out_count)
        *out_count = count;
}

/// @brief Keep the selected row within the current viewport after keyboard movement.
/// @param grid Grid whose scroll row may change.
static void datagrid_ensure_selection_visible(vg_datagrid_t *grid) {
    if (!grid || grid->selected_row == SIZE_MAX)
        return;
    size_t first = 0;
    size_t count = 0;
    datagrid_viewport_bounds(grid, &first, &count);
    if (grid->selected_row < first) {
        grid->scroll_row = grid->selected_row;
    } else if (count > 0 && grid->selected_row >= first + count) {
        grid->scroll_row = grid->selected_row - count + 1;
    }
    grid->viewport_first_row = grid->scroll_row;
}

/// @brief Return the column containing one widget-local X coordinate.
/// @param grid Grid to inspect.
/// @param local_x Widget-local X coordinate.
/// @return Zero-based column, or -1 outside effective columns.
static int datagrid_column_at_x(const vg_datagrid_t *grid, float local_x) {
    if (!grid || local_x < 0.0f)
        return -1;
    float x = 0.0f;
    for (int col = 0; col < grid->col_count; ++col) {
        x += (float)vg_datagrid_column_width(grid, col);
        if (local_x < x)
            return col;
    }
    return -1;
}

/// @brief Find a resizable column boundary near one widget-local X coordinate.
/// @param grid Grid to inspect.
/// @param local_x Widget-local X coordinate.
/// @return Boundary's preceding column, or -1 when none is enabled/in range.
static int datagrid_resize_boundary_at_x(const vg_datagrid_t *grid, float local_x) {
    if (!grid || !grid->resizable_columns)
        return -1;
    float x = 0.0f;
    for (int col = 0; col < grid->col_count; ++col) {
        x += (float)vg_datagrid_column_width(grid, col);
        if (grid->resizable_columns[col] && fabsf(local_x - x) <= VG_DATAGRID_RESIZE_HIT_SLOP)
            return col;
    }
    return -1;
}

/// @brief Return whether any opt-in interaction mode is enabled.
/// @param grid Grid to inspect.
/// @return true for selectable/editable/sortable/resizable grids.
static bool datagrid_has_interaction(const vg_datagrid_t *grid) {
    if (!grid)
        return false;
    if (grid->selectable || grid->editable)
        return true;
    for (int col = 0; col < grid->col_count; ++col) {
        if ((grid->sortable_columns && grid->sortable_columns[col]) ||
            (grid->resizable_columns && grid->resizable_columns[col]))
            return true;
    }
    return false;
}

/// @brief VTable measure using cached column widths and bounded virtual height.
/// @param widget Grid base widget.
/// @param avail_w Available width (constraints are applied after intrinsic measurement).
/// @param avail_h Available height (constraints are applied after intrinsic measurement).
static void datagrid_measure(vg_widget_t *widget, float avail_w, float avail_h) {
    vg_datagrid_t *grid = (vg_datagrid_t *)widget;
    (void)avail_w;
    (void)avail_h;
    float total_w = 0.0f;
    for (int c = 0; c < grid->col_count; c++)
        total_w += (float)vg_datagrid_column_width(grid, c);
    float lh = grid->line_height > 0.0f ? grid->line_height : 18.0f;
    size_t rows = datagrid_total_rows(grid);
    if (grid->virtual_mode && grid->viewport_row_count == 0 && rows > 10)
        rows = 10;
    if (grid->virtual_mode && grid->viewport_row_count > 0 && rows > grid->viewport_row_count)
        rows = grid->viewport_row_count;
    if (grid->has_headers && rows < SIZE_MAX)
        rows++;
    widget->measured_width = total_w;
    widget->measured_height = rows > (size_t)(FLT_MAX / lh) ? FLT_MAX : (float)rows * lh;
    vg_widget_apply_constraints(widget);
}

/// @brief VTable arrange stores the physical grid bounds.
/// @param widget Grid base widget.
/// @param x Physical left coordinate.
/// @param y Physical top coordinate.
/// @param w Physical width.
/// @param h Physical height.
static void datagrid_arrange(vg_widget_t *widget, float x, float y, float w, float h) {
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
}

/// @brief Draw a sort-direction chevron in a header cell without allocating.
/// @param win Target ZannaGFX window.
/// @param x Center X coordinate.
/// @param y Center Y coordinate.
/// @param direction -1 descending or 1 ascending.
/// @param color Theme-derived indicator color.
static void datagrid_paint_sort_indicator(
    vgfx_window_t win, int32_t x, int32_t y, int direction, uint32_t color) {
    int32_t dy = direction > 0 ? -2 : 2;
    vgfx_line(win, x - 3, y + dy, x, y - dy, color);
    vgfx_line(win, x, y - dy, x + 3, y + dy, color);
}

/// @brief VTable paint visits only visible rows and uses cached effective column widths.
/// @param widget Grid base widget.
/// @param canvas ZannaGFX window/canvas.
static void datagrid_paint(vg_widget_t *widget, void *canvas) {
    vg_datagrid_t *grid = (vg_datagrid_t *)widget;
    vgfx_window_t win = (vgfx_window_t)canvas;
    if (widget->width <= 0.0f || widget->height <= 0.0f)
        return;

    float ox = widget->x;
    float oy = widget->y;
    float lh = grid->line_height > 0.0f ? grid->line_height : 18.0f;
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t background = theme ? theme->colors.bg_primary : grid->bg_color;
    uint32_t foreground = theme ? theme->colors.fg_primary : grid->fg_color;
    uint32_t header_foreground = theme ? theme->colors.fg_primary : grid->header_color;
    uint32_t grid_color = theme ? theme->colors.border_secondary : grid->grid_color;
    uint32_t selected_background = theme ? theme->colors.bg_selected : grid->bg_color;
    uint32_t accent = theme ? theme->colors.border_focus : grid->header_color;

    vgfx_fill_rect(
        win, (int32_t)ox, (int32_t)oy, (int32_t)widget->width, (int32_t)widget->height, background);

    if (grid->col_count <= 0) {
        vgfx_rect(win,
                  (int32_t)widget->x,
                  (int32_t)widget->y,
                  (int32_t)widget->width,
                  (int32_t)widget->height,
                  (widget->state & VG_STATE_FOCUSED) ? accent : grid_color);
        return;
    }

    // Vertically center text within a row using the font's metrics.
    vg_font_metrics_t fm = {0};
    bool font_is_live = grid->font && vg_font_is_live(grid->font) && grid->font_size > 0.0f;
    if (font_is_live)
        vg_font_get_metrics(grid->font, grid->font_size, &fm);
    float baseline = (lh - (float)(fm.ascent - fm.descent)) * 0.5f + (float)fm.ascent;

    if (widget->width > 2.0f && widget->height > 2.0f)
        vgfx_set_clip(win,
                      (int32_t)widget->x + 1,
                      (int32_t)widget->y + 1,
                      (int32_t)widget->width - 2,
                      (int32_t)widget->height - 2);

    float row_y = oy;

    // Header row.
    if (grid->has_headers && grid->headers) {
        uint32_t header_background = theme ? theme->colors.bg_tertiary : background;
        vgfx_fill_rect(win,
                       (int32_t)ox,
                       (int32_t)row_y,
                       (int32_t)widget->width,
                       (int32_t)lh,
                       header_background);
        float cx = ox;
        for (int c = 0; c < grid->col_count; c++) {
            int cw = vg_datagrid_column_width(grid, c);
            const char *h = grid->headers[c];
            if (h && font_is_live)
                vg_font_draw_text(canvas,
                                  grid->font,
                                  grid->font_size,
                                  cx + grid->cell_padding,
                                  row_y + baseline,
                                  h,
                                  header_foreground);
            if (c == grid->sort_column && grid->sort_direction != 0 && cw >= 14)
                datagrid_paint_sort_indicator(win,
                                              (int32_t)(cx + (float)cw - 8.0f),
                                              (int32_t)(row_y + lh * 0.5f),
                                              grid->sort_direction,
                                              accent);
            cx += (float)cw;
            vgfx_fill_rect(win, (int32_t)cx - 1, (int32_t)row_y, 1, (int32_t)lh, grid_color);
        }
        vgfx_fill_rect(
            win, (int32_t)ox, (int32_t)(row_y + lh) - 1, (int32_t)widget->width, 1, grid_color);
        row_y += lh;
    }

    size_t first = 0;
    size_t count = 0;
    datagrid_viewport_bounds(grid, &first, &count);
    bool sparse_found = false;
    size_t sparse_index =
        grid->virtual_mode ? datagrid_virtual_lower_bound(grid, first, 0, &sparse_found) : 0;
    (void)sparse_found;

    for (size_t visible = 0; visible < count; ++visible) {
        size_t row = first + visible;
        float cx = ox;
        if (row == grid->selected_row)
            vgfx_fill_rect(win,
                           (int32_t)ox,
                           (int32_t)row_y,
                           (int32_t)widget->width,
                           (int32_t)lh,
                           selected_background);
        for (int col = 0; col < grid->col_count; ++col) {
            int width = vg_datagrid_column_width(grid, col);
            const char *text = NULL;
            if (grid->virtual_mode) {
                while (sparse_index < grid->virtual_cell_count &&
                       (grid->virtual_cells[sparse_index].row < row ||
                        (grid->virtual_cells[sparse_index].row == row &&
                         grid->virtual_cells[sparse_index].col < col)))
                    sparse_index++;
                if (sparse_index < grid->virtual_cell_count &&
                    grid->virtual_cells[sparse_index].row == row &&
                    grid->virtual_cells[sparse_index].col == col) {
                    text = grid->virtual_cells[sparse_index].text;
                    sparse_index++;
                }
            } else if (grid->cells) {
                text = grid->cells[row * (size_t)grid->col_count + (size_t)col];
            }
            if (text && font_is_live && width > 0)
                vg_font_draw_text(canvas,
                                  grid->font,
                                  grid->font_size,
                                  cx + grid->cell_padding,
                                  row_y + baseline,
                                  text,
                                  foreground);
            cx += (float)width;
            vgfx_fill_rect(win, (int32_t)cx - 1, (int32_t)row_y, 1, (int32_t)lh, grid_color);

            bool selected_cell = row == grid->selected_row && col == grid->selected_col;
            bool editing_cell =
                grid->editing && row == grid->editing_row && col == grid->editing_col;
            if ((selected_cell || editing_cell) && width > 1) {
                vgfx_rect(win,
                          (int32_t)(cx - (float)width),
                          (int32_t)row_y,
                          width,
                          (int32_t)lh,
                          editing_cell ? accent : header_foreground);
            }
        }
        row_y += lh;
        vgfx_fill_rect(win, (int32_t)ox, (int32_t)row_y - 1, (int32_t)widget->width, 1, grid_color);
    }

    if (widget->width > 2.0f && widget->height > 2.0f)
        vgfx_clear_clip(win);
    vgfx_rect(win,
              (int32_t)widget->x,
              (int32_t)widget->y,
              (int32_t)widget->width,
              (int32_t)widget->height,
              (widget->state & VG_STATE_FOCUSED) ? accent : grid_color);
}

/// @brief VTable event handler for resize, sort, selection, activation, scrolling, and keyboard.
/// @param widget Grid base widget.
/// @param event Target-local GUI event.
/// @return true when the event changed/operated the grid.
static bool datagrid_handle_event(vg_widget_t *widget, vg_event_t *event) {
    vg_datagrid_t *grid = (vg_datagrid_t *)widget;
    if (!event || !widget->enabled || !datagrid_has_interaction(grid))
        return false;
    float line_height = grid->line_height > 0.0f ? grid->line_height : 18.0f;
    bool mouse_event = event->type == VG_EVENT_MOUSE_DOWN || event->type == VG_EVENT_MOUSE_MOVE ||
                       event->type == VG_EVENT_MOUSE_UP || event->type == VG_EVENT_CLICK ||
                       event->type == VG_EVENT_DOUBLE_CLICK;
    bool in_header =
        mouse_event && grid->has_headers && event->mouse.y >= 0.0f && event->mouse.y < line_height;

    if (event->type == VG_EVENT_MOUSE_DOWN && in_header) {
        int boundary = datagrid_resize_boundary_at_x(grid, event->mouse.x);
        if (boundary >= 0) {
            grid->resizing_column = true;
            grid->resize_column = boundary;
            grid->resize_start_x = event->mouse.x;
            grid->resize_start_width = (float)vg_datagrid_column_width(grid, boundary);
            grid->resize_drag_changed = false;
            vg_widget_set_input_capture(widget);
            event->handled = true;
            return true;
        }
    }

    if (event->type == VG_EVENT_MOUSE_MOVE && grid->resizing_column &&
        vg_widget_get_input_capture() == widget) {
        float width = grid->resize_start_width + event->mouse.x - grid->resize_start_x;
        float old_width = grid->column_widths[grid->resize_column];
        if (vg_datagrid_set_column_width(grid, grid->resize_column, width) &&
            grid->column_widths[grid->resize_column] != old_width)
            grid->resize_drag_changed = true;
        event->handled = true;
        return true;
    }

    if (event->type == VG_EVENT_MOUSE_UP && grid->resizing_column) {
        if (vg_widget_get_input_capture() == widget)
            vg_widget_release_input_capture();
        grid->resizing_column = false;
        grid->resize_column = -1;
        // A resize-boundary gesture owns its following click even when the pointer did not move;
        // otherwise a click on a sortable header boundary can unexpectedly sort the next column.
        grid->suppress_click = true;
        event->handled = true;
        return true;
    }

    if (event->type == VG_EVENT_CLICK) {
        if (grid->suppress_click) {
            grid->suppress_click = false;
            event->handled = true;
            return true;
        }
        int col = datagrid_column_at_x(grid, event->mouse.x);
        if (col < 0)
            return false;
        if (in_header && grid->sortable_columns && grid->sortable_columns[col]) {
            int direction = grid->sort_column == col ? grid->sort_direction : 0;
            direction = direction == 0 ? 1 : (direction > 0 ? -1 : 0);
            (void)vg_datagrid_set_sort(grid, col, direction);
            event->handled = true;
            return true;
        }
        float data_y = event->mouse.y - (grid->has_headers ? line_height : 0.0f);
        if (grid->selectable && data_y >= 0.0f) {
            size_t first = 0;
            size_t count = 0;
            datagrid_viewport_bounds(grid, &first, &count);
            size_t offset = (size_t)(data_y / line_height);
            if (offset < count && vg_datagrid_select_cell(grid, first + offset, col)) {
                event->handled = true;
                return true;
            }
        }
    }

    if (event->type == VG_EVENT_DOUBLE_CLICK && (grid->selectable || grid->editable)) {
        int col = datagrid_column_at_x(grid, event->mouse.x);
        float data_y = event->mouse.y - (grid->has_headers ? line_height : 0.0f);
        if (col >= 0 && data_y >= 0.0f) {
            size_t first = 0;
            size_t count = 0;
            datagrid_viewport_bounds(grid, &first, &count);
            size_t offset = (size_t)(data_y / line_height);
            bool valid = offset < count && datagrid_cell_is_valid(grid, first + offset, col);
            if (valid) {
                if (grid->selectable)
                    (void)vg_datagrid_select_cell(grid, first + offset, col);
                vg_widget_note_activation(widget);
                if (grid->editable)
                    (void)vg_datagrid_begin_edit(grid, first + offset, col);
                event->handled = true;
                return true;
            }
        }
    }

    if (event->type == VG_EVENT_MOUSE_WHEEL && datagrid_total_rows(grid) > 0) {
        double delta = (double)event->wheel.delta_y;
        if (!isfinite(delta) || delta == 0.0)
            return false;
        double scaled = ceil(fabs(delta) * 3.0);
        size_t steps = scaled >= (double)SIZE_MAX ? SIZE_MAX : (size_t)scaled;
        if (steps == 0)
            steps = 1;
        size_t old_row = grid->scroll_row;
        size_t row = old_row;
        if (delta > 0.0)
            row = row > steps ? row - steps : 0;
        else
            row = steps > SIZE_MAX - row ? SIZE_MAX : row + steps;
        size_t last_row = datagrid_total_rows(grid) - 1u;
        if (row > last_row)
            row = last_row;
        if (row == old_row)
            return false;
        vg_datagrid_scroll_to_row(grid, row);
        event->handled = true;
        return true;
    }

    if (event->type != VG_EVENT_KEY_DOWN)
        return false;
    if (grid->editing && event->key.key == VG_KEY_ESCAPE) {
        vg_datagrid_cancel_edit(grid);
        event->handled = true;
        return true;
    }
    if (datagrid_total_rows(grid) == 0 || grid->col_count <= 0)
        return false;
    size_t row = grid->selected_row == SIZE_MAX ? 0 : grid->selected_row;
    int col = grid->selected_col < 0 ? 0 : grid->selected_col;
    if (event->key.key == VG_KEY_F2) {
        if (grid->editable && vg_datagrid_begin_edit(grid, row, col)) {
            event->handled = true;
            return true;
        }
        return false;
    }
    if (!grid->selectable)
        return false;
    switch (event->key.key) {
        case VG_KEY_UP:
            if (row > 0)
                row--;
            break;
        case VG_KEY_DOWN:
            if (row + 1 < datagrid_total_rows(grid))
                row++;
            break;
        case VG_KEY_LEFT:
            if (col > 0)
                col--;
            break;
        case VG_KEY_RIGHT:
            if (col + 1 < grid->col_count)
                col++;
            break;
        case VG_KEY_HOME:
            col = 0;
            break;
        case VG_KEY_END:
            col = grid->col_count - 1;
            break;
        case VG_KEY_ENTER:
            (void)vg_datagrid_select_cell(grid, row, col);
            vg_widget_note_activation(widget);
            event->handled = true;
            return true;
        default:
            return false;
    }
    (void)vg_datagrid_select_cell(grid, row, col);
    datagrid_ensure_selection_visible(grid);
    widget->needs_paint = true;
    event->handled = true;
    return true;
}

/// @brief VTable focus capability follows explicit interaction opt-in.
/// @param widget Grid base widget.
/// @return true when visible/enabled and at least one interaction mode is enabled.
static bool datagrid_can_focus(vg_widget_t *widget) {
    return widget && widget->visible && widget->enabled &&
           datagrid_has_interaction((vg_datagrid_t *)widget);
}

/// @brief VTable destroy releases capture and all owned table storage.
/// @param widget Grid base widget.
static void datagrid_destroy(vg_widget_t *widget) {
    vg_datagrid_t *grid = (vg_datagrid_t *)widget;
    if (vg_widget_get_input_capture() == widget)
        vg_widget_release_input_capture();
    datagrid_free_cells(grid);
    datagrid_free_virtual_cells(grid);
    datagrid_free_headers(grid);
    datagrid_free_column_metadata(grid);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Creates an empty data grid with theme-derived defaults.
///
/// @details The grid begins without columns, rows, selection, sorting, or edit
/// state. Its default font and row height come from the current theme when that
/// font is live, otherwise an 18-pixel row height is used.
///
/// @param parent Widget to receive the grid as a child; may be null.
/// @return Newly allocated grid, or null on allocation failure.
vg_datagrid_t *vg_datagrid_create(vg_widget_t *parent) {
    vg_datagrid_t *grid = (vg_datagrid_t *)calloc(1, sizeof(vg_datagrid_t));
    if (!grid)
        return NULL;

    vg_widget_init(&grid->base, VG_WIDGET_DATAGRID, &g_datagrid_vtable);

    grid->cell_padding = 6.0f;

    vg_theme_t *theme = vg_theme_get_current();
    grid->font = theme->typography.font_regular;
    grid->font_size = theme->typography.size_normal;
    if (grid->font && vg_font_is_live(grid->font) && grid->font_size > 0.0f) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(grid->font, grid->font_size, &metrics);
        grid->line_height =
            metrics.line_height > 0 ? (float)metrics.line_height : grid->font_size * 1.4f;
    } else {
        grid->line_height = 18.0f;
    }
    grid->bg_color = theme->colors.bg_primary;
    grid->fg_color = theme->colors.fg_primary;
    grid->header_color = theme->colors.accent_primary;
    grid->grid_color = theme->colors.border_secondary;
    grid->selected_row = SIZE_MAX;
    grid->selected_col = -1;
    grid->sort_column = -1;
    grid->resize_column = -1;
    grid->resized_column = -1;
    grid->editing_row = SIZE_MAX;
    grid->editing_col = -1;
    datagrid_update_accessible_value(grid);

    if (parent)
        vg_widget_add_child(parent, &grid->base);

    return grid;
}

/// @brief Destroys a grid and all dense, sparse, header, and metadata storage.
///
/// @param grid Grid to destroy; may be null.
void vg_datagrid_destroy(vg_datagrid_t *grid) {
    if (!grid)
        return;
    vg_widget_destroy(&grid->base);
}

/// @brief Replaces the grid's column schema with an empty schema of a given size.
///
/// @details Negative counts normalize to zero. New header and interaction
/// metadata arrays are allocated atomically before existing dense or virtual
/// rows are discarded. Selection, sort, resize, edit, and viewport state reset
/// to their defaults. Allocation failure preserves the current grid.
///
/// @param grid Grid to reconfigure.
/// @param count Requested column count; negative values select zero columns.
void vg_datagrid_set_columns(vg_datagrid_t *grid, int count) {
    if (!grid)
        return;
    if (count < 0)
        count = 0;
    char **new_headers = NULL;
    bool *new_sortable = NULL;
    float *new_widths = NULL;
    int *new_auto_widths = NULL;
    bool *new_resizable = NULL;
    if (count > 0) {
        if ((size_t)count > SIZE_MAX / sizeof(*new_headers))
            return;
        new_headers = (char **)calloc((size_t)count, sizeof(*new_headers));
        new_sortable = (bool *)calloc((size_t)count, sizeof(*new_sortable));
        new_widths = (float *)calloc((size_t)count, sizeof(*new_widths));
        new_auto_widths = (int *)calloc((size_t)count, sizeof(*new_auto_widths));
        new_resizable = (bool *)calloc((size_t)count, sizeof(*new_resizable));
        if (!new_headers || !new_sortable || !new_widths || !new_auto_widths || !new_resizable) {
            free(new_headers);
            free(new_sortable);
            free(new_widths);
            free(new_auto_widths);
            free(new_resizable);
            return;
        }
    }
    bool changed = grid->col_count != count || grid->row_count > 0 || grid->has_headers ||
                   grid->virtual_mode || grid->virtual_cell_count > 0;
    bool had_selection = grid->selected_row != SIZE_MAX;
    bool had_sort = grid->sort_direction != 0;
    bool was_editing = grid->editing;
    datagrid_free_cells(grid);
    datagrid_free_virtual_cells(grid);
    datagrid_free_headers(grid);
    datagrid_free_column_metadata(grid);
    grid->col_count = count;
    grid->headers = new_headers;
    grid->sortable_columns = new_sortable;
    grid->column_widths = new_widths;
    grid->auto_column_widths = new_auto_widths;
    grid->resizable_columns = new_resizable;
    grid->virtual_mode = false;
    grid->virtual_row_count = 0;
    grid->viewport_first_row = 0;
    grid->viewport_row_count = 0;
    grid->scroll_row = 0;
    grid->selected_row = SIZE_MAX;
    grid->selected_col = -1;
    grid->sort_column = -1;
    grid->sort_direction = 0;
    grid->resizing_column = false;
    grid->resize_column = -1;
    grid->editing = false;
    grid->editing_row = SIZE_MAX;
    grid->editing_col = -1;
    datagrid_recompute_all_auto_widths(grid);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    if (had_selection)
        datagrid_note_selection(grid);
    if (had_sort)
        datagrid_note_sort(grid);
    if (was_editing)
        vg_widget_note_revision(&grid->base);
    if (changed)
        vg_widget_note_change(&grid->base);
}

/// @brief Sets or clears the copied header text for one column.
///
/// @details Null or empty text clears the header. The grid displays its header
/// row whenever at least one column has non-empty text. Automatic width and
/// layout caches are updated after successful replacement.
///
/// @param grid Grid to update.
/// @param col Zero-based column index.
/// @param text Header text to copy; may be null or empty to clear.
void vg_datagrid_set_header(vg_datagrid_t *grid, int col, const char *text) {
    if (!grid || !grid->headers || col < 0 || col >= grid->col_count)
        return;
    const char *normalized = text && text[0] ? text : NULL;
    const char *old = grid->headers[col];
    if ((!old && !normalized) || (old && normalized && strcmp(old, normalized) == 0))
        return;
    char *replacement = datagrid_dup(normalized);
    if (normalized && !replacement)
        return;
    int old_width = datagrid_measure_text_width(grid, old);
    free(grid->headers[col]);
    grid->headers[col] = replacement;
    grid->has_headers = false;
    for (int i = 0; i < grid->col_count; ++i) {
        if (grid->headers[i]) {
            grid->has_headers = true;
            break;
        }
    }
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    datagrid_update_auto_width_after_text_change(grid, col, old_width, replacement);
    vg_widget_note_change(&grid->base);
}

/// @brief Sets or clears one dense cell, growing row storage as needed.
///
/// @details Null or empty text represents an empty cell. Writing through this
/// compatibility API exits sparse virtual mode, releases its materialized
/// cells, and establishes a dense row count through @p row. Input text is copied
/// before mutation, so allocation failure preserves the prior value.
///
/// @param grid Grid to update.
/// @param row Non-negative dense row index.
/// @param col Zero-based column index.
/// @param text Cell text to copy; may be null or empty to clear.
void vg_datagrid_set_cell(vg_datagrid_t *grid, int row, int col, const char *text) {
    if (!grid || col < 0 || col >= grid->col_count || row < 0 || row == INT_MAX)
        return;
    const char *normalized = text && text[0] ? text : NULL;
    const char *old = NULL;
    if (!grid->virtual_mode && row < grid->row_count && grid->cells) {
        old = grid->cells[(size_t)row * (size_t)grid->col_count + (size_t)col];
        if (datagrid_text_equal(old, normalized))
            return;
    }
    int old_width = datagrid_measure_text_width(grid, old);
    char *replacement = datagrid_dup(normalized);
    if (normalized && !replacement)
        return;
    if (!datagrid_ensure_rows(grid, row + 1)) {
        free(replacement);
        return;
    }
    bool exited_virtual_mode = grid->virtual_mode;
    if (exited_virtual_mode) {
        datagrid_free_virtual_cells(grid);
        grid->virtual_mode = false;
        grid->virtual_row_count = 0;
        grid->viewport_first_row = 0;
        grid->scroll_row = 0;
    }
    if (row + 1 > grid->row_count)
        grid->row_count = row + 1;
    size_t idx = (size_t)row * (size_t)grid->col_count + (size_t)col;
    free(grid->cells[idx]);
    grid->cells[idx] = replacement;
    if (exited_virtual_mode)
        datagrid_recompute_all_auto_widths(grid);
    else
        datagrid_update_auto_width_after_text_change(grid, col, old_width, replacement);
    datagrid_clamp_row_state(grid);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_change(&grid->base);
}

/// @brief Returns one dense or sparse logical cell value.
///
/// @param grid Grid to inspect.
/// @param row Logical row index.
/// @param col Zero-based column index.
/// @return Borrowed text owned by @p grid, or null for an empty or invalid cell.
const char *vg_datagrid_get_cell(const vg_datagrid_t *grid, size_t row, int col) {
    return datagrid_get_cell_at(grid, row, col);
}

/// @brief Removes every row and cell while preserving the column schema.
///
/// @details Dense or sparse storage is released according to the current mode.
/// Headers and column settings remain intact, automatic widths are recomputed,
/// and invalid selection or editing state is cleared.
///
/// @param grid Grid to clear; may be null.
void vg_datagrid_clear(vg_datagrid_t *grid) {
    if (!grid)
        return;
    if (datagrid_total_rows(grid) == 0 && grid->virtual_cell_count == 0)
        return;
    if (grid->virtual_mode) {
        datagrid_free_virtual_cells(grid);
        grid->virtual_row_count = 0;
    } else {
        datagrid_free_cells(grid);
    }
    datagrid_recompute_all_auto_widths(grid);
    datagrid_clamp_row_state(grid);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_change(&grid->base);
}

/// @brief Sets the borrowed grid font and refreshes text-dependent geometry.
///
/// @details A non-finite or non-positive size retains the current size. A live
/// font with valid metrics updates the row height. All automatic column widths
/// are recomputed and layout plus paint are invalidated.
///
/// @param grid Grid to configure.
/// @param font Borrowed font; may be null to disable text rendering.
/// @param size Finite positive font size, or an invalid value to retain the
///             current size.
void vg_datagrid_set_font(vg_datagrid_t *grid, vg_font_t *font, float size) {
    if (!grid)
        return;
    float normalized_size = isfinite(size) && size > 0.0f ? size : grid->font_size;
    if (grid->font == font && grid->font_size == normalized_size)
        return;
    grid->font = font;
    grid->font_size = normalized_size;
    if (font && vg_font_is_live(font) && grid->font_size > 0.0f) {
        vg_font_metrics_t metrics = {0};
        vg_font_get_metrics(font, grid->font_size, &metrics);
        if (metrics.line_height > 0)
            grid->line_height = (float)metrics.line_height;
    }
    datagrid_recompute_all_auto_widths(grid);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Returns a column's effective rounded width.
///
/// @param grid Grid to inspect.
/// @param col Zero-based column index.
/// @return Explicit width when configured, otherwise cached automatic width;
///         zero for an invalid column.
int vg_datagrid_column_width(const vg_datagrid_t *grid, int col) {
    if (!grid || col < 0 || col >= grid->col_count)
        return 0;
    if (grid->column_widths && grid->column_widths[col] > 0.0f)
        return (int)(grid->column_widths[col] + 0.5f);
    return grid->auto_column_widths ? grid->auto_column_widths[col] : 0;
}

/// @brief Returns the logical row count through the compatibility integer API.
///
/// @param grid Grid to inspect.
/// @return Logical row count saturated at `INT_MAX`, or zero for null.
int vg_datagrid_row_count(const vg_datagrid_t *grid) {
    size_t rows = datagrid_total_rows(grid);
    return rows > (size_t)INT_MAX ? INT_MAX : (int)rows;
}

/// @brief Return the exact dense-or-sparse logical row count.
/// @param grid Grid to inspect.
/// @return Full `size_t` row count, or zero for NULL.
size_t vg_datagrid_logical_row_count(const vg_datagrid_t *grid) {
    return datagrid_total_rows(grid);
}

/// @brief Returns the configured number of columns.
///
/// @param grid Grid to inspect.
/// @return Non-negative column count, or zero for null.
int vg_datagrid_column_count(const vg_datagrid_t *grid) {
    return grid ? grid->col_count : 0;
}

/// @brief Set the viewport's first row and optional explicit visible-row count.
/// @param grid Grid to update.
/// @param first Requested first logical row.
/// @param count Maximum rows to paint, or zero for height-derived capacity.
void vg_datagrid_set_viewport_rows(vg_datagrid_t *grid, size_t first, size_t count) {
    if (!grid)
        return;
    size_t rows = datagrid_total_rows(grid);
    if (rows == 0)
        first = 0;
    else if (first >= rows)
        first = rows - 1;
    if (grid->scroll_row == first && grid->viewport_first_row == first &&
        grid->viewport_row_count == count)
        return;
    grid->scroll_row = first;
    grid->viewport_first_row = first;
    grid->viewport_row_count = count;
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Switch to sparse virtual mode and set its logical row count.
/// @details Dense storage is cleared only when entering virtual mode. Shrinking frees sparse cells
///          outside the new range without touching surviving entries.
/// @param grid Grid to update.
/// @param count New logical virtual row count.
void vg_datagrid_set_virtual_row_count(vg_datagrid_t *grid, size_t count) {
    if (!grid)
        return;
    bool entering = !grid->virtual_mode;
    if (!entering && grid->virtual_row_count == count)
        return;
    if (entering) {
        datagrid_free_cells(grid);
        grid->virtual_mode = true;
    }
    if (count < grid->virtual_row_count && grid->virtual_cell_count > 0) {
        bool found = false;
        size_t first_removed = datagrid_virtual_lower_bound(grid, count, 0, &found);
        (void)found;
        for (size_t i = first_removed; i < grid->virtual_cell_count; ++i)
            free(grid->virtual_cells[i].text);
        grid->virtual_cell_count = first_removed;
    }
    grid->virtual_row_count = count;
    datagrid_recompute_all_auto_widths(grid);
    datagrid_clamp_row_state(grid);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_change(&grid->base);
}

/// @brief Set, replace, or remove one sparse virtual cell atomically.
/// @param grid Virtual grid to update.
/// @param row Zero-based logical row.
/// @param col Zero-based column.
/// @param text UTF-8 text; NULL/empty removes an existing materialization.
/// @return true for a valid successful request, including unchanged text.
bool vg_datagrid_set_virtual_cell(vg_datagrid_t *grid, size_t row, int col, const char *text) {
    if (!grid || !grid->virtual_mode || row >= grid->virtual_row_count || col < 0 ||
        col >= grid->col_count)
        return false;
    const char *normalized = text && text[0] ? text : NULL;
    bool found = false;
    size_t index = datagrid_virtual_lower_bound(grid, row, col, &found);
    const char *old = found ? grid->virtual_cells[index].text : NULL;
    if (datagrid_text_equal(old, normalized))
        return true;
    int old_width = datagrid_measure_text_width(grid, old);

    char *replacement = datagrid_dup(normalized);
    if (normalized && !replacement)
        return false;
    if (found) {
        free(grid->virtual_cells[index].text);
        if (!replacement) {
            if (index + 1 < grid->virtual_cell_count) {
                memmove(&grid->virtual_cells[index],
                        &grid->virtual_cells[index + 1],
                        (grid->virtual_cell_count - index - 1) * sizeof(*grid->virtual_cells));
            }
            grid->virtual_cell_count--;
        } else {
            grid->virtual_cells[index].text = replacement;
        }
    } else {
        if (!replacement)
            return true;
        if (grid->virtual_cell_count == grid->virtual_cell_capacity) {
            size_t capacity = grid->virtual_cell_capacity ? grid->virtual_cell_capacity * 2u : 16u;
            if (capacity < grid->virtual_cell_capacity ||
                capacity > SIZE_MAX / sizeof(*grid->virtual_cells)) {
                free(replacement);
                return false;
            }
            vg_datagrid_virtual_cell_t *cells = (vg_datagrid_virtual_cell_t *)realloc(
                grid->virtual_cells, capacity * sizeof(*grid->virtual_cells));
            if (!cells) {
                free(replacement);
                return false;
            }
            grid->virtual_cells = cells;
            grid->virtual_cell_capacity = capacity;
        }
        if (index < grid->virtual_cell_count) {
            memmove(&grid->virtual_cells[index + 1],
                    &grid->virtual_cells[index],
                    (grid->virtual_cell_count - index) * sizeof(*grid->virtual_cells));
        }
        grid->virtual_cells[index].row = row;
        grid->virtual_cells[index].col = col;
        grid->virtual_cells[index].text = replacement;
        grid->virtual_cell_count++;
    }
    datagrid_update_auto_width_after_text_change(grid, col, old_width, replacement);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    vg_widget_note_change(&grid->base);
    return true;
}

/// @brief Enable or disable pointer/keyboard cell selection.
/// @param grid Grid to configure.
/// @param enabled true to enable selection.
void vg_datagrid_set_selectable(vg_datagrid_t *grid, bool enabled) {
    if (!grid || grid->selectable == enabled)
        return;
    grid->selectable = enabled;
    if (!enabled && grid->selected_row != SIZE_MAX) {
        grid->selected_row = SIZE_MAX;
        grid->selected_col = -1;
        datagrid_note_selection(grid);
    } else {
        vg_widget_note_revision(&grid->base);
    }
    grid->base.needs_paint = true;
}

/// @brief Return the selected logical row.
/// @param grid Grid to inspect.
/// @return Selected row, or SIZE_MAX when absent.
size_t vg_datagrid_get_selected_row(const vg_datagrid_t *grid) {
    return grid ? grid->selected_row : SIZE_MAX;
}

/// @brief Return the selected column.
/// @param grid Grid to inspect.
/// @return Selected column, or -1 when absent.
int vg_datagrid_get_selected_column(const vg_datagrid_t *grid) {
    return grid ? grid->selected_col : -1;
}

/// @brief Select one valid logical cell through the common revision path.
/// @param grid Selectable grid to update.
/// @param row Logical row.
/// @param col Logical column.
/// @return true for valid selection, including an unchanged cell.
bool vg_datagrid_select_cell(vg_datagrid_t *grid, size_t row, int col) {
    if (!grid || !grid->selectable || !datagrid_cell_is_valid(grid, row, col))
        return false;
    if (grid->selected_row == row && grid->selected_col == col)
        return true;
    grid->selected_row = row;
    grid->selected_col = col;
    grid->base.needs_paint = true;
    datagrid_note_selection(grid);
    return true;
}

/// @brief Clear grid selection and publish one independent selection edge.
/// @param grid Grid to update.
void vg_datagrid_clear_selection(vg_datagrid_t *grid) {
    if (!grid || grid->selected_row == SIZE_MAX)
        return;
    grid->selected_row = SIZE_MAX;
    grid->selected_col = -1;
    grid->base.needs_paint = true;
    datagrid_note_selection(grid);
}

/// @brief Consume the independent selection transition edge.
/// @param grid Grid to inspect.
/// @return true once after unreported selection transitions.
bool vg_datagrid_was_selection_changed(vg_datagrid_t *grid) {
    if (!grid || grid->selection_version == grid->reported_selection_version)
        return false;
    grid->reported_selection_version = grid->selection_version;
    return true;
}

/// @brief Enable or disable sort requests for one column.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param enabled true to enable sort requests.
void vg_datagrid_set_sortable(vg_datagrid_t *grid, int col, bool enabled) {
    if (!grid || !grid->sortable_columns || col < 0 || col >= grid->col_count ||
        grid->sortable_columns[col] == enabled)
        return;
    grid->sortable_columns[col] = enabled;
    if (!enabled && grid->sort_column == col && grid->sort_direction != 0) {
        grid->sort_column = -1;
        grid->sort_direction = 0;
        datagrid_note_sort(grid);
    } else {
        vg_widget_note_revision(&grid->base);
    }
}

/// @brief Set a normalized sort request without reordering caller-owned data.
/// @param grid Grid to update.
/// @param col Sort column for nonzero direction.
/// @param direction Negative descending, zero none, positive ascending.
/// @return true for valid requests, including unchanged state.
bool vg_datagrid_set_sort(vg_datagrid_t *grid, int col, int direction) {
    if (!grid)
        return false;
    direction = direction < 0 ? -1 : (direction > 0 ? 1 : 0);
    if (direction != 0 && (!grid->sortable_columns || col < 0 || col >= grid->col_count ||
                           !grid->sortable_columns[col]))
        return false;
    int normalized_col = direction == 0 ? -1 : col;
    if (grid->sort_column == normalized_col && grid->sort_direction == direction)
        return true;
    grid->sort_column = normalized_col;
    grid->sort_direction = direction;
    datagrid_note_sort(grid);
    return true;
}

/// @brief Return the active sort column.
/// @param grid Grid to inspect.
/// @return Column or -1 when unsorted.
int vg_datagrid_get_sort_column(const vg_datagrid_t *grid) {
    return grid ? grid->sort_column : -1;
}

/// @brief Return the active normalized sort direction.
/// @param grid Grid to inspect.
/// @return -1, 0, or 1.
int vg_datagrid_get_sort_direction(const vg_datagrid_t *grid) {
    return grid ? grid->sort_direction : 0;
}

/// @brief Consume the independent sort transition edge.
/// @param grid Grid to inspect.
/// @return true once after unreported sort changes.
bool vg_datagrid_was_sort_changed(vg_datagrid_t *grid) {
    if (!grid || grid->sort_version == grid->reported_sort_version)
        return false;
    grid->reported_sort_version = grid->sort_version;
    return true;
}

/// @brief Set an explicit column width or reset to cached automatic sizing.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param width Physical width; zero selects automatic sizing.
/// @return true for a valid request, including unchanged state.
bool vg_datagrid_set_column_width(vg_datagrid_t *grid, int col, float width) {
    if (!grid || !grid->column_widths || col < 0 || col >= grid->col_count || !isfinite(width) ||
        width < 0.0f)
        return false;
    if (width > 0.0f && width < VG_DATAGRID_MIN_COLUMN_WIDTH)
        width = VG_DATAGRID_MIN_COLUMN_WIDTH;
    if (width > VG_DATAGRID_MAX_COLUMN_WIDTH)
        width = VG_DATAGRID_MAX_COLUMN_WIDTH;
    if (grid->column_widths[col] == width)
        return true;
    int old_effective = vg_datagrid_column_width(grid, col);
    grid->column_widths[col] = width;
    int new_effective = vg_datagrid_column_width(grid, col);
    grid->base.needs_layout = true;
    grid->base.needs_paint = true;
    if (old_effective != new_effective)
        datagrid_note_resize(grid, col);
    else
        vg_widget_note_revision(&grid->base);
    return true;
}

/// @brief Enable or disable pointer resizing for one column boundary.
/// @param grid Grid to update.
/// @param col Zero-based column.
/// @param enabled true to permit resizing.
void vg_datagrid_set_column_resizable(vg_datagrid_t *grid, int col, bool enabled) {
    if (!grid || !grid->resizable_columns || col < 0 || col >= grid->col_count ||
        grid->resizable_columns[col] == enabled)
        return;
    grid->resizable_columns[col] = enabled;
    vg_widget_note_revision(&grid->base);
}

/// @brief Consume the independent effective-width transition edge.
/// @param grid Grid to inspect.
/// @return true once after unreported effective width changes.
bool vg_datagrid_was_column_resized(vg_datagrid_t *grid) {
    if (!grid || grid->resize_version == grid->reported_resize_version)
        return false;
    grid->reported_resize_version = grid->resize_version;
    return true;
}

/// @brief Return the most recently resized column.
/// @param grid Grid to inspect.
/// @return Column or -1 when no effective resize exists.
int vg_datagrid_get_resized_column(const vg_datagrid_t *grid) {
    return grid ? grid->resized_column : -1;
}

/// @brief Enable or disable externally-driven edit mode.
/// @param grid Grid to update.
/// @param enabled true to permit BeginEdit/CommitEdit.
void vg_datagrid_set_editable(vg_datagrid_t *grid, bool enabled) {
    if (!grid || grid->editable == enabled)
        return;
    grid->editable = enabled;
    if (!enabled && grid->editing)
        vg_datagrid_cancel_edit(grid);
    else
        vg_widget_note_revision(&grid->base);
}

/// @brief Begin controller-driven editing of one valid cell.
/// @param grid Editable grid to update.
/// @param row Logical row.
/// @param col Logical column.
/// @return true when editing is active for the requested cell.
bool vg_datagrid_begin_edit(vg_datagrid_t *grid, size_t row, int col) {
    if (!grid || !grid->editable || !datagrid_cell_is_valid(grid, row, col))
        return false;
    if (grid->editing && grid->editing_row == row && grid->editing_col == col)
        return true;
    grid->editing = true;
    grid->editing_row = row;
    grid->editing_col = col;
    if (grid->selectable)
        (void)vg_datagrid_select_cell(grid, row, col);
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
    return true;
}

/// @brief Commit copied text to the active dense or sparse cell and close edit mode.
/// @param grid Grid with an active edit.
/// @param text UTF-8 replacement, or NULL/empty to clear.
/// @return true when the active edit committed successfully.
bool vg_datagrid_commit_edit(vg_datagrid_t *grid, const char *text) {
    if (!grid || !grid->editing ||
        !datagrid_cell_is_valid(grid, grid->editing_row, grid->editing_col))
        return false;
    size_t row = grid->editing_row;
    int col = grid->editing_col;
    const char *old = datagrid_get_cell_at(grid, row, col);
    bool changed = !datagrid_text_equal(old, text);
    bool success = false;
    if (grid->virtual_mode) {
        success = vg_datagrid_set_virtual_cell(grid, row, col, text);
    } else if (row <= (size_t)INT_MAX) {
        vg_datagrid_set_cell(grid, (int)row, col, text);
        success = datagrid_text_equal(datagrid_get_cell_at(grid, row, col), text);
    }
    if (!success)
        return false;
    grid->editing = false;
    grid->editing_row = SIZE_MAX;
    grid->editing_col = -1;
    grid->base.needs_paint = true;
    if (changed)
        datagrid_increment_version(&grid->edit_version);
    vg_widget_note_revision(&grid->base);
    return true;
}

/// @brief Cancel an active edit without changing cell content.
/// @param grid Grid to update.
void vg_datagrid_cancel_edit(vg_datagrid_t *grid) {
    if (!grid || !grid->editing)
        return;
    grid->editing = false;
    grid->editing_row = SIZE_MAX;
    grid->editing_col = -1;
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Query whether a valid edit controller is active.
/// @param grid Grid to inspect.
/// @return true when editing.
bool vg_datagrid_is_editing(const vg_datagrid_t *grid) {
    return grid ? grid->editing : false;
}

/// @brief Consume the independent effective-cell-edit edge.
/// @param grid Grid to inspect.
/// @return true once after unreported committed edits.
bool vg_datagrid_was_cell_edited(vg_datagrid_t *grid) {
    if (!grid || grid->edit_version == grid->reported_edit_version)
        return false;
    grid->reported_edit_version = grid->edit_version;
    return true;
}

/// @brief Make one logical row the first visible viewport row.
/// @param grid Grid to update.
/// @param row Requested row, clamped to the last logical row.
void vg_datagrid_scroll_to_row(vg_datagrid_t *grid, size_t row) {
    if (!grid)
        return;
    size_t rows = datagrid_total_rows(grid);
    if (rows == 0)
        row = 0;
    else if (row >= rows)
        row = rows - 1;
    if (grid->scroll_row == row && grid->viewport_first_row == row)
        return;
    grid->scroll_row = row;
    grid->viewport_first_row = row;
    grid->base.needs_paint = true;
    vg_widget_note_revision(&grid->base);
}

/// @brief Return the first logical viewport row.
/// @param grid Grid to inspect.
/// @return Zero-based scroll row, or zero for NULL.
size_t vg_datagrid_get_scroll_row(const vg_datagrid_t *grid) {
    return grid ? grid->scroll_row : 0;
}
