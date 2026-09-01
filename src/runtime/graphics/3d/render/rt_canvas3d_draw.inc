//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_draw.inc
// Purpose: Canvas3D rendering dispatch for clears, screen overlays, mesh queueing,
//   retained geometry binding, camera-relative rebasing, and text submission.
// Key invariants:
//   - Included only by rt_canvas3d.c; this is not a standalone translation unit.
//   - Heap mesh commands bind immutable retained revisions before publication.
//   - Rebased, skinned, and otherwise dynamic geometry keeps an explicit frame lifetime.
// Ownership/Lifetime:
//   - Draw-resource references and native geometry revisions remain owned by the
//     Canvas3D frame until backend submission and cleanup finish.
// Links: rt_canvas3d.c, rt_canvas3d_internal.h, rt_canvas3d_snapshot.c,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Canvas3D clear, overlay, camera-begin, and mesh-draw entry points.
/// @details This implementation fragment is included by `rt_canvas3d.c`. It snapshots or retains
/// every payload needed after a draw call returns, so queued backend commands never depend on
/// caller mutation or temporary geometry lifetime.

/*==========================================================================
 * Rendering — dispatches through backend vtable
 *=========================================================================*/

/// @brief Return whether a GPU clear also needs the legacy CPU window mirror cleared.
///
/// @details Native backends with `readback_rgba` own screenshot/capture data, so repainting
///          the window's CPU framebuffer on every `Clear` only burns memory bandwidth. Custom
///          non-software backends without readback still get the mirror for compatibility with
///          `Screenshot` fallback behavior.
/// @param c Borrowed canvas whose backend and render-target mode are inspected.
/// @return Non-zero only for a windowed non-software backend without native readback.
static int canvas3d_clear_needs_cpu_window_mirror(const rt_canvas3d *c) {
    return c && c->backend && c->backend != &vgfx3d_software_backend && !c->render_target &&
           !c->backend->readback_rgba;
}

/// @brief Clear the framebuffer and depth buffer with the given background color.
/// @details Must be called at the start of each frame before Begin. Fog and
///          ambient light persist until explicitly changed.
/// @param obj Borrowed Canvas3D handle.
/// @param r Normalized red clear channel.
/// @param g Normalized green clear channel.
/// @param b Normalized blue clear channel.
void rt_canvas3d_clear(void *obj, double r, double g, double b) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->backend || (!c->gfx_win && !c->render_target))
        return;
    if (c->frame_finalized)
        canvas3d_reset_finalized_frame_state(c);
    c->cpu_finalized_this_render = 0;
    float cr = canvas3d_clamp01_f64(r);
    float cg = canvas3d_clamp01_f64(g);
    float cb = canvas3d_clamp01_f64(b);
    if (c->backend->clear)
        c->backend->clear(c->backend_ctx, c->gfx_win, cr, cg, cb);

    /* Only maintain the legacy CPU mirror when there is no backend readback hook. */
    if (canvas3d_clear_needs_cpu_window_mirror(c)) {
        vgfx_framebuffer_t fb;
        if (vgfx_get_framebuffer(c->gfx_win, &fb) && fb.pixels &&
            canvas3d_rgba8_stride_valid(fb.width, fb.height, fb.stride)) {
            uint8_t r8 = canvas3d_clamp01_to_u8(cr);
            uint8_t g8 = canvas3d_clamp01_to_u8(cg);
            uint8_t b8 = canvas3d_clamp01_to_u8(cb);
            size_t row_bytes = (size_t)fb.width * 4u;
            uint8_t *first_row = fb.pixels;
            for (int32_t x = 0; x < fb.width; x++) {
                uint8_t *px = &first_row[(size_t)x * 4u];
                px[0] = r8;
                px[1] = g8;
                px[2] = b8;
                px[3] = 0xFF;
            }
            for (int32_t y = 1; y < fb.height; y++) {
                uint8_t *row = &fb.pixels[(size_t)y * (size_t)fb.stride];
                memcpy(row, first_row, row_bytes);
            }
        }
    }
}

/// @brief Decide which deferred pass owns 2D screen-space draws based on canvas mode.
/// @param c Borrowed canvas.
/// @return Main pass during an explicit 2D frame; otherwise final screen-overlay pass.
static deferred_pass_t canvas3d_screen_pass_kind(const rt_canvas3d *c) {
    return (c && c->frame_is_2d) ? DEFERRED_PASS_MAIN : DEFERRED_PASS_SCREEN_OVERLAY;
}

/// @brief Compute a robust finite-length estimate for a 2D screen-space vector.
/// @details The scale-first form avoids overflowing `dx * dx + dy * dy` for large but finite
///          coordinates while depending only on `sqrtf`, which is already part of the native
///          runtime symbol surface. Infinite or NaN intermediate values are returned to the caller
///          so degenerate line fallback handling remains centralized.
/// @param dx X component of the vector.
/// @param dy Y component of the vector.
/// @return Euclidean length when finite, zero for a zero vector, or a non-finite value when the
///         input subtraction overflowed.
static float canvas3d_screen_vector_len(float dx, float dy) {
    float ax = dx < 0.0f ? -dx : dx;
    float ay = dy < 0.0f ? -dy : dy;
    float scale = ax > ay ? ax : ay;
    float sx;
    float sy;
    if (!isfinite(scale) || scale <= 0.0f)
        return scale;
    sx = dx / scale;
    sy = dy / scale;
    return scale * sqrtf(sx * sx + sy * sy);
}

/// @brief Create the deferred draw command used for final-overlay screen geometry.
/// @details Screen-space helper vertices already carry their tint in vertex colors.
///          Final-overlay replay happens through the backend mesh path after post-FX, so the
///          material diffuse color must stay neutral or the overlay tint is multiplied twice.
///          The alpha scalar is also neutralized for this replay command because the vertex alpha
///          remains the single source of overlay opacity. Neutralizing it would silently
///          reclassify a translucent overlay as opaque — GPU backends choose their pipeline
///          blend state from the command scalars, never from vertex alpha — so the original
///          command's blend classification is pinned first (NL-ZANNA-001).
/// @param cmd Prepared screen-geometry command whose vertices and indices are retained elsewhere.
/// @return A copy of `cmd` with neutral diffuse color and alpha suitable for final-overlay replay.
static vgfx3d_draw_cmd_t canvas3d_make_final_overlay_screen_cmd(const vgfx3d_draw_cmd_t *cmd) {
    vgfx3d_draw_cmd_t overlay_cmd = *cmd;
    if (vgfx3d_draw_cmd_uses_alpha_blend(cmd))
        overlay_cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    overlay_cmd.diffuse_color[0] = 1.0f;
    overlay_cmd.diffuse_color[1] = 1.0f;
    overlay_cmd.diffuse_color[2] = 1.0f;
    overlay_cmd.diffuse_color[3] = 1.0f;
    overlay_cmd.alpha = 1.0f;
    return overlay_cmd;
}

/// @brief Allocate transient screen-geometry storage for overlay draw commands.
/// @details Final-overlay draws first try the retained arena so common HUD primitives avoid one
/// malloc/free pair per command while still keeping pointers stable through post-FX replay. If the
/// retained arena cannot satisfy the request without moving prior commands, this falls back to the
/// existing malloc-backed temp-buffer path. Normal overlay draws always use malloc so their
/// lifetime remains tied to the regular per-frame temp cleanup.
/// @param c Canvas that owns the overlay lifetime state.
/// @param bytes Combined vertex and index byte count to allocate.
/// @param final_overlay Non-zero when the command is being recorded for final-overlay replay.
/// @param out_from_final_overlay_arena Receives non-zero when the returned pointer is arena-owned.
/// @return Writable storage for the draw command, or NULL on allocation failure.
static uint8_t *canvas3d_alloc_screen_geometry_block(rt_canvas3d *c,
                                                     size_t bytes,
                                                     int final_overlay,
                                                     int *out_from_final_overlay_arena) {
    uint8_t *block;
    if (out_from_final_overlay_arena)
        *out_from_final_overlay_arena = 0;
    if (bytes == 0u)
        return NULL;
    if (final_overlay) {
        block = (uint8_t *)canvas3d_alloc_final_overlay_arena(c, bytes, sizeof(void *));
        if (block) {
            if (out_from_final_overlay_arena)
                *out_from_final_overlay_arena = 1;
            return block;
        }
    }
    return (uint8_t *)malloc(bytes);
}

/// @brief Append a 2D triangle list (positions + UVs + color) to the deferred queue.
///
/// Used as the underlying primitive for `canvas3d_queue_screen_rect`
/// and `canvas3d_queue_screen_line`. Manages the auto-grow of the
/// command buffer and decides whether the geometry lands in the
/// pre-3D, post-3D, or HUD pass via `canvas3d_screen_pass_kind`.
/// @param c Canvas with an active compatible frame.
/// @param vertices Borrowed source vertex array copied into frame-owned storage.
/// @param vertex_count Positive number of vertices.
/// @param indices Borrowed source triangle-index array copied into frame-owned storage.
/// @param index_count Positive number of indices.
/// @param r Red tint channel stored in the queued vertex payload.
/// @param g Green tint channel.
/// @param b Blue tint channel.
/// @param a Alpha channel and command opacity.
/// @return Non-zero when the geometry was published with correct frame ownership; zero on invalid
/// input, overflow, allocation, tracking, or queue failure.
static int canvas3d_queue_screen_geometry(rt_canvas3d *c,
                                          const vgfx3d_vertex_t *vertices,
                                          int32_t vertex_count,
                                          const uint32_t *indices,
                                          int32_t index_count,
                                          float r,
                                          float g,
                                          float b,
                                          float a) {
    size_t vertex_bytes;
    size_t index_bytes;
    size_t total_bytes;
    uint8_t *block;
    vgfx3d_vertex_t *verts_copy;
    uint32_t *indices_copy;
    vgfx3d_draw_cmd_t cmd;
    int block_from_final_overlay_arena = 0;

    if (!c || !c->in_frame || !vertices || vertex_count <= 0 || !indices || index_count <= 0)
        return 0;
    if ((size_t)vertex_count > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
        (size_t)index_count > SIZE_MAX / sizeof(uint32_t))
        return 0;
    vertex_bytes = (size_t)vertex_count * sizeof(vgfx3d_vertex_t);
    index_bytes = (size_t)index_count * sizeof(uint32_t);
    if (vertex_bytes > SIZE_MAX - index_bytes)
        return 0;
    total_bytes = vertex_bytes + index_bytes;
    block = canvas3d_alloc_screen_geometry_block(
        c, total_bytes, c->final_overlay_recording, &block_from_final_overlay_arena);
    if (!block)
        return 0;
    verts_copy = (vgfx3d_vertex_t *)block;
    indices_copy = (uint32_t *)(block + vertex_bytes);
    memcpy(verts_copy, vertices, vertex_bytes);
    memcpy(indices_copy, indices, index_bytes);

    memset(&cmd, 0, sizeof(cmd));
    cmd.vertices = verts_copy;
    cmd.vertex_count = (uint32_t)vertex_count;
    cmd.indices = indices_copy;
    cmd.index_count = (uint32_t)index_count;
    cmd.validated_index_count = cmd.index_count;
    cmd.model_matrix[0] = cmd.model_matrix[5] = cmd.model_matrix[10] = cmd.model_matrix[15] = 1.0f;
    cmd.diffuse_color[0] = 1.0f;
    cmd.diffuse_color[1] = 1.0f;
    cmd.diffuse_color[2] = 1.0f;
    cmd.diffuse_color[3] = 1.0f;
    /* Vertex alpha is the single source of overlay opacity: every fragment
     * stage multiplies diffuse.a * cmd.alpha * vertex.a, so carrying the
     * opacity here as well double-darkened translucent overlays. A
     * translucent queue must also classify as BLEND, or fragment stages
     * force OPAQUE-mode alpha to 1 and composite it fully opaque while the
     * pipeline blend classifier simultaneously infers blending from the
     * scalar — the NL-ZANNA-001 opaque-scrim defect. */
    cmd.alpha = 1.0f;
    if (a < 0.999f)
        cmd.alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
    cmd.unlit = 1;
    cmd.disable_depth_test = 1;

    if (c->final_overlay_recording) {
        vgfx3d_draw_cmd_t overlay_cmd = canvas3d_make_final_overlay_screen_cmd(&cmd);
        if (!block_from_final_overlay_arena &&
            !canvas3d_track_final_overlay_temp_buffer(c, block)) {
            free(block);
            return 0;
        }
        if (!canvas3d_enqueue_final_overlay_draw(c, &overlay_cmd, NULL, NULL)) {
            if (!block_from_final_overlay_arena)
                canvas3d_release_tracked_final_overlay_temp_buffer(c, block);
            return 0;
        }
        return 1;
    }

    if (!canvas3d_track_temp_buffer(c, block)) {
        free(block);
        return 0;
    }

    if (!canvas3d_enqueue_draw(c,
                               &cmd,
                               DEFERRED_DRAW_MESH,
                               canvas3d_screen_pass_kind(c),
                               NULL,
                               0,
                               0,
                               0,
                               0,
                               0.0f,
                               NULL,
                               NULL)) {
        canvas3d_release_tracked_temp_buffer(c, block);
        return 0;
    }
    return 1;
}

