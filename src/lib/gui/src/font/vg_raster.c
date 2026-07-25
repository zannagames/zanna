//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_raster.c
// Purpose: Glyph rasterisation engine — converts TTF quadratic-Bezier outlines
//          to antialiased 8-bit alpha bitmaps at arbitrary pixel sizes.
// Key invariants:
//   - OVERSAMPLE (4) vertical supersampling is used for coverage antialiasing.
//   - Polygon conversion flips y from TTF upward-positive to bitmap
//     downward-positive after outline_to_polygon returns.
//   - Maximum bitmap dimension is MAX_GLYPH_BITMAP_DIM (4096); inputs exceeding
//     this limit return NULL to avoid runaway allocation.
//   - Even-odd fill rule is used for scanline rasterisation.
// Ownership/Lifetime:
//   - vg_rasterize_glyph returns a heap-allocated vg_glyph_t (caller owns it).
//   - All internal working buffers are freed before return.
// Links: lib/gui/src/font/vg_ttf_internal.h,
//        lib/gui/src/font/vg_font.c,
//        lib/gui/src/font/vg_cache.c
//
//===----------------------------------------------------------------------===//
#include "vg_ttf_internal.h"
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements quadratic-outline flattening and antialiased glyph rasterization.
/// @details TrueType contours are transformed into bitmap coordinates, recursively flattened into
/// polylines, converted to a sorted edge list, and filled with vertically supersampled even-odd
/// scanlines. All working buffers are bounded and released before the result is returned.

//=============================================================================
// Constants
//=============================================================================

#define MAX_POINTS 16384
#define MAX_GLYPH_BITMAP_DIM 4096
#define MAX_CURVE_FLATTEN_DEPTH 32
#define CURVE_TOLERANCE 0.25f
#define OVERSAMPLE 4 // Supersampling factor for antialiasing

//=============================================================================
// Point and Edge Structures
//=============================================================================

/// @brief One two-dimensional vertex in the flattened glyph polygon.
typedef struct {
    float x, y;
} raster_point_t;

/// @brief One non-horizontal polygon edge prepared for scanline intersection.
typedef struct {
    float x0, y0, x1, y1;
    float dx; // Precomputed 1/(y1-y0) for scanline intersection
} raster_edge_t;

//=============================================================================
// Quadratic Bezier Flattening
//=============================================================================

/// @brief Recursively flatten a quadratic Bezier into polyline endpoints via
///        de Casteljau subdivision until within CURVE_TOLERANCE.
/// @param x0 Starting point x coordinate.
/// @param y0 Starting point y coordinate.
/// @param x1 Quadratic control-point x coordinate.
/// @param y1 Quadratic control-point y coordinate.
/// @param x2 Ending point x coordinate.
/// @param y2 Ending point y coordinate.
/// @param tolerance Maximum control-point distance from the endpoint chord.
/// @param out Destination polygon-point array.
/// @param max_points Capacity of @p out.
/// @param count Number of points already stored in @p out.
/// @param depth Current recursive subdivision depth.
/// @param truncated Optional flag set when a depth or output-capacity bound is reached.
/// @return Updated number of stored output points.
static int flatten_quadratic(float x0,
                             float y0,
                             float x1,
                             float y1,
                             float x2,
                             float y2,
                             float tolerance,
                             raster_point_t *out,
                             int max_points,
                             int count,
                             int depth,
                             int *truncated) {
    if (depth > MAX_CURVE_FLATTEN_DEPTH) {
        if (truncated)
            *truncated = 1;
        return count;
    }

    // Calculate flatness (distance from control point to line)
    float dx = x2 - x0;
    float dy = y2 - y0;
    float len_sq = dx * dx + dy * dy;

    if (len_sq < 0.0001f) {
        // Degenerate case - just add endpoint
        if (count < max_points) {
            out[count].x = x2;
            out[count].y = y2;
            return count + 1;
        }
        if (truncated)
            *truncated = 1;
        return count;
    }

    float d = fabsf((x1 - x0) * dy - (y1 - y0) * dx) / sqrtf(len_sq);

    if (d <= tolerance) {
        // Flat enough, output line segment endpoint
        if (count < max_points) {
            out[count].x = x2;
            out[count].y = y2;
            return count + 1;
        }
        if (truncated)
            *truncated = 1;
        return count;
    }

    // Subdivide at midpoint using de Casteljau's algorithm
    float x01 = (x0 + x1) * 0.5f;
    float y01 = (y0 + y1) * 0.5f;
    float x12 = (x1 + x2) * 0.5f;
    float y12 = (y1 + y2) * 0.5f;
    float x012 = (x01 + x12) * 0.5f;
    float y012 = (y01 + y12) * 0.5f;

    count = flatten_quadratic(
        x0, y0, x01, y01, x012, y012, tolerance, out, max_points, count, depth + 1, truncated);
    count = flatten_quadratic(
        x012, y012, x12, y12, x2, y2, tolerance, out, max_points, count, depth + 1, truncated);

    return count;
}

