//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Drawing Primitives
//
// Implements classical raster graphics algorithms for drawing lines, circles,
// and rectangles. Core raster loops use integer arithmetic for deterministic,
// portable behavior; bounded line clipping computes intersections before the
// integer Bresenham walk. Clipping handles partially visible shapes gracefully.
//
// Algorithms Implemented:
//   - Bresenham's Line Algorithm (1965): Integer-only line rasterization
//   - Midpoint Circle Algorithm (1977): 8-way symmetric circle outline
//   - Scanline Fill: Horizontal line-based filling for rectangles and circles
//
// All drawing operations are bounds-checked.  Pixels outside the window are
// silently discarded (no error generated).  This allows drawing shapes that
// extend beyond the viewport without special-casing.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Drawing primitive implementations using classical raster algorithms.
/// @details Provides Bresenham line drawing, midpoint circle rendering, and
///          rectangle operations. Rasterization uses widened integer arithmetic;
///          a bounded Cohen-Sutherland-style preclip computes line intersections
///          before the integer pixel walk. Every primitive honors the effective
///          framebuffer and compositor clip.

#include "vgfx.h"
#include "vgfx_internal.h"
#include <limits.h>
#include <stdlib.h> /* abs */
#include <string.h>

/// @brief Compute the magnitude of a widened int32-coordinate delta.
/// @param value Difference derived from two signed-32 coordinates.
/// @return Nonnegative magnitude, which is representable for that input domain.
static int64_t vgfx_abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

/// @brief Saturate a widened coordinate to the signed-32 drawing domain.
/// @param value Candidate coordinate.
/// @return @p value clamped to INT32_MIN through INT32_MAX.
static int32_t clamp_i64_to_i32(int64_t value) {
    if (value > INT32_MAX)
        return INT32_MAX;
    if (value < INT32_MIN)
        return INT32_MIN;
    return (int32_t)value;
}

/// @brief Resolve the nonempty intersection of framebuffer and active clip bounds.
/// @param win Window whose physical drawing region is queried.
/// @param min_x Receives inclusive left bound.
/// @param min_y Receives inclusive top bound.
/// @param max_x Receives exclusive right bound.
/// @param max_y Receives exclusive bottom bound.
/// @return 1 when a drawable region exists; otherwise 0.
static int get_effective_clip_bounds(
    const struct vgfx_window *win, int64_t *min_x, int64_t *min_y, int64_t *max_x, int64_t *max_y) {
    if (!win || !min_x || !min_y || !max_x || !max_y || win->width <= 0 || win->height <= 0)
        return 0;

    int64_t left = 0;
    int64_t top = 0;
    int64_t right = win->width;
    int64_t bottom = win->height;

    if (win->clip_enabled) {
        if (win->clip_w <= 0 || win->clip_h <= 0)
            return 0;
        int64_t clip_left = win->clip_x;
        int64_t clip_top = win->clip_y;
        int64_t clip_right = (int64_t)win->clip_x + (int64_t)win->clip_w;
        int64_t clip_bottom = (int64_t)win->clip_y + (int64_t)win->clip_h;
        if (clip_left > left)
            left = clip_left;
        if (clip_top > top)
            top = clip_top;
        if (clip_right < right)
            right = clip_right;
        if (clip_bottom < bottom)
            bottom = clip_bottom;
    }

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > win->width)
        right = win->width;
    if (bottom > win->height)
        bottom = win->height;
    if (left >= right || top >= bottom)
        return 0;

    *min_x = left;
    *min_y = top;
    *max_x = right;
    *max_y = bottom;
    return 1;
}

/// @brief Test whether a circle can touch the current effective clip bounds.
/// @details Uses int64 arithmetic so large coordinates and radii cannot
///          overflow while computing the bounding box. Extremely large radii
///          are rejected to avoid unbounded midpoint loops.
/// @param cx Circle center X coordinate.
/// @param cy Circle center Y coordinate.
/// @param radius Nonnegative candidate radius.
/// @param min_x Inclusive clip left edge.
/// @param min_y Inclusive clip top edge.
/// @param max_x Exclusive clip right edge.
/// @param max_y Exclusive clip bottom edge.
/// @return 1 when the circle bounding box intersects the clip; otherwise 0.
static int circle_intersects_clip_bounds(int32_t cx,
                                         int32_t cy,
                                         int32_t radius,
                                         int64_t min_x,
                                         int64_t min_y,
                                         int64_t max_x,
                                         int64_t max_y) {
    if (radius < 0 || radius > INT32_MAX / 4)
        return 0;
    int64_t r = radius;
    int64_t left = (int64_t)cx - r;
    int64_t right = (int64_t)cx + r;
    int64_t top = (int64_t)cy - r;
    int64_t bottom = (int64_t)cy + r;
    return right >= min_x && bottom >= min_y && left < max_x && top < max_y;
}

/// @brief Canonicalize a window to an explicitly enabled empty clip.
/// @param win Valid window whose future drawing is suppressed.
static void set_empty_clip(struct vgfx_window *win) {
    win->clip_x = 0;
    win->clip_y = 0;
    win->clip_w = 0;
    win->clip_h = 0;
    win->clip_enabled = 1;
}