/// @brief Intersect a screen-space rect with the overlay clip rect (Plan 08).
/// @param c Borrowed canvas providing the optional clip rectangle.
/// @param[in,out] x Rectangle left edge, trimmed in place.
/// @param[in,out] y Rectangle top edge, trimmed in place.
/// @param[in,out] w Rectangle width, trimmed in place.
/// @param[in,out] h Rectangle height, trimmed in place.
/// @return 0 when the rect is fully clipped away, 1 when (possibly trimmed) drawable.
static int canvas3d_overlay_clip_rect(
    const rt_canvas3d *c, float *x, float *y, float *w, float *h) {
    float x0;
    float y0;
    float x1;
    float y1;
    if (!c || !c->overlay_clip_active)
        return 1;
    x0 = *x > c->overlay_clip_x ? *x : c->overlay_clip_x;
    y0 = *y > c->overlay_clip_y ? *y : c->overlay_clip_y;
    x1 = (*x + *w) < (c->overlay_clip_x + c->overlay_clip_w)
             ? (*x + *w)
             : (c->overlay_clip_x + c->overlay_clip_w);
    y1 = (*y + *h) < (c->overlay_clip_y + c->overlay_clip_h)
             ? (*y + *h)
             : (c->overlay_clip_y + c->overlay_clip_h);
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return 1;
}

/// @brief Linearly interpolate every attribute in a screen-space overlay vertex.
/// @details Rounded-rectangle clipping intersects generated fan triangles against the active
/// overlay clip rectangle. Interpolating the complete vertex keeps future per-vertex overlay
/// attributes correct even though current rounded rectangles use only position, normal, and color.
/// @param a First endpoint.
/// @param b Second endpoint.
/// @param t Interpolation factor in the closed range [0, 1].
/// @return The interpolated vertex.
static vgfx3d_vertex_t canvas3d_lerp_screen_vertex(const vgfx3d_vertex_t *a,
                                                   const vgfx3d_vertex_t *b,
                                                   float t) {
    vgfx3d_vertex_t out = *a;
    float inv_t = 1.0f - t;
    for (int i = 0; i < 3; ++i) {
        out.pos[i] = a->pos[i] * inv_t + b->pos[i] * t;
        out.normal[i] = a->normal[i] * inv_t + b->normal[i] * t;
    }
    for (int i = 0; i < 2; ++i) {
        out.uv[i] = a->uv[i] * inv_t + b->uv[i] * t;
        out.uv1[i] = a->uv1[i] * inv_t + b->uv1[i] * t;
    }
    for (int i = 0; i < 4; ++i) {
        out.color[i] = a->color[i] * inv_t + b->color[i] * t;
        out.tangent[i] = a->tangent[i] * inv_t + b->tangent[i] * t;
        out.bone_weights[i] = a->bone_weights[i] * inv_t + b->bone_weights[i] * t;
    }
    return out;
}

/// @brief Test whether a screen-space vertex is inside one clip-rect edge.
/// @param v Vertex to test.
/// @param edge Edge selector: 0 left, 1 right, 2 top, 3 bottom.
/// @param value Coordinate of the clip edge.
/// @return Non-zero when @p v is on the drawable side of the edge.
static int canvas3d_screen_vertex_inside_clip_edge(const vgfx3d_vertex_t *v,
                                                   int edge,
                                                   float value) {
    switch (edge) {
        case 0:
            return v->pos[0] >= value;
        case 1:
            return v->pos[0] <= value;
        case 2:
            return v->pos[1] >= value;
        default:
            return v->pos[1] <= value;
    }
}

/// @brief Intersect a segment against one axis-aligned clip edge.
/// @details The returned vertex lies on the edge and carries linearly interpolated attributes from
/// the two original vertices. Degenerate segments fall back to @p a, which is safe because the
/// caller only asks for intersections on crossing edges.
/// @param a First segment endpoint.
/// @param b Second segment endpoint.
/// @param edge Edge selector: 0 left, 1 right, 2 top, 3 bottom.
/// @param value Coordinate of the clip edge.
/// @return Interpolated vertex on the requested edge.
static vgfx3d_vertex_t canvas3d_intersect_screen_clip_edge(const vgfx3d_vertex_t *a,
                                                           const vgfx3d_vertex_t *b,
                                                           int edge,
                                                           float value) {
    const int axis = edge < 2 ? 0 : 1;
    float denom = b->pos[axis] - a->pos[axis];
    float t = 0.0f;
    if (fabsf(denom) > 1.0e-8f)
        t = (value - a->pos[axis]) / denom;
    if (t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    return canvas3d_lerp_screen_vertex(a, b, t);
}

/// @brief Clip a convex screen-space polygon against one axis-aligned clip edge.
/// @details Implements one Sutherland-Hodgman pass. The output capacity is explicit so rounded-rect
/// clipping cannot overflow its fixed stack buffers if future tessellation settings change.
/// @param in_vertices Input polygon vertices in winding order.
/// @param in_count Number of input vertices.
/// @param out_vertices Destination for the clipped polygon.
/// @param out_capacity Maximum number of destination vertices.
/// @param edge Edge selector: 0 left, 1 right, 2 top, 3 bottom.
/// @param value Coordinate of the clip edge.
/// @return Clipped vertex count, or -1 if @p out_capacity is insufficient.
static int canvas3d_clip_screen_polygon_edge(const vgfx3d_vertex_t *in_vertices,
                                             int in_count,
                                             vgfx3d_vertex_t *out_vertices,
                                             int out_capacity,
                                             int edge,
                                             float value) {
    int out_count = 0;
    if (!in_vertices || !out_vertices || in_count <= 0 || out_capacity <= 0)
        return 0;
    for (int i = 0; i < in_count; ++i) {
        const vgfx3d_vertex_t *current = &in_vertices[i];
        const vgfx3d_vertex_t *previous = &in_vertices[(i + in_count - 1) % in_count];
        int current_inside = canvas3d_screen_vertex_inside_clip_edge(current, edge, value);
        int previous_inside = canvas3d_screen_vertex_inside_clip_edge(previous, edge, value);
        if (current_inside != previous_inside) {
            if (out_count >= out_capacity)
                return -1;
            out_vertices[out_count++] =
                canvas3d_intersect_screen_clip_edge(previous, current, edge, value);
        }
        if (current_inside) {
            if (out_count >= out_capacity)
                return -1;
            out_vertices[out_count++] = *current;
        }
    }
    return out_count;
}

/// @brief Queue rounded-rectangle fan triangles after exact clipping to the overlay clip rect.
/// @details Each source triangle is clipped independently against the active clip rectangle and
/// then re-triangulated. This prevents partially clipped rounded rectangles from leaking pixels
/// outside the clip region without requiring backend-specific scissor state.
/// @param c Canvas with an active overlay clip rectangle.
/// @param vertices Source rounded-rectangle fan vertices.
/// @param indices Source triangle-list indices.
/// @param index_count Number of source indices.
/// @param r Red color component.
/// @param g Green color component.
/// @param b Blue color component.
/// @param a Alpha component.
/// @return Non-zero on success. A fully clipped rounded rectangle is treated as success.
static int canvas3d_queue_clipped_round_rect_geometry(rt_canvas3d *c,
                                                      const vgfx3d_vertex_t *vertices,
                                                      const uint32_t *indices,
                                                      int32_t index_count,
                                                      float r,
                                                      float g,
                                                      float b,
                                                      float a) {
    enum {
        RR_CLIP_POLY_CAP = 16,
        RR_CLIP_MAX_VERTS = 4 * (6 + 1) * RR_CLIP_POLY_CAP,
        RR_CLIP_MAX_INDICES = 4 * (6 + 1) * (RR_CLIP_POLY_CAP - 2) * 3
    };

    vgfx3d_vertex_t out_vertices[RR_CLIP_MAX_VERTS];
    uint32_t out_indices[RR_CLIP_MAX_INDICES];
    int32_t out_vertex_count = 0;
    int32_t out_index_count = 0;
    if (!c || !vertices || !indices || index_count <= 0)
        return 0;
    const float edge_value[4] = {c->overlay_clip_x,
                                 c->overlay_clip_x + c->overlay_clip_w,
                                 c->overlay_clip_y,
                                 c->overlay_clip_y + c->overlay_clip_h};
    for (int32_t tri = 0; tri + 2 < index_count; tri += 3) {
        vgfx3d_vertex_t poly_a[RR_CLIP_POLY_CAP];
        vgfx3d_vertex_t poly_b[RR_CLIP_POLY_CAP];
        const vgfx3d_vertex_t *current = poly_a;
        vgfx3d_vertex_t *next = poly_b;
        int current_count = 3;
        poly_a[0] = vertices[indices[tri + 0]];
        poly_a[1] = vertices[indices[tri + 1]];
        poly_a[2] = vertices[indices[tri + 2]];
        for (int edge = 0; edge < 4; ++edge) {
            int clipped_count = canvas3d_clip_screen_polygon_edge(
                current, current_count, next, RR_CLIP_POLY_CAP, edge, edge_value[edge]);
            if (clipped_count < 0)
                return 0;
            if (clipped_count == 0) {
                current_count = 0;
                break;
            }
            current_count = clipped_count;
            current = next;
            next = next == poly_a ? poly_b : poly_a;
        }
        if (current_count < 3)
            continue;
        if (out_vertex_count > RR_CLIP_MAX_VERTS - current_count)
            return 0;
        if (out_index_count > RR_CLIP_MAX_INDICES - (current_count - 2) * 3)
            return 0;
        {
            uint32_t base = (uint32_t)out_vertex_count;
            for (int i = 0; i < current_count; ++i)
                out_vertices[out_vertex_count++] = current[i];
            for (int i = 1; i + 1 < current_count; ++i) {
                out_indices[out_index_count++] = base;
                out_indices[out_index_count++] = base + (uint32_t)i;
                out_indices[out_index_count++] = base + (uint32_t)(i + 1);
            }
        }
    }
    if (out_vertex_count == 0 || out_index_count == 0)
        return 1;
    return canvas3d_queue_screen_geometry(
        c, out_vertices, out_vertex_count, out_indices, out_index_count, r, g, b, a);
}

/// @brief Convenience wrapper: queue a screen-space rectangle as two triangles.
/// @param c Canvas with an active compatible frame.
/// @param x Left edge in screen pixels.
/// @param y Top edge in screen pixels.
/// @param w Width in screen pixels.
/// @param h Height in screen pixels.
/// @param r Red vertex-color channel.
/// @param g Green vertex-color channel.
/// @param b Blue vertex-color channel.
/// @param a Alpha vertex-color channel.
/// @return Non-zero on successful queueing or complete clipping; zero on invalid input/failure.
int canvas3d_queue_screen_rect(
    rt_canvas3d *c, float x, float y, float w, float h, float r, float g, float b, float a) {
    vgfx3d_vertex_t verts[4];
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    if (!canvas3d_overlay_clip_rect(c, &x, &y, &w, &h))
        return 1; /* fully clipped away — not an error */

    memset(verts, 0, sizeof(verts));
    verts[0].pos[0] = x;
    verts[0].pos[1] = y;
    verts[1].pos[0] = x + w;
    verts[1].pos[1] = y;
    verts[2].pos[0] = x + w;
    verts[2].pos[1] = y + h;
    verts[3].pos[0] = x;
    verts[3].pos[1] = y + h;
    for (int i = 0; i < 4; i++) {
        verts[i].normal[2] = 1.0f;
        verts[i].color[0] = r;
        verts[i].color[1] = g;
        verts[i].color[2] = b;
        verts[i].color[3] = a;
    }
    return canvas3d_queue_screen_geometry(c, verts, 4, indices, 6, r, g, b, a);
}

/// @brief Queue a screen-space rounded rectangle as a triangle fan (Plan 08).
/// @details The perimeter walks four quarter-arcs (6 segments each) between the straight
///          edges and fans around the rect center — convex, so the fan is exact. The
///          radius clamps to half the smaller dimension. When an overlay clip rect is active,
///          generated triangles are clipped edge-exactly before queueing.
/// @param c Canvas with an active compatible frame.
/// @param x Left edge in screen pixels.
/// @param y Top edge in screen pixels.
/// @param w Positive width in screen pixels.
/// @param h Positive height in screen pixels.
/// @param radius Corner radius in screen pixels, sanitized and clamped.
/// @param r Red vertex-color channel.
/// @param g Green vertex-color channel.
/// @param b Blue vertex-color channel.
/// @param a Alpha vertex-color channel.
/// @return Non-zero on successful queueing or complete clipping; zero on invalid input/failure.
int canvas3d_queue_screen_round_rect(rt_canvas3d *c,
                                     float x,
                                     float y,
                                     float w,
                                     float h,
                                     float radius,
                                     float r,
                                     float g,
                                     float b,
                                     float a) {
    enum { RR_SEG = 6 };

    vgfx3d_vertex_t verts[1 + 4 * (RR_SEG + 1)];
    uint32_t indices[4 * (RR_SEG + 1) * 3];
    int32_t perim = 0;
    const float half_min = (w < h ? w : h) * 0.5f;

    if (!c || w <= 0.0f || h <= 0.0f)
        return 0;
    if (!isfinite(radius) || radius < 0.0f)
        radius = 0.0f;
    if (radius > half_min)
        radius = half_min;
    if (radius < 0.5f)
        return canvas3d_queue_screen_rect(c, x, y, w, h, r, g, b, a);

    /* Corner centers in draw order: TL, TR, BR, BL; each quarter-arc spans 90 deg. */
    {
        const float ccx[4] = {x + radius, x + w - radius, x + w - radius, x + radius};
        const float ccy[4] = {y + radius, y + radius, y + h - radius, y + h - radius};
        const float start_ang[4] = {3.14159265f, 4.71238898f, 0.0f, 1.57079633f};

        memset(verts, 0, sizeof(verts));
        verts[0].pos[0] = x + w * 0.5f;
        verts[0].pos[1] = y + h * 0.5f;
        for (int corner = 0; corner < 4; corner++) {
            for (int s = 0; s <= RR_SEG; s++) {
                float ang = start_ang[corner] + 1.57079633f * (float)s / (float)RR_SEG;
                vgfx3d_vertex_t *v = &verts[1 + perim];
                v->pos[0] = ccx[corner] + cosf(ang) * radius;
                v->pos[1] = ccy[corner] + sinf(ang) * radius;
                perim++;
            }
        }
        for (int i = 0; i <= perim; i++) {
            verts[i].normal[2] = 1.0f;
            verts[i].color[0] = r;
            verts[i].color[1] = g;
            verts[i].color[2] = b;
            verts[i].color[3] = a;
        }
        for (int i = 0; i < perim; i++) {
            indices[i * 3 + 0] = 0;
            indices[i * 3 + 1] = (uint32_t)(1 + i);
            indices[i * 3 + 2] = (uint32_t)(1 + ((i + 1) % perim));
        }
    }
    if (c->overlay_clip_active) {
        float bx = x;
        float by = y;
        float bw = w;
        float bh = h;
        if (!canvas3d_overlay_clip_rect(c, &bx, &by, &bw, &bh))
            return 1; /* fully outside the clip rect */
        return canvas3d_queue_clipped_round_rect_geometry(c, verts, indices, perim * 3, r, g, b, a);
    }
    return canvas3d_queue_screen_geometry(c, verts, 1 + perim, indices, perim * 3, r, g, b, a);
}

/// @brief Retain a GC object (material/texture) referenced by a final-overlay draw.
/// @details The HUD/final-overlay queue replays after post-FX, past the normal
///          end-of-frame temp-object sweep, so textured overlay draws need their
///          backing material kept alive separately. Released in
///          `canvas3d_clear_final_overlay` once the overlay has replayed.
/// @param c Canvas owning the final-overlay retention list.
/// @param obj Non-NULL GC-managed object to retain.
/// @return Non-zero when the retain was recorded; zero on invalid input, overflow, or allocation
/// failure.
int canvas3d_track_final_overlay_temp_object(rt_canvas3d *c, void *obj) {
    if (!c || !obj)
        return 0;
    if (c->final_overlay_temp_obj_capacity < 0)
        return 0;
    if (!c->final_overlay_temp_objects) {
        c->final_overlay_temp_obj_count = 0;
        c->final_overlay_temp_obj_capacity = 0;
    } else {
        if (c->final_overlay_temp_obj_count < 0)
            c->final_overlay_temp_obj_count = 0;
        if (c->final_overlay_temp_obj_count > c->final_overlay_temp_obj_capacity)
            c->final_overlay_temp_obj_count = c->final_overlay_temp_obj_capacity;
    }
    for (int32_t i = 0; i < c->final_overlay_temp_obj_count; ++i) {
        if (c->final_overlay_temp_objects[i] == obj)
            return 1;
    }
    if (c->final_overlay_temp_obj_count >= INT32_MAX)
        return 0;
    if (c->final_overlay_temp_obj_count >= c->final_overlay_temp_obj_capacity) {
        if (c->final_overlay_temp_obj_capacity > INT32_MAX / 2)
            return 0;
        int32_t new_cap =
            c->final_overlay_temp_obj_capacity == 0 ? 8 : c->final_overlay_temp_obj_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(void *))
            return 0;
        void **nb =
            (void **)realloc(c->final_overlay_temp_objects, (size_t)new_cap * sizeof(void *));
        if (!nb)
            return 0;
        c->final_overlay_temp_objects = nb;
        c->final_overlay_temp_obj_capacity = new_cap;
    }
    rt_obj_retain_maybe(obj);
    c->final_overlay_temp_objects[c->final_overlay_temp_obj_count++] = obj;
    return 1;
}

