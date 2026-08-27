//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_sw_shadow.inc
// Purpose: Software wireframe lines and tiled shadow-map depth rasterization.
// Key invariants:
//   - Shadow matrices, slots, dimensions, tile bounds, and alpha-mask samples are validated.
//   - Partial worker submission falls back synchronously so no tile remains unrendered.
// Ownership/Lifetime:
//   - The software context owns shadow buffers and worker scratch for the active pass.
//   - Worker jobs borrow draw data until their completion barrier returns.
// Links: vgfx3d_backend_sw.c, vgfx3d_backend_sw_raster.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_sw_shadow.inc
 * @brief Implements software wireframe lines and clipped, tiled shadow-depth rasterization.
 *
 * Shadow draws transform and clip mesh triangles in homogeneous light space, preserve alpha-mask
 * attributes, bin resulting screen triangles into fixed-size tiles, and rasterize active tiles
 * through the context worker pool. Serial paths remain available for small draws or unavailable
 * worker resources, while canvas-owned depth storage remains valid across shadow reuse.
 */

/*==========================================================================
 * Wireframe — Bresenham line
 *=========================================================================*/

/// @brief Bresenham-style line rasterizer used for wireframe mode.
///
/// Walks the longer of dx or dy, stepping the shorter axis via an
/// error-accumulator. No anti-aliasing — we trade quality for the
/// software backend's per-pixel speed.
/// @param pixels Borrowed mutable RGBA8 framebuffer.
/// @param fb_w Positive framebuffer width in pixels.
/// @param fb_h Positive framebuffer height in pixels.
/// @param stride Framebuffer row stride in bytes.
/// @param x0 Starting pixel X coordinate.
/// @param y0 Starting pixel Y coordinate.
/// @param x1 Ending pixel X coordinate.
/// @param y1 Ending pixel Y coordinate.
/// @param r Red byte written to covered pixels.
/// @param g Green byte written to covered pixels.
/// @param b Blue byte written to covered pixels.
static void draw_line(uint8_t *pixels,
                      int32_t fb_w,
                      int32_t fb_h,
                      int32_t stride,
                      int x0,
                      int y0,
                      int x1,
                      int y1,
                      uint8_t r,
                      uint8_t g,
                      uint8_t b) {
    int64_t current_x = x0;
    int64_t current_y = y0;
    int64_t end_x = x1;
    int64_t end_y = y1;
    int64_t delta_x = end_x - current_x;
    int64_t delta_y = end_y - current_y;
    int64_t dx = delta_x < 0 ? -delta_x : delta_x;
    int64_t dy = -(delta_y < 0 ? -delta_y : delta_y);
    int64_t step_x = current_x < end_x ? 1 : -1;
    int64_t step_y = current_y < end_y ? 1 : -1;
    int64_t err = dx + dy;
    if (!sw_rgba_surface_is_valid(pixels, fb_w, fb_h, stride))
        return;
    for (;;) {
        if (current_x >= 0 && current_x < fb_w && current_y >= 0 && current_y < fb_h) {
            uint8_t *dst = pixels + (size_t)current_y * (size_t)stride + (size_t)current_x * 4u;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = 0xFF;
        }
        if (current_x == end_x && current_y == end_y)
            break;
        int64_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            current_x += step_x;
        }
        if (e2 <= dx) {
            err += dx;
            current_y += step_y;
        }
    }
}

/*==========================================================================
 * Shadow map — depth-only rasterization
 *=========================================================================*/

/// @brief Advance a shadow edge-function accumulator by a signed number of steps.
/// @param value Edge-function value at the starting sample.
/// @param delta Per-step edge-function increment.
/// @param steps Signed number of pixel or scanline steps.
/// @return The advanced edge-function value.
static float shadow_advance_edge_value(float value, float delta, int steps) {
    /* Closed-form advance, matching sw_advance_edge_value in the colour raster path: avoids a
     * per-step loop (previously O(steps) on scissored shadow tiles with a large min-y offset)
     * and keeps the two rasterizers' edge-function stepping consistent. */
    return value + delta * (float)steps;
}