/// @brief Intersect the current effective clip with one physical-pixel rectangle.
/// @details Both inputs are already canonicalized against the framebuffer. Empty inputs preserve
///          an empty current clip. All edge arithmetic uses 64 bits to avoid signed overflow.
/// @param win Window whose current clip is narrowed.
/// @param x Physical-pixel left edge of the limiting rectangle.
/// @param y Physical-pixel top edge of the limiting rectangle.
/// @param w Non-negative physical-pixel width.
/// @param h Non-negative physical-pixel height.
static void intersect_current_clip(
    struct vgfx_window *win, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!win || !win->clip_enabled || win->clip_w <= 0 || win->clip_h <= 0 || w <= 0 || h <= 0) {
        if (win)
            set_empty_clip(win);
        return;
    }
    int64_t left = win->clip_x > x ? win->clip_x : x;
    int64_t top = win->clip_y > y ? win->clip_y : y;
    int64_t current_right = (int64_t)win->clip_x + win->clip_w;
    int64_t current_bottom = (int64_t)win->clip_y + win->clip_h;
    int64_t limit_right = (int64_t)x + w;
    int64_t limit_bottom = (int64_t)y + h;
    int64_t right = current_right < limit_right ? current_right : limit_right;
    int64_t bottom = current_bottom < limit_bottom ? current_bottom : limit_bottom;
    if (left >= right || top >= bottom) {
        set_empty_clip(win);
        return;
    }
    win->clip_x = (int32_t)left;
    win->clip_y = (int32_t)top;
    win->clip_w = (int32_t)(right - left);
    win->clip_h = (int32_t)(bottom - top);
    win->clip_enabled = 1;
}

//===----------------------------------------------------------------------===//
// Context Structures for Algorithm Callbacks
//===----------------------------------------------------------------------===//
// These structures carry state through the generic algorithm implementations
// to the pixel-plotting callbacks.  Avoids global state and enables
// multiple concurrent drawing operations.
//===----------------------------------------------------------------------===//

/// @brief Context for single-pixel plotting callbacks.
/// @details Passed to plot_callback() by the Bresenham line and midpoint
///          circle algorithms.  Contains window handle and color.
typedef struct {
    struct vgfx_window *win; ///< Target window for drawing
    int64_t min_x;           ///< Inclusive clipped minimum X.
    int64_t min_y;           ///< Inclusive clipped minimum Y.
    int64_t max_x;           ///< Exclusive clipped maximum X.
    int64_t max_y;           ///< Exclusive clipped maximum Y.
    uint32_t rgba_word;      ///< Host word whose object bytes are RGBA.
} plot_context_t;

/// @brief Context for horizontal line drawing callbacks.
/// @details Passed to hline_callback() by the filled circle algorithm.
///          Contains window handle and color for scanline fills.
typedef struct {
    struct vgfx_window *win; ///< Target window for drawing
    int64_t min_x;           ///< Inclusive clipped minimum X.
    int64_t min_y;           ///< Inclusive clipped minimum Y.
    int64_t max_x;           ///< Exclusive clipped maximum X.
    int64_t max_y;           ///< Exclusive clipped maximum Y.
    uint32_t rgba_word;      ///< Host word whose object bytes are RGBA.
} hline_context_t;

/// @brief Build a host-endian word whose object representation is RGBA bytes.
/// @details Framebuffer pixels are byte-addressed RGBA.  Initializing the word
///          via `memcpy` preserves those bytes on both little- and big-endian
///          targets while letting hot drawing loops use packed stores.
/// @param color RGB color in 0x00RRGGBB format.
/// @return A uint32_t whose memory bytes are R,G,B,0xFF.
static uint32_t draw_rgba_word(vgfx_color_t color) {
    uint8_t bytes[4] = {(uint8_t)((color >> 16) & 0xFF),
                        (uint8_t)((color >> 8) & 0xFF),
                        (uint8_t)(color & 0xFF),
                        0xFF};
    uint32_t word = 0;
    memcpy(&word, bytes, sizeof(word));
    return word;
}

/// @brief Store one packed RGBA pixel in a framebuffer.
/// @details Uses `memcpy` rather than a `uint32_t *` cast because the
///          framebuffer API exposes byte storage and callers may not satisfy the
///          aliasing or alignment requirements of wider integer stores.
/// @param win Target window with an RGBA framebuffer.
/// @param x Pixel X coordinate, already known to be in bounds.
/// @param y Pixel Y coordinate, already known to be in bounds.
/// @param rgba_word Host word whose object bytes are R,G,B,A.
static inline void store_rgba_word(struct vgfx_window *win,
                                   int32_t x,
                                   int32_t y,
                                   uint32_t rgba_word) {
    uint8_t *pixel = win->pixels + ((size_t)y * (size_t)win->stride) + ((size_t)x * 4u);
    memcpy(pixel, &rgba_word, sizeof(rgba_word));
}

/// @brief Fill a clipped horizontal span with a packed RGBA pixel.
/// @details Writes each packed pixel through `memcpy` to preserve the
///          framebuffer's byte-addressed contract on strict-alignment targets.
/// @param win Target window with an RGBA framebuffer.
/// @param x First pixel X coordinate.
/// @param y Pixel row.
/// @param count Number of pixels to write.
/// @param rgba_word Host word whose object bytes are R,G,B,A.
static void fill_rgba_span(
    struct vgfx_window *win, int32_t x, int32_t y, int32_t count, uint32_t rgba_word) {
    uint8_t *dst = win->pixels + ((size_t)y * (size_t)win->stride) + ((size_t)x * 4u);
    if (count <= 0)
        return;
    memcpy(dst, &rgba_word, sizeof(rgba_word));
    size_t filled = 1;
    size_t total = (size_t)count;
    while (filled < total) {
        size_t copy = filled;
        if (copy > total - filled)
            copy = total - filled;
        memcpy(dst + filled * 4u, dst, copy * 4u);
        filled += copy;
    }
}

//===----------------------------------------------------------------------===//
// Algorithm Callbacks
//===----------------------------------------------------------------------===//
// Generic callbacks that bridge the abstract algorithms (which don't know
// about windows or colors) to the concrete framebuffer operations.
//===----------------------------------------------------------------------===//