//=============================================================================
// Convert Glyph Outline to Polygon Points
//=============================================================================

/// @brief Convert a TTF glyph outline (on/off-curve points) to a flat polygon
///        by flattening quadratic Bezier curves into polyline segments.
/// @param points_x Source outline x coordinates in font units.
/// @param points_y Source outline y coordinates in font units.
/// @param flags Per-point on-curve flags.
/// @param contour_ends Inclusive source point index ending each contour.
/// @param num_contours Number of readable entries in @p contour_ends.
/// @param scale Font-unit-to-pixel scale.
/// @param offset_x Horizontal translation applied after scaling.
/// @param offset_y Vertical translation applied after scaling.
/// @param out Destination flattened point array.
/// @param max_points Capacity of @p out.
/// @param out_contour_ends Receives the inclusive final point index of each emitted contour.
/// @param out_contour_count Receives the number of emitted contours.
/// @return Number of output points, or `-1` when flattening exceeds a safety bound.
static int outline_to_polygon(float *points_x,
                              float *points_y,
                              uint8_t *flags,
                              int *contour_ends,
                              int num_contours,
                              float scale,
                              float offset_x,
                              float offset_y,
                              raster_point_t *out,
                              int max_points,
                              int *out_contour_ends,
                              int *out_contour_count) {
    int count = 0;
    int truncated = 0;
    if (out_contour_count)
        *out_contour_count = 0;

    for (int c = 0; c < num_contours; c++) {
        int contour_end = contour_ends[c];
        int contour_start = (c == 0) ? 0 : contour_ends[c - 1] + 1;
        int contour_len = contour_end - contour_start + 1;

        if (contour_len < 2) {
            continue;
        }

        int polygon_contour_start = count;

        // Process points in contour
        for (int i = 0; i < contour_len; i++) {
            int idx = contour_start + i;
            int next_idx = contour_start + ((i + 1) % contour_len);

            float x0 = points_x[idx] * scale + offset_x;
            float y0 = points_y[idx] * scale + offset_y;
            float x1 = points_x[next_idx] * scale + offset_x;
            float y1 = points_y[next_idx] * scale + offset_y;

            bool on0 = flags[idx];
            bool on1 = flags[next_idx];

            if (on0 && on1) {
                // Line segment
                if (count < max_points) {
                    out[count].x = x0;
                    out[count].y = y0;
                    count++;
                } else {
                    truncated = 1;
                }
            } else if (on0 && !on1) {
                // Current on-curve, next is control point
                // Find the end point
                int end_idx = contour_start + ((i + 2) % contour_len);
                float x2, y2;

                if (flags[end_idx]) {
                    // End point is on-curve
                    x2 = points_x[end_idx] * scale + offset_x;
                    y2 = points_y[end_idx] * scale + offset_y;
                } else {
                    // End point is also off-curve, use midpoint
                    x2 = (x1 + points_x[end_idx] * scale + offset_x) * 0.5f;
                    y2 = (y1 + points_y[end_idx] * scale + offset_y) * 0.5f;
                }

                // Start with current point
                if (count < max_points) {
                    out[count].x = x0;
                    out[count].y = y0;
                    count++;
                } else {
                    truncated = 1;
                }

                // Flatten the curve
                count = flatten_quadratic(
                    x0, y0, x1, y1, x2, y2, CURVE_TOLERANCE, out, max_points, count, 0, &truncated);
            } else if (!on0 && !on1) {
                // Both off-curve - implicit on-curve at midpoint
                // The midpoint becomes our "on-curve" start
                // and x1,y1 is the control for next segment
                // This case is handled by the previous iteration
            } else if (!on0 && on1) {
                // Current off-curve, next on-curve
                // This is handled by previous point's curve
            }
        }

        if (count - polygon_contour_start >= 2 && out_contour_ends && out_contour_count) {
            out_contour_ends[*out_contour_count] = count - 1;
            (*out_contour_count)++;
        }
    }

    return truncated ? -1 : count;
}

