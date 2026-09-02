//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_sw_vertex.inc
// Purpose: Software vertex transformation, frustum clipping, and Gouraud-light preparation.
// Key invariants:
//   - Model, skinning, history, tangent, and projection inputs are finite before transformation.
//   - Clip interpolation preserves every fragment attribute needed by the raster stage.
// Ownership/Lifetime:
//   - Transformed and clipped vertices live in context-owned reusable scratch storage.
//   - Source mesh and animation streams are borrowed from the active draw snapshot.
// Links: vgfx3d_backend_sw.c, vgfx3d_backend_sw_raster.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_sw_vertex.inc
 * @brief Implements software-backend vertex scratch management, homogeneous clipping, and
 *        Gouraud lighting.
 *
 * Source vertices are transformed into context-owned pipeline storage, clipped against the six
 * homogeneous view-frustum planes while preserving all interpolants, and optionally lit per
 * vertex for the legacy software shading path.
 */

/*==========================================================================
 * Pipeline vertex
 *=========================================================================*/

struct pipe_vert {
    float clip[4];
    float world[3];
    float normal[3];
    float tangent[4];
    float uv[2];
    float uv1[2];
    float color[4];
    /* ADR 0312 decal projector coords (s, t, d) + facing, from the BIND position. */
    float decal[4];
};

/// @brief Grow-on-demand scratch buffer for per-draw transformed vertices.
/// @details The software backend transforms each draw's vertices into a working buffer
///   before rasterisation. Reallocating the buffer per draw would pressure the heap and
///   waste cycles on memory-only work, so the context caches the largest buffer ever
///   needed; subsequent smaller draws reuse it free. Returns the scratch pointer or
///   `NULL` on overflow / allocation failure.
/// @param ctx Borrowed software context that owns the reusable scratch allocation.
/// @param count Positive number of pipeline vertices required by the draw.
/// @return Context-owned storage for at least @p count vertices, or null on invalid input,
///         arithmetic overflow, or allocation failure.
static pipe_vert_t *sw_acquire_pipe_vertices(sw_context_t *ctx, uint32_t count) {
    size_t allocation_count;

    if (!ctx || count == 0)
        return NULL;
    if (ctx->vertex_scratch_capacity >= count)
        return ctx->vertex_scratch;
    if (!sw_geometric_capacity(
            ctx->vertex_scratch_capacity, (size_t)count, sizeof(pipe_vert_t), &allocation_count) ||
        allocation_count > UINT32_MAX)
        return NULL;
    pipe_vert_t *new_scratch =
        (pipe_vert_t *)realloc(ctx->vertex_scratch, allocation_count * sizeof(pipe_vert_t));
    if (!new_scratch)
        return NULL;
    ctx->vertex_scratch = new_scratch;
    ctx->vertex_scratch_capacity = (uint32_t)allocation_count;
    return ctx->vertex_scratch;
}