/// @brief Callback for single-pixel plotting (used by line and circle algorithms).
/// @details Extracts window and color from the context and plots a pixel.
///          Used as the plot callback for bresenham_line() and midpoint_circle().
///
/// @param x   X coordinate in pixels
/// @param y   Y coordinate in pixels
/// @param ctx Opaque context pointer (actually plot_context_t*)
///
/// @pre  ctx points to a valid plot_context_t structure
static void plot_callback(int32_t x, int32_t y, void *ctx) {
    plot_context_t *pctx = (plot_context_t *)ctx;
    if ((int64_t)x < pctx->min_x || (int64_t)x >= pctx->max_x || (int64_t)y < pctx->min_y ||
        (int64_t)y >= pctx->max_y) {
        return;
    }
    store_rgba_word(pctx->win, x, y, pctx->rgba_word);
}

/// @brief Callback for horizontal line drawing (used by filled circle).
/// @details Draws a horizontal scanline from x0 to x1 at row y, with full
///          bounds checking and clipping.  Used by filled_circle() to fill
///          the interior of the circle.
///
/// @param x0  Start X coordinate (inclusive)
/// @param x1  End X coordinate (inclusive)
/// @param y   Y coordinate (row number)
/// @param ctx Opaque context pointer (actually hline_context_t*)
///
/// @pre  ctx points to a valid hline_context_t structure
/// @post Horizontal line segment drawn from max(x0, 0) to min(x1, width-1) at y
static void hline_callback(int32_t x0, int32_t x1, int32_t y, void *ctx) {
    hline_context_t *hctx = (hline_context_t *)ctx;
    struct vgfx_window *win = hctx->win;
    /* Bounds check Y coordinate (reject entire scanline if out of clip bounds) */
    if ((int64_t)y < hctx->min_y || (int64_t)y >= hctx->max_y)
        return;

    /* Ensure x0 <= x1 (swap if needed) */
    if (x0 > x1) {
        int32_t tmp = x0;
        x0 = x1;
        x1 = tmp;
    }

    /* Clip X coordinates to clip bounds */
    if ((int64_t)x0 < hctx->min_x)
        x0 = (int32_t)hctx->min_x;
    if ((int64_t)x1 >= hctx->max_x)
        x1 = (int32_t)(hctx->max_x - 1);
    if (x0 > x1)
        return; /* Scanline entirely clipped */
    fill_rgba_span(win, x0, y, x1 - x0 + 1, hctx->rgba_word);
}

//===----------------------------------------------------------------------===//
// Bresenham Line Algorithm
//===----------------------------------------------------------------------===//
// Classic integer-only line rasterization.  Handles all octants correctly
// without floating point arithmetic, division, or multiplication in the
// inner loop.  Only uses addition, subtraction, and bit shifts.
//
// Reference: Bresenham, J. E. (1965). "Algorithm for computer control
//            of a digital plotter". IBM Systems Journal, 4(1), 25-30.
//===----------------------------------------------------------------------===//