/// @brief Rasterize one screen-space triangle into a shadow depth buffer within a scissor.
/// @details Uses edge functions and perspective-correct alpha/UV interpolation. Opaque fragments
///          update depth directly; masked fragments sample the base-color alpha using the
///          material-selected UV set and transform before applying the alpha cutoff.
/// @param depth Borrowed mutable shadow depth buffer.
/// @param sw Positive shadow-map width in pixels.
/// @param sh Positive shadow-map height in pixels.
/// @param sx Mutable array of three screen-space X coordinates; winding normalization may swap
///           lanes one and two.
/// @param sy Mutable array of three screen-space Y coordinates.
/// @param sz Mutable array of three normalized shadow depths.
/// @param su Optional first-UV-set U coordinates.
/// @param sv Optional first-UV-set V coordinates.
/// @param su1 Optional second-UV-set U coordinates.
/// @param sv1 Optional second-UV-set V coordinates.
/// @param sa Optional vertex-alpha values; absent input defaults to one.
/// @param sinv_w Optional reciprocal clip-space W values for perspective correction.
/// @param cmd Optional borrowed draw command supplying mask and texture-transform state.
/// @param alpha_tex Optional borrowed base-color texture view for alpha masking.
/// @param clip_min_x Inclusive scissor minimum X coordinate.
/// @param clip_min_y Inclusive scissor minimum Y coordinate.
/// @param clip_max_x Inclusive scissor maximum X coordinate.
/// @param clip_max_y Inclusive scissor maximum Y coordinate.
static void shadow_raster_tri_scissored(float *depth,
                                        int32_t sw,
                                        int32_t sh,
                                        float *sx,
                                        float *sy,
                                        float *sz,
                                        const float *su,
                                        const float *sv,
                                        const float *su1,
                                        const float *sv1,
                                        const float *sa,
                                        const float *sinv_w,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        const sw_pixels_view *alpha_tex,
                                        int32_t clip_min_x,
                                        int32_t clip_min_y,
                                        int32_t clip_max_x,
                                        int32_t clip_max_y) {
    if (!depth || !sx || !sy || !sz || sw <= 0 || sh <= 0)
        return;
    for (int lane = 0; lane < 3; lane++) {
        if (!isfinite(sx[lane]) || !isfinite(sy[lane]) || !isfinite(sz[lane]) ||
            (sinv_w && (!isfinite(sinv_w[lane]) || sinv_w[lane] <= 0.0f)))
            return;
    }
    float u[3] = {su ? su[0] : 0.0f, su ? su[1] : 0.0f, su ? su[2] : 0.0f};
    float vcoord[3] = {sv ? sv[0] : 0.0f, sv ? sv[1] : 0.0f, sv ? sv[2] : 0.0f};
    float u1[3] = {su1 ? su1[0] : 0.0f, su1 ? su1[1] : 0.0f, su1 ? su1[2] : 0.0f};
    float v1coord[3] = {sv1 ? sv1[0] : 0.0f, sv1 ? sv1[1] : 0.0f, sv1 ? sv1[2] : 0.0f};
    float alpha_v[3] = {sa ? sa[0] : 1.0f, sa ? sa[1] : 1.0f, sa ? sa[2] : 1.0f};
    float inv_w[3] = {
        sinv_w ? sinv_w[0] : 1.0f, sinv_w ? sinv_w[1] : 1.0f, sinv_w ? sinv_w[2] : 1.0f};
    /* Screen-space area (winding check) */
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (area < 0.0f) {
        /* Swap v1/v2 to ensure positive area */
        float t;
        t = sx[1];
        sx[1] = sx[2];
        sx[2] = t;
        t = sy[1];
        sy[1] = sy[2];
        sy[2] = t;
        t = sz[1];
        sz[1] = sz[2];
        sz[2] = t;
        t = u[1];
        u[1] = u[2];
        u[2] = t;
        t = vcoord[1];
        vcoord[1] = vcoord[2];
        vcoord[2] = t;
        t = u1[1];
        u1[1] = u1[2];
        u1[2] = t;
        t = v1coord[1];
        v1coord[1] = v1coord[2];
        v1coord[2] = t;
        t = alpha_v[1];
        alpha_v[1] = alpha_v[2];
        alpha_v[2] = t;
        t = inv_w[1];
        inv_w[1] = inv_w[2];
        inv_w[2] = t;
        area = -area;
    }
    if (area < 1e-12f)
        return;

    float inv_area = 1.0f / area;
    int min_x = sw_floor_pixel_coord(fminf(fminf(sx[0], sx[1]), sx[2]), sw - 1);
    int max_x = sw_ceil_pixel_coord(fmaxf(fmaxf(sx[0], sx[1]), sx[2]), sw - 1);
    int min_y = sw_floor_pixel_coord(fminf(fminf(sy[0], sy[1]), sy[2]), sh - 1);
    int max_y = sw_ceil_pixel_coord(fmaxf(fmaxf(sy[0], sy[1]), sy[2]), sh - 1);
    min_x = min_x > clip_min_x ? min_x : clip_min_x;
    max_x = max_x < clip_max_x ? max_x : clip_max_x;
    min_y = min_y > clip_min_y ? min_y : clip_min_y;
    max_y = max_y < clip_max_y ? max_y : clip_max_y;
    if (min_x > max_x || min_y > max_y)
        return;

    float e12_dx = sx[2] - sx[1], e12_dy = sy[2] - sy[1];
    float e20_dx = sx[0] - sx[2], e20_dy = sy[0] - sy[2];
    float e01_dx = sx[1] - sx[0], e01_dy = sy[1] - sy[0];
    int full_min_x = sw_floor_pixel_coord(fminf(fminf(sx[0], sx[1]), sx[2]), sw - 1);
    int full_min_y = sw_floor_pixel_coord(fminf(fminf(sy[0], sy[1]), sy[2]), sh - 1);
    float px0 = (float)full_min_x + 0.5f, py0 = (float)full_min_y + 0.5f;
    float row_w0 = e12_dx * (py0 - sy[1]) - e12_dy * (px0 - sx[1]);
    float row_w1 = e20_dx * (py0 - sy[2]) - e20_dy * (px0 - sx[2]);
    float row_w2 = e01_dx * (py0 - sy[0]) - e01_dy * (px0 - sx[0]);
    const float edge_epsilon = 0.0f;

    row_w0 = shadow_advance_edge_value(row_w0, e12_dx, min_y - full_min_y);
    row_w1 = shadow_advance_edge_value(row_w1, e20_dx, min_y - full_min_y);
    row_w2 = shadow_advance_edge_value(row_w2, e01_dx, min_y - full_min_y);
    for (int y = min_y; y <= max_y; y++) {
        float w0 = shadow_advance_edge_value(row_w0, -e12_dy, min_x - full_min_x);
        float w1 = shadow_advance_edge_value(row_w1, -e20_dy, min_x - full_min_x);
        float w2 = shadow_advance_edge_value(row_w2, -e01_dy, min_x - full_min_x);
        for (int x = min_x; x <= max_x; x++) {
            if (w0 >= edge_epsilon && w1 >= edge_epsilon && w2 >= edge_epsilon) {
                float b0 = w0 * inv_area, b1 = w1 * inv_area, b2 = w2 * inv_area;
                float z = b0 * sz[0] + b1 * sz[1] + b2 * sz[2];
                size_t idx = (size_t)y * (size_t)sw + (size_t)x;
                if (cmd && cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK) {
                    float attr_b0;
                    float attr_b1;
                    float attr_b2;
                    if (!sw_perspective_attribute_weights(
                            b0, b1, b2, inv_w[0], inv_w[1], inv_w[2], &attr_b0, &attr_b1, &attr_b2))
                        goto next_shadow_pixel;
                    float alpha =
                        cmd->diffuse_color[3] * cmd->alpha *
                        (attr_b0 * alpha_v[0] + attr_b1 * alpha_v[1] + attr_b2 * alpha_v[2]);
                    if (alpha_tex) {
                        float tr, tg, tb, ta;
                        int use_uv1 =
                            cmd->texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR] > 0;
                        const float *m =
                            cmd->texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
                        float base_u = use_uv1
                                           ? (attr_b0 * u1[0] + attr_b1 * u1[1] + attr_b2 * u1[2])
                                           : (attr_b0 * u[0] + attr_b1 * u[1] + attr_b2 * u[2]);
                        float base_v =
                            use_uv1
                                ? (attr_b0 * v1coord[0] + attr_b1 * v1coord[1] +
                                   attr_b2 * v1coord[2])
                                : (attr_b0 * vcoord[0] + attr_b1 * vcoord[1] + attr_b2 * vcoord[2]);
                        float tex_u = base_u * m[0] + base_v * m[1] + m[4];
                        float tex_v = base_u * m[2] + base_v * m[3] + m[5];
                        sample_texture_slot_ex(alpha_tex,
                                               cmd,
                                               RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR,
                                               tex_u,
                                               tex_v,
                                               &tr,
                                               &tg,
                                               &tb,
                                               &ta);
                        alpha *= ta;
                    }
                    if (alpha < cmd->alpha_cutoff)
                        goto next_shadow_pixel;
                }
                if (isfinite(z) && (!isfinite(depth[idx]) || z < depth[idx]))
                    depth[idx] = z;
            }
        next_shadow_pixel:
            w0 -= e12_dy;
            w1 -= e20_dy;
            w2 -= e01_dy;
        }
        row_w0 += e12_dx;
        row_w1 += e20_dx;
        row_w2 += e01_dx;
    }
}

/// @brief Rasterize one screen-space triangle across the complete shadow map.
/// @details Convenience wrapper around @c shadow_raster_tri_scissored() using full-map bounds.
/// @param depth Borrowed mutable shadow depth buffer.
/// @param sw Positive shadow-map width in pixels.
/// @param sh Positive shadow-map height in pixels.
/// @param sx Mutable array of three screen-space X coordinates.
/// @param sy Mutable array of three screen-space Y coordinates.
/// @param sz Mutable array of three normalized shadow depths.
/// @param su Optional first-UV-set U coordinates.
/// @param sv Optional first-UV-set V coordinates.
/// @param su1 Optional second-UV-set U coordinates.
/// @param sv1 Optional second-UV-set V coordinates.
/// @param sa Optional vertex-alpha values.
/// @param sinv_w Optional reciprocal clip-space W values.
/// @param cmd Optional borrowed draw command supplying alpha-mask state.
/// @param alpha_tex Optional borrowed base-color texture view for alpha masking.
static void shadow_raster_tri(float *depth,
                              int32_t sw,
                              int32_t sh,
                              float *sx,
                              float *sy,
                              float *sz,
                              const float *su,
                              const float *sv,
                              const float *su1,
                              const float *sv1,
                              const float *sa,
                              const float *sinv_w,
                              const vgfx3d_draw_cmd_t *cmd,
                              const sw_pixels_view *alpha_tex) {
    shadow_raster_tri_scissored(depth,
                                sw,
                                sh,
                                sx,
                                sy,
                                sz,
                                su,
                                sv,
                                su1,
                                sv1,
                                sa,
                                sinv_w,
                                cmd,
                                alpha_tex,
                                0,
                                0,
                                sw - 1,
                                sh - 1);
}

typedef struct {
    float clip[4];
    float uv[2];
    float uv1[2];
    float color_alpha;
} shadow_clip_vertex_t;