/// @brief Cached `ZANNA_3D_DEBUG` env-var query for tracing.
///
/// Set the env var to anything non-zero to enable per-draw debug
/// output (vertex counts, shader paths, etc.). Cached on first call
/// to avoid repeated `getenv` overhead in the hot path.
/// @return Non-zero when software Graphics3D debug tracing is enabled.
static int sw_debug_enabled(void) {
    /* 0 = uninitialized, 1 = one thread is reading the environment, 2 = disabled,
     * 3 = enabled. This is a real once gate rather than a racy "benign" cache: the C
     * memory model does not permit concurrent plain reads/writes merely because an
     * aligned machine store happens to be indivisible on supported CPUs. */
    static int state = 0;
    int current = rt_atomic_load_i32(&state, __ATOMIC_ACQUIRE);
    if (current >= 2)
        return current == 3;
    int expected = 0;
    if (rt_atomic_compare_exchange_i32(&state, &expected, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const char *env = getenv("ZANNA_3D_DEBUG");
        int enabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
        rt_atomic_store_i32(&state, enabled ? 3 : 2, __ATOMIC_RELEASE);
        return enabled;
    }
    while ((current = rt_atomic_load_i32(&state, __ATOMIC_ACQUIRE)) == 1) {
        /* The winner performs one getenv call and immediately publishes the result. */
    }
    return current == 3;
}

#define MAX_CLIP_VERTS 9

/// @brief Linearly interpolate every attribute of two pipeline vertices.
///
/// Used during clipping to construct intersection vertices on the
/// frustum boundary. Interpolates clip pos, world pos, normal,
/// tangent, UV, and color.
/// @param a Borrowed edge-start pipeline vertex.
/// @param b Borrowed edge-end pipeline vertex.
/// @param t Interpolation parameter, conventionally in `[0, 1]`.
/// @param out Receives all interpolated pipeline attributes.
static void pipe_lerp(const pipe_vert_t *a, const pipe_vert_t *b, float t, pipe_vert_t *out) {
    float s = 1.0f - t;
    for (int i = 0; i < 4; i++)
        out->clip[i] = s * a->clip[i] + t * b->clip[i];
    for (int i = 0; i < 3; i++)
        out->world[i] = s * a->world[i] + t * b->world[i];
    for (int i = 0; i < 3; i++)
        out->normal[i] = s * a->normal[i] + t * b->normal[i];
    for (int i = 0; i < 4; i++)
        out->tangent[i] = s * a->tangent[i] + t * b->tangent[i];
    for (int i = 0; i < 2; i++)
        out->uv[i] = s * a->uv[i] + t * b->uv[i];
    for (int i = 0; i < 2; i++)
        out->uv1[i] = s * a->uv1[i] + t * b->uv1[i];
    for (int i = 0; i < 4; i++)
        out->color[i] = s * a->color[i] + t * b->color[i];
    for (int i = 0; i < 4; i++)
        out->decal[i] = s * a->decal[i] + t * b->decal[i];
}

/*==========================================================================
 * Sutherland-Hodgman frustum clipping
 *=========================================================================*/

/// @brief Signed distance from a clip-space vertex to one of six frustum planes.
/// In homogeneous clip coordinates, a point is inside the view volume when
/// -w <= x,y,z <= w. Each plane's distance is arranged so that >= 0 = inside:
///   0: left   (-x <= w  →  x+w >= 0)    1: right (x <= w  →  w-x >= 0)
///   2: bottom (-y <= w  →  y+w >= 0)    3: top   (y <= w  →  w-y >= 0)
///   4: near   (-z <= w  →  z+w >= 0)    5: far   (z <= w  →  w-z >= 0)
/// @param v Borrowed pipeline vertex containing homogeneous clip coordinates.
/// @param plane Frustum-plane index from zero through five.
/// @return Signed homogeneous distance, non-negative on the plane's inside half-space.
static float clip_dist(const pipe_vert_t *v, int plane) {
    float x = v->clip[0], y = v->clip[1], z = v->clip[2], w = v->clip[3];
    switch (plane) {
        case 0:
            return x + w;
        case 1:
            return w - x;
        case 2:
            return y + w;
        case 3:
            return w - y;
        case 4:
            return z + w;
        case 5:
            return w - z;
        default:
            return 0.0f;
    }
}

/// @brief Sutherland-Hodgman: clip a convex polygon against one frustum plane.
///
/// Walks each edge `i → j`. If `i` is inside, emit it; if the edge
/// crosses the plane, emit the intersection. Result is a new convex
/// polygon in `out`. Bounded by `MAX_CLIP_VERTS` (9) — extra
/// intersections beyond that are dropped (won't happen for one
/// triangle clipped against six planes).
/// @param in Borrowed input polygon.
/// @param in_count Number of valid input vertices.
/// @param out Receives the clipped polygon with capacity for @c MAX_CLIP_VERTS vertices.
/// @param plane Frustum-plane index to clip against.
/// @return Number of output vertices, or zero when no polygon remains.
static int clip_poly_plane(const pipe_vert_t *in, int in_count, pipe_vert_t *out, int plane) {
    if (in_count < 1)
        return 0;
    int out_count = 0;
    for (int i = 0; i < in_count; i++) {
        int j = (i + 1) % in_count;
        float di = clip_dist(&in[i], plane);
        float dj = clip_dist(&in[j], plane);
        if (di >= 0.0f) {
            if (out_count < MAX_CLIP_VERTS)
                out[out_count++] = in[i];
            if (dj < 0.0f) {
                float denom = di - dj;
                if (fabsf(denom) > 1e-10f) {
                    float t = di / denom;
                    if (out_count < MAX_CLIP_VERTS)
                        pipe_lerp(&in[i], &in[j], t, &out[out_count++]);
                }
            }
        } else if (dj >= 0.0f) {
            float denom = di - dj;
            if (fabsf(denom) < 1e-10f)
                continue;
            float t = di / denom;
            if (out_count < MAX_CLIP_VERTS)
                pipe_lerp(&in[i], &in[j], t, &out[out_count++]);
        }
    }
    return out_count;
}

/// @brief Clip a triangle against all 6 frustum planes, producing a convex polygon.
/// Uses double-buffer ping-pong (buf_a ↔ buf_b) to avoid allocation.
/// Returns vertex count (>= 3) or 0 if fully clipped.
/// @param tri Borrowed array of three pipeline vertices.
/// @param out Receives the clipped polygon with capacity for @c MAX_CLIP_VERTS vertices.
/// @return Clipped vertex count of at least three, or zero when fully outside.
static int clip_triangle(const pipe_vert_t *tri, pipe_vert_t *out) {
    pipe_vert_t buf_a[MAX_CLIP_VERTS], buf_b[MAX_CLIP_VERTS];
    memcpy(buf_a, tri, 3 * sizeof(pipe_vert_t));
    int count = 3;
    pipe_vert_t *src = buf_a;
    pipe_vert_t *dst = buf_b;
    for (int plane = 0; plane < 6; plane++) {
        count = clip_poly_plane(src, count, dst, plane);
        if (count < 3)
            return 0;
        pipe_vert_t *tmp = src;
        src = dst;
        dst = tmp;
    }
    memcpy(out, src, (size_t)count * sizeof(pipe_vert_t));
    return count;
}

/*==========================================================================
 * Blinn-Phong per-vertex lighting (Gouraud)
 *=========================================================================*/

/// @brief Per-vertex Blinn-Phong lighting (Gouraud shading).
///
/// For each light, computes diffuse + Blinn-Phong specular contribution
/// and accumulates into the vertex color. Native rectangle/sphere emitters use
/// their closest analytic point; volume lights add isotropic local irradiance.
/// Shaded color is later interpolated across triangle pixels (Gouraud).
/// Materials with `unlit` flag bypass lighting entirely.
/// @param v Mutable pipeline vertex whose color is replaced by the material/light result.
/// @param ctx Borrowed software context containing camera state.
/// @param cmd Borrowed sanitized draw command containing material properties.
/// @param lights Optional borrowed sanitized light array.
/// @param light_count Number of entries supplied in @p lights.
/// @param ambient Optional borrowed three-component ambient color; null means black.
static void compute_lighting(pipe_vert_t *v,
                             const sw_context_t *ctx,
                             const vgfx3d_draw_cmd_t *cmd,
                             const vgfx3d_light_params_t *lights,
                             int32_t light_count,
                             const float *ambient) {
    static const float zero_ambient[3] = {0.0f, 0.0f, 0.0f};
    if (!v || !cmd)
        return;
    if (!ambient)
        ambient = zero_ambient;
    light_count = sw_sanitize_light_count(lights, light_count);
    if (cmd->unlit) {
        v->color[0] = cmd->diffuse_color[0] * v->color[0];
        v->color[1] = cmd->diffuse_color[1] * v->color[1];
        v->color[2] = cmd->diffuse_color[2] * v->color[2];
        /* Material alpha applies to unlit draws too — the GPU shaders compute
         * diffuse.a * materialAlpha * vertex.a, so skipping cmd->alpha here made
         * Material3D.SetAlpha a no-op for unlit software draws (parity bug). */
        v->color[3] = cmd->diffuse_color[3] * v->color[3] * cmd->alpha;
        return;
    }

    float nx = v->normal[0], ny = v->normal[1], nz = v->normal[2];
    sw_normalize3(&nx, &ny, &nz, 0.0f, 0.0f, 1.0f);

    float vx;
    float vy;
    float vz;

    sw_compute_view_vector(ctx, v->world[0], v->world[1], v->world[2], &vx, &vy, &vz);

    /* Effective diffuse = material diffuse × vertex color */
    float dr = cmd->diffuse_color[0] * v->color[0];
    float dg = cmd->diffuse_color[1] * v->color[1];
    float db = cmd->diffuse_color[2] * v->color[2];

    float r = ambient[0] * dr;
    float g = ambient[1] * dg;
    float b = ambient[2] * db;

    for (int32_t li = 0; li < light_count; li++) {
        const vgfx3d_light_params_t *light = &lights[li];
        float lx, ly, lz, atten = 1.0f;
        if (light->type == 2) {
            r += light->color[0] * light->intensity * dr;
            g += light->color[1] * light->intensity * dg;
            b += light->color[2] * light->intensity * db;
            continue;
        } else {
            int light_mode = sw_eval_analytic_light(
                light, v->world[0], v->world[1], v->world[2], &lx, &ly, &lz, &atten);
            if (light_mode == 0)
                continue;
            if (light_mode == 2) {
                r += light->color[0] * light->intensity * dr * atten;
                g += light->color[1] * light->intensity * dg * atten;
                b += light->color[2] * light->intensity * db * atten;
                continue;
            }
        }

        float intensity = light->intensity;
        float ndl = nx * lx + ny * ly + nz * lz;
        if (ndl < 0.0f)
            ndl = 0.0f;

        r += light->color[0] * intensity * ndl * dr * atten;
        g += light->color[1] * intensity * ndl * dg * atten;
        b += light->color[2] * intensity * ndl * db * atten;

        if (ndl > 0.0f && cmd->shininess > 0.0f) {
            float hx = lx + vx, hy = ly + vy, hz = lz + vz;
            if (!sw_normalize3(&hx, &hy, &hz, 0.0f, 0.0f, 1.0f))
                continue;
            float ndh = nx * hx + ny * hy + nz * hz;
            if (ndh < 0.0f)
                ndh = 0.0f;
            float spec = sw_material_powf(ndh, cmd->shininess);
            r += light->color[0] * intensity * spec * cmd->specular[0] * atten;
            g += light->color[1] * intensity * spec * cmd->specular[1] * atten;
            b += light->color[2] * intensity * spec * cmd->specular[2] * atten;
        }
    }

    /* Emissive contribution (additive, independent of lighting) */
    float emissive_scale = cmd->emissive_intensity;
    r += cmd->emissive_color[0] * emissive_scale;
    g += cmd->emissive_color[1] * emissive_scale;
    b += cmd->emissive_color[2] * emissive_scale;

    /* Apply shading model post-processing */
    switch (cmd->shading_model) {
        case 1: { /* Toon: quantize diffuse to N bands */
            float bands = cmd->custom_params[0] > 0.5f ? cmd->custom_params[0] : 4.0f;
            r = floorf(r * bands) / bands;
            g = floorf(g * bands) / bands;
            b = floorf(b * bands) / bands;
            break;
        }
        case 4: { /* Fresnel: angle-dependent alpha */
            float ndv = nx * vx + ny * vy + nz * vz;
            if (ndv < 0.0f)
                ndv = 0.0f;
            float power = cmd->custom_params[0] > 0.1f ? cmd->custom_params[0] : 3.0f;
            float bias = cmd->custom_params[1];
            float fresnel = clamp01f(sw_material_powf(1.0f - ndv, power) + bias);
            v->color[3] *= fresnel;
            break;
        }
        case 5: { /* Emissive glow: boost emissive by strength */
            float strength = cmd->custom_params[0] > 0.0f ? cmd->custom_params[0] : 2.0f;
            r += cmd->emissive_color[0] * emissive_scale * (strength - 1.0f);
            g += cmd->emissive_color[1] * emissive_scale * (strength - 1.0f);
            b += cmd->emissive_color[2] * emissive_scale * (strength - 1.0f);
            break;
        }
        default: /* 0=BlinnPhong (already computed), 2=PBR, 3=Unlit (handled above) */
            break;
    }

    v->color[0] = r;
    v->color[1] = g;
    v->color[2] = b;
    v->color[3] = v->color[3] * cmd->diffuse_color[3] * cmd->alpha;
}