/// @brief Release one final-overlay temp object before the overlay replay phase.
/// @details Used by queueing failure paths after @ref canvas3d_track_final_overlay_temp_object
///          has retained a material but a later buffer or command enqueue step fails. Removes the
///          first matching slot, drops the retained reference, and keeps the dense array compact.
/// @param c Canvas whose final-overlay temp-object list owns the reference.
/// @param obj Object to untrack and release. NULL is ignored.
void canvas3d_release_tracked_final_overlay_temp_object(rt_canvas3d *c, void *obj) {
    if (!c || !obj)
        return;
    if (c->final_overlay_temp_obj_capacity < 0)
        return;
    if (!c->final_overlay_temp_objects) {
        c->final_overlay_temp_obj_count = 0;
        c->final_overlay_temp_obj_capacity = 0;
        return;
    }
    if (c->final_overlay_temp_obj_count < 0)
        c->final_overlay_temp_obj_count = 0;
    if (c->final_overlay_temp_obj_count > c->final_overlay_temp_obj_capacity)
        c->final_overlay_temp_obj_count = c->final_overlay_temp_obj_capacity;
    for (int32_t i = 0; i < c->final_overlay_temp_obj_count; ++i) {
        if (c->final_overlay_temp_objects[i] == obj) {
            int32_t last = c->final_overlay_temp_obj_count - 1;
            c->final_overlay_temp_objects[i] = c->final_overlay_temp_objects[last];
            c->final_overlay_temp_objects[last] = NULL;
            c->final_overlay_temp_obj_count = last;
            if (rt_obj_release_check0(obj))
                rt_obj_free(obj);
            return;
        }
    }
}

/// @brief Queue a screen-space textured quad (image blit) over pixel rect (x,y)-(x+w,y+h),
///        sampling `pixels` across UV (0,0)-(1,1). Unlit, depth-test disabled.
/// @details Builds a transient unlit material from `pixels` and reuses the standard
///          material->draw-command path (`canvas3d_fill_material_cmd`) so texture
///          upload/sampling is identical across backends. The material is retained for
///          the frame (or until the final overlay replays, for HUD draws) so the
///          deferred command's resolved texture stays valid through submission.
/// @param c Canvas with an active compatible frame.
/// @param x Destination left edge in screen pixels.
/// @param y Destination top edge in screen pixels.
/// @param w Positive destination width.
/// @param h Positive destination height.
/// @param pixels Borrowed supported texture source retained through a transient material.
/// @param u0 Source U coordinate at the left edge.
/// @param v0 Source V coordinate at the top edge.
/// @param u1 Source U coordinate at the right edge.
/// @param v1 Source V coordinate at the bottom edge.
/// @return Non-zero on successful queueing or complete clipping; zero on invalid input or any
/// allocation, retention, tracking, or queue failure.
int canvas3d_queue_screen_image_uv(rt_canvas3d *c,
                                   float x,
                                   float y,
                                   float w,
                                   float h,
                                   void *pixels,
                                   float u0,
                                   float v0,
                                   float u1,
                                   float v1) {
    vgfx3d_vertex_t verts[4];
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    size_t vertex_bytes;
    size_t index_bytes;
    size_t total_bytes;
    uint8_t *block;
    vgfx3d_vertex_t *verts_copy;
    uint32_t *indices_copy;
    vgfx3d_draw_cmd_t cmd;
    void *mat;
    int block_from_final_overlay_arena = 0;

    if (!c || !c->in_frame || !pixels || w <= 0.0f || h <= 0.0f)
        return 0;

    /* Plan 08: clip the destination rect and remap the sampled UV sub-range so the
     * visible portion keeps its texel mapping. */
    if (c->overlay_clip_active) {
        float cx = x;
        float cy = y;
        float cw = w;
        float ch = h;
        if (!canvas3d_overlay_clip_rect(c, &cx, &cy, &cw, &ch))
            return 1; /* fully clipped away — not an error */
        {
            float fx0 = (cx - x) / w;
            float fy0 = (cy - y) / h;
            float fx1 = (cx + cw - x) / w;
            float fy1 = (cy + ch - y) / h;
            float du = u1 - u0;
            float dv = v1 - v0;
            u1 = u0 + du * fx1;
            v1 = v0 + dv * fy1;
            u0 = u0 + du * fx0;
            v0 = v0 + dv * fy0;
        }
        x = cx;
        y = cy;
        w = cw;
        h = ch;
    }

    memset(verts, 0, sizeof(verts));
    verts[0].pos[0] = x;
    verts[0].pos[1] = y;
    verts[1].pos[0] = x + w;
    verts[1].pos[1] = y;
    verts[2].pos[0] = x + w;
    verts[2].pos[1] = y + h;
    verts[3].pos[0] = x;
    verts[3].pos[1] = y + h;
    verts[0].uv[0] = u0;
    verts[0].uv[1] = v0;
    verts[1].uv[0] = u1;
    verts[1].uv[1] = v0;
    verts[2].uv[0] = u1;
    verts[2].uv[1] = v1;
    verts[3].uv[0] = u0;
    verts[3].uv[1] = v1;
    for (int i = 0; i < 4; i++) {
        verts[i].normal[2] = 1.0f;
        verts[i].color[0] = 1.0f;
        verts[i].color[1] = 1.0f;
        verts[i].color[2] = 1.0f;
        verts[i].color[3] = 1.0f;
    }

    vertex_bytes = sizeof(verts);
    index_bytes = sizeof(indices);
    total_bytes = vertex_bytes + index_bytes;

    mat = rt_material3d_new_textured(pixels);
    if (!mat)
        return 0;
    rt_material3d_set_unlit(mat, 1);

    block = canvas3d_alloc_screen_geometry_block(
        c, total_bytes, c->final_overlay_recording, &block_from_final_overlay_arena);
    if (!block) {
        if (rt_obj_release_check0(mat))
            rt_obj_free(mat);
        return 0;
    }
    verts_copy = (vgfx3d_vertex_t *)block;
    indices_copy = (uint32_t *)(block + vertex_bytes);
    memcpy(verts_copy, verts, vertex_bytes);
    memcpy(indices_copy, indices, index_bytes);

    memset(&cmd, 0, sizeof(cmd));
    cmd.vertices = verts_copy;
    cmd.vertex_count = 4;
    cmd.indices = indices_copy;
    cmd.index_count = 6;
    cmd.validated_index_count = cmd.index_count;
    cmd.model_matrix[0] = cmd.model_matrix[5] = cmd.model_matrix[10] = cmd.model_matrix[15] = 1.0f;
    canvas3d_fill_material_cmd(c, (const rt_material3d *)mat, &cmd);
    cmd.unlit = 1;
    cmd.disable_depth_test = 1;
    /* A screen quad exists only to show its Pixels: while the upload is still budget-paused the
     * backend must draw nothing rather than the material's white base colour. */
    cmd.texture_required = 1;
    cmd.alpha = 1.0f;

    if (c->final_overlay_recording) {
        if (!canvas3d_track_final_overlay_temp_object(c, mat)) {
            if (rt_obj_release_check0(mat))
                rt_obj_free(mat);
            if (!block_from_final_overlay_arena)
                free(block);
            return 0;
        }
        if (rt_obj_release_check0(mat)) /* canvas now owns the frame reference */
            rt_obj_free(mat);
        if (!block_from_final_overlay_arena &&
            !canvas3d_track_final_overlay_temp_buffer(c, block)) {
            canvas3d_release_tracked_final_overlay_temp_object(c, mat);
            free(block);
            return 0;
        }
        if (!canvas3d_enqueue_final_overlay_draw(c, &cmd, NULL, NULL)) {
            if (!block_from_final_overlay_arena)
                canvas3d_release_tracked_final_overlay_temp_buffer(c, block);
            canvas3d_release_tracked_final_overlay_temp_object(c, mat);
            return 0;
        }
        return 1;
    }

    if (!canvas3d_track_temp_object(c, mat)) {
        if (rt_obj_release_check0(mat))
            rt_obj_free(mat);
        free(block);
        return 0;
    }
    if (rt_obj_release_check0(mat)) /* canvas now owns the frame reference */
        rt_obj_free(mat);
    if (!canvas3d_track_temp_buffer(c, block)) {
        canvas3d_release_tracked_temp_object(c, mat);
        free(block);
        return 0;
    }
    if (!canvas3d_enqueue_draw(c,
                               &cmd,
                               DEFERRED_DRAW_MESH,
                               canvas3d_screen_pass_kind(c),
                               NULL,
                               0,
                               0,
                               0,
                               0,
                               0.0f,
                               NULL,
                               NULL)) {
        canvas3d_release_tracked_temp_buffer(c, block);
        canvas3d_release_tracked_temp_object(c, mat);
        return 0;
    }
    return 1;
}