/// @brief Signed distance from a clip-space vertex to the @p plane-th frustum plane.
/// @details Plane indexing (0..5) is left, right, bottom, top, near, far. Positive
///   results mean the vertex is on the inside half-space of that plane. Used by the
///   shadow-pass Sutherland-Hodgman polygon clipper to test which side each input
///   vertex falls on.
/// @param v Borrowed homogeneous clip-space vertex.
/// @param plane Frustum-plane index from zero through five.
/// @return Signed homogeneous distance, non-negative on the plane's inside half-space.
static float shadow_clip_distance(const shadow_clip_vertex_t *v, int plane) {
    switch (plane) {
        case 0:
            return v->clip[0] + v->clip[3]; /* left */
        case 1:
            return v->clip[3] - v->clip[0]; /* right */
        case 2:
            return v->clip[1] + v->clip[3]; /* bottom */
        case 3:
            return v->clip[3] - v->clip[1]; /* top */
        case 4:
            return v->clip[2] + v->clip[3]; /* near */
        default:
            return v->clip[3] - v->clip[2]; /* far */
    }
}

/// @brief Linearly interpolate two clip-space vertices at parameter @p t.
/// @details Used by the shadow polygon clipper to construct the new vertex generated
///   when an edge crosses a clip plane. The interpolation runs on the homogeneous
///   clip-space coordinates so the resulting position lies on the original edge.
/// @param a Borrowed edge-start vertex.
/// @param b Borrowed edge-end vertex.
/// @param t Interpolation parameter, conventionally in `[0, 1]`.
/// @return Interpolated clip coordinates, UV sets, and vertex alpha.
static shadow_clip_vertex_t shadow_clip_lerp(const shadow_clip_vertex_t *a,
                                             const shadow_clip_vertex_t *b,
                                             float t) {
    shadow_clip_vertex_t out;
    for (int i = 0; i < 4; i++)
        out.clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    out.uv[0] = a->uv[0] + (b->uv[0] - a->uv[0]) * t;
    out.uv[1] = a->uv[1] + (b->uv[1] - a->uv[1]) * t;
    out.uv1[0] = a->uv1[0] + (b->uv1[0] - a->uv1[0]) * t;
    out.uv1[1] = a->uv1[1] + (b->uv1[1] - a->uv1[1]) * t;
    out.color_alpha = a->color_alpha + (b->color_alpha - a->color_alpha) * t;
    return out;
}

/// @brief Sutherland-Hodgman clip a polygon in 4D clip space against the 6 frustum planes.
/// @details Walks each plane in turn; for every input edge, emits the entry intersection
///   when one endpoint is outside, and emits the inside endpoint unchanged. The output
///   polygon replaces the input via a `tmp[]` swap buffer (max 16 vertices). Returns the
///   final clipped polygon vertex count, with the result written back to @p poly. Used
///   by the shadow-pass shadow-map rasterizer to clip triangles against the shadow frustum.
/// @param poly In/out polygon storage with capacity for 16 vertices.
/// @param count Initial number of valid vertices in @p poly.
/// @return Number of vertices remaining after all six planes, or zero when fully clipped.
static int shadow_clip_polygon(shadow_clip_vertex_t *poly, int count) {
    shadow_clip_vertex_t tmp[16];
    for (int plane = 0; plane < 6; plane++) {
        int out_count = 0;
        if (count <= 0)
            return 0;
        shadow_clip_vertex_t prev = poly[count - 1];
        float prev_d = shadow_clip_distance(&prev, plane);
        int prev_inside = prev_d >= 0.0f;
        for (int i = 0; i < count; i++) {
            shadow_clip_vertex_t cur = poly[i];
            float cur_d = shadow_clip_distance(&cur, plane);
            int cur_inside = cur_d >= 0.0f;
            if (cur_inside != prev_inside) {
                float denom = prev_d - cur_d;
                float t = fabsf(denom) > 1e-8f ? prev_d / denom : 0.0f;
                if (out_count < (int)(sizeof(tmp) / sizeof(tmp[0])))
                    tmp[out_count++] = shadow_clip_lerp(&prev, &cur, t);
            }
            if (cur_inside && out_count < (int)(sizeof(tmp) / sizeof(tmp[0])))
                tmp[out_count++] = cur;
            prev = cur;
            prev_d = cur_d;
            prev_inside = cur_inside;
        }
        if (out_count <= 0)
            return 0;
        memcpy(poly, tmp, (size_t)out_count * sizeof(*poly));
        count = out_count;
    }
    return count;
}

typedef struct {
    float sx[3], sy[3], sz[3];
    float u[3], v[3], u1[3], v1[3], alpha[3], inv_w[3];
} sw_shadow_screen_tri_t;

typedef struct {
    sw_shadow_screen_tri_t *items;
    uint32_t count;
    uint32_t capacity;
} sw_shadow_tri_list_t;

typedef struct {
    uint32_t *indices;
    uint32_t count;
    uint32_t capacity;
} sw_shadow_tile_bin_t;

typedef struct {
    sw_shadow_tile_bin_t *bins;
    int32_t *active_indices;
    int32_t cols;
    int32_t rows;
    int32_t count;
    int32_t capacity;
    int32_t active_count;
    int32_t active_capacity;
} sw_shadow_tile_grid_t;

typedef struct {
    const sw_shadow_tile_grid_t *grid;
    const sw_shadow_tri_list_t *tris;
    int32_t *next_tile;
    float *depth;
    int32_t sw;
    int32_t sh;
    const vgfx3d_draw_cmd_t *cmd;
    const sw_pixels_view *alpha_tex;
} sw_shadow_raster_task_t;

typedef struct {
    const vgfx3d_draw_cmd_t *cmd;
    uint32_t tri_begin;
    uint32_t tri_end;
    const float *lmvp;
    float half_w;
    float half_h;
    sw_shadow_tri_list_t out;
    int32_t *failed;
} sw_shadow_collect_task_t;

/// @brief Return worker-local shadow clipping lists to their context-owned scratch slots.
/// @param ctx Mutable software context that owns the retained allocations.
/// @param tasks Completed task array whose list pointers are transferred back to @p ctx.
/// @param task_count Number of initialized entries in @p tasks.
static void sw_shadow_retain_task_scratch(sw_context_t *ctx,
                                          sw_shadow_collect_task_t tasks[SW_MAX_TASKS],
                                          int32_t task_count) {
    if (!ctx || !tasks || task_count < 0 || task_count > SW_MAX_TASKS)
        return;
    for (int32_t i = 0; i < task_count; i++) {
        ctx->shadow_task_triangle_scratch[i] = tasks[i].out.items;
        ctx->shadow_task_triangle_scratch_capacity[i] = tasks[i].out.capacity;
        tasks[i].out.items = NULL;
        tasks[i].out.count = 0;
        tasks[i].out.capacity = 0;
    }
}

/// @brief Ensure a shadow-triangle list can hold at least the requested number of entries.
/// @param list Mutable list whose backing storage may grow.
/// @param needed Required total capacity.
/// @return 1 when sufficient storage is available, otherwise 0 on invalid input, overflow, or
///         allocation failure.
static int sw_shadow_tri_list_reserve(sw_shadow_tri_list_t *list, uint32_t needed) {
    uint32_t new_capacity;
    sw_shadow_screen_tri_t *new_items;
    if (!list)
        return 0;
    if (needed <= list->capacity)
        return 1;
    new_capacity = list->capacity ? list->capacity : 64u;
    while (new_capacity < needed) {
        if (new_capacity > UINT32_MAX / 2u) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2u;
    }
    if (!rt_alloc_count_ok(new_capacity, sizeof(sw_shadow_screen_tri_t)))
        return 0;
    new_items =
        (sw_shadow_screen_tri_t *)realloc(list->items, (size_t)new_capacity * sizeof(*new_items));
    if (!new_items)
        return 0;
    list->items = new_items;
    list->capacity = new_capacity;
    return 1;
}