//=============================================================================
// Edge Comparison for Sorting
//=============================================================================

/// @brief Compare floating-point values in ascending order for `qsort`.
/// @param a Address of the first `float`.
/// @param b Address of the second `float`.
/// @return A negative, zero, or positive value according to the ordering of @p a and @p b.
static int raster_cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/// @brief Compare raster edges by their minimum y coordinate.
/// @param a Address of the first @ref raster_edge_t.
/// @param b Address of the second @ref raster_edge_t.
/// @return A negative value when @p a starts first, a positive value when @p b starts first, or
/// zero when their minimum y coordinates match.
static int compare_edges(const void *a, const void *b) {
    const raster_edge_t *ea = (const raster_edge_t *)a;
    const raster_edge_t *eb = (const raster_edge_t *)b;

    float min_y_a = (ea->y0 < ea->y1) ? ea->y0 : ea->y1;
    float min_y_b = (eb->y0 < eb->y1) ? eb->y0 : eb->y1;

    if (min_y_a < min_y_b)
        return -1;
    if (min_y_a > min_y_b)
        return 1;
    return 0;
}

//=============================================================================
// Build Edge List from Polygon
//=============================================================================

/// @brief Build a sorted edge list from a flattened multi-contour polygon.
/// @details Each contour is closed from its last point to its first. Horizontal edges are omitted,
/// and the remaining edges are sorted by minimum y for early termination during scan conversion.
/// @param points Flattened polygon vertices.
/// @param count Number of readable entries in @p points.
/// @param contour_ends Inclusive final point index for each contour.
/// @param contour_count Number of readable entries in @p contour_ends.
/// @param edges Destination array with capacity for at least @p count entries.
/// @return Number of non-horizontal edges written to @p edges.
static int build_edges(raster_point_t *points,
                       int count,
                       const int *contour_ends,
                       int contour_count,
                       raster_edge_t *edges) {
    int edge_count = 0;
    int contour_start = 0;

    for (int c = 0; c < contour_count && contour_start < count; c++) {
        int contour_end = contour_ends[c];
        if (contour_end >= count)
            contour_end = count - 1;
        if (contour_end - contour_start + 1 < 2) {
            contour_start = contour_end + 1;
            continue;
        }

        for (int i = contour_start; i <= contour_end; i++) {
            int j = (i == contour_end) ? contour_start : i + 1;

            // Skip horizontal edges
            if (fabsf(points[i].y - points[j].y) < 0.001f)
                continue;

            raster_edge_t *e = &edges[edge_count++];
            e->x0 = points[i].x;
            e->y0 = points[i].y;
            e->x1 = points[j].x;
            e->y1 = points[j].y;

            // Precompute for scanline intersection
            e->dx = (e->x1 - e->x0) / (e->y1 - e->y0);
        }

        contour_start = contour_end + 1;
    }

    // Sort edges by minimum y
    qsort(edges, edge_count, sizeof(raster_edge_t), compare_edges);

    return edge_count;
}