/// @brief Queue a screen-space textured quad sampling `pixels` over the full UV range.
/// @param c Canvas with an active compatible frame.
/// @param x Destination left edge in screen pixels.
/// @param y Destination top edge in screen pixels.
/// @param w Positive destination width.
/// @param h Positive destination height.
/// @param pixels Borrowed supported texture source.
/// @return Same success/failure contract as `canvas3d_queue_screen_image_uv`.
int canvas3d_queue_screen_image(rt_canvas3d *c, float x, float y, float w, float h, void *pixels) {
    return canvas3d_queue_screen_image_uv(c, x, y, w, h, pixels, 0.0f, 0.0f, 1.0f, 1.0f);
}

/// @brief Queue a screen-space line as a thin quad (tessellated triangles).
///
/// Width is in screen pixels. The endpoints define the centerline;
/// the quad is built by extruding by `width/2` perpendicular to
/// the segment direction. Properly aligned for sub-pixel positions.
/// @param c Canvas with an active compatible frame.
/// @param x0 Start X in screen pixels.
/// @param y0 Start Y in screen pixels.
/// @param x1 End X in screen pixels.
/// @param y1 End Y in screen pixels.
/// @param thickness Positive line width, sanitized and capped in screen pixels.
/// @param r Red vertex-color channel.
/// @param g Green vertex-color channel.
/// @param b Blue vertex-color channel.
/// @param a Alpha vertex-color channel.
/// @return Non-zero on successful queueing or complete clipping; zero on invalid coordinates or
/// queue failure.
int canvas3d_queue_screen_line(rt_canvas3d *c,
                               float x0,
                               float y0,
                               float x1,
                               float y1,
                               float thickness,
                               float r,
                               float g,
                               float b,
                               float a) {
    float dx;
    float dy;
    float len;
    float px;
    float py;
    float half;
    vgfx3d_vertex_t verts[4];
    static const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return 0;
    if (!isfinite(thickness) || thickness <= 0.0f)
        thickness = 1.0f;
    if (thickness > 4096.0f)
        thickness = 4096.0f;

    /* Plan 08: Liang-Barsky clip of the centerline against the overlay clip rect. */
    if (c && c->overlay_clip_active) {
        float t0 = 0.0f;
        float t1 = 1.0f;
        float ldx = x1 - x0;
        float ldy = y1 - y0;
        const float p[4] = {-ldx, ldx, -ldy, ldy};
        const float q[4] = {x0 - c->overlay_clip_x,
                            c->overlay_clip_x + c->overlay_clip_w - x0,
                            y0 - c->overlay_clip_y,
                            c->overlay_clip_y + c->overlay_clip_h - y0};
        for (int e = 0; e < 4; e++) {
            if (p[e] == 0.0f) {
                if (q[e] < 0.0f)
                    return 1; /* parallel and outside — fully clipped */
            } else {
                float t = q[e] / p[e];
                if (p[e] < 0.0f) {
                    if (t > t1)
                        return 1;
                    if (t > t0)
                        t0 = t;
                } else {
                    if (t < t0)
                        return 1;
                    if (t < t1)
                        t1 = t;
                }
            }
        }
        x1 = x0 + ldx * t1;
        y1 = y0 + ldy * t1;
        x0 = x0 + ldx * t0;
        y0 = y0 + ldy * t0;
    }

    dx = x1 - x0;
    dy = y1 - y0;
    len = canvas3d_screen_vector_len(dx, dy);
    if (!isfinite(len) || len < 1e-4f)
        return canvas3d_queue_screen_rect(
            c, x0 - thickness * 0.5f, y0 - thickness * 0.5f, thickness, thickness, r, g, b, a);
    px = -dy / len;
    py = dx / len;
    half = thickness * 0.5f;
    memset(verts, 0, sizeof(verts));
    verts[0].pos[0] = x0 - px * half;
    verts[0].pos[1] = y0 - py * half;
    verts[1].pos[0] = x0 + px * half;
    verts[1].pos[1] = y0 + py * half;
    verts[2].pos[0] = x1 + px * half;
    verts[2].pos[1] = y1 + py * half;
    verts[3].pos[0] = x1 - px * half;
    verts[3].pos[1] = y1 - py * half;
    for (int i = 0; i < 4; i++) {
        verts[i].normal[2] = 1.0f;
        verts[i].color[0] = r;
        verts[i].color[1] = g;
        verts[i].color[2] = b;
        verts[i].color[3] = a;
    }
    return canvas3d_queue_screen_geometry(c, verts, 4, indices, 6, r, g, b, a);
}

/// @brief Advance the positive frame epoch without invoking signed overflow.
/// @details A serial discontinuity invalidates every canvas-owned table whose entries are stamped
///   with the old epoch. External animation and particle owners key on serial inequality and
///   therefore naturally start a new snapshot when the serial becomes one.
/// @param c Canvas whose serial and temporal caches are advanced.
static void canvas3d_advance_frame_serial(rt_canvas3d *c) {
    if (!c)
        return;
    if (c->frame_serial > 0 && c->frame_serial < INT64_MAX) {
        c->frame_serial++;
        return;
    }

    c->frame_serial = 1;
    c->occlusion_frame_serial = 0;
    canvas3d_clear_motion_history(c);
    canvas3d_clear_occlusion_history(c);
    free(c->texture_stream_entries);
    c->texture_stream_entries = NULL;
    c->texture_stream_capacity = 0;
    c->texture_stream_live = 0;
    canvas3d_clear_aa_text_cache(c);
}

/// @brief Switch the canvas into 2D-overlay mode for the next batch of draw calls.
///
/// All subsequent screen-space draws (rects, lines, sprites, text)
/// queue into the post-3D HUD pass, which renders after the 3D
/// scene, postFX, and tonemapping. Pair with `rt_canvas3d_end`
/// to finish the overlay pass.
/// @param obj Borrowed Canvas3D handle not already inside a frame.
void rt_canvas3d_begin_2d(void *obj) {
    vgfx3d_camera_params_t params = {0};

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->backend)
        return;
    if (c->in_frame) {
        rt_trap("Canvas3D.Begin2D: Begin/End must not nest");
        return;
    }
    if (c->frame_finalized)
        canvas3d_reset_finalized_frame_state(c);
    if (c->backend->show_gpu_layer)
        c->backend->show_gpu_layer(c->backend_ctx);

    canvas3d_build_ortho_camera(c, &params);

    c->cached_cam_pos[0] = 0.0f;
    c->cached_cam_pos[1] = 0.0f;
    c->cached_cam_pos[2] = 1.0f;
    c->cached_render_cam_pos[0] = 0.0f;
    c->cached_render_cam_pos[1] = 0.0f;
    c->cached_render_cam_pos[2] = 1.0f;
    c->cached_world_cam_pos[0] = 0.0;
    c->cached_world_cam_pos[1] = 0.0;
    c->cached_world_cam_pos[2] = 1.0;
    canvas3d_reset_camera_relative_origin(c);
    c->cached_cam_forward[0] = params.forward[0];
    c->cached_cam_forward[1] = params.forward[1];
    c->cached_cam_forward[2] = params.forward[2];
    c->cached_cam_near = 0.1f;
    c->cached_cam_far = 1000.0f;
    c->cached_cam_is_ortho = params.is_ortho;
    canvas3d_advance_frame_serial(c);
    c->cpu_finalized_this_render = 0;
    canvas3d_prune_motion_history(c);
    canvas3d_reset_frame_submission_stats(c);
    c->draw_count = 0;
    canvas3d_reset_frame_light_snapshots(c);
    c->frame_is_2d = 1;
    c->last_texture_upload_bytes = 0;
    // Cache the full VP product so cached_vp has consistent semantics between
    // Begin2D and Begin3D. Previously 2D mode stored only the projection —
    // overlay code that unprojected via cached_vp got inconsistent results
    // across modes. For an identity view this still equals projection, but
    // the shape of the math is now the same as Begin3D. (params.view and
    // params.projection are already float[16], so no conversion needed.)
    {
        const float *vf = params.view;
        const float *pf = params.projection;
        for (int r = 0; r < 4; r++)
            for (int col = 0; col < 4; col++)
                c->cached_vp[r * 4 + col] =
                    pf[r * 4 + 0] * vf[0 * 4 + col] + pf[r * 4 + 1] * vf[1 * 4 + col] +
                    pf[r * 4 + 2] * vf[2 * 4 + col] + pf[r * 4 + 3] * vf[3 * 4 + col];
    }

    canvas3d_latch_gpu_postfx_state(c);
    c->backend->begin_frame(c->backend_ctx, &params);
    c->in_frame = 1;
}

/// @brief Begin recording a final overlay pass that is composited after post-FX.
/// @details Unlike Begin2D, this records retained commands without immediately opening a backend
/// frame. It traps if a frame is active or finalization already completed.
/// @param obj Borrowed Canvas3D handle.
void rt_canvas3d_begin_overlay(void *obj) {
    vgfx3d_camera_params_t params;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->backend)
        return;
    if (c->frame_finalized) {
        rt_trap("Canvas3D.BeginOverlay: frame is already finalized");
        return;
    }
    if (c->in_frame) {
        rt_trap("Canvas3D.BeginOverlay: Begin/End must not nest");
        return;
    }

    canvas3d_build_ortho_camera(c, &params);
    c->cached_cam_pos[0] = 0.0f;
    c->cached_cam_pos[1] = 0.0f;
    c->cached_cam_pos[2] = 1.0f;
    canvas3d_reset_camera_relative_origin(c);
    c->cached_cam_forward[0] = params.forward[0];
    c->cached_cam_forward[1] = params.forward[1];
    c->cached_cam_forward[2] = params.forward[2];
    c->cached_cam_near = 0.1f;
    c->cached_cam_far = 1000.0f;
    c->cached_cam_is_ortho = params.is_ortho;
    c->frame_is_2d = 1;
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            c->cached_vp[r * 4 + col] = params.projection[r * 4 + 0] * params.view[0 * 4 + col] +
                                        params.projection[r * 4 + 1] * params.view[1 * 4 + col] +
                                        params.projection[r * 4 + 2] * params.view[2 * 4 + col] +
                                        params.projection[r * 4 + 3] * params.view[3 * 4 + col];
    c->final_overlay_recording = 1;
    c->in_frame = 1;
}