/// @brief Append one screen-space shadow triangle to a growable list.
/// @param list Mutable destination list.
/// @param tri Borrowed triangle copied into the list.
/// @return 1 on success, otherwise 0 for invalid input, count overflow, or allocation failure.
static int sw_shadow_tri_list_push(sw_shadow_tri_list_t *list, const sw_shadow_screen_tri_t *tri) {
    if (!list || !tri)
        return 0;
    if (list->count == UINT32_MAX)
        return 0;
    if (!sw_shadow_tri_list_reserve(list, list->count + 1u))
        return 0;
    list->items[list->count++] = *tri;
    return 1;
}

/// @brief Release every tile-bin allocation and reset the shadow tile grid.
/// @param grid Mutable grid to release; null is a no-op.
static void sw_shadow_tile_grid_free(sw_shadow_tile_grid_t *grid) {
    if (!grid)
        return;
    if (grid->bins) {
        for (int32_t i = 0; i < grid->capacity; i++)
            free(grid->bins[i].indices);
    }
    free(grid->active_indices);
    free(grid->bins);
    grid->bins = NULL;
    grid->active_indices = NULL;
    grid->cols = 0;
    grid->rows = 0;
    grid->count = 0;
    grid->capacity = 0;
    grid->active_count = 0;
    grid->active_capacity = 0;
}

/// @brief Initialize an empty fixed-size tile grid for a shadow-map extent.
/// @param grid Mutable grid receiving zeroed bins and dimensions.
/// @param width Positive shadow-map width in pixels.
/// @param height Positive shadow-map height in pixels.
/// @return 1 when all bins are allocated, otherwise 0 for invalid dimensions, overflow, or OOM.
static int sw_shadow_tile_grid_prepare(sw_shadow_tile_grid_t *grid, int32_t width, int32_t height) {
    int32_t cols;
    int32_t rows;
    int32_t count;
    if (!grid || width <= 0 || height <= 0)
        return 0;
    cols = ((width - 1) / SW_TILE_SIZE) + 1;
    rows = ((height - 1) / SW_TILE_SIZE) + 1;
    if (cols <= 0 || rows <= 0 || cols > INT32_MAX / rows)
        return 0;
    count = cols * rows;
    for (int32_t index = 0; index < grid->capacity; ++index)
        grid->bins[index].count = 0;
    grid->active_count = 0;
    if (count > grid->capacity) {
        sw_shadow_tile_bin_t *grown;
        if ((size_t)count > SIZE_MAX / sizeof(*grid->bins))
            return 0;
        grown = (sw_shadow_tile_bin_t *)realloc(grid->bins, (size_t)count * sizeof(*grid->bins));
        if (!grown)
            return 0;
        memset(grown + grid->capacity, 0, (size_t)(count - grid->capacity) * sizeof(*grid->bins));
        grid->bins = grown;
        grid->capacity = count;
    }
    grid->cols = cols;
    grid->rows = rows;
    grid->count = count;
    return 1;
}

/// @brief Append a newly non-empty tile to the grid's active-tile work list.
/// @param grid Mutable tile grid whose active-index storage may grow.
/// @param tile_index Valid linear tile index to append exactly once.
/// @return 1 on success, otherwise 0 for invalid input, overflow, or allocation failure.
static int sw_shadow_tile_grid_mark_active(sw_shadow_tile_grid_t *grid, int32_t tile_index) {
    int32_t new_capacity;
    int32_t *new_indices;
    if (!grid || tile_index < 0 || tile_index >= grid->count)
        return 0;
    if (grid->active_count < grid->active_capacity) {
        grid->active_indices[grid->active_count++] = tile_index;
        return 1;
    }
    if (grid->active_count == INT32_MAX)
        return 0;
    if (grid->active_capacity > INT32_MAX / 2)
        new_capacity = grid->active_count + 1;
    else
        new_capacity = grid->active_capacity ? grid->active_capacity * 2 : 32;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(int32_t))
        return 0;
    new_indices =
        (int32_t *)realloc(grid->active_indices, (size_t)new_capacity * sizeof(*new_indices));
    if (!new_indices)
        return 0;
    grid->active_indices = new_indices;
    grid->active_capacity = new_capacity;
    grid->active_indices[grid->active_count++] = tile_index;
    return 1;
}

/// @brief Append a triangle index to one growable shadow tile bin.
/// @param bin Mutable tile bin whose index storage may grow.
/// @param tri_index Index into the shared collected-triangle list.
/// @return 1 on success, otherwise 0 for invalid input, overflow, or allocation failure.
static int sw_shadow_tile_bin_append(sw_shadow_tile_bin_t *bin, uint32_t tri_index) {
    uint32_t new_capacity;
    uint32_t *new_indices;
    if (!bin)
        return 0;
    if (bin->count == UINT32_MAX)
        return 0;
    if (bin->count < bin->capacity) {
        bin->indices[bin->count++] = tri_index;
        return 1;
    }
    new_capacity = bin->capacity ? bin->capacity * 2u : 16u;
    if (new_capacity <= bin->capacity)
        new_capacity = bin->count + 1u;
    if (!rt_alloc_count_ok(new_capacity, sizeof(uint32_t)))
        return 0;
    new_indices = (uint32_t *)realloc(bin->indices, (size_t)new_capacity * sizeof(*new_indices));
    if (!new_indices)
        return 0;
    bin->indices = new_indices;
    bin->capacity = new_capacity;
    bin->indices[bin->count++] = tri_index;
    return 1;
}

/// @brief Compute a finite, clipped pixel bounding box for a screen-space shadow triangle.
/// @param tri Borrowed screen-space triangle.
/// @param sw Positive shadow-map width in pixels.
/// @param sh Positive shadow-map height in pixels.
/// @param out_min_x Receives the inclusive minimum X coordinate.
/// @param out_min_y Receives the inclusive minimum Y coordinate.
/// @param out_max_x Receives the inclusive maximum X coordinate.
/// @param out_max_y Receives the inclusive maximum Y coordinate.
/// @return 1 for a finite non-degenerate on-map triangle, otherwise 0.
static int sw_shadow_tri_bounds(const sw_shadow_screen_tri_t *tri,
                                int32_t sw,
                                int32_t sh,
                                int32_t *out_min_x,
                                int32_t *out_min_y,
                                int32_t *out_max_x,
                                int32_t *out_max_y) {
    float area;
    int32_t min_x, min_y, max_x, max_y;
    if (!tri || sw <= 0 || sh <= 0 || !out_min_x || !out_min_y || !out_max_x || !out_max_y)
        return 0;
    for (int lane = 0; lane < 3; lane++) {
        if (!isfinite(tri->sx[lane]) || !isfinite(tri->sy[lane]))
            return 0;
    }
    area = (tri->sx[1] - tri->sx[0]) * (tri->sy[2] - tri->sy[0]) -
           (tri->sx[2] - tri->sx[0]) * (tri->sy[1] - tri->sy[0]);
    if (!isfinite(area) || fabsf(area) < 1e-12f)
        return 0;
    min_x = sw_floor_pixel_coord(fminf(fminf(tri->sx[0], tri->sx[1]), tri->sx[2]), sw - 1);
    max_x = sw_ceil_pixel_coord(fmaxf(fmaxf(tri->sx[0], tri->sx[1]), tri->sx[2]), sw - 1);
    min_y = sw_floor_pixel_coord(fminf(fminf(tri->sy[0], tri->sy[1]), tri->sy[2]), sh - 1);
    max_y = sw_ceil_pixel_coord(fmaxf(fmaxf(tri->sy[0], tri->sy[1]), tri->sy[2]), sh - 1);
    if (min_x > max_x || min_y > max_y)
        return 0;
    *out_min_x = min_x;
    *out_min_y = min_y;
    *out_max_x = max_x;
    *out_max_y = max_y;
    return 1;
}

