//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_layout.h
// Purpose: Layout system for automatic widget positioning and sizing.
//          Provides VBox, HBox, Flex, Grid, and Dock layout containers.
// Key invariants:
//   - A layout container is a regular vg_widget_t and can be nested arbitrarily.
//   - Children participate via their flex factor, margins, padding, and constraints.
//   - Destroying a layout container destroys all children recursively.
// Ownership/Lifetime:
//   - Layout containers own their configuration data (grid column/row arrays).
//   - The grid column_widths and row_heights arrays are freed with the container.
// Links: lib/gui/src/core/vg_layout.c,
//        lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_theme.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// @file
/// @brief Declares retained VBox, HBox, Flex, Grid, and Dock layout containers.
/// @details Layout widgets own algorithm-specific configuration while ordinary child widgets
/// contribute sizing constraints, margins, flex factors, grid placement, and dock positions.
/// Public checked operations preserve existing tree state when validation or allocation fails.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Widget base type arranged by layout containers.
typedef struct vg_widget vg_widget_t;

//=============================================================================
// Layout Types
//=============================================================================

/// @brief Identifies which layout algorithm a container uses.
///
/// @details Stored inside the container widget and consulted during the
///          arrange pass to dispatch to the correct internal layout function.
typedef enum vg_layout_type {
    VG_LAYOUT_NONE,     ///< Manual positioning -- no automatic layout.
    VG_LAYOUT_VBOX,     ///< Vertical stack layout (top-to-bottom).
    VG_LAYOUT_HBOX,     ///< Horizontal stack layout (left-to-right).
    VG_LAYOUT_FLEX,     ///< Flexbox-like layout with direction, wrapping, and gap.
    VG_LAYOUT_GRID,     ///< Two-dimensional row/column grid layout.
    VG_LAYOUT_ABSOLUTE, ///< Absolute positioning (children use their x/y directly).
    VG_LAYOUT_DOCK,     ///< Dock layout -- edges are claimed first, remainder fills.
} vg_layout_type_t;

//=============================================================================
// Alignment
//=============================================================================

/// @brief Cross-axis alignment options for layout containers.
///
/// @details Controls how children are positioned along the axis perpendicular
///          to the main layout direction. For a VBox (main axis = vertical),
///          the cross axis is horizontal; for an HBox, it is vertical.
typedef enum vg_align {
    VG_ALIGN_START,    ///< Align to the start of the cross axis.
    VG_ALIGN_CENTER,   ///< Centre along the cross axis.
    VG_ALIGN_END,      ///< Align to the end of the cross axis.
    VG_ALIGN_STRETCH,  ///< Stretch to fill the entire cross axis.
    VG_ALIGN_BASELINE, ///< Align text baselines (applicable to text widgets).
} vg_align_t;

/// @brief Horizontal text alignment options.
typedef enum vg_h_align {
    VG_ALIGN_H_LEFT,   ///< Left-aligned text.
    VG_ALIGN_H_CENTER, ///< Horizontally centred text.
    VG_ALIGN_H_RIGHT,  ///< Right-aligned text.
} vg_h_align_t;

/// @brief Vertical text alignment options.
typedef enum vg_v_align {
    VG_ALIGN_V_TOP,      ///< Text aligned to the top edge.
    VG_ALIGN_V_CENTER,   ///< Text vertically centred.
    VG_ALIGN_V_BOTTOM,   ///< Text aligned to the bottom edge.
    VG_ALIGN_V_BASELINE, ///< Aligned to the typographic baseline.
} vg_v_align_t;

//=============================================================================
// Justify Content
//=============================================================================

/// @brief Main-axis distribution strategies for layout containers.
///
/// @details Controls how children and any leftover space are distributed
///          along the primary layout direction.
typedef enum vg_justify {
    VG_JUSTIFY_START,         ///< Pack children at the start of the main axis.
    VG_JUSTIFY_CENTER,        ///< Centre children along the main axis.
    VG_JUSTIFY_END,           ///< Pack children at the end of the main axis.
    VG_JUSTIFY_SPACE_BETWEEN, ///< Equal space between children; none at edges.
    VG_JUSTIFY_SPACE_AROUND,  ///< Equal space around each child (half-space at edges).
    VG_JUSTIFY_SPACE_EVENLY,  ///< Equal space between and around every child.
} vg_justify_t;

//=============================================================================
// Direction
//=============================================================================