/// @brief Finish recording a final overlay pass.
/// @param obj Borrowed Canvas3D handle; a non-recording canvas is unchanged.
void rt_canvas3d_end_overlay(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!c->final_overlay_recording)
        return;
    c->final_overlay_recording = 0;
    c->in_frame = 0;
    c->frame_is_2d = 0;
}

/// @brief Discard recorded final-overlay commands for the current frame.
/// @param obj Borrowed Canvas3D handle whose retained overlay resources are released.
void rt_canvas3d_clear_overlay(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    canvas3d_clear_final_overlay(c);
}

/// @brief Draw a filled rectangle through the 3D pipeline (screen-space coords).
/// Must be called between Begin2D/End or Begin/End.
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param x Left edge in screen pixels.
/// @param y Top edge in screen pixels.
/// @param w Positive width in screen pixels.
/// @param h Positive height in screen pixels.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_rect_3d(
    void *obj, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color) {
    float r;
    float g;
    float b;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (!c->in_frame || !c->backend)
        return;
    r = (float)((color >> 16) & 0xFF) / 255.0f;
    g = (float)((color >> 8) & 0xFF) / 255.0f;
    b = (float)(color & 0xFF) / 255.0f;
    (void)canvas3d_queue_screen_rect(c, (float)x, (float)y, (float)w, (float)h, r, g, b, 1.0f);
}

/// @brief Draw text through the 3D pipeline using the 5×7 bitmap font at a size multiplier.
/// Each character's "on" pixels are rendered as (2×user_scale)² quads batched into one
/// mesh; dots are clipped against the overlay clip rect at emission time (Plan 08).
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param x Left text origin in screen pixels.
/// @param y Top text origin in screen pixels.
/// @param text Borrowed runtime string.
/// @param color Packed RGB runtime color.
/// @param user_scale Positive size multiplier; invalid values use one and large values are capped.
void rt_canvas3d_draw_text_3d_scaled(
    void *obj, int64_t x, int64_t y, rt_string text, int64_t color, double user_scale) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c || !text)
        return;
    if (!c->in_frame || !c->backend)
        return;
    if (!isfinite(user_scale) || user_scale <= 0.0)
        user_scale = 1.0;
    if (user_scale > 64.0)
        user_scale = 64.0;

    const char *str = rt_string_cstr(text);
    size_t text_len;
    const char *text_end;
    if (!str)
        return;
    text_len = strnlen(str, RT_CANVAS3D_TEXT_MAX_BYTES);
    text_end = str + text_len;

    float r = (float)((color >> 16) & 0xFF) / 255.0f;
    float g = (float)((color >> 8) & 0xFF) / 255.0f;
    float b = (float)(color & 0xFF) / 255.0f;

    /* Reference the font data from draw_text2d (defined later in this file).
     * We duplicate the font table reference here for self-containment. */
    static const uint8_t font5x7[95][7] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x04, 0x04, 0x04, 0x04, 0x00, 0x04, 0x00},
        {0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x00, 0x00},
        {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}, {0x19, 0x1A, 0x04, 0x0B, 0x13, 0x00, 0x00},
        {0x08, 0x14, 0x08, 0x15, 0x12, 0x0D, 0x00}, {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x02, 0x04, 0x04, 0x04, 0x04, 0x02, 0x00}, {0x08, 0x04, 0x04, 0x04, 0x04, 0x08, 0x00},
        {0x04, 0x15, 0x0E, 0x15, 0x04, 0x00, 0x00}, {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},
        {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00}, {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00},
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x0E, 0x00}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x0E, 0x00},
        {0x0E, 0x11, 0x01, 0x06, 0x08, 0x1F, 0x00}, {0x0E, 0x11, 0x02, 0x01, 0x11, 0x0E, 0x00},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x00}, {0x1F, 0x10, 0x1E, 0x01, 0x11, 0x0E, 0x00},
        {0x06, 0x08, 0x1E, 0x11, 0x11, 0x0E, 0x00}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x00},
        {0x0E, 0x11, 0x0E, 0x11, 0x11, 0x0E, 0x00}, {0x0E, 0x11, 0x0F, 0x01, 0x02, 0x0C, 0x00},
        {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00}, {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x08},
        {0x02, 0x04, 0x08, 0x04, 0x02, 0x00, 0x00}, {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
        {0x08, 0x04, 0x02, 0x04, 0x08, 0x00, 0x00}, {0x0E, 0x11, 0x02, 0x04, 0x00, 0x04, 0x00},
        {0x0E, 0x11, 0x17, 0x17, 0x16, 0x10, 0x0E}, {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x00},
        {0x1E, 0x11, 0x1E, 0x11, 0x11, 0x1E, 0x00}, {0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E, 0x00},
        {0x1E, 0x11, 0x11, 0x11, 0x11, 0x1E, 0x00}, {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x1F, 0x00},
        {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x00}, {0x0E, 0x11, 0x10, 0x13, 0x11, 0x0E, 0x00},
        {0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00},
        {0x01, 0x01, 0x01, 0x01, 0x11, 0x0E, 0x00}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00}, {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x00},
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x00}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00},
        {0x1E, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x00}, {0x0E, 0x11, 0x11, 0x15, 0x12, 0x0D, 0x00},
        {0x1E, 0x11, 0x1E, 0x14, 0x12, 0x11, 0x00}, {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x1E, 0x00},
        {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00},
        {0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x00}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00},
        {0x11, 0x0A, 0x04, 0x04, 0x0A, 0x11, 0x00}, {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x00},
        {0x1F, 0x02, 0x04, 0x08, 0x10, 0x1F, 0x00}, {0x0E, 0x08, 0x08, 0x08, 0x08, 0x0E, 0x00},
        {0x10, 0x08, 0x04, 0x02, 0x01, 0x00, 0x00}, {0x0E, 0x02, 0x02, 0x02, 0x02, 0x0E, 0x00},
        {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00},
        {0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F, 0x00},
        {0x10, 0x10, 0x1E, 0x11, 0x11, 0x1E, 0x00}, {0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E, 0x00},
        {0x01, 0x01, 0x0F, 0x11, 0x11, 0x0F, 0x00}, {0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E, 0x00},
        {0x06, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x00}, {0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E, 0x00},
        {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x00}, {0x04, 0x00, 0x0C, 0x04, 0x04, 0x0E, 0x00},
        {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}, {0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x00},
        {0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00}, {0x00, 0x1A, 0x15, 0x15, 0x11, 0x11, 0x00},
        {0x00, 0x1E, 0x11, 0x11, 0x11, 0x11, 0x00}, {0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00},
        {0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10, 0x00}, {0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01, 0x00},
        {0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x00}, {0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E, 0x00},
        {0x08, 0x1E, 0x08, 0x08, 0x0A, 0x04, 0x00}, {0x00, 0x11, 0x11, 0x11, 0x13, 0x0D, 0x00},
        {0x00, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x00}, {0x00, 0x11, 0x11, 0x15, 0x15, 0x0A, 0x00},
        {0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00}, {0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E, 0x00},
        {0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F, 0x00}, {0x02, 0x04, 0x0C, 0x04, 0x04, 0x02, 0x00},
        {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x08, 0x04, 0x06, 0x04, 0x04, 0x08, 0x00},
        {0x00, 0x00, 0x0D, 0x12, 0x00, 0x00, 0x00},
    };

    /* Count "on" pixels to size the reusable scratch mesh exactly. */
    size_t quad_count = 0;
    for (const char *p = str; p < text_end; p++) {
        int ch = *p;
        if (ch < 32 || ch > 126)
            ch = 32;
        const uint8_t *glyph = font5x7[ch - 32];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyph[row] & (1 << (4 - col))) {
                    if (quad_count >= (size_t)INT32_MAX / 6u) {
                        rt_trap("Canvas3D.DrawText3D: text is too large");
                        return;
                    }
                    quad_count++;
                }
    }

    if (quad_count == 0)
        return;

    if (quad_count > (size_t)INT32_MAX / 4u || quad_count > (size_t)INT32_MAX / 6u) {
        rt_trap("Canvas3D.DrawText3D: text is too large");
        return;
    }
    size_t vertex_total = quad_count * 4u;
    size_t index_total = quad_count * 6u;
    int32_t vertex_count = (int32_t)vertex_total;
    int32_t index_count = (int32_t)index_total;
    if (!ensure_text_capacity(c, vertex_count, index_count))
        return;

    float scale = 2.0f * (float)user_scale; /* pixel size for each font dot */
    float cx = (float)x;
    int32_t quad_idx = 0;

    for (const char *p = str; p < text_end; p++) {
        int ch = *p;
        if (ch < 32 || ch > 126)
            ch = 32;
        const uint8_t *glyph = font5x7[ch - 32];

        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row] & (1 << (4 - col))) {
                    float px = cx + col * scale;
                    float py = (float)y + row * scale;
                    float pw = scale;
                    float ph = scale;
                    int32_t vi;
                    int32_t ii;

                    if (!canvas3d_overlay_clip_rect(c, &px, &py, &pw, &ph))
                        continue; /* dot fully outside the overlay clip rect */

                    vi = quad_idx * 4;
                    ii = quad_idx * 6;

                    /* 4 vertices for this pixel quad */
                    for (int v = 0; v < 4; v++) {
                        memset(&c->text_vertices[vi + v], 0, sizeof(vgfx3d_vertex_t));
                        c->text_vertices[vi + v].normal[2] = 1.0f;
                        c->text_vertices[vi + v].color[0] = r;
                        c->text_vertices[vi + v].color[1] = g;
                        c->text_vertices[vi + v].color[2] = b;
                        c->text_vertices[vi + v].color[3] = 1.0f;
                    }
                    c->text_vertices[vi + 0].pos[0] = px;
                    c->text_vertices[vi + 0].pos[1] = py;
                    c->text_vertices[vi + 1].pos[0] = px + pw;
                    c->text_vertices[vi + 1].pos[1] = py;
                    c->text_vertices[vi + 2].pos[0] = px + pw;
                    c->text_vertices[vi + 2].pos[1] = py + ph;
                    c->text_vertices[vi + 3].pos[0] = px;
                    c->text_vertices[vi + 3].pos[1] = py + ph;

                    c->text_indices[ii + 0] = (uint32_t)vi;
                    c->text_indices[ii + 1] = (uint32_t)(vi + 1);
                    c->text_indices[ii + 2] = (uint32_t)(vi + 2);
                    c->text_indices[ii + 3] = (uint32_t)vi;
                    c->text_indices[ii + 4] = (uint32_t)(vi + 2);
                    c->text_indices[ii + 5] = (uint32_t)(vi + 3);
                    quad_idx++;
                }
            }
        }
        cx += 6.0f * scale; /* char width + 1px spacing */
    }

    if (quad_idx <= 0)
        return; /* every dot was clipped away */
    (void)canvas3d_queue_screen_geometry(
        c, c->text_vertices, quad_idx * 4, c->text_indices, quad_idx * 6, r, g, b, 1.0f);
}

/// @brief Draw text through the 3D pipeline using the 5×7 bitmap font at the default size.
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param x Left text origin in screen pixels.
/// @param y Top text origin in screen pixels.
/// @param text Borrowed runtime string.
/// @param color Packed RGB runtime color.
void rt_canvas3d_draw_text_3d(void *obj, int64_t x, int64_t y, rt_string text, int64_t color) {
    rt_canvas3d_draw_text_3d_scaled(obj, x, y, text, color, 1.0);
}