/// @brief Bin every collected shadow triangle into all intersected tiles.
/// @param grid Mutable initialized tile grid receiving triangle indices.
/// @param tris Borrowed collected screen-space triangles.
/// @param sw Positive shadow-map width in pixels.
/// @param sh Positive shadow-map height in pixels.
/// @return 1 when binning completes, otherwise 0 on invalid input or allocation failure.
static int sw_shadow_bin_triangles(sw_shadow_tile_grid_t *grid,
                                   const sw_shadow_tri_list_t *tris,
                                   int32_t sw,
                                   int32_t sh) {
    if (!grid || !grid->bins || !tris)
        return 0;
    for (uint32_t tri_index = 0; tri_index < tris->count; tri_index++) {
        int32_t min_x, min_y, max_x, max_y;
        if (!sw_shadow_tri_bounds(&tris->items[tri_index], sw, sh, &min_x, &min_y, &max_x, &max_y))
            continue;
        int32_t tx0 = min_x / SW_TILE_SIZE;
        int32_t tx1 = max_x / SW_TILE_SIZE;
        int32_t ty0 = min_y / SW_TILE_SIZE;
        int32_t ty1 = max_y / SW_TILE_SIZE;
        if (tx0 < 0)
            tx0 = 0;
        if (ty0 < 0)
            ty0 = 0;
        if (tx1 >= grid->cols)
            tx1 = grid->cols - 1;
        if (ty1 >= grid->rows)
            ty1 = grid->rows - 1;
        for (int32_t ty = ty0; ty <= ty1; ty++) {
            for (int32_t tx = tx0; tx <= tx1; tx++) {
                int32_t tile_min_x = tx * SW_TILE_SIZE;
                int32_t tile_min_y = ty * SW_TILE_SIZE;
                int32_t tile_max_x = tile_min_x + SW_TILE_SIZE - 1;
                int32_t tile_max_y = tile_min_y + SW_TILE_SIZE - 1;
                const sw_shadow_screen_tri_t *tri = &tris->items[tri_index];
                if (tile_max_x >= sw)
                    tile_max_x = sw - 1;
                if (tile_max_y >= sh)
                    tile_max_y = sh - 1;
                if (!sw_triangle_overlaps_pixel_rect(tri->sx[0],
                                                     tri->sy[0],
                                                     tri->sx[1],
                                                     tri->sy[1],
                                                     tri->sx[2],
                                                     tri->sy[2],
                                                     tile_min_x,
                                                     tile_min_y,
                                                     tile_max_x,
                                                     tile_max_y))
                    continue;
                sw_shadow_tile_bin_t *bin = &grid->bins[ty * grid->cols + tx];
                uint32_t prior_count = bin->count;
                if (!sw_shadow_tile_bin_append(bin, tri_index))
                    return 0;
                if (prior_count == 0 &&
                    !sw_shadow_tile_grid_mark_active(grid, ty * grid->cols + tx))
                    return 0;
            }
        }
    }
    return 1;
}

/// @brief Transform, frustum-clip, triangulate, and collect a contiguous source-triangle range.
/// @param cmd Borrowed sanitized indexed draw command.
/// @param tri_begin Inclusive source triangle index.
/// @param tri_end Exclusive source triangle index.
/// @param lmvp Borrowed row-major light-model-view-projection matrix.
/// @param half_w Half the shadow-map width used for viewport conversion.
/// @param half_h Half the shadow-map height used for viewport conversion.
/// @param out Mutable list receiving zero or more clipped screen triangles.
/// @return 1 when the range is processed, otherwise 0 on invalid input or list-growth failure.
static int sw_shadow_collect_triangles_range(const vgfx3d_draw_cmd_t *cmd,
                                             uint32_t tri_begin,
                                             uint32_t tri_end,
                                             const float *lmvp,
                                             float half_w,
                                             float half_h,
                                             sw_shadow_tri_list_t *out) {
    if (!cmd || !lmvp || !out)
        return 0;
    for (uint32_t tri_index = tri_begin; tri_index < tri_end; tri_index++) {
        uint32_t i = tri_index * 3u;
        uint32_t i0 = cmd->indices[i], i1 = cmd->indices[i + 1], i2 = cmd->indices[i + 2];
        if (i0 >= cmd->vertex_count || i1 >= cmd->vertex_count || i2 >= cmd->vertex_count)
            continue;

        shadow_clip_vertex_t clipped[16];
        for (int vi = 0; vi < 3; vi++) {
            const uint32_t idx[3] = {i0, i1, i2};
            const vgfx3d_vertex_t *v = &cmd->vertices[idx[vi]];
            float pos4[4] = {v->pos[0], v->pos[1], v->pos[2], 1.0f};
            mat4f_transform4(lmvp, pos4, clipped[vi].clip);
            clipped[vi].uv[0] = v->uv[0];
            clipped[vi].uv[1] = v->uv[1];
            clipped[vi].uv1[0] = v->uv1[0];
            clipped[vi].uv1[1] = v->uv1[1];
            clipped[vi].color_alpha = v->color[3];
        }
        int clipped_count = shadow_clip_polygon(clipped, 3);
        if (clipped_count < 3)
            continue;

        for (int tri = 1; tri + 1 < clipped_count; tri++) {
            const shadow_clip_vertex_t *fan[3] = {&clipped[0], &clipped[tri], &clipped[tri + 1]};
            sw_shadow_screen_tri_t out_tri;
            int ok = 1;
            memset(&out_tri, 0, sizeof(out_tri));
            for (int vi = 0; vi < 3; vi++) {
                float w = fan[vi]->clip[3];
                if (w <= 1e-7f || !isfinite(w)) {
                    ok = 0;
                    break;
                }
                float iw = 1.0f / w;
                float ndc_x = fan[vi]->clip[0] * iw;
                float ndc_y = fan[vi]->clip[1] * iw;
                float ndc_z = fan[vi]->clip[2] * iw;
                if (!isfinite(ndc_x) || !isfinite(ndc_y) || !isfinite(ndc_z)) {
                    ok = 0;
                    break;
                }
                out_tri.sx[vi] = (ndc_x + 1.0f) * half_w;
                out_tri.sy[vi] = (1.0f - ndc_y) * half_h;
                out_tri.sz[vi] = ndc_z * 0.5f + 0.5f;
                if (out_tri.sz[vi] < 0.0f || out_tri.sz[vi] > 1.0f) {
                    ok = 0;
                    break;
                }
                out_tri.u[vi] = fan[vi]->uv[0];
                out_tri.v[vi] = fan[vi]->uv[1];
                out_tri.u1[vi] = fan[vi]->uv1[0];
                out_tri.v1[vi] = fan[vi]->uv1[1];
                out_tri.alpha[vi] = fan[vi]->color_alpha;
                out_tri.inv_w[vi] = iw;
            }
            if (ok && !sw_shadow_tri_list_push(out, &out_tri))
                return 0;
        }
    }
    return 1;
}

/// @brief Thread-pool callback that collects one assigned source-triangle range.
/// @param arg Borrowed pointer to a @c sw_shadow_collect_task_t valid through the completion
///            barrier.
static void sw_shadow_collect_task_run(void *arg) {
    sw_shadow_collect_task_t *task = (sw_shadow_collect_task_t *)arg;
    if (!task)
        return;
    if (!sw_shadow_collect_triangles_range(task->cmd,
                                           task->tri_begin,
                                           task->tri_end,
                                           task->lmvp,
                                           task->half_w,
                                           task->half_h,
                                           &task->out)) {
        rt_atomic_store_i32(task->failed, 1, __ATOMIC_RELAXED);
    }
}