/// @brief Draw a line from (x0, y0) to (x1, y1) using Bresenham's algorithm.
/// @details Integer-only line rasterization that works correctly in all octants.
///          Uses an error accumulator to decide when to step in the minor axis
///          direction.  The algorithm is symmetric: swapping endpoints produces
///          the same pixels in reverse order.
///
///          Key properties:
///            - No floating point: uses only int32_t arithmetic
///            - No division or multiplication in inner loop
///            - Handles all 8 octants (dx > dy, dy > dx, positive/negative slopes)
///            - Each pixel is plotted exactly once
///
/// @param x0   Start X coordinate
/// @param y0   Start Y coordinate
/// @param x1   End X coordinate
/// @param y1   End Y coordinate
/// @param plot Callback function invoked for each pixel (x, y)
/// @param ctx  Opaque context passed to plot callback
///
/// @pre  plot != NULL
/// @post plot() called for each pixel along the line from (x0, y0) to (x1, y1)
static void bresenham_line(int32_t x0,
                           int32_t y0,
                           int32_t x1,
                           int32_t y1,
                           void (*plot)(int32_t x, int32_t y, void *ctx),
                           void *ctx) {
    /* Calculate absolute deltas (line extents) in widened math. */
    int64_t dx = vgfx_abs_i64((int64_t)x1 - (int64_t)x0);
    int64_t dy = vgfx_abs_i64((int64_t)y1 - (int64_t)y0);

    /* Determine step direction for each axis (-1 or +1) */
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;

    /* Initialize error term (decision parameter)
     * err represents 2 * accumulated error scaled by dx and dy
     * When err crosses zero, step in the minor axis */
    int64_t err = dx - dy;

    /* Current position (start at beginning of line) */
    int32_t x = x0;
    int32_t y = y0;

    /* Iterate until we reach the endpoint (inclusive) */
    while (1) {
        /* Plot current pixel */
        plot(x, y, ctx);

        /* Check if we've reached the endpoint */
        if (x == x1 && y == y1)
            break;

        /* Calculate 2 * error for comparison (avoids division) */
        int64_t e2 = 2 * err;

        /* Step in X direction if error threshold exceeded */
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }

        /* Step in Y direction if error threshold exceeded */
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

enum {
    CLIP_LEFT = 1,
    CLIP_RIGHT = 2,
    CLIP_TOP = 4,
    CLIP_BOTTOM = 8,
};

/// @brief Classify a point against inclusive line-clipping bounds.
/// @param x Point X coordinate.
/// @param y Point Y coordinate.
/// @param min_x Inclusive left edge.
/// @param min_y Inclusive top edge.
/// @param max_x Inclusive right edge.
/// @param max_y Inclusive bottom edge.
/// @return Bitwise combination of CLIP_LEFT, CLIP_RIGHT, CLIP_TOP, and CLIP_BOTTOM.
static int line_outcode(
    int64_t x, int64_t y, int64_t min_x, int64_t min_y, int64_t max_x, int64_t max_y) {
    int code = 0;
    if (x < min_x)
        code |= CLIP_LEFT;
    else if (x > max_x)
        code |= CLIP_RIGHT;
    if (y < min_y)
        code |= CLIP_TOP;
    else if (y > max_y)
        code |= CLIP_BOTTOM;
    return code;
}

/// @brief Round a finite clipping intersection to the nearest integer coordinate.
/// @param value Finite value within the signed-64 coordinate working range.
/// @return Nearest integer, with half values rounded away from zero.
static int64_t round_double_to_i64(double value) {
    return (int64_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

/// @brief Clip a segment to a nonempty rectangle before rasterization.
/// @details Uses iterative outcodes and double-precision edge intersections,
///          writing inclusive endpoint coordinates suitable for Bresenham.
/// @param x0 In/out first endpoint X.
/// @param y0 In/out first endpoint Y.
/// @param x1 In/out second endpoint X.
/// @param y1 In/out second endpoint Y.
/// @param min_x Inclusive left bound.
/// @param min_y Inclusive top bound.
/// @param max_exclusive_x Exclusive right bound.
/// @param max_exclusive_y Exclusive bottom bound.
/// @return 1 when a visible segment remains; 0 when fully outside.
static int clip_line_to_bounds(int64_t *x0,
                               int64_t *y0,
                               int64_t *x1,
                               int64_t *y1,
                               int64_t min_x,
                               int64_t min_y,
                               int64_t max_exclusive_x,
                               int64_t max_exclusive_y) {
    int64_t max_x = max_exclusive_x - 1;
    int64_t max_y = max_exclusive_y - 1;
    int out0 = line_outcode(*x0, *y0, min_x, min_y, max_x, max_y);
    int out1 = line_outcode(*x1, *y1, min_x, min_y, max_x, max_y);

    while (1) {
        if ((out0 | out1) == 0)
            return 1;
        if ((out0 & out1) != 0)
            return 0;

        int out = out0 ? out0 : out1;
        int64_t x = 0;
        int64_t y = 0;

        if (out & CLIP_TOP) {
            if (*y1 == *y0)
                return 0;
            x = round_double_to_i64((double)*x0 + (double)(*x1 - *x0) * (double)(min_y - *y0) /
                                                      (double)(*y1 - *y0));
            y = min_y;
        } else if (out & CLIP_BOTTOM) {
            if (*y1 == *y0)
                return 0;
            x = round_double_to_i64((double)*x0 + (double)(*x1 - *x0) * (double)(max_y - *y0) /
                                                      (double)(*y1 - *y0));
            y = max_y;
        } else if (out & CLIP_RIGHT) {
            if (*x1 == *x0)
                return 0;
            y = round_double_to_i64((double)*y0 + (double)(*y1 - *y0) * (double)(max_x - *x0) /
                                                      (double)(*x1 - *x0));
            x = max_x;
        } else {
            if (*x1 == *x0)
                return 0;
            y = round_double_to_i64((double)*y0 + (double)(*y1 - *y0) * (double)(min_x - *x0) /
                                                      (double)(*x1 - *x0));
            x = min_x;
        }

        if (out == out0) {
            *x0 = x;
            *y0 = y;
            out0 = line_outcode(*x0, *y0, min_x, min_y, max_x, max_y);
        } else {
            *x1 = x;
            *y1 = y;
            out1 = line_outcode(*x1, *y1, min_x, min_y, max_x, max_y);
        }
    }
}

//===----------------------------------------------------------------------===//
// Midpoint Circle Algorithm (Outline)
//===----------------------------------------------------------------------===//
// Integer-only circle rasterization exploiting 8-way symmetry.  Computes
// one octant and reflects it to the other 7 octants.  Uses a decision
// parameter to determine whether the midpoint is inside or outside the
// ideal circle.
//
// Reference: Bresenham, J. E. (1977). "A linear algorithm for incremental
//            digital display of circular arcs". Communications of the ACM.
//===----------------------------------------------------------------------===//

/// @brief Invoke a pixel callback only when widened coordinates fit its signed-32 ABI.
/// @param x Candidate X coordinate.
/// @param y Candidate Y coordinate.
/// @param plot Pixel callback.
/// @param ctx Opaque callback state.
static void plot_checked_i64(int64_t x,
                             int64_t y,
                             void (*plot)(int32_t x, int32_t y, void *ctx),
                             void *ctx) {
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
        return;
    plot((int32_t)x, (int32_t)y, ctx);
}

/// @brief Clamp a widened horizontal span to the callback's signed-32 ABI.
/// @param x0 First candidate endpoint.
/// @param x1 Second candidate endpoint.
/// @param y Candidate row.
/// @param hline Horizontal-span callback.
/// @param ctx Opaque callback state.
static void hline_checked_i64(int64_t x0,
                              int64_t x1,
                              int64_t y,
                              void (*hline)(int32_t x0, int32_t x1, int32_t y, void *ctx),
                              void *ctx) {
    if (y < INT32_MIN || y > INT32_MAX)
        return;
    if (x0 > x1) {
        int64_t tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    if (x1 < INT32_MIN || x0 > INT32_MAX)
        return;
    if (x0 < INT32_MIN)
        x0 = INT32_MIN;
    if (x1 > INT32_MAX)
        x1 = INT32_MAX;
    hline((int32_t)x0, (int32_t)x1, (int32_t)y, ctx);
}

/// @brief Draw a circle outline using the midpoint circle algorithm.
/// @details Computes one octant with a widened integer decision parameter and
///          reflects each point through eight-way symmetry. Radius zero emits
///          the center once; a negative radius emits nothing.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Circle radius in pixels.
/// @param plot Callback invoked for each representable perimeter pixel.
/// @param ctx Opaque callback state.
static void midpoint_circle(int32_t cx,
                            int32_t cy,
                            int32_t radius,
                            void (*plot)(int32_t x, int32_t y, void *ctx),
                            void *ctx) {
    /* Special case: radius 0 is just a single point at the center */
    if (radius == 0) {
        plot(cx, cy, ctx);
        return;
    }

    /* Negative radius is invalid (reject without drawing) */
    if (radius < 0)
        return;

    /* Initial position in first octant: (0, radius)
     * This corresponds to the topmost point of the circle */
    int64_t x = 0;
    int64_t y = radius;

    /* Initial decision parameter: d = 1 - radius
     * This determines whether the midpoint between (x+1, y) and (x+1, y-1)
     * is inside or outside the ideal circle */
    int64_t d = 1 - (int64_t)radius;

    /* Plot initial 8 symmetric points (all octants at x=0)
     * Each octant has a reflection about the X axis, Y axis, and Y=X line */
    plot_checked_i64((int64_t)cx + x, (int64_t)cy + y, plot, ctx);
    plot_checked_i64((int64_t)cx - x, (int64_t)cy + y, plot, ctx);
    plot_checked_i64((int64_t)cx + x, (int64_t)cy - y, plot, ctx);
    plot_checked_i64((int64_t)cx - x, (int64_t)cy - y, plot, ctx);
    plot_checked_i64((int64_t)cx + y, (int64_t)cy + x, plot, ctx);
    plot_checked_i64((int64_t)cx - y, (int64_t)cy + x, plot, ctx);
    plot_checked_i64((int64_t)cx + y, (int64_t)cy - x, plot, ctx);
    plot_checked_i64((int64_t)cx - y, (int64_t)cy - x, plot, ctx);

    /* Iterate through first octant (while x < y, i.e., slope > -1) */
    while (x < y) {
        x++; /* Always step horizontally in first octant */

        /* Update decision parameter and Y coordinate based on midpoint test */
        if (d < 0) {
            /* Midpoint is inside circle: step horizontally only (x+1, y) */
            d += 2 * x + 1;
        } else {
            /* Midpoint is outside circle: step diagonally (x+1, y-1) */
            y--;
            d += 2 * (x - y) + 1;
        }

        /* Plot 8 symmetric points for this (x, y) */
        plot_checked_i64((int64_t)cx + x, (int64_t)cy + y, plot, ctx);
        plot_checked_i64((int64_t)cx - x, (int64_t)cy + y, plot, ctx);
        plot_checked_i64((int64_t)cx + x, (int64_t)cy - y, plot, ctx);
        plot_checked_i64((int64_t)cx - x, (int64_t)cy - y, plot, ctx);
        plot_checked_i64((int64_t)cx + y, (int64_t)cy + x, plot, ctx);
        plot_checked_i64((int64_t)cx - y, (int64_t)cy + x, plot, ctx);
        plot_checked_i64((int64_t)cx + y, (int64_t)cy - x, plot, ctx);
        plot_checked_i64((int64_t)cx - y, (int64_t)cy - x, plot, ctx);
    }
}

//===----------------------------------------------------------------------===//
// Filled Circle (Scanline Fill)
//===----------------------------------------------------------------------===//
// Derived from the midpoint circle algorithm.  Instead of plotting 8 points
// per iteration, draws 4 horizontal scanlines to fill the circle interior.
// This produces a solid filled circle with correct rounding at the edges.
//===----------------------------------------------------------------------===//

/// @brief Draw a filled circle using scanline fill derived from midpoint algorithm.
/// @details Uses the same decision logic as midpoint_circle(), but instead of
///          plotting 8 symmetric points, draws 4 horizontal scanlines spanning
///          the circle's interior.  Each iteration fills the current Y levels
///          in all four quadrants, producing a solid filled circle.
///
///          Key properties:
///            - No floating point: uses only int32_t arithmetic
///            - Exploits 4-way symmetry for horizontal lines
///            - Each row is filled exactly once (no overdraw)
///            - Handles clipping in hline_callback
///
/// @param cx     Center X coordinate
/// @param cy     Center Y coordinate
/// @param radius Circle radius in pixels (must be >= 0)
/// @param hline  Callback function invoked for each horizontal scanline
/// @param ctx    Opaque context passed to hline callback
///
/// @pre  hline != NULL
/// @pre  radius >= 0
/// @post hline() called for all horizontal scanlines filling the circle interior
static void filled_circle(int32_t cx,
                          int32_t cy,
                          int32_t radius,
                          void (*hline)(int32_t x0, int32_t x1, int32_t y, void *ctx),
                          void *ctx) {
    /* Special case: radius 0 is just a single point (degenerate scanline) */
    if (radius == 0) {
        hline(cx, cx, cy, ctx);
        return;
    }

    /* Negative radius is invalid (reject without drawing) */
    if (radius < 0)
        return;

    /* Initial position in first octant (same as outline algorithm) */
    int64_t x = 0;
    int64_t y = radius;
    int64_t d = 1 - (int64_t)radius;

    /* Fill initial horizontal lines (corresponds to initial 8-way symmetry)
     * These are the scanlines at y = cy ± radius and y = cy ± 0 */
    hline_checked_i64((int64_t)cx - x, (int64_t)cx + x, (int64_t)cy + y, hline, ctx);
    hline_checked_i64((int64_t)cx - x, (int64_t)cx + x, (int64_t)cy - y, hline, ctx);
    hline_checked_i64((int64_t)cx - y, (int64_t)cx + y, (int64_t)cy + x, hline, ctx);
    hline_checked_i64((int64_t)cx - y, (int64_t)cx + y, (int64_t)cy - x, hline, ctx);

    /* Iterate through first octant (while x < y) */
    while (x < y) {
        x++;

        /* Update decision parameter (same as outline algorithm) */
        if (d < 0) {
            d += 2 * x + 1;
        } else {
            y--;
            d += 2 * (x - y) + 1;
        }

        /* Fill 4 horizontal lines using 4-way symmetry
         * Two scanlines span the width at y = cy ± y (outer)
         * Two scanlines span the width at y = cy ± x (inner)
         * Together these fill the entire circle interior */
        hline_checked_i64((int64_t)cx - x, (int64_t)cx + x, (int64_t)cy + y, hline, ctx);
        hline_checked_i64((int64_t)cx - x, (int64_t)cx + x, (int64_t)cy - y, hline, ctx);
        hline_checked_i64((int64_t)cx - y, (int64_t)cx + y, (int64_t)cy + x, hline, ctx);
        hline_checked_i64((int64_t)cx - y, (int64_t)cx + y, (int64_t)cy - x, hline, ctx);
    }
}

//===----------------------------------------------------------------------===//
// Public Drawing Primitives
//===----------------------------------------------------------------------===//
// These are the entry points called from vgfx.c (forwarded from the public API).
// They set up the appropriate context structures and invoke the algorithm
// implementations above.
//===----------------------------------------------------------------------===//

/// @brief Draw a line from (x1, y1) to (x2, y2).
/// @details Public entry point for Bresenham line drawing.  Sets up the
///          plot context and invokes the Bresenham algorithm.  Pixels are
///          clipped to the window bounds.
///
/// @param window Window handle
/// @param x1     Start X coordinate
/// @param y1     Start Y coordinate
/// @param x2     End X coordinate
/// @param y2     End Y coordinate
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @pre  window is a valid struct vgfx_window*
/// @post Line pixels drawn from (x1, y1) to (x2, y2) in the framebuffer
void vgfx_draw_line(
    vgfx_window_t window, int32_t x1, int32_t y1, int32_t x2, int32_t y2, vgfx_color_t color) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    int64_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    if (!get_effective_clip_bounds(win, &min_x, &min_y, &max_x, &max_y))
        return;

    int64_t cx1 = x1, cy1 = y1, cx2 = x2, cy2 = y2;
    if (!clip_line_to_bounds(&cx1, &cy1, &cx2, &cy2, min_x, min_y, max_x, max_y))
        return;

    /* Set up plot context (passed to bresenham_line via plot_callback) */
    plot_context_t ctx = {.win = win,
                          .min_x = min_x,
                          .min_y = min_y,
                          .max_x = max_x,
                          .max_y = max_y,
                          .rgba_word = draw_rgba_word(color)};

    /* Draw line using Bresenham algorithm */
    bresenham_line((int32_t)cx1, (int32_t)cy1, (int32_t)cx2, (int32_t)cy2, plot_callback, &ctx);
}

/// @brief Draw a rectangle outline.
/// @details Draws the four edges of a rectangle using vgfx_draw_line().
///          The rectangle has top-left corner at (x, y) and dimensions w × h.
///          Zero or negative dimensions are silently rejected (no drawing).
///
/// @param window Window handle
/// @param x      Top-left X coordinate
/// @param y      Top-left Y coordinate
/// @param w      Width in pixels (must be > 0)
/// @param h      Height in pixels (must be > 0)
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @pre  window is a valid struct vgfx_window*
/// @post Rectangle outline drawn at (x, y) with dimensions w × h
void vgfx_draw_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color) {
    /* Trivial reject: zero or negative dimensions (invalid rectangle) */
    if (w <= 0 || h <= 0)
        return;

    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = (int64_t)x + (int64_t)w - 1;
    int64_t y1 = (int64_t)y + (int64_t)h - 1;

    /* Draw four edges of rectangle using line primitive
     * Top:    (x, y) to (x+w-1, y)
     * Bottom: (x, y+h-1) to (x+w-1, y+h-1)
     * Left:   (x, y) to (x, y+h-1)
     * Right:  (x+w-1, y) to (x+w-1, y+h-1)
     */
    vgfx_draw_line(window,
                   clamp_i64_to_i32(x0),
                   clamp_i64_to_i32(y0),
                   clamp_i64_to_i32(x1),
                   clamp_i64_to_i32(y0),
                   color);
    vgfx_draw_line(window,
                   clamp_i64_to_i32(x0),
                   clamp_i64_to_i32(y1),
                   clamp_i64_to_i32(x1),
                   clamp_i64_to_i32(y1),
                   color);
    vgfx_draw_line(window,
                   clamp_i64_to_i32(x0),
                   clamp_i64_to_i32(y0),
                   clamp_i64_to_i32(x0),
                   clamp_i64_to_i32(y1),
                   color);
    vgfx_draw_line(window,
                   clamp_i64_to_i32(x1),
                   clamp_i64_to_i32(y0),
                   clamp_i64_to_i32(x1),
                   clamp_i64_to_i32(y1),
                   color);
}

/// @brief Draw a filled rectangle.
/// @details Fills a rectangle with top-left corner at (x, y) and dimensions
///          w × h.  Uses optimized scanline filling (no overdraw).  The
///          rectangle is clipped to the window bounds before rendering.
///
/// @param window Window handle
/// @param x      Top-left X coordinate
/// @param y      Top-left Y coordinate
/// @param w      Width in pixels (must be > 0)
/// @param h      Height in pixels (must be > 0)
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @pre  window is a valid struct vgfx_window*
/// @post Rectangle filled at (x, y) with dimensions w × h (clipped to window)
void vgfx_draw_fill_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    /* Trivial reject: zero or negative dimensions (invalid rectangle) */
    if (w <= 0 || h <= 0)
        return;

    int64_t clip_min_x = 0, clip_min_y = 0, clip_max_x = 0, clip_max_y = 0;
    if (!get_effective_clip_bounds(win, &clip_min_x, &clip_min_y, &clip_max_x, &clip_max_y))
        return;

    /* Clip rectangle to clip bounds */
    int64_t rect_x2 = (int64_t)x + (int64_t)w;
    int64_t rect_y2 = (int64_t)y + (int64_t)h;
    int64_t clipped_x1 = ((int64_t)x < clip_min_x) ? clip_min_x : (int64_t)x;
    int64_t clipped_y1 = ((int64_t)y < clip_min_y) ? clip_min_y : (int64_t)y;
    int64_t clipped_x2 = (rect_x2 > clip_max_x) ? clip_max_x : rect_x2;
    int64_t clipped_y2 = (rect_y2 > clip_max_y) ? clip_max_y : rect_y2;

    /* Check if rectangle is completely out of bounds (no pixels to draw) */
    if (clipped_x1 >= clipped_x2 || clipped_y1 >= clipped_y2)
        return;

    int32_t x1 = (int32_t)clipped_x1;
    int32_t y1 = (int32_t)clipped_y1;
    int32_t x2 = (int32_t)clipped_x2;
    int32_t y2 = (int32_t)clipped_y2;

    uint32_t rgba_word = draw_rgba_word(color);

    /* Fill each scanline (row-by-row rendering) */
    for (int32_t row = y1; row < y2; row++) {
        fill_rgba_span(win, x1, row, x2 - x1, rgba_word);
    }
}

/// @brief Draw a circle outline.
/// @details Public entry point for midpoint circle drawing.  Sets up the
///          plot context and invokes the midpoint circle algorithm.  Pixels
///          are clipped to the window bounds.
///
/// @param window Window handle
/// @param cx     Center X coordinate
/// @param cy     Center Y coordinate
/// @param radius Radius in pixels (must be >= 0)
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @pre  window is a valid struct vgfx_window*
/// @post Circle outline drawn centered at (cx, cy) with given radius
void vgfx_draw_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    /* Trivial reject: negative radius (invalid circle) */
    if (radius < 0)
        return;

    int64_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    if (!get_effective_clip_bounds(win, &min_x, &min_y, &max_x, &max_y))
        return;
    if (!circle_intersects_clip_bounds(cx, cy, radius, min_x, min_y, max_x, max_y))
        return;

    /* Set up plot context (passed to midpoint_circle via plot_callback) */
    plot_context_t ctx = {.win = win,
                          .min_x = min_x,
                          .min_y = min_y,
                          .max_x = max_x,
                          .max_y = max_y,
                          .rgba_word = draw_rgba_word(color)};

    /* Draw circle outline using midpoint algorithm */
    midpoint_circle(cx, cy, radius, plot_callback, &ctx);
}