/// @brief Shared Begin implementation for the main scene pass and the view-model pass.
/// @details The view-model variant renders over the finished scene color while
///   clearing depth (`load_existing_color=1`, fresh depth), so camera-space
///   weapon geometry never depth-clips against world walls yet self-occludes
///   correctly. A positive @p fov_override_deg replaces the camera's vertical
///   FOV terms in the perspective projection (world FOV stays independent).
/// @param obj Borrowed Canvas3D handle that is not already inside a frame.
/// @param camera Borrowed live Camera3D providing the view and projection state.
/// @param fov_override_deg Positive vertical field-of-view override in degrees;
///   a non-positive value preserves the camera's configured field of view.
/// @param view_model_pass Non-zero to preserve scene color and begin with fresh
///   depth for view-model rendering; zero to begin the main scene pass.
static void canvas3d_begin_camera_pass(void *obj,
                                       void *camera,
                                       double fov_override_deg,
                                       int8_t view_model_pass) {
    vgfx3d_camera_params_t params = {0};
    int32_t output_w = 0;
    int32_t output_h = 0;
    double render_aspect = 0.0;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    rt_camera3d *cam = rt_camera3d_checked_or_stack(camera);
    if (!c || !cam)
        return;
    if (!c->backend)
        return;
    if (c->in_frame) {
        rt_trap("Canvas3D.Begin: Begin/End must not nest");
        return;
    }
    if (c->frame_finalized)
        canvas3d_reset_finalized_frame_state(c);

    /* Show GPU layer for 3D rendering (in case it was hidden for 2D menu) */
    if (c->backend->show_gpu_layer)
        c->backend->show_gpu_layer(c->backend_ctx);

    canvas3d_active_output_size(c, &output_w, &output_h);
    if (output_w > 0 && output_h > 0)
        render_aspect = (double)output_w / (double)output_h;

    {
        double frame_dt = rt_canvas3d_get_delta_time_sec(c);
        if (frame_dt > 0.0)
            rt_camera3d_update_shake_for_frame_token(cam, frame_dt, c->timing_serial);
    }

    double cam_x = cam->eye[0] + cam->shake_offset[0];
    double cam_y = cam->eye[1] + cam->shake_offset[1];
    double cam_z = cam->eye[2] + cam->shake_offset[2];
    double upload_view[16];

    memcpy(upload_view, cam->view, sizeof(upload_view));
    if (c->camera_relative_upload) {
        if (!isfinite(cam_x) || !isfinite(cam_y) || !isfinite(cam_z)) {
            rt_trap("Canvas3D.Begin: camera position must contain finite values");
            return;
        }
        c->camera_relative_origin[0] = cam_x;
        c->camera_relative_origin[1] = cam_y;
        c->camera_relative_origin[2] = cam_z;
        upload_view[3] = 0.0;
        upload_view[7] = 0.0;
        upload_view[11] = 0.0;
    } else {
        canvas3d_reset_camera_relative_origin(c);
    }

    if (!canvas3d_mat4_d2f_checked(upload_view, params.view)) {
        rt_trap("Canvas3D.Begin: camera view matrix must contain finite float-range values");
        return;
    }
    rt_camera3d_get_render_projection(cam, render_aspect, params.projection);
    if (fov_override_deg > 0.0 && !cam->is_ortho && isfinite(fov_override_deg)) {
        /* Same layout as build_perspective: m[0] = f/aspect, m[5] = f. */
        double fov = fov_override_deg;
        if (fov < 1.0)
            fov = 1.0;
        if (fov > 179.0)
            fov = 179.0;
        double f = 1.0 / tan(fov * 0.017453292519943295 * 0.5); /* deg → rad */
        double aspect = render_aspect > 1e-6 ? render_aspect : 1.0;
        params.projection[0] = (float)(f / aspect);
        params.projection[5] = (float)f;
    }
    canvas3d_apply_backend_depth_convention(c, params.projection);
    if (!canvas3d_matrices_f32_are_finite(params.projection, 1)) {
        rt_trap("Canvas3D.Begin: camera projection matrix must contain finite values");
        return;
    }
    if (!c->camera_relative_upload &&
        (!canvas3d_double_fits_float(cam_x) || !canvas3d_double_fits_float(cam_y) ||
         !canvas3d_double_fits_float(cam_z))) {
        rt_trap("Canvas3D.Begin: camera position must contain finite float-range values");
        return;
    }
    if (c->camera_relative_upload) {
        params.position[0] = 0.0f;
        params.position[1] = 0.0f;
        params.position[2] = 0.0f;
    } else {
        params.position[0] = (float)cam_x;
        params.position[1] = (float)cam_y;
        params.position[2] = (float)cam_z;
    }
    canvas3d_extract_view_forward(cam->view, params.forward);
    params.is_ortho = cam->is_ortho;
    params.fog_enabled = c->fog_enabled;
    params.fog_near = c->fog_near;
    params.fog_far = c->fog_far;
    params.fog_color[0] = c->fog_color[0];
    params.fog_color[1] = c->fog_color[1];
    params.fog_color[2] = c->fog_color[2];
    params.height_fog_enabled = c->height_fog_enabled;
    params.height_fog_base = c->height_fog_base;
    params.height_fog_falloff = c->height_fog_falloff;
    params.height_fog_density = c->height_fog_density;
    params.height_fog_blend = c->height_fog_blend;
    params.height_fog_sun_color[0] = c->height_fog_sun_color[0];
    params.height_fog_sun_color[1] = c->height_fog_sun_color[1];
    params.height_fog_sun_color[2] = c->height_fog_sun_color[2];
    params.height_fog_sun_power = c->height_fog_sun_power;
    params.height_fog_sun_amount = 0.0f;
    if (c->height_fog_enabled && c->height_fog_sun_amount > 0.0f) {
        /* Sun inscattering follows the first enabled directional light; without
         * one the term stays off for this frame. Direction points TOWARD the sun. */
        for (int li = 0; li < VGFX3D_MAX_LIGHTS; ++li) {
            rt_light3d *sun = c->lights[li];
            if (!sun || !sun->enabled || sun->type != 0)
                continue;
            double dx = -sun->direction[0];
            double dy = -sun->direction[1];
            double dz = -sun->direction[2];
            double len = sqrt(dx * dx + dy * dy + dz * dz);
            if (isfinite(len) && len > 1e-9) {
                params.height_fog_sun_dir[0] = (float)(dx / len);
                params.height_fog_sun_dir[1] = (float)(dy / len);
                params.height_fog_sun_dir[2] = (float)(dz / len);
                params.height_fog_sun_amount = c->height_fog_sun_amount;
            }
            break;
        }
    }
    params.load_existing_color = view_model_pass ? 1 : 0;
    params.load_existing_depth = 0;
    if (c->ibl_enabled && c->skybox && rt_cubemap3d_ensure_ibl(c->skybox)) {
        params.ibl_enabled = 1;
        params.ibl_intensity = c->ibl_intensity;
        memcpy(params.ibl_sh, c->skybox->ibl_sh, sizeof(params.ibl_sh));
    }
    params.shadow_strength = c->shadow_strength;
    params.shadow_slope_bias = c->shadow_slope_bias;
    params.shadow_quality = c->shadow_quality;
    params.znear = (float)rt_camera3d_get_near_plane(cam);
    params.zfar = (float)rt_camera3d_get_far_plane(cam);

    /* Cache camera position for transparency sort key computation */
    c->cached_cam_pos[0] = params.position[0];
    c->cached_cam_pos[1] = params.position[1];
    c->cached_cam_pos[2] = params.position[2];
    c->cached_world_cam_pos[0] = cam_x;
    c->cached_world_cam_pos[1] = cam_y;
    c->cached_world_cam_pos[2] = cam_z;
    c->cached_render_cam_pos[0] = params.position[0];
    c->cached_render_cam_pos[1] = params.position[1];
    c->cached_render_cam_pos[2] = params.position[2];
    c->cached_cam_forward[0] = params.forward[0];
    c->cached_cam_forward[1] = params.forward[1];
    c->cached_cam_forward[2] = params.forward[2];
    c->cached_cam_near = (float)rt_camera3d_get_near_plane(cam);
    c->cached_cam_far = (float)rt_camera3d_get_far_plane(cam);
    c->cached_cam_is_ortho = params.is_ortho;
    /* ADR 0310: only window-backed camera passes participate in CPU occlusion —
     * render-target brackets (insets, mirrors, probe bakes) and view-model
     * passes neither wipe the history (their camera/identity never reaches the
     * signature check) nor advance the occlusion serial (covered streaks stay
     * consecutive across them). */
    if (!c->render_target && !view_model_pass) {
        if (c->occlusion_frame_serial < INT64_MAX)
            c->occlusion_frame_serial++;
        canvas3d_update_occlusion_state_for_begin(c, output_w, output_h);
    }

    /* Reset draw command queue for this frame */
    canvas3d_advance_frame_serial(c);
    c->cpu_finalized_this_render = 0;
    canvas3d_prune_motion_history(c);
    canvas3d_reset_frame_submission_stats(c);
    /* Plan 07: froxel tables are camera-dependent — invalidate the revision ring
     * so this frame's first stamped draw rebuilds against the new view. */
    if (c->cluster_tables) {
        vgfx3d_cluster_table_t *ring = (vgfx3d_cluster_table_t *)c->cluster_tables;
        for (int32_t ci = 0; ci < c->cluster_table_count; ci++)
            ring[ci].lights_revision = 0;
    }
    c->draw_count = 0;
    canvas3d_reset_frame_light_snapshots(c);
    c->frame_is_2d = 0;
    c->frame_is_view_model = view_model_pass ? 1 : 0;
    c->last_instanced_fallback_count = 0;
    c->last_instanced_fallback_dropped_count = 0;
    c->last_draw_count = 0;
    memset(c->last_pass_draw_count, 0, sizeof(c->last_pass_draw_count));
    memset(c->last_pass_instance_count, 0, sizeof(c->last_pass_instance_count));
    c->last_occluded_draw_count = 0;
    c->last_frustum_culled_draw_count = 0;
    c->last_cpu_occluded_draw_count = 0;
    c->last_occlusion_candidate_count = 0;
    c->last_texture_upload_bytes = 0;
    c->last_mesh_snapshot_bytes = 0;
    c->last_mesh_snapshot_drop_count = 0;
    c->last_mesh_snapshot_dropped_bytes = 0;

    /* Cache VP matrix for debug drawing (backend-agnostic) */
    {
        /* VP = P * V (row-major) */
        for (int r = 0; r < 4; r++)
            for (int col = 0; col < 4; col++)
                c->cached_vp[r * 4 + col] =
                    params.projection[r * 4 + 0] * params.view[0 * 4 + col] +
                    params.projection[r * 4 + 1] * params.view[1 * 4 + col] +
                    params.projection[r * 4 + 2] * params.view[2 * 4 + col] +
                    params.projection[r * 4 + 3] * params.view[3 * 4 + col];
    }
    memcpy(c->last_scene_vp, c->cached_vp, sizeof(c->last_scene_vp));
    memcpy(c->last_scene_cam_pos, c->cached_cam_pos, sizeof(c->last_scene_cam_pos));
    c->has_last_scene_vp = 1;

    canvas3d_latch_gpu_postfx_state(c);
    c->backend->begin_frame(c->backend_ctx, &params);
    c->in_frame = 1;
}

/// @brief Begin a 3D rendering frame with the given camera.
/// @details Must be called after Clear and before any DrawMesh calls. Captures
///          the camera's view/projection matrices, resets the deferred draw queue,
///          and updates per-frame timing state. Begin/End must not be nested.
/// @param obj    Canvas handle.
/// @param camera Camera3D handle providing view and projection matrices.
void rt_canvas3d_begin(void *obj, void *camera) {
    canvas3d_begin_camera_pass(obj, camera, 0.0, 0);
}

/// @brief Begin a view-model pass over the finished scene (weapon rendering).
/// @details Call after `End` of the world pass. Keeps the scene color, clears
///   depth, and renders subsequent draws with the camera's view but an
///   independent FOV (@p fov_y_degrees; <= 0 keeps the camera FOV). View-model
///   draws never depth-clip against world geometry, still self-occlude, use the
///   canvas light set, and neither cast shadows nor redraw the skybox. Close
///   the pass with `End` before HUD overlay drawing.
/// @param obj Borrowed Canvas3D handle after the main scene pass has ended.
/// @param camera Borrowed Camera3D supplying the view and clipping planes.
/// @param fov_y_degrees Positive vertical field of view in degrees for the
///   view-model pass; a non-positive value preserves the camera field of view.
void rt_canvas3d_begin_view_model(void *obj, void *camera, double fov_y_degrees) {
    canvas3d_begin_camera_pass(obj, camera, fov_y_degrees, 1);
}

/// @brief Monotonically-increasing per-frame serial; used for cache invalidation.
///
/// Resources keyed off `(canvas, serial)` know they're stale when
/// their cached serial is older than the canvas's current value.
/// @param obj Borrowed Canvas3D handle.
/// @return The positive current frame serial, or zero for an invalid canvas or
///   before the first frame has begun.
int64_t rt_canvas3d_get_frame_serial(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return (c && c->frame_serial > 0) ? c->frame_serial : 0;
}