/// @brief Collect clipped screen-space shadow triangles across the context worker pool.
/// @details Partitions source triangles into deterministic contiguous ranges, runs unsent tasks
///          synchronously if submission stops, then concatenates results in task order.
/// @param ctx Borrowed software context containing the worker pool.
/// @param cmd Borrowed sanitized indexed draw command.
/// @param index_count Validated index count, rounded to complete triangles by the caller.
/// @param lmvp Borrowed row-major light-model-view-projection matrix.
/// @param half_w Half the shadow-map width used for viewport conversion.
/// @param half_h Half the shadow-map height used for viewport conversion.
/// @param out Mutable list receiving all collected triangles.
/// @return 1 on complete collection, otherwise 0 when parallel prerequisites or allocation fail.
static int sw_shadow_collect_triangles_parallel(sw_context_t *ctx,
                                                const vgfx3d_draw_cmd_t *cmd,
                                                uint32_t index_count,
                                                const float *lmvp,
                                                float half_w,
                                                float half_h,
                                                sw_shadow_tri_list_t *out) {
    sw_shadow_collect_task_t tasks[SW_MAX_TASKS];
    uint32_t tri_count;
    int32_t task_count;
    int32_t submitted = 0;
    int32_t failed = 0;
    if (!ctx || !ctx->worker_pool || ctx->worker_count <= 1 || !cmd || !lmvp || !out)
        return 0;
    tri_count = index_count / 3u;
    if (tri_count == 0)
        return 1;
    task_count = (int32_t)(ctx->worker_count + 1 < (int64_t)tri_count ? ctx->worker_count + 1
                                                                      : (int64_t)tri_count);
    memset(tasks, 0, sizeof(tasks));
    for (int32_t i = 0; i < task_count; i++) {
        tasks[i].cmd = cmd;
        tasks[i].tri_begin = (uint32_t)(((uint64_t)tri_count * (uint64_t)i) / (uint64_t)task_count);
        tasks[i].tri_end =
            (uint32_t)(((uint64_t)tri_count * (uint64_t)(i + 1)) / (uint64_t)task_count);
        tasks[i].lmvp = lmvp;
        tasks[i].half_w = half_w;
        tasks[i].half_h = half_h;
        tasks[i].failed = &failed;
        tasks[i].out.items = (sw_shadow_screen_tri_t *)ctx->shadow_task_triangle_scratch[i];
        tasks[i].out.capacity = ctx->shadow_task_triangle_scratch_capacity[i];
        ctx->shadow_task_triangle_scratch[i] = NULL;
        ctx->shadow_task_triangle_scratch_capacity[i] = 0;
    }
    for (int32_t i = 1; i < task_count; i++) {
        if (!rt_threadpool_submit_fn(ctx->worker_pool, sw_shadow_collect_task_run, &tasks[i]))
            break;
        submitted++;
    }
    for (int32_t i = submitted + 1; i < task_count; i++)
        sw_shadow_collect_task_run(&tasks[i]);
    sw_shadow_collect_task_run(&tasks[0]);
    if (submitted > 0)
        rt_threadpool_wait(ctx->worker_pool);
    if (rt_atomic_load_i32(&failed, __ATOMIC_RELAXED)) {
        sw_shadow_retain_task_scratch(ctx, tasks, task_count);
        return 0;
    }

    uint32_t total = 0;
    for (int32_t i = 0; i < task_count; i++) {
        if (UINT32_MAX - total < tasks[i].out.count) {
            sw_shadow_retain_task_scratch(ctx, tasks, task_count);
            return 0;
        }
        total += tasks[i].out.count;
    }
    if (!sw_shadow_tri_list_reserve(out, total)) {
        sw_shadow_retain_task_scratch(ctx, tasks, task_count);
        return 0;
    }
    for (int32_t i = 0; i < task_count; i++) {
        if (tasks[i].out.count > 0) {
            memcpy(&out->items[out->count],
                   tasks[i].out.items,
                   (size_t)tasks[i].out.count * sizeof(sw_shadow_screen_tri_t));
            out->count += tasks[i].out.count;
        }
    }
    sw_shadow_retain_task_scratch(ctx, tasks, task_count);
    return 1;
}

/// @brief Thread-pool callback that claims active tiles and rasterizes their triangle bins.
/// @param arg Borrowed pointer to shared @c sw_shadow_raster_task_t state valid through the
///            completion barrier.
static void sw_shadow_raster_task_run(void *arg) {
    sw_shadow_raster_task_t *task = (sw_shadow_raster_task_t *)arg;
    if (!task || !task->grid || !task->tris || !task->next_tile)
        return;
    for (;;) {
        int32_t tile_index = rt_atomic_fetch_add_i32(task->next_tile, 1, __ATOMIC_RELAXED);
        if (tile_index >= task->grid->active_count)
            break;
        tile_index = task->grid->active_indices[tile_index];
        int32_t tx = tile_index % task->grid->cols;
        int32_t ty = tile_index / task->grid->cols;
        int32_t min_x = tx * SW_TILE_SIZE;
        int32_t min_y = ty * SW_TILE_SIZE;
        int32_t max_x = min_x + SW_TILE_SIZE - 1;
        int32_t max_y = min_y + SW_TILE_SIZE - 1;
        const sw_shadow_tile_bin_t *bin = &task->grid->bins[tile_index];
        if (max_x >= task->sw)
            max_x = task->sw - 1;
        if (max_y >= task->sh)
            max_y = task->sh - 1;
        for (uint32_t i = 0; i < bin->count; i++) {
            const sw_shadow_screen_tri_t *tri = &task->tris->items[bin->indices[i]];
            float sx[3], sy[3], sz[3], u[3], v[3], u1[3], v1[3], alpha[3], inv_w[3];
            memcpy(sx, tri->sx, sizeof(sx));
            memcpy(sy, tri->sy, sizeof(sy));
            memcpy(sz, tri->sz, sizeof(sz));
            memcpy(u, tri->u, sizeof(u));
            memcpy(v, tri->v, sizeof(v));
            memcpy(u1, tri->u1, sizeof(u1));
            memcpy(v1, tri->v1, sizeof(v1));
            memcpy(alpha, tri->alpha, sizeof(alpha));
            memcpy(inv_w, tri->inv_w, sizeof(inv_w));
            shadow_raster_tri_scissored(task->depth,
                                        task->sw,
                                        task->sh,
                                        sx,
                                        sy,
                                        sz,
                                        u,
                                        v,
                                        u1,
                                        v1,
                                        alpha,
                                        inv_w,
                                        task->cmd,
                                        task->alpha_tex,
                                        min_x,
                                        min_y,
                                        max_x,
                                        max_y);
        }
    }
}