//=============================================================================
// Scanline Rasterization with Coverage-Based Antialiasing
//=============================================================================

/// @brief Fill a bitmap using scanline rasterisation with OVERSAMPLE vertical
///        supersampling and even-odd fill rule.
/// @details The function clears the output first, intersects each fractional scanline with active
/// edges, sorts the x coordinates, and accumulates fractional coverage between intersection pairs.
/// Allocation or bounds failures leave a fully or partially cleared bitmap and return silently.
/// @param points Flattened polygon vertices.
/// @param point_count Number of readable entries in @p points.
/// @param contour_ends Inclusive final point index for each contour.
/// @param contour_count Number of readable entries in @p contour_ends.
/// @param width Output bitmap width in pixels.
/// @param height Output bitmap height in pixels.
/// @param bitmap Destination row-major eight-bit alpha bitmap.
static void rasterize_scanlines(raster_point_t *points,
                                int point_count,
                                const int *contour_ends,
                                int contour_count,
                                int width,
                                int height,
                                uint8_t *bitmap) {
    if (!bitmap || width <= 0 || height <= 0)
        return;

    size_t bitmap_size = 0;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return;
    bitmap_size = (size_t)width * (size_t)height;

    // Clear bitmap
    memset(bitmap, 0, bitmap_size);

    if (point_count < 3 || !contour_ends || contour_count <= 0)
        return;

    // Build edge list
    if ((size_t)point_count > SIZE_MAX / sizeof(raster_edge_t))
        return;
    raster_edge_t *edges = malloc((size_t)point_count * sizeof(raster_edge_t));
    if (!edges)
        return;

    int edge_count = build_edges(points, point_count, contour_ends, contour_count, edges);
    if (edge_count <= 0) {
        free(edges);
        return;
    }

    // Supersampled scanline buffer
    float *coverage = calloc(width, sizeof(float));
    if (!coverage) {
        free(edges);
        return;
    }
    float *intersections = malloc((size_t)edge_count * sizeof(float));
    if (!intersections) {
        free(coverage);
        free(edges);
        return;
    }

    // Process each row with supersampling
    for (int y = 0; y < height; y++) {
        memset(coverage, 0, width * sizeof(float));

        // Supersample vertically
        for (int sub = 0; sub < OVERSAMPLE; sub++) {
            float scan_y = y + (sub + 0.5f) / OVERSAMPLE;

            // Find intersections with active edges
            int num_intersections = 0;

            for (int e = 0; e < edge_count; e++) {
                float y0 = edges[e].y0;
                float y1 = edges[e].y1;
                float min_y = (y0 < y1) ? y0 : y1;
                float max_y = (y0 > y1) ? y0 : y1;
                if (min_y > scan_y)
                    break;
                if (max_y <= scan_y)
                    continue;

                // Check if edge crosses this scanline
                bool crosses = (y0 <= scan_y && y1 > scan_y) || (y1 <= scan_y && y0 > scan_y);

                if (crosses) {
                    // Calculate x intersection
                    float t = (scan_y - y0) / (y1 - y0);
                    float x = edges[e].x0 + t * (edges[e].x1 - edges[e].x0);
                    intersections[num_intersections++] = x;
                }
            }

            // Sort intersections (qsort: O(n log n) vs previous O(n²) bubble sort)
            if (num_intersections > 1) {
                qsort(intersections, num_intersections, sizeof(float), raster_cmp_float);
            }

            // Fill between pairs (even-odd rule)
            for (int i = 0; i + 1 < num_intersections; i += 2) {
                float x0f = intersections[i];
                float x1f = intersections[i + 1];

                int x0 = (int)floorf(x0f);
                int x1 = (int)ceilf(x1f);

                if (x0 < 0)
                    x0 = 0;
                if (x1 > width)
                    x1 = width;

                // Add coverage for this subsample
                for (int x = x0; x < x1; x++) {
                    float left = (x < x0f) ? x0f : x;
                    float right = (x + 1 > x1f) ? x1f : x + 1;
                    if (right > left) {
                        coverage[x] += (right - left) / OVERSAMPLE;
                    }
                }
            }
        }

        // Convert coverage to 8-bit alpha
        for (int x = 0; x < width; x++) {
            float c = coverage[x];
            if (c > 0) {
                if (c > 1.0f)
                    c = 1.0f;
                bitmap[y * width + x] = (uint8_t)(c * 255.0f + 0.5f);
            }
        }
    }

    free(intersections);
    free(coverage);
    free(edges);
}