/// @brief Main-axis direction for Flex containers.
typedef enum vg_direction {
    VG_DIRECTION_ROW,            ///< Left-to-right row layout.
    VG_DIRECTION_ROW_REVERSE,    ///< Right-to-left row layout.
    VG_DIRECTION_COLUMN,         ///< Top-to-bottom column layout.
    VG_DIRECTION_COLUMN_REVERSE, ///< Bottom-to-top column layout.
} vg_direction_t;

/// @brief Line-wrapping policy for Flex containers.
/// @details `VG_FLEX_WRAP_REVERSE` forms the same lines as normal wrapping but
///          places the first line at the cross-axis end and subsequent lines
///          toward the cross-axis start.
typedef enum vg_flex_wrap {
    VG_FLEX_NO_WRAP,      ///< Keep all children on one flex line.
    VG_FLEX_WRAP,         ///< Wrap overflowing children toward the cross-axis end.
    VG_FLEX_WRAP_REVERSE, ///< Wrap overflowing children toward the cross-axis start.
} vg_flex_wrap_t;

//=============================================================================
// Dock Position
//=============================================================================

/// @brief Docking position for children in a Dock layout container.
///
/// @details Each non-FILL child claims space from one edge of the remaining
///          area. The last child (or any child with VG_DOCK_FILL) receives
///          whatever space remains.
typedef enum vg_dock {
    VG_DOCK_NONE,   ///< Not docked (uses manual position).
    VG_DOCK_LEFT,   ///< Dock to the left edge.
    VG_DOCK_TOP,    ///< Dock to the top edge.
    VG_DOCK_RIGHT,  ///< Dock to the right edge.
    VG_DOCK_BOTTOM, ///< Dock to the bottom edge.
    VG_DOCK_FILL,   ///< Fill all remaining space after other docked children.
} vg_dock_t;

//=============================================================================
// VBox Layout Data
//=============================================================================

/// @brief Configuration data for a vertical box layout container.
typedef struct vg_vbox_layout {
    float spacing;        ///< Vertical gap between consecutive children (pixels).
    vg_align_t align;     ///< Cross-axis (horizontal) alignment of children.
    vg_justify_t justify; ///< Main-axis (vertical) distribution of children.
} vg_vbox_layout_t;

//=============================================================================
// HBox Layout Data
//=============================================================================

/// @brief Configuration data for a horizontal box layout container.
typedef struct vg_hbox_layout {
    float spacing;        ///< Horizontal gap between consecutive children (pixels).
    vg_align_t align;     ///< Cross-axis (vertical) alignment of children.
    vg_justify_t justify; ///< Main-axis (horizontal) distribution of children.
} vg_hbox_layout_t;

//=============================================================================
// Flex Layout Data
//=============================================================================

/// @brief Configuration data for a Flexbox-style layout container.
///
/// @details Provides CSS-Flexbox-like semantics: a configurable main-axis
///          direction, wrapping, per-item and per-line alignment, and a
///          uniform gap between items.
typedef struct vg_flex_layout {
    vg_direction_t direction;     ///< Main-axis direction (row or column, optionally reversed).
    vg_align_t align_items;       ///< Default cross-axis alignment for each child.
    vg_justify_t justify_content; ///< Distribution of children along the main axis.
    vg_align_t align_content;     ///< Alignment of wrapped lines along the cross axis.
    float gap;                    ///< Uniform gap between adjacent items (pixels).
    vg_flex_wrap_t wrap;          ///< Line-wrapping and cross-axis line-order policy.
    void *scratch_children;       ///< Internal reusable child-pointer scratch buffer.
    int scratch_child_capacity;   ///< Capacity of scratch_children in widget pointers.
    void *scratch_lines;          ///< Internal reusable wrapped-line scratch buffer.
    int scratch_line_capacity;    ///< Capacity of scratch_lines in line records.
} vg_flex_layout_t;

//=============================================================================
// Grid Layout Data
//=============================================================================

/// @brief Configuration data for a two-dimensional grid layout container.
///
/// @details Defines a fixed number of columns and rows with optional
///          per-column widths and per-row heights. If the width/height
///          arrays are NULL, columns and rows share space equally.
typedef struct vg_grid_layout {
    int columns;          ///< Number of columns in the grid.
    int rows;             ///< Number of rows in the grid.
    float column_gap;     ///< Horizontal gap between columns (pixels).
    float row_gap;        ///< Vertical gap between rows (pixels).
    float *column_widths; ///< Per-column width overrides (NULL = equal width).
    float *row_heights;   ///< Per-row height overrides (NULL = equal height).
} vg_grid_layout_t;