/// @brief Draw a filled circle.
/// @details Public entry point for filled circle drawing.  Sets up the
///          hline context and invokes the scanline fill algorithm.  Scanlines
///          are clipped to the window bounds.
///
/// @param window Window handle
/// @param cx     Center X coordinate
/// @param cy     Center Y coordinate
/// @param radius Radius in pixels (must be >= 0)
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @pre  window is a valid struct vgfx_window*
/// @post Filled circle drawn centered at (cx, cy) with given radius
void vgfx_draw_fill_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    /* Trivial reject: negative radius (invalid circle) */
    if (radius < 0)
        return;

    int64_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    if (!get_effective_clip_bounds(win, &min_x, &min_y, &max_x, &max_y))
        return;
    if (!circle_intersects_clip_bounds(cx, cy, radius, min_x, min_y, max_x, max_y))
        return;

    /* Set up horizontal line context (passed to filled_circle via hline_callback) */
    hline_context_t ctx = {.win = win,
                           .min_x = min_x,
                           .min_y = min_y,
                           .max_x = max_x,
                           .max_y = max_y,
                           .rgba_word = draw_rgba_word(color)};

    /* Fill circle using scanline algorithm */
    filled_circle(cx, cy, radius, hline_callback, &ctx);
}

//===----------------------------------------------------------------------===//
// Clipping Functions
//===----------------------------------------------------------------------===//