//=============================================================================
// Main Rasterization Entry Point
//=============================================================================

/// @brief Rasterize one TrueType glyph into an owned alpha bitmap.
/// @details Reads the glyph outline and horizontal metrics, scales them to @p size, flattens all
/// quadratic contours, flips the upward-positive font coordinate system into bitmap coordinates,
/// and produces an antialiased one-byte-per-pixel coverage map. Empty glyphs return valid metrics
/// with a NULL bitmap. Invalid dimensions, malformed outlines, or allocation failures return NULL.
/// @param font Parsed font containing the requested glyph and metric tables.
/// @param glyph_id Font-local glyph identifier.
/// @param size Finite positive target size in pixels.
/// @return Heap-allocated glyph owned by the caller, including its bitmap, or NULL on failure.
vg_glyph_t *vg_rasterize_glyph(vg_font_t *font, uint16_t glyph_id, float size) {
    if (!font || !isfinite(size) || size <= 0.0f || font->head.units_per_em == 0)
        return NULL;

    // Calculate scale factor
    float scale = size / (float)font->head.units_per_em;
    if (!isfinite(scale) || scale <= 0.0f)
        return NULL;

    // Get glyph outline
    float *points_x = NULL;
    float *points_y = NULL;
    uint8_t *flags = NULL;
    int *contour_ends = NULL;
    int num_points = 0;
    int num_contours = 0;

    if (!ttf_get_glyph_outline(font,
                               glyph_id,
                               &points_x,
                               &points_y,
                               &flags,
                               &contour_ends,
                               &num_points,
                               &num_contours)) {
        return NULL;
    }

    // Get horizontal metrics
    int advance_width, left_side_bearing;
    ttf_get_h_metrics(font, glyph_id, &advance_width, &left_side_bearing);

    // Allocate glyph
    vg_glyph_t *glyph = calloc(1, sizeof(vg_glyph_t));
    if (!glyph) {
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }

    float scaled_advance = advance_width * scale;
    if (!isfinite(scaled_advance))
        scaled_advance = 0.0f;
    if (scaled_advance > (float)INT_MAX)
        glyph->advance = INT_MAX;
    else if (scaled_advance < (float)INT_MIN)
        glyph->advance = INT_MIN;
    else
        glyph->advance = (int)(scaled_advance + (scaled_advance >= 0.0f ? 0.5f : -0.5f));

    // Empty glyph (like space)
    if (num_points == 0) {
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        glyph->width = 0;
        glyph->height = 0;
        glyph->bearing_x = 0;
        glyph->bearing_y = 0;
        glyph->bitmap = NULL;
        return glyph;
    }

    // Calculate bounding box
    float min_x = points_x[0], max_x = points_x[0];
    float min_y = points_y[0], max_y = points_y[0];

    for (int i = 1; i < num_points; i++) {
        if (points_x[i] < min_x)
            min_x = points_x[i];
        if (points_x[i] > max_x)
            max_x = points_x[i];
        if (points_y[i] < min_y)
            min_y = points_y[i];
        if (points_y[i] > max_y)
            max_y = points_y[i];
    }

    // Scale bounding box
    min_x *= scale;
    max_x *= scale;
    min_y *= scale;
    max_y *= scale;
    if (!isfinite(min_x) || !isfinite(max_x) || !isfinite(min_y) || !isfinite(max_y) ||
        max_x < min_x || max_y < min_y) {
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }

    // Calculate bitmap dimensions with padding
    int padding = 1;
    float bmp_width_f = ceilf(max_x - min_x) + (float)(padding * 2);
    float bmp_height_f = ceilf(max_y - min_y) + (float)(padding * 2);
    if (!isfinite(bmp_width_f) || !isfinite(bmp_height_f) ||
        bmp_width_f > (float)MAX_GLYPH_BITMAP_DIM || bmp_height_f > (float)MAX_GLYPH_BITMAP_DIM) {
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }
    int bmp_width = (int)bmp_width_f;
    int bmp_height = (int)bmp_height_f;

    if (bmp_width <= 0)
        bmp_width = 1;
    if (bmp_height <= 0)
        bmp_height = 1;

    // Calculate offsets
    float offset_x = -min_x + padding;
    float offset_y = -min_y + padding;

    // Set glyph metrics
    // Note: TTF y increases upward, bitmap y increases downward
    glyph->width = bmp_width;
    glyph->height = bmp_height;
    float bearing_x_f = floorf(min_x);
    float bearing_y_f = ceilf(max_y);
    glyph->bearing_x = (bearing_x_f > (float)INT_MAX)   ? INT_MAX
                       : (bearing_x_f < (float)INT_MIN) ? INT_MIN
                                                        : (int)bearing_x_f;
    glyph->bearing_y = (bearing_y_f > (float)INT_MAX) ? INT_MAX
                       : (bearing_y_f < (float)INT_MIN)
                           ? INT_MIN
                           : (int)bearing_y_f; // Top of glyph relative to baseline

    // Convert outline to polygon
    raster_point_t *polygon = malloc(MAX_POINTS * sizeof(raster_point_t));
    if (!polygon) {
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }

    int *polygon_contour_ends = calloc((size_t)num_contours, sizeof(int));
    if (!polygon_contour_ends) {
        free(polygon);
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }

    int polygon_contour_count = 0;
    int polygon_count = outline_to_polygon(points_x,
                                           points_y,
                                           flags,
                                           contour_ends,
                                           num_contours,
                                           scale,
                                           offset_x,
                                           offset_y,
                                           polygon,
                                           MAX_POINTS,
                                           polygon_contour_ends,
                                           &polygon_contour_count);
    if (polygon_count < 0) {
        free(polygon_contour_ends);
        free(polygon);
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }

    // Flip y coordinates (TTF y-up to bitmap y-down)
    for (int i = 0; i < polygon_count; i++) {
        polygon[i].y = bmp_height - polygon[i].y;
    }

    // Allocate and rasterize bitmap
    size_t bitmap_size = (size_t)bmp_width * (size_t)bmp_height;
    if ((size_t)bmp_width != 0 && bitmap_size / (size_t)bmp_width != (size_t)bmp_height) {
        free(polygon_contour_ends);
        free(polygon);
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }
    glyph->bitmap = calloc(bitmap_size, 1);
    if (!glyph->bitmap) {
        free(polygon_contour_ends);
        free(polygon);
        free(glyph);
        free(points_x);
        free(points_y);
        free(flags);
        free(contour_ends);
        return NULL;
    }
    rasterize_scanlines(polygon,
                        polygon_count,
                        polygon_contour_ends,
                        polygon_contour_count,
                        bmp_width,
                        bmp_height,
                        glyph->bitmap);

    // Cleanup
    free(polygon_contour_ends);
    free(polygon);
    free(points_x);
    free(points_y);
    free(flags);
    free(contour_ends);

    return glyph;
}