//=============================================================================
// Grid Item Data (stored in widget)
//=============================================================================

/// @brief Per-child placement data for a grid layout, specifying which cell(s)
///        the child occupies.
typedef struct vg_grid_item {
    int column;   ///< Zero-based column index where the child starts.
    int row;      ///< Zero-based row index where the child starts.
    int col_span; ///< Number of columns the child spans (>= 1).
    int row_span; ///< Number of rows the child spans (>= 1).
} vg_grid_item_t;

//=============================================================================
// VBox Layout API
//=============================================================================

/// @brief Create a vertical box container widget with the given spacing.
///
/// @details Children added to this container will be arranged in a vertical
///          stack, separated by @p spacing pixels.
///
/// @param spacing Vertical gap between children in pixels.
/// @return Newly allocated VBox widget, or NULL if allocation fails.
vg_widget_t *vg_vbox_create(float spacing);

/// @brief Set the vertical spacing between children in a VBox container.
///
/// @param vbox    The VBox widget to update.
/// @param spacing New spacing value in pixels.
void vg_vbox_set_spacing(vg_widget_t *vbox, float spacing);

/// @brief Set the cross-axis (horizontal) alignment for children of a VBox.
///
/// @param vbox  The VBox widget to update.
/// @param align The alignment strategy to apply.
void vg_vbox_set_align(vg_widget_t *vbox, vg_align_t align);

/// @brief Set the main-axis (vertical) distribution strategy for a VBox.
///
/// @param vbox    The VBox widget to update.
/// @param justify The justification strategy to apply.
void vg_vbox_set_justify(vg_widget_t *vbox, vg_justify_t justify);

/// @brief Read the cross-axis alignment of a VBox.
/// @param vbox VBox widget to inspect.
/// @return Current alignment, or `VG_ALIGN_START` for a non-VBox handle.
vg_align_t vg_vbox_get_align(const vg_widget_t *vbox);

/// @brief Read the main-axis justification of a VBox.
/// @param vbox VBox widget to inspect.
/// @return Current justification, or `VG_JUSTIFY_START` for a non-VBox handle.
vg_justify_t vg_vbox_get_justify(const vg_widget_t *vbox);

//=============================================================================
// HBox Layout API
//=============================================================================

/// @brief Create a horizontal box container widget with the given spacing.
///
/// @details Children added to this container will be arranged in a horizontal
///          row, separated by @p spacing pixels.
///
/// @param spacing Horizontal gap between children in pixels.
/// @return Newly allocated HBox widget, or NULL if allocation fails.
vg_widget_t *vg_hbox_create(float spacing);

/// @brief Set the horizontal spacing between children in an HBox container.
///
/// @param hbox    The HBox widget to update.
/// @param spacing New spacing value in pixels.
void vg_hbox_set_spacing(vg_widget_t *hbox, float spacing);

/// @brief Set the cross-axis (vertical) alignment for children of an HBox.
///
/// @param hbox  The HBox widget to update.
/// @param align The alignment strategy to apply.
void vg_hbox_set_align(vg_widget_t *hbox, vg_align_t align);

/// @brief Set the main-axis (horizontal) distribution strategy for an HBox.
///
/// @param hbox    The HBox widget to update.
/// @param justify The justification strategy to apply.
void vg_hbox_set_justify(vg_widget_t *hbox, vg_justify_t justify);

/// @brief Read the cross-axis alignment of an HBox.
/// @param hbox HBox widget to inspect.
/// @return Current alignment, or `VG_ALIGN_START` for a non-HBox handle.
vg_align_t vg_hbox_get_align(const vg_widget_t *hbox);

/// @brief Read the main-axis justification of an HBox.
/// @param hbox HBox widget to inspect.
/// @return Current justification, or `VG_JUSTIFY_START` for a non-HBox handle.
vg_justify_t vg_hbox_get_justify(const vg_widget_t *hbox);

//=============================================================================
// Flex Layout API
//=============================================================================

/// @brief Create a Flexbox-style container widget with default settings.
///
/// @details Defaults to row direction, start alignment, start justification,
///          no gap, and no wrapping. Use the setter functions to customise.
///
/// @return Newly allocated Flex widget, or NULL if allocation fails.
vg_widget_t *vg_flex_create(void);