/// @brief Set the clipping rectangle for all drawing operations.
/// @details All subsequent drawing operations will be clipped to the specified
///          rectangle. The clip rectangle is intersected with the window bounds.
///
/// @param window Window handle
/// @param x      Left edge X coordinate of clip rect
/// @param y      Top edge Y coordinate of clip rect
/// @param w      Width of clip rect
/// @param h      Height of clip rect
///
/// @pre  window is a valid struct vgfx_window*
/// @post All drawing operations will be clipped to (x, y, w, h)
void vgfx_set_clip(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    if (w <= 0 || h <= 0) {
        set_empty_clip(win);
        return;
    }

    /* Scale clip rect to physical pixels when coord_scale is active */
    float cs = vgfx_internal_coord_scale(win);
    if (cs > 1.0f) {
        x = vgfx_internal_scale_up_i32(x, cs);
        y = vgfx_internal_scale_up_i32(y, cs);
        w = vgfx_internal_scale_up_i32(w, cs);
        h = vgfx_internal_scale_up_i32(h, cs);
    }

    if (w <= 0 || h <= 0) {
        set_empty_clip(win);
        return;
    }

    int64_t left = x;
    int64_t top = y;
    int64_t right = (int64_t)x + (int64_t)w;
    int64_t bottom = (int64_t)y + (int64_t)h;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > win->width)
        right = win->width;
    if (bottom > win->height)
        bottom = win->height;

    if (left >= right || top >= bottom) {
        set_empty_clip(win);
        return;
    }

    win->clip_x = (int32_t)left;
    win->clip_y = (int32_t)top;
    win->clip_w = (int32_t)(right - left);
    win->clip_h = (int32_t)(bottom - top);
    win->clip_enabled = 1;
    if (win->clip_limit_depth > 0) {
        const struct vgfx_clip_limit_frame *limit =
            &win->clip_limit_stack[win->clip_limit_depth - 1];
        intersect_current_clip(win, limit->limit_x, limit->limit_y, limit->limit_w, limit->limit_h);
    }
}