/// @brief Internal opt-in used by Game3D floating origin to keep backend floats near the camera.
/// @details Changing the mode invalidates accumulated occlusion history. Turning
///   it off also clears the stored camera-relative origin.
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to rebase backend uploads around the per-frame camera
///   origin; zero to upload absolute coordinates.
void rt_canvas3d_set_camera_relative_upload(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    int8_t next;
    if (!c)
        return;
    next = enabled ? 1 : 0;
    if (c->camera_relative_upload != next) {
        canvas3d_clear_occlusion_history(c);
        c->occlusion_state_valid = 0;
    }
    c->camera_relative_upload = next;
    if (!c->camera_relative_upload)
        canvas3d_reset_camera_relative_origin(c);
}

/// @brief Queue a 3D mesh draw with a model matrix and a sort key for transparency ordering.
///
/// `sort_key` is used by the deferred renderer to depth-sort
/// translucent draws back-to-front before flushing. Opaque
/// objects ignore it (the depth buffer handles their order).
/// @details Valid inputs are captured into a deferred draw for the current
///   frame. Heap-backed mesh, material, texture, and splat resources are
///   retained through replay; mutable geometry and prior animation arrays are
///   snapshotted when required. Invalid handles, an inactive frame, an
///   unavailable backend, or empty/non-resident geometry cause no draw.
/// @param obj Borrowed Canvas3D handle with an active 3D pass.
/// @param mesh_obj Borrowed live Mesh3D whose geometry is queued or snapshotted.
/// @param model_matrix Borrowed array of 16 row-major doubles; its value is
///   converted and captured before this function returns.
/// @param material_obj Borrowed live Material3D; resources needed by the queued
///   command are retained through frame cleanup.
/// @param motion_key Optional stable instance identity used for previous-model
///   history and deterministic transparent ordering; may be `NULL`.
/// @param prev_bone_palette Optional borrowed previous-frame bone palette used
///   for motion data; may be `NULL`.
/// @param prev_morph_weights Optional borrowed previous-frame morph-weight array
///   used for motion data; may be `NULL`.
/// @param explicit_local_bounds_min Optional three-component local-space AABB
///   minimum; used only when paired with a valid @p explicit_local_bounds_max.
/// @param explicit_local_bounds_max Optional three-component local-space AABB
///   maximum; used only when paired with a valid @p explicit_local_bounds_min.
/// @param conservative_bounds Non-zero to mark bounds as conservative when
///   finalizing visibility and culling policy.
/// @param disable_occlusion Non-zero to exclude this draw from occlusion culling.
/// @param culling_pad Non-negative world-space padding added for culling;
///   non-finite or negative values are treated as zero.
void rt_canvas3d_draw_mesh_matrix_keyed_bounds(void *obj,
                                               void *mesh_obj,
                                               const double *model_matrix,
                                               void *material_obj,
                                               const void *motion_key,
                                               const float *prev_bone_palette,
                                               const float *prev_morph_weights,
                                               const float *explicit_local_bounds_min,
                                               const float *explicit_local_bounds_max,
                                               int8_t conservative_bounds,
                                               int8_t disable_occlusion,
                                               float culling_pad) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    rt_material3d *mat =
        (rt_material3d *)rt_g3d_checked_or_null(material_obj, RT_G3D_MATERIAL3D_CLASS_ID);
    int8_t pending_has_splat = 0;
    const void *pending_splat_map = NULL;
    const void *pending_splat_layers[4] = {NULL, NULL, NULL, NULL};
    float pending_splat_layer_scales[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float validated_model_matrix[16];
    double vertex_rebase_origin[3] = {0.0, 0.0, 0.0};
    int rebase_identity_vertices = 0;
    void *new_draw_refs[CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP];
    int32_t new_draw_ref_count = 0;
    int suppress_normal_map = 0;
    uint32_t safe_vertex_count = 0;
    uint32_t safe_index_count = 0;
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (!mesh && mesh_obj && !rt_heap_is_payload(mesh_obj))
        mesh = (rt_mesh3d *)mesh_obj;
#endif
    if (!c)
        return;
    if (!mesh || !model_matrix || !mat) {
        canvas3d_clear_pending_splat(c);
        return;
    }
    if (!c->in_frame || (!c->gfx_win && !c->render_target) || !c->backend || !mesh->resident) {
        canvas3d_clear_pending_splat(c);
        return;
    }
    if (canvas3d_mat4_is_identity(model_matrix) ||
        canvas3d_mesh_positions64_needs_vertex_rebase(mesh)) {
        rebase_identity_vertices = canvas3d_compute_vertex_rebase_shift(
            c, model_matrix, vertex_rebase_origin, validated_model_matrix);
    }
    if (!rebase_identity_vertices &&
        !canvas3d_model_mat4_d2f_checked(c, model_matrix, validated_model_matrix)) {
        canvas3d_clear_pending_splat(c);
        rt_trap("Canvas3D.DrawMesh: model matrix must contain finite float-range values");
        return;
    }
    rt_mesh3d_repair_geometry_counts(mesh);

    if (mesh->morph_targets_ref && mesh->morph_deltas == NULL && mesh->morph_weights == NULL &&
        mesh->morph_shape_count == 0) {
        rt_canvas3d_draw_mesh_matrix_morphed_bounds(obj,
                                                    mesh_obj,
                                                    model_matrix,
                                                    material_obj,
                                                    motion_key,
                                                    mesh->morph_targets_ref,
                                                    explicit_local_bounds_min,
                                                    explicit_local_bounds_max,
                                                    conservative_bounds,
                                                    disable_occlusion);
        canvas3d_clear_pending_splat(c);
        return;
    }

    pending_has_splat = c->pending_has_splat;
    pending_splat_map = c->pending_splat_map;
    for (int i = 0; i < 4; i++) {
        pending_splat_layers[i] = c->pending_splat_layers[i];
        pending_splat_layer_scales[i] =
            canvas3d_sanitize_splat_layer_scale(c->pending_splat_layer_scales[i]);
    }

    safe_vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    safe_index_count = rt_mesh3d_validated_index_count(mesh);
    if (safe_vertex_count == 0 || safe_index_count == 0)
        return;
    int needs_generated_tangents = canvas3d_prepare_normal_map_tangent_state(mesh, mat);
    rt_mesh3d_refresh_bounds(mesh);

    if (rt_heap_is_payload(mesh_obj)) {
        if (!canvas3d_track_draw_resource(c,
                                          mesh_obj,
                                          new_draw_refs,
                                          &new_draw_ref_count,
                                          CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP))
            return;
    }
    if (!canvas3d_track_draw_resource(c,
                                      material_obj,
                                      new_draw_refs,
                                      &new_draw_ref_count,
                                      CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
        return;
    }
    if (!canvas3d_track_material_resources(
            c, mat, new_draw_refs, &new_draw_ref_count, CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
        return;
    }
    if (!canvas3d_track_draw_resource(c,
                                      pending_splat_map,
                                      new_draw_refs,
                                      &new_draw_ref_count,
                                      CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
        canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (!canvas3d_track_draw_resource(c,
                                          pending_splat_layers[i],
                                          new_draw_refs,
                                          &new_draw_ref_count,
                                          CANVAS3D_DRAW_RESOURCE_ROLLBACK_CAP)) {
            canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
            return;
        }
    }

    static const deferred_draw_t zero_queued_draw = {0};
    canvas3d_world_bounds_cache_key_t bounds_cache_key;
    deferred_draw_t queued;
    deferred_draw_t *dd = &queued;
    *dd = zero_queued_draw;
    dd->kind = DEFERRED_DRAW_MESH;
    dd->pass_kind = DEFERRED_PASS_MAIN;
    dd->visible = 1;
    dd->light_offset = -1;
    canvas3d_make_deferred_world_bounds_cache_key(
        &bounds_cache_key, mesh_obj ? mesh_obj : mesh, model_matrix);

    vgfx3d_vertex_t *queued_vertices = mesh->vertices;
    uint32_t *queued_indices = mesh->indices;
    if (needs_generated_tangents && !rebase_identity_vertices) {
        if (!canvas3d_snapshot_mesh_geometry_with_tangents_cached(
                c, mesh, mesh_obj, &queued_vertices, &queued_indices)) {
            if (canvas3d_should_snapshot_geometry(mesh, mesh_obj)) {
                if (!canvas3d_snapshot_mesh_geometry_cached(
                        c, mesh, mesh_obj, &queued_vertices, &queued_indices))
                    goto fail_after_refs;
            } else {
                queued_vertices = mesh->vertices;
                queued_indices = mesh->indices;
            }
            needs_generated_tangents = 0;
            suppress_normal_map = 1;
        }
    } else if (needs_generated_tangents || rebase_identity_vertices) {
        int snapshot_ok =
            rebase_identity_vertices
                ? canvas3d_snapshot_mesh_geometry_rebased(
                      c, mesh, vertex_rebase_origin, &queued_vertices, &queued_indices)
                : canvas3d_snapshot_mesh_geometry(c, mesh, &queued_vertices, &queued_indices);
        if (!snapshot_ok) {
            if (rebase_identity_vertices || canvas3d_should_snapshot_geometry(mesh, mesh_obj))
                goto fail_after_refs;
            queued_vertices = mesh->vertices;
            queued_indices = mesh->indices;
            rebase_identity_vertices = 0;
            if (!canvas3d_model_mat4_d2f_checked(c, model_matrix, validated_model_matrix))
                goto fail_after_refs;
            if (needs_generated_tangents) {
                needs_generated_tangents = 0;
                suppress_normal_map = 1;
            }
        }
        if (needs_generated_tangents &&
            !canvas3d_generate_snapshot_tangents(mesh, queued_vertices, queued_indices)) {
            needs_generated_tangents = 0;
            suppress_normal_map = 1;
        }
    } else if (canvas3d_should_snapshot_geometry(mesh, mesh_obj) &&
               !canvas3d_snapshot_mesh_geometry_cached(
                   c, mesh, mesh_obj, &queued_vertices, &queued_indices)) {
        goto fail_after_refs;
    }

    /* Build draw command */
    dd->cmd.vertices = queued_vertices;
    dd->cmd.vertex_count = safe_vertex_count;
    dd->cmd.indices = queued_indices;
    dd->cmd.index_count = safe_index_count;
    canvas3d_mark_cmd_validated_indices(&dd->cmd, mesh);
    dd->cmd.geometry_key =
        (rt_heap_is_payload(mesh_obj) && !rebase_identity_vertices)
            ? (needs_generated_tangents ? (const void *)&mesh->retained_tangent_cache_key
                                        : mesh_obj)
            : NULL;
    dd->cmd.geometry_revision = dd->cmd.geometry_key ? mesh->geometry_revision : 0;
    dd->cmd.compact_vertex_stream = (dd->cmd.geometry_key && mesh->compact_streams) ? 1 : 0;
    memcpy(dd->cmd.model_matrix, validated_model_matrix, sizeof(dd->cmd.model_matrix));
    canvas3d_resolve_previous_model(c,
                                    (uintptr_t)motion_key,
                                    dd->cmd.model_matrix,
                                    dd->cmd.prev_model_matrix,
                                    &dd->cmd.has_prev_model_matrix);
    canvas3d_texture_stream_touch_material(c, mat);
    canvas3d_fill_material_cmd(c, mat, &dd->cmd);
    canvas3d_apply_ibl_env(c, &dd->cmd);
    if (suppress_normal_map)
        canvas3d_disable_cmd_normal_map(&dd->cmd);

    /* Consume pending terrain splat data (if set by terrain draw path) */
    dd->cmd.has_splat = pending_has_splat;
    dd->cmd.splat_map = pending_splat_map;
    for (int i = 0; i < 4; i++) {
        dd->cmd.splat_layers[i] = pending_splat_layers[i];
        dd->cmd.splat_layer_scales[i] = pending_splat_layer_scales[i];
    }

    canvas3d_bind_skinning_cmd(c, &dd->cmd, mesh, prev_bone_palette);

    canvas3d_bind_morph_cmd(c, &dd->cmd, mesh, prev_morph_weights);

    /* Build light params (flattened at most once per light generation per frame) */
    if (!canvas3d_assign_frame_lights(c, dd)) {
        rt_trap("Canvas3D.DrawMesh: light snapshot allocation failed");
        goto fail_after_refs;
    }
    dd->ambient[0] = c->ambient[0];
    dd->ambient[1] = c->ambient[1];
    dd->ambient[2] = c->ambient[2];
    dd->wireframe = c->wireframe;
    dd->backface_cull = canvas3d_material_backface_cull(c, mat);
    dd->requires_blend = canvas3d_cmd_requires_blend(&dd->cmd);
    dd->culling_pad = (isfinite(culling_pad) && culling_pad > 0.0f) ? culling_pad : 0.0f;
    dd->has_local_bounds = 1;
    if (explicit_local_bounds_min && explicit_local_bounds_max &&
        canvas3d_bounds_are_valid(explicit_local_bounds_min, explicit_local_bounds_max)) {
        memcpy(dd->local_bounds_min, explicit_local_bounds_min, sizeof(dd->local_bounds_min));
        memcpy(dd->local_bounds_max, explicit_local_bounds_max, sizeof(dd->local_bounds_max));
    } else if (rebase_identity_vertices) {
        canvas3d_compute_vertices_aabb(
            queued_vertices, safe_vertex_count, dd->local_bounds_min, dd->local_bounds_max);
    } else {
        canvas3d_copy_or_compute_local_bounds(
            mesh, queued_vertices, dd->local_bounds_min, dd->local_bounds_max);
    }
    canvas3d_finalize_deferred_visibility_flags(dd, conservative_bounds, disable_occlusion);
    canvas3d_texture_stream_record_cmd(c, &dd->cmd, dd->local_bounds_min, dd->local_bounds_max);

    dd->sort_key = canvas3d_sanitize_sort_key(canvas3d_compute_sort_key(c,
                                                                        dd->cmd.model_matrix,
                                                                        dd->local_bounds_min,
                                                                        dd->local_bounds_max,
                                                                        dd->has_local_bounds,
                                                                        dd->requires_blend),
                                              dd->requires_blend);
    /* Without a caller motion key, mesh^material alone collapses every identical prop onto
     * one identity, so the transparent-sort tie-break degrades to submission order — alpha
     * shimmer whenever the app iterates an unordered container. Mixing in the quantized
     * translation keeps statics stable across frames while separating instances (the same
     * scheme the occlusion fingerprint uses). */
    if ((uintptr_t)motion_key) {
        dd->stable_sort_id = (uintptr_t)motion_key;
    } else {
        uint64_t id = (uint64_t)((uintptr_t)mesh_obj ^ ((uintptr_t)material_obj << 1));
        id = canvas3d_occlusion_mix_quantized_float(id, dd->cmd.model_matrix[3]);
        id = canvas3d_occlusion_mix_quantized_float(id, dd->cmd.model_matrix[7]);
        id = canvas3d_occlusion_mix_quantized_float(id, dd->cmd.model_matrix[11]);
        dd->stable_sort_id = (uintptr_t)(id ? id : 1u);
    }
    dd->occlusion_key = dd->stable_sort_id ? dd->stable_sort_id : (uintptr_t)queued_vertices;

    if (canvas3d_append_prepared_deferred_draw(c, &queued, &bounds_cache_key)) {
        canvas3d_clear_pending_splat(c);
        return;
    }
    canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
    rt_trap("Canvas3D.DrawMesh: deferred draw queue allocation failed");
    return;

fail_after_refs:
    canvas3d_release_new_draw_resources(c, new_draw_refs, new_draw_ref_count);
}

/// @brief Queue a 3D mesh draw with a model matrix and a sort key for transparency ordering.
///
/// `sort_key` is used by the deferred renderer to depth-sort
/// translucent draws back-to-front before flushing. Opaque
/// objects ignore it (the depth buffer handles their order).
/// @details Uses mesh-derived local bounds, normal culling policy, and no
///   culling padding. Resource lifetime and invalid-input behavior match
///   rt_canvas3d_draw_mesh_matrix_keyed_bounds().
/// @param obj Borrowed Canvas3D handle with an active 3D pass.
/// @param mesh_obj Borrowed live Mesh3D to queue.
/// @param model_matrix Borrowed array of 16 row-major doubles captured for the
///   deferred command.
/// @param material_obj Borrowed live Material3D whose required resources are
///   retained through frame cleanup.
/// @param motion_key Optional stable instance identity for motion history and
///   transparent ordering; may be `NULL`.
/// @param prev_bone_palette Optional borrowed previous-frame bone palette; may
///   be `NULL`.
/// @param prev_morph_weights Optional borrowed previous-frame morph weights; may
///   be `NULL`.
void rt_canvas3d_draw_mesh_matrix_keyed(void *obj,
                                        void *mesh_obj,
                                        const double *model_matrix,
                                        void *material_obj,
                                        const void *motion_key,
                                        const float *prev_bone_palette,
                                        const float *prev_morph_weights) {
    rt_canvas3d_draw_mesh_matrix_keyed_bounds(obj,
                                              mesh_obj,
                                              model_matrix,
                                              material_obj,
                                              motion_key,
                                              prev_bone_palette,
                                              prev_morph_weights,
                                              NULL,
                                              NULL,
                                              0,
                                              0,
                                              0.0f);
}

/// @brief Convenience: queue a mesh draw without an explicit sort key (uses default).
/// @param obj Borrowed Canvas3D handle with an active 3D pass.
/// @param mesh_obj Borrowed live Mesh3D to queue.
/// @param model_matrix Borrowed array of 16 row-major doubles captured for the
///   deferred command.
/// @param material_obj Borrowed live Material3D whose required resources are
///   retained through frame cleanup.
/// @see rt_canvas3d_draw_mesh_matrix_keyed
void rt_canvas3d_draw_mesh_matrix(void *obj,
                                  void *mesh_obj,
                                  const double *model_matrix,
                                  void *material_obj) {
    rt_canvas3d_draw_mesh_matrix_keyed(obj, mesh_obj, model_matrix, material_obj, NULL, NULL, NULL);
}

/// @brief Submit a mesh for drawing with the given transform and material.
/// @details Defers the draw into the per-frame queue. Actual rendering happens
///          in End(), which sorts opaque draws front-to-back and transparent draws
///          back-to-front for correct alpha blending. Inputs are borrowed at the
///          call boundary; the deferred command retains or snapshots every
///          resource it needs through frame cleanup.
/// @param obj Borrowed Canvas3D handle with an active 3D pass.
/// @param mesh_obj Borrowed live Mesh3D to submit.
/// @param transform_obj Borrowed Mat4 whose matrix is read during submission and
///   whose identity contributes to per-instance motion history.
/// @param material_obj Borrowed live Material3D applied to the draw.
void rt_canvas3d_draw_mesh(void *obj, void *mesh_obj, void *transform_obj, void *material_obj) {
    mat4_impl *transform = canvas3d_mat4_checked(transform_obj);
    uintptr_t motion_key;
    if (!transform)
        return;
    if (mesh_obj && !rt_g3d_has_class(mesh_obj, RT_G3D_MESH3D_CLASS_ID))
        return;
    motion_key = canvas3d_mesh_transform_motion_key(mesh_obj, material_obj, transform_obj);
    rt_canvas3d_draw_mesh_matrix_keyed(
        obj, mesh_obj, transform->m, material_obj, (const void *)motion_key, NULL, NULL);
}

/// @brief Apply a height-weighted XZ wind sway to a mesh's vertices in place.
/// @details Base vertices (lowest local-Y) stay planted; displacement scales with
///   the square of normalized height along (dir_x, dir_z), modulated by sin(phase) with a smaller
///   perpendicular flutter. Only the XZ position changes (height is preserved). Marks the
///   mesh geometry dirty so cached bounds/tangents/upload refresh. NULL/degenerate-safe.
/// @param mesh Mutable Mesh3D to deform; `NULL`, empty, or vertically degenerate
///   meshes are left unchanged.
/// @param dir_x X component of the horizontal sway direction.
/// @param dir_z Z component of the horizontal sway direction.
/// @param strength Maximum displacement scale; non-positive values leave the
///   mesh unchanged.
/// @param phase Animation phase in radians used by the sway and flutter waves.
void canvas3d_deform_mesh_wind(
    rt_mesh3d *mesh, double dir_x, double dir_z, double strength, double phase) {
    uint32_t n;
    float min_y;
    float max_y;
    float range;
    double swing;
    double flutter;
    if (!mesh || !mesh->vertices || mesh->vertex_count == 0 || !(strength > 0.0))
        return;
    n = mesh->vertex_count;
    min_y = mesh->vertices[0].pos[1];
    max_y = min_y;
    for (uint32_t i = 1; i < n; i++) {
        float y = mesh->vertices[i].pos[1];
        if (y < min_y)
            min_y = y;
        if (y > max_y)
            max_y = y;
    }
    range = max_y - min_y;
    if (!(range > 1e-4f))
        return;
    swing = sin(phase);
    flutter = sin(phase * 1.7 + 1.3);
    for (uint32_t i = 0; i < n; i++) {
        float h = (mesh->vertices[i].pos[1] - min_y) / range;
        if (h < 0.0f)
            h = 0.0f;
        else if (h > 1.0f)
            h = 1.0f;
        double w = (double)h * (double)h * strength;
        double sway = w * swing;
        double cross = w * 0.35 * flutter;
        double dx = dir_x * sway - dir_z * cross;
        double dz = dir_z * sway + dir_x * cross;
        mesh->vertices[i].pos[0] += (float)dx;
        mesh->vertices[i].pos[2] += (float)dz;
        if (mesh->positions64) {
            mesh->positions64[(size_t)i * 3u + 0u] += dx;
            mesh->positions64[(size_t)i * 3u + 2u] += dz;
        }
    }
    rt_mesh3d_touch_geometry_now(mesh);
}

/// @brief Draw a mesh with a per-vertex wind sway (e.g. tree canopies, foliage).
/// @details A height-weighted XZ displacement is applied on the CPU to a transient
///   clone of the mesh, so the trunk/base (low local-Y) stays planted while the
///   canopy (high local-Y) sways along (dir_x, dir_z). `phase` advances the sway
///   over time; pass per-instance phase offsets (e.g. derived from world position)
///   so neighbouring meshes move out of step. Works on every backend because it
///   deforms geometry before submission rather than relying on a vertex shader.
///   `strength <= 0` (or a degenerate mesh) falls back to a plain DrawMesh.
/// @param obj Borrowed Canvas3D handle with an active 3D pass.
/// @param mesh_obj Borrowed live Mesh3D that is cloned before deformation.
/// @param transform_obj Borrowed Mat4 used to place the submitted clone.
/// @param material_obj Borrowed live Material3D applied to the submitted clone.
/// @param dir_x X component of the horizontal sway direction.
/// @param dir_z Z component of the horizontal sway direction.
/// @param strength Maximum wind displacement scale; non-positive values bypass
///   cloning and queue the original mesh.
/// @param phase Animation phase in radians; vary it per instance to decorrelate
///   neighboring motion.
void rt_canvas3d_draw_mesh_wind(void *obj,
                                void *mesh_obj,
                                void *transform_obj,
                                void *material_obj,
                                double dir_x,
                                double dir_z,
                                double strength,
                                double phase) {
    rt_mesh3d *clone;
    void *clone_obj;
    if (!rt_canvas3d_checked_or_stack(obj))
        return;
    if (!mesh_obj || !rt_g3d_has_class(mesh_obj, RT_G3D_MESH3D_CLASS_ID))
        return;
    if (!transform_obj || !material_obj)
        return;
    if (!(strength > 0.0)) {
        rt_canvas3d_draw_mesh(obj, mesh_obj, transform_obj, material_obj);
        return;
    }

    clone_obj = rt_mesh3d_clone(mesh_obj);
    if (!clone_obj) {
        rt_canvas3d_draw_mesh(obj, mesh_obj, transform_obj, material_obj);
        return;
    }
    clone = (rt_mesh3d *)rt_g3d_checked_or_null(clone_obj, RT_G3D_MESH3D_CLASS_ID);
    canvas3d_deform_mesh_wind(clone, dir_x, dir_z, strength, phase);

    rt_canvas3d_draw_mesh(obj, clone_obj, transform_obj, material_obj);
    /* Keep the deformed clone alive until the deferred main pass replays, then
       drop our construction reference so the canvas owns the frame lifetime. */
    if (rt_canvas3d_add_temp_object(obj, clone_obj)) {
        if (rt_obj_release_check0(clone_obj))
            rt_obj_free(clone_obj);
    }
}