/// @brief Set the main-axis direction of a Flex container.
///
/// @param flex      The Flex widget to update.
/// @param direction The direction (row, row-reverse, column, column-reverse).
void vg_flex_set_direction(vg_widget_t *flex, vg_direction_t direction);

/// @brief Set the default cross-axis alignment for items in a Flex container.
///
/// @param flex  The Flex widget to update.
/// @param align The alignment to apply to each child on the cross axis.
void vg_flex_set_align_items(vg_widget_t *flex, vg_align_t align);

/// @brief Set the main-axis distribution of children in a Flex container.
///
/// @param flex    The Flex widget to update.
/// @param justify The justification strategy for the main axis.
void vg_flex_set_justify_content(vg_widget_t *flex, vg_justify_t justify);

/// @brief Set the uniform gap between items in a Flex container.
///
/// @param flex The Flex widget to update.
/// @param gap  Gap in pixels between adjacent items.
void vg_flex_set_gap(vg_widget_t *flex, float gap);

/// @brief Set the primary-axis spacing/gap for a supported layout container.
/// @details Applies to VBox, HBox, and Flex containers. Other widgets are a
///          safe no-op.
/// @param container VBox, HBox, or Flex container to update.
/// @param spacing New primary-axis gap in pixels.
void vg_container_set_spacing(vg_widget_t *container, float spacing);

/// @brief Enable or disable line wrapping in a Flex container.
///
/// @details When wrapping is enabled, children that exceed the main-axis
///          extent are moved to the next line.
///
/// @param flex The Flex widget to update.
/// @param wrap true to enable wrapping, false to keep all items on one line.
void vg_flex_set_wrap(vg_widget_t *flex, bool wrap);

/// @brief Set the complete Flex line-wrapping policy.
/// @details Unlike the compatibility boolean setter, this operation can select
///          reverse cross-axis line placement. Invalid values become no-wrap.
/// @param flex Flex widget to update.
/// @param wrap Wrapping policy.
void vg_flex_set_wrap_mode(vg_widget_t *flex, vg_flex_wrap_t wrap);

/// @brief Identify the concrete layout algorithm owned by a widget.
/// @details The query compares the layout vtable and never interprets opaque
///          implementation data, making it safe for runtime handle validation.
/// @param widget Live widget to inspect; may be NULL.
/// @return Concrete layout kind, or `VG_LAYOUT_NONE` for an ordinary widget.
vg_layout_type_t vg_layout_get_type(const vg_widget_t *widget);

//=============================================================================
// Grid Layout API
//=============================================================================

/// @brief Create a grid container with the specified number of columns and rows.
///
/// @param columns Number of columns (must be >= 1).
/// @param rows    Number of rows (must be >= 1).
/// @return Newly allocated Grid widget, or NULL if allocation fails.
vg_widget_t *vg_grid_create(int columns, int rows);

/// @brief Change the number of columns in a Grid container.
///
/// @param grid    The Grid widget to update.
/// @param columns New column count.
void vg_grid_set_columns(vg_widget_t *grid, int columns);

/// @brief Change the number of rows in a Grid container.
///
/// @param grid The Grid widget to update.
/// @param rows New row count.
void vg_grid_set_rows(vg_widget_t *grid, int rows);

/// @brief Set the gap between columns and rows in a Grid container.
///
/// @param grid       The Grid widget to update.
/// @param column_gap Horizontal gap between columns in pixels.
/// @param row_gap    Vertical gap between rows in pixels.
void vg_grid_set_gap(vg_widget_t *grid, float column_gap, float row_gap);

/// @brief Override the width of a specific column in a Grid container.
///
/// @details If no column widths have been set, all columns share space
///          equally. Calling this for any column allocates a width array and
///          sets that column's width; unset columns remain auto-sized.
///
/// @param grid   The Grid widget to update.
/// @param column Zero-based column index.
/// @param width  Track definition: positive fixed pixels, zero auto/content,
///               or a negative fractional weight.
void vg_grid_set_column_width(vg_widget_t *grid, int column, float width);

/// @brief Override the height of a specific row in a Grid container.
///
/// @param grid   The Grid widget to update.
/// @param row    Zero-based row index.
/// @param height Track definition: positive fixed pixels, zero auto/content,
///               or a negative fractional weight.
void vg_grid_set_row_height(vg_widget_t *grid, int row, float height);