/// @brief Clear the clipping rectangle, restoring full-window drawing.
/// @details After calling this function, drawing operations can affect any
///          pixel within the window bounds.
///
/// @param window Window handle
///
/// @pre  window is a valid struct vgfx_window*
/// @post Clipping is disabled; drawing affects entire window
void vgfx_clear_clip(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;

    if (win->clip_limit_depth > 0) {
        const struct vgfx_clip_limit_frame *limit =
            &win->clip_limit_stack[win->clip_limit_depth - 1];
        win->clip_x = limit->limit_x;
        win->clip_y = limit->limit_y;
        win->clip_w = limit->limit_w;
        win->clip_h = limit->limit_h;
        win->clip_enabled = 1;
    } else {
        win->clip_enabled = 0;
    }
}

/// @brief Query the current effective framebuffer clipping rectangle.
/// @details The returned rectangle is expressed in physical framebuffer pixels.
///          Disabled clipping reports the full framebuffer and returns 0, while
///          active clipping reports the effective intersected clip and returns 1.
/// @param window Window handle.
/// @param out_x Optional destination for clip left edge.
/// @param out_y Optional destination for clip top edge.
/// @param out_w Optional destination for clip width.
/// @param out_h Optional destination for clip height.
/// @return 1 when explicit clipping is active, 0 otherwise.
int vgfx_get_clip(
    vgfx_window_t window, int32_t *out_x, int32_t *out_y, int32_t *out_w, int32_t *out_h) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win) {
        if (out_x)
            *out_x = 0;
        if (out_y)
            *out_y = 0;
        if (out_w)
            *out_w = 0;
        if (out_h)
            *out_h = 0;
        return 0;
    }

    int32_t x = 0;
    int32_t y = 0;
    int32_t w = win->width;
    int32_t h = win->height;
    int active = win->clip_enabled;
    if (active) {
        x = win->clip_x;
        y = win->clip_y;
        w = win->clip_w;
        h = win->clip_h;
    }

    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    return active ? 1 : 0;
}