/// @brief Rasterize all active shadow tiles using dynamically claimed worker tasks.
/// @param ctx Borrowed software context containing the worker pool.
/// @param grid Borrowed binned tile grid.
/// @param tris Borrowed collected screen-space triangles referenced by the bins.
/// @param depth Borrowed mutable shadow depth buffer.
/// @param sw Positive shadow-map width in pixels.
/// @param sh Positive shadow-map height in pixels.
/// @param cmd Borrowed sanitized draw command supplying alpha-mask state.
/// @param alpha_tex Optional borrowed base-color view for cutout shadow sampling.
/// @return 1 after all active tiles finish, otherwise 0 when prerequisites are unavailable.
static int sw_shadow_raster_tiles_parallel(sw_context_t *ctx,
                                           const sw_shadow_tile_grid_t *grid,
                                           const sw_shadow_tri_list_t *tris,
                                           float *depth,
                                           int32_t sw,
                                           int32_t sh,
                                           const vgfx3d_draw_cmd_t *cmd,
                                           const sw_pixels_view *alpha_tex) {
    sw_shadow_raster_task_t tasks[SW_MAX_TASKS];
    int32_t next_tile = 0;
    int32_t task_count;
    int32_t submitted = 0;
    if (!ctx || !ctx->worker_pool || !grid || grid->active_count <= 0 || !tris || !depth)
        return 0;
    task_count = (int32_t)(ctx->worker_count + 1 < (int64_t)grid->active_count
                               ? ctx->worker_count + 1
                               : (int64_t)grid->active_count);
    for (int32_t i = 0; i < task_count; i++) {
        tasks[i].grid = grid;
        tasks[i].tris = tris;
        tasks[i].next_tile = &next_tile;
        tasks[i].depth = depth;
        tasks[i].sw = sw;
        tasks[i].sh = sh;
        tasks[i].cmd = cmd;
        tasks[i].alpha_tex = alpha_tex;
    }
    for (int32_t i = 1; i < task_count; i++) {
        if (!rt_threadpool_submit_fn(ctx->worker_pool, sw_shadow_raster_task_run, &tasks[i]))
            break;
        submitted++;
    }
    for (int32_t i = submitted + 1; i < task_count; i++)
        sw_shadow_raster_task_run(&tasks[i]);
    sw_shadow_raster_task_run(&tasks[0]);
    if (submitted > 0)
        rt_threadpool_wait(ctx->worker_pool);
    return 1;
}

/// @brief Attempt the complete parallel collection, binning, and rasterization path for one draw.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param cmd Borrowed sanitized indexed draw command.
/// @return 1 when the draw was fully handled, including valid no-output cases; otherwise 0 so the
///         caller can use the serial path.
static int sw_shadow_draw_parallel(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    int32_t slot;
    sw_pixels_view alpha_view;
    sw_pixels_view *alpha_tex = NULL;
    uint32_t index_count;
    float lmvp[16];
    float half_w, half_h;
    sw_shadow_tri_list_t tris = {0};
    sw_shadow_tile_grid_t *grid = NULL;
    int ok = 0;

    if (!ctx || !ctx->worker_pool || ctx->worker_count <= 1)
        return 0;
    if (rt_threadpool_current_worker_pool() == ctx->worker_pool)
        return 0;
    if (!cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 || cmd->index_count == 0)
        return 0;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count < SW_PARALLEL_MIN_INDICES)
        return 0;
    slot = ctx->shadow_pass_slot;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS || !ctx->shadow_depth[slot])
        return 0;
    if ((cmd->additive_blend || vgfx3d_draw_cmd_uses_alpha_blend(cmd)) &&
        cmd->shadow_mode != RT_MATERIAL3D_SHADOW_MODE_CAST)
        return 0;
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK && cmd->texture) {
        if (setup_pixels_view(cmd->texture, &alpha_view))
            alpha_tex = &alpha_view;
    }
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK && cmd->texture_asset && !alpha_tex)
        return 0;

    mat4f_mul(ctx->shadow_vp[slot], cmd->model_matrix, lmvp);
    half_w = (float)ctx->shadow_w[slot] * 0.5f;
    half_h = (float)ctx->shadow_h[slot] * 0.5f;

    tris.items = (sw_shadow_screen_tri_t *)ctx->shadow_triangle_scratch;
    tris.capacity = ctx->shadow_triangle_scratch_capacity;
    ctx->shadow_triangle_scratch = NULL;
    ctx->shadow_triangle_scratch_capacity = 0;
    if (!sw_shadow_collect_triangles_parallel(ctx, cmd, index_count, lmvp, half_w, half_h, &tris))
        goto cleanup;
    if (tris.count == 0) {
        ok = 1;
        goto cleanup;
    }
    if (!ctx->shadow_tile_grid_scratch) {
        ctx->shadow_tile_grid_scratch = calloc(1, sizeof(sw_shadow_tile_grid_t));
        if (!ctx->shadow_tile_grid_scratch)
            goto cleanup;
    }
    grid = (sw_shadow_tile_grid_t *)ctx->shadow_tile_grid_scratch;
    if (!sw_shadow_tile_grid_prepare(grid, ctx->shadow_w[slot], ctx->shadow_h[slot]))
        goto cleanup;
    if (!sw_shadow_bin_triangles(grid, &tris, ctx->shadow_w[slot], ctx->shadow_h[slot]))
        goto cleanup;
    if (grid->active_count == 0) {
        ok = 1;
        goto cleanup;
    }
    if (!sw_shadow_raster_tiles_parallel(ctx,
                                         grid,
                                         &tris,
                                         ctx->shadow_depth[slot],
                                         ctx->shadow_w[slot],
                                         ctx->shadow_h[slot],
                                         cmd,
                                         alpha_tex))
        goto cleanup;
    ok = 1;

cleanup:
    ctx->shadow_triangle_scratch = tris.items;
    ctx->shadow_triangle_scratch_capacity = tris.capacity;
    return ok;
}

/// @brief Backend `shadow_begin` op — allocate / clear the shadow depth buffer.
///
/// Caller-provided depth buffer (so the canvas owns the storage and
/// can resize without consulting the backend). Captures the light VP
/// matrix that `shadow_draw` will use.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param slot Shadow-resource array slot to arm.
/// @param depth_buf Borrowed canvas-owned depth storage for @p w times @p h floats.
/// @param w Positive shadow-map width in pixels.
/// @param h Positive shadow-map height in pixels.
/// @param light_vp Borrowed 16-float row-major light view-projection matrix.
static void sw_shadow_begin(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    size_t pixel_count;
    if (!ctx)
        return;
    ctx->shadow_pass_slot = -1;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return;
    ctx->shadow_complete[slot] = 0;
    if (!depth_buf || !vgfx3d_shadow_matrix_is_usable(light_vp) || w <= 0 || h <= 0) {
        sw_recompute_shadow_count(ctx);
        return;
    }
    if ((size_t)w > SIZE_MAX / (size_t)h) {
        sw_recompute_shadow_count(ctx);
        return;
    }
    pixel_count = (size_t)w * (size_t)h;
    if (pixel_count > (size_t)INT32_MAX) {
        sw_recompute_shadow_count(ctx);
        return;
    }
    ctx->shadow_pass_slot = (int8_t)slot;
    ctx->shadow_depth[slot] = depth_buf;
    ctx->shadow_w[slot] = w;
    ctx->shadow_h[slot] = h;
    memcpy(ctx->shadow_vp[slot], light_vp, 16 * sizeof(float));
    /* Clear shadow depth to FLT_MAX */
    sw_fill_depth_buffer(depth_buf, pixel_count, FLT_MAX);
}