/// @brief Place a child widget in a specific cell (or span of cells) in the grid.
///
/// @details The child must already be a child of the grid container. This
///          function records the cell placement metadata so that the grid
///          layout algorithm knows where to position the child.
///
/// @param grid     The Grid widget.
/// @param child    The child widget to place.
/// @param column   Starting column index (zero-based).
/// @param row      Starting row index (zero-based).
/// @param col_span Number of columns the child spans (>= 1).
/// @param row_span Number of rows the child spans (>= 1).
void vg_grid_place(
    vg_widget_t *grid, vg_widget_t *child, int column, int row, int col_span, int row_span);

/// @brief Validate and record one direct child's grid placement atomically.
/// @details The child must already be a direct child of the grid. Negative or
///          out-of-range indices, non-positive spans, and allocation failure
///          leave existing placement metadata unchanged.
/// @param grid Grid layout container.
/// @param child Existing direct child of @p grid.
/// @param column Zero-based starting column.
/// @param row Zero-based starting row.
/// @param col_span Positive number of columns to span.
/// @param row_span Positive number of rows to span.
/// @return true when placement metadata was committed, otherwise false.
bool vg_grid_place_checked(
    vg_widget_t *grid, vg_widget_t *child, int column, int row, int col_span, int row_span);

//=============================================================================
// Dock Layout API
//=============================================================================

/// @brief Create a Dock layout container.
///
/// @details In a dock layout, each child claims space from an edge of the
///          remaining area. Children docked to left/right claim horizontal
///          bands, top/bottom claim vertical bands, and fill claims whatever
///          is left. This is the layout strategy used for IDE panel arrangements.
///
/// @return Newly allocated Dock widget, or NULL if allocation fails.
vg_widget_t *vg_dock_create(void);

/// @brief Add a child to a Dock container at the specified docking position.
///
/// @param dock     The Dock widget.
/// @param child    The child widget to add.
/// @param position The edge to dock the child to (left, top, right, bottom, or fill).
void vg_dock_add(vg_widget_t *dock, vg_widget_t *child, vg_dock_t position);

/// @brief Add or update a child in a Dock container with status reporting.
/// @details A detached child is attached after registry capacity is secured. A
///          child owned by another parent, an invalid dock position, or an
///          allocation failure leaves both trees unchanged.
/// @param dock Dock layout container.
/// @param child Detached widget or existing direct child of @p dock.
/// @param position Left, top, right, bottom, or fill.
/// @return true when the dock assignment was committed, otherwise false.
bool vg_dock_add_checked(vg_widget_t *dock, vg_widget_t *child, vg_dock_t position);

/// @brief Set the physical gap reserved between consecutive docked regions.
/// @param dock Dock layout container.
/// @param gap Non-negative gap in pixels; invalid values become zero.
void vg_dock_set_gap(vg_widget_t *dock, float gap);

//=============================================================================
// Layout Engine Functions (internal use)
//=============================================================================

/// @brief Execute the VBox layout algorithm on a container's children.
///
/// @param container The VBox container widget.
/// @param width     Available width for the container.
/// @param height    Available height for the container.
void vg_layout_vbox(vg_widget_t *container, float width, float height);

/// @brief Execute the HBox layout algorithm on a container's children.
///
/// @param container The HBox container widget.
/// @param width     Available width for the container.
/// @param height    Available height for the container.
void vg_layout_hbox(vg_widget_t *container, float width, float height);

/// @brief Execute the Flex layout algorithm on a container's children.
///
/// @param container The Flex container widget.
/// @param width     Available width for the container.
/// @param height    Available height for the container.
void vg_layout_flex(vg_widget_t *container, float width, float height);

/// @brief Execute the Grid layout algorithm on a container's children.
///
/// @param container The Grid container widget.
/// @param width     Available width for the container.
/// @param height    Available height for the container.
void vg_layout_grid(vg_widget_t *container, float width, float height);

/// @brief Execute the Dock layout algorithm on a container's children.
///
/// @param container The Dock container widget.
/// @param width     Available width for the container.
/// @param height    Available height for the container.
void vg_layout_dock(vg_widget_t *container, float width, float height);

/// @brief Internal hook used by the widget tree when a child is detached.
///
/// @details Layout containers that keep side tables keyed by child pointer
///          must clear those entries when the generic widget hierarchy removes
///          the child. Application code should not call this directly.
void vg_layout_on_child_detached(vg_widget_t *parent, vg_widget_t *child);

#ifdef __cplusplus
}
#endif