/// @brief Establish one allocation-free clip ceiling scope.
/// @details See the public declaration for the contract. The requested rectangle is first
///          canonicalized by the ordinary clip implementation so coordinate scaling and window
///          clipping remain identical; that step also intersects against the innermost active
///          ceiling, so nested scopes compose. A pre-existing clip is then intersected into the
///          new ceiling.
/// @param window Window whose drawing state is constrained.
/// @param x Requested limit left edge in drawing coordinates.
/// @param y Requested limit top edge in drawing coordinates.
/// @param w Requested limit width.
/// @param h Requested limit height.
/// @return 1 when the scope was established; 0 for a NULL window or exhausted nesting depth.
int vgfx_push_clip_limit(vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win || win->clip_limit_depth >= VGFX_CLIP_LIMIT_MAX_DEPTH)
        return 0;

    struct vgfx_clip_limit_frame *frame = &win->clip_limit_stack[win->clip_limit_depth];
    frame->saved_enabled = win->clip_enabled;
    frame->saved_x = win->clip_x;
    frame->saved_y = win->clip_y;
    frame->saved_w = win->clip_w;
    frame->saved_h = win->clip_h;

    // vgfx_set_clip canonicalizes coordinates and intersects with the current
    // innermost ceiling (depth unchanged so far), making nested scopes compose.
    vgfx_set_clip(window, x, y, w, h);
    if (frame->saved_enabled) {
        intersect_current_clip(
            win, frame->saved_x, frame->saved_y, frame->saved_w, frame->saved_h);
    }
    frame->limit_x = win->clip_x;
    frame->limit_y = win->clip_y;
    frame->limit_w = win->clip_w;
    frame->limit_h = win->clip_h;
    win->clip_limit_depth++;
    return 1;
}

/// @brief End the innermost clip-limit scope and restore the clip captured by its push.
/// @param window Window whose innermost limit scope ends; NULL is ignored.
void vgfx_pop_clip_limit(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win || win->clip_limit_depth <= 0)
        return;
    win->clip_limit_depth--;
    const struct vgfx_clip_limit_frame *frame = &win->clip_limit_stack[win->clip_limit_depth];
    win->clip_enabled = frame->saved_enabled;
    win->clip_x = frame->saved_x;
    win->clip_y = frame->saved_y;
    win->clip_w = frame->saved_w;
    win->clip_h = frame->saved_h;
}

//===----------------------------------------------------------------------===//
// End of Drawing Primitives
//===----------------------------------------------------------------------===//