/// @brief Backend `shadow_draw` op — rasterize one mesh into the shadow depth buffer.
///
/// Same vertex transform path as `submit_draw` (model matrix + light VP)
/// but feeds each triangle to `shadow_raster_tri` instead of the full
/// rasterizer. Triangles are clipped in homogeneous light clip-space before
/// perspective divide so near-plane crossings cannot poison the shadow map.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param cmd Borrowed sanitized indexed draw command to cast into the active shadow slot.
static void sw_shadow_draw_serial(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    int32_t slot;
    sw_pixels_view alpha_view;
    sw_pixels_view *alpha_tex = NULL;
    uint32_t index_count;
    if (!ctx || !cmd || !cmd->vertices || !cmd->indices || cmd->vertex_count == 0 ||
        cmd->index_count == 0)
        return;
    index_count = vgfx3d_draw_cmd_validated_index_count(cmd);
    if (index_count == 0)
        return;
    slot = ctx->shadow_pass_slot;
    if (slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS || !ctx->shadow_depth[slot])
        return;
    /* Skip transparent objects unless Canvas3D explicitly converted them to shadow cutouts. */
    if ((cmd->additive_blend || vgfx3d_draw_cmd_uses_alpha_blend(cmd)) &&
        cmd->shadow_mode != RT_MATERIAL3D_SHADOW_MODE_CAST)
        return;
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK && cmd->texture) {
        if (setup_pixels_view(cmd->texture, &alpha_view))
            alpha_tex = &alpha_view;
    }
    if (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK && cmd->texture_asset && !alpha_tex) {
        /* Native-only cutout textures cannot be sampled by the software shadow pass. Skipping is
         * conservative; casting the full mesh would create backend-specific solid shadow flicker.
         */
        return;
    }

    /* Build light MVP = shadow_vp * model */
    float lmvp[16];
    mat4f_mul(ctx->shadow_vp[slot], cmd->model_matrix, lmvp);

    float half_w = (float)ctx->shadow_w[slot] * 0.5f;
    float half_h = (float)ctx->shadow_h[slot] * 0.5f;

    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint32_t i0 = cmd->indices[i], i1 = cmd->indices[i + 1], i2 = cmd->indices[i + 2];
        if (i0 >= cmd->vertex_count || i1 >= cmd->vertex_count || i2 >= cmd->vertex_count)
            continue;

        shadow_clip_vertex_t clipped[16];
        for (int vi = 0; vi < 3; vi++) {
            const uint32_t idx[3] = {i0, i1, i2};
            const vgfx3d_vertex_t *v = &cmd->vertices[idx[vi]];
            float pos4[4] = {v->pos[0], v->pos[1], v->pos[2], 1.0f};
            mat4f_transform4(lmvp, pos4, clipped[vi].clip);
            clipped[vi].uv[0] = v->uv[0];
            clipped[vi].uv[1] = v->uv[1];
            clipped[vi].uv1[0] = v->uv1[0];
            clipped[vi].uv1[1] = v->uv1[1];
            clipped[vi].color_alpha = v->color[3];
        }
        int clipped_count = shadow_clip_polygon(clipped, 3);
        if (clipped_count < 3)
            continue;

        for (int tri = 1; tri + 1 < clipped_count; tri++) {
            const shadow_clip_vertex_t *fan[3] = {&clipped[0], &clipped[tri], &clipped[tri + 1]};
            float screen_x[3], screen_y[3], screen_z[3];
            float tex_u[3], tex_v[3], tex_u1[3], tex_v1[3], vert_alpha[3], inv_w[3];
            int ok = 1;
            for (int vi = 0; vi < 3; vi++) {
                float w = fan[vi]->clip[3];
                if (w <= 1e-7f || !isfinite(w)) {
                    ok = 0;
                    break;
                }
                float iw = 1.0f / w;
                float ndc_x = fan[vi]->clip[0] * iw;
                float ndc_y = fan[vi]->clip[1] * iw;
                float ndc_z = fan[vi]->clip[2] * iw;
                if (!isfinite(ndc_x) || !isfinite(ndc_y) || !isfinite(ndc_z)) {
                    ok = 0;
                    break;
                }
                screen_x[vi] = (ndc_x + 1.0f) * half_w;
                screen_y[vi] = (1.0f - ndc_y) * half_h;
                screen_z[vi] = ndc_z * 0.5f + 0.5f;
                if (screen_z[vi] < 0.0f || screen_z[vi] > 1.0f) {
                    ok = 0;
                    break;
                }
                tex_u[vi] = fan[vi]->uv[0];
                tex_v[vi] = fan[vi]->uv[1];
                tex_u1[vi] = fan[vi]->uv1[0];
                tex_v1[vi] = fan[vi]->uv1[1];
                vert_alpha[vi] = fan[vi]->color_alpha;
                inv_w[vi] = iw;
            }
            if (!ok)
                continue;
            shadow_raster_tri(ctx->shadow_depth[slot],
                              ctx->shadow_w[slot],
                              ctx->shadow_h[slot],
                              screen_x,
                              screen_y,
                              screen_z,
                              tex_u,
                              tex_v,
                              tex_u1,
                              tex_v1,
                              vert_alpha,
                              inv_w,
                              cmd,
                              alpha_tex);
        }
    }
}

/// @brief Sanitize and rasterize one shadow-casting draw through the parallel or serial path.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param cmd Borrowed indexed draw command; null is a no-op.
static void sw_shadow_draw(void *ctx_ptr, const vgfx3d_draw_cmd_t *cmd) {
    vgfx3d_draw_cmd_t safe_cmd;
    if (!cmd)
        return;
    sw_sanitize_draw_command(cmd, &safe_cmd);
    if (sw_shadow_draw_parallel(ctx_ptr, &safe_cmd))
        return;
    sw_shadow_draw_serial(ctx_ptr, &safe_cmd);
}

/// @brief Backend `shadow_end` op — mark shadow map ready and store the bias.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param slot Shadow-resource slot whose active pass is ending.
/// @param bias Requested constant comparison bias, clamped to the backend-safe range.
static void sw_shadow_end(void *ctx_ptr, int32_t slot, float bias) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    if (!ctx || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return;
    if (ctx->shadow_pass_slot != slot)
        return;
    ctx->shadow_bias[slot] = vgfx3d_clamp_float_param(bias, -0.05f, 0.05f, 0.0f);
    ctx->shadow_complete[slot] =
        (ctx->shadow_depth[slot] && ctx->shadow_w[slot] > 0 && ctx->shadow_h[slot] > 0) ? 1 : 0;
    ctx->shadow_pass_slot = -1;
    sw_recompute_shadow_count(ctx);
}

/// @brief Backend `shadow_reuse` op — re-arm a slot from its previous-frame depth.
///
/// The depth buffer is canvas-owned malloc storage that persists across frames,
/// so when Canvas3D proves the slot's contents are unchanged it only needs the
/// per-frame slot registration (pointer/size/VP + complete flag) redone without
/// the FLT_MAX clear or any rasterization.
/// @param ctx_ptr Opaque borrowed pointer to the active @c sw_context_t.
/// @param slot Shadow-resource array slot to re-arm.
/// @param depth_buf Borrowed canvas-owned prior-frame depth storage.
/// @param w Positive shadow-map width in pixels.
/// @param h Positive shadow-map height in pixels.
/// @param light_vp Borrowed 16-float row-major light view-projection matrix.
/// @return 1 when the prior depth buffer is registered as complete, otherwise 0.
static int8_t sw_shadow_reuse(
    void *ctx_ptr, int32_t slot, float *depth_buf, int32_t w, int32_t h, const float *light_vp) {
    sw_context_t *ctx = (sw_context_t *)ctx_ptr;
    if (!ctx || slot < 0 || slot >= VGFX3D_MAX_SHADOW_LIGHTS)
        return 0;
    if (!depth_buf || !vgfx3d_shadow_matrix_is_usable(light_vp) || w <= 0 || h <= 0)
        return 0;
    if ((size_t)w > SIZE_MAX / (size_t)h || (size_t)w * (size_t)h > (size_t)INT32_MAX)
        return 0;
    ctx->shadow_depth[slot] = depth_buf;
    ctx->shadow_w[slot] = w;
    ctx->shadow_h[slot] = h;
    memcpy(ctx->shadow_vp[slot], light_vp, 16 * sizeof(float));
    ctx->shadow_complete[slot] = 1;
    sw_recompute_shadow_count(ctx);
    return 1;
}
