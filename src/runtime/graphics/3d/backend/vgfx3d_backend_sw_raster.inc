//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_sw_raster.inc
// Purpose: Software edge-function triangle rasterization and fragment shading.
// Key invariants:
//   - Clipped bounds, surface strides, indices, and perspective denominators are validated first.
//   - Every interpolated attribute uses the same finite perspective-correct barycentric weights.
// Ownership/Lifetime:
//   - Raster jobs borrow immutable draw snapshots and write disjoint framebuffer tile regions.
//   - Texture views borrow Pixels storage retained by the enclosing Canvas command snapshot.
// Links: vgfx3d_backend_sw.c, vgfx3d_backend_sw_vertex.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_sw_raster.inc
 * @brief Implements edge-function triangle rasterization and per-fragment shading for the
 *        software Graphics3D backend.
 *
 * The fragment performs perspective-correct attribute interpolation, texture and terrain-splat
 * sampling, PBR or legacy lighting, environment reflections, shadowing, fog, alpha handling,
 * depth testing, and top-left-rule framebuffer coverage. Scissored entry points allow independent
 * worker jobs to shade disjoint tile regions deterministically.
 */

/*==========================================================================
 * Edge-function triangle rasterizer
 *=========================================================================*/

static inline void sw_compute_view_vector(const sw_context_t *ctx,
                                          float wx,
                                          float wy,
                                          float wz,
                                          float *out_vx,
                                          float *out_vy,
                                          float *out_vz);
static inline float sw_compute_fog_distance(const sw_context_t *ctx, float wx, float wy, float wz);

/// @brief Perspective-correct, slot-transformed UV pairs for every texture
///   slot consumed by the per-pixel lighting stages.
struct sw_slot_uvs {
    float normal_u, normal_v;
    float specular_u, specular_v;
    float emissive_u, emissive_v;
    float mr_u, mr_v;
    float ao_u, ao_v;
    float lm_u, lm_v; /* raw TEXCOORD_1 for the baked lightmap atlas */
};

static void sw_apply_environment_reflection(const vgfx3d_draw_cmd_t *cmd,
                                            const sw_pixels_view *normal_map,
                                            const sw_pixels_view *metallic_roughness_map,
                                            float attr_b0,
                                            float attr_b1,
                                            float attr_b2,
                                            float uv_b0,
                                            float uv_b1,
                                            float uv_b2,
                                            const screen_vert_t *v0,
                                            const screen_vert_t *v1,
                                            const screen_vert_t *v2,
                                            const sw_context_t *ctx,
                                            float normal_sign,
                                            const float *precomputed_world_normal,
                                            const struct sw_slot_uvs *precomputed_uvs,
                                            float *inout_r,
                                            float *inout_g,
                                            float *inout_b);

// Inline math helpers — used heavily in the per-pixel PBR shading path,
// inlined for the obvious reason that one indirect call per pixel
// would dominate the cost.

/// @brief 3-vector dot product (six args, avoids array packing overhead).
/// @param ax First vector X component.
/// @param ay First vector Y component.
/// @param az First vector Z component.
/// @param bx Second vector X component.
/// @param by Second vector Y component.
/// @param bz Second vector Z component.
/// @return Scalar dot product of the two vectors.
static inline float dot3f(float ax, float ay, float az, float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

/// @brief Convert screen-space barycentrics into perspective-correct attribute weights.
/// @param b0 First screen-space barycentric coordinate.
/// @param b1 Second screen-space barycentric coordinate.
/// @param b2 Third screen-space barycentric coordinate.
/// @param inv_w0 Reciprocal clip-space W for the first vertex.
/// @param inv_w1 Reciprocal clip-space W for the second vertex.
/// @param inv_w2 Reciprocal clip-space W for the third vertex.
/// @param out_b0 Receives the corrected first weight.
/// @param out_b1 Receives the corrected second weight.
/// @param out_b2 Receives the corrected third weight.
/// @return Non-zero when finite normalized weights were written, otherwise zero.
static int sw_perspective_attribute_weights(float b0,
                                            float b1,
                                            float b2,
                                            float inv_w0,
                                            float inv_w1,
                                            float inv_w2,
                                            float *out_b0,
                                            float *out_b1,
                                            float *out_b2) {
    double w0;
    double w1;
    double w2;
    double sum;
    if (!out_b0 || !out_b1 || !out_b2 || !isfinite(b0) || !isfinite(b1) || !isfinite(b2) ||
        !isfinite(inv_w0) || !isfinite(inv_w1) || !isfinite(inv_w2) || inv_w0 <= 0.0f ||
        inv_w1 <= 0.0f || inv_w2 <= 0.0f)
        return 0;
    w0 = (double)b0 * (double)inv_w0;
    w1 = (double)b1 * (double)inv_w1;
    w2 = (double)b2 * (double)inv_w2;
    sum = w0 + w1 + w2;
    if (!isfinite(sum) || sum <= 1e-20)
        return 0;
    *out_b0 = (float)(w0 / sum);
    *out_b1 = (float)(w1 / sum);
    *out_b2 = (float)(w2 / sum);
    return isfinite(*out_b0) && isfinite(*out_b1) && isfinite(*out_b2);
}

/// @brief In-place 3-vector normalization (no-op for zero-length vectors).
/// @param x In/out X component.
/// @param y In/out Y component.
/// @param z In/out Z component.
static inline void normalize3f(float *x, float *y, float *z) {
    (void)sw_normalize3(x, y, z, 0.0f, 0.0f, 0.0f);
}

/// @brief Compute the unit view vector from a world-space point to the camera.
/// @details Branches on projection mode: orthographic cameras have a
///   constant view direction (camera looks along `-cam_forward` everywhere
///   in the scene), so we return the negated forward vector regardless of
///   the surface position. Perspective cameras have a per-fragment view
///   direction that points from the fragment back to the eye position.
///   The result is renormalized because subtle floating-point error in
///   the subtraction can drift the magnitude away from 1.0 enough to
///   affect specular highlight shape.
/// @param ctx Borrowed software context containing camera pose and projection type.
/// @param wx World-space point X coordinate.
/// @param wy World-space point Y coordinate.
/// @param wz World-space point Z coordinate.
/// @param out_vx Receives the normalized view-direction X component.
/// @param out_vy Receives the normalized view-direction Y component.
/// @param out_vz Receives the normalized view-direction Z component.
static inline void sw_compute_view_vector(const sw_context_t *ctx,
                                          float wx,
                                          float wy,
                                          float wz,
                                          float *out_vx,
                                          float *out_vy,
                                          float *out_vz) {
    float vx;
    float vy;
    float vz;

    if (!ctx || !out_vx || !out_vy || !out_vz) {
        return;
    }
    if (ctx->cam_is_ortho) {
        vx = -ctx->cam_forward[0];
        vy = -ctx->cam_forward[1];
        vz = -ctx->cam_forward[2];
    } else {
        vx = ctx->cam_pos[0] - wx;
        vy = ctx->cam_pos[1] - wy;
        vz = ctx->cam_pos[2] - wz;
    }
    normalize3f(&vx, &vy, &vz);
    *out_vx = vx;
    *out_vy = vy;
    *out_vz = vz;
}

/// @brief Compute the effective fog distance from the camera to a world-space point.
/// @details Orthographic cameras use the *depth along forward* (projection
///   of the world→camera delta onto `cam_forward`, absolute value) so fog
///   density depends only on how far in front of the camera a fragment
///   sits, not its lateral offset — matches how fog attenuation behaves
///   in real ortho-rendered scenes. Perspective cameras use plain
///   euclidean distance to the eye, giving the familiar radial fog
///   falloff. Null context returns 0 so missed-setup paths produce no
///   fog rather than crashing.
/// @param ctx Borrowed software context containing camera pose and projection type.
/// @param wx World-space point X coordinate.
/// @param wy World-space point Y coordinate.
/// @param wz World-space point Z coordinate.
/// @return Non-negative forward depth for orthographic cameras or Euclidean eye distance for
///         perspective cameras; zero for a null context.
static inline float sw_compute_fog_distance(const sw_context_t *ctx, float wx, float wy, float wz) {
    if (!ctx)
        return 0.0f;
    if (ctx->cam_is_ortho) {
        return fabsf((wx - ctx->cam_pos[0]) * ctx->cam_forward[0] +
                     (wy - ctx->cam_pos[1]) * ctx->cam_forward[1] +
                     (wz - ctx->cam_pos[2]) * ctx->cam_forward[2]);
    }
    {
        float dx = wx - ctx->cam_pos[0];
        float dy = wy - ctx->cam_pos[1];
        float dz = wz - ctx->cam_pos[2];
        return sw_length3(dx, dy, dz);
    }
}

/// @brief Blend an environment-cubemap reflection into the shaded fragment color.
/// @details Barycentric-interpolates the world position, perturbs the
///   surface normal through the optional tangent-space normal map,
///   computes the reflection vector about the view direction, samples
///   the bound environment cubemap (optionally roughness-blurred via
///   `rt_cubemap_sample_roughness` when a metallic/roughness map is
///   bound), and lerps the result into `inout_rgb` by the material's
///   reflectivity scalar. All early-out branches (no cubemap bound,
///   zero reflectivity, no valid view) leave `inout_rgb` untouched so
///   the caller can invoke this unconditionally and not pay for
///   reflection work on surfaces that don't want it.
/// @param cmd Borrowed draw command containing environment and material settings.
/// @param normal_map Optional borrowed normal-map view.
/// @param metallic_roughness_map Optional borrowed PBR material-map view.
/// @param attr_b0 Perspective-correct first attribute weight.
/// @param attr_b1 Perspective-correct second attribute weight.
/// @param attr_b2 Perspective-correct third attribute weight.
/// @param uv_b0 First screen-space weight used by slot-specific UV interpolation.
/// @param uv_b1 Second screen-space weight used by slot-specific UV interpolation.
/// @param uv_b2 Third screen-space weight used by slot-specific UV interpolation.
/// @param v0 Borrowed first triangle vertex.
/// @param v1 Borrowed second triangle vertex.
/// @param v2 Borrowed third triangle vertex.
/// @param ctx Borrowed software context containing camera state.
/// @param normal_sign -1 for a back-facing cull-off triangle, +1 otherwise (ZB-21);
///   only consulted when the fallback recompute runs (the main pass's
///   precomputed normal already carries the flip).
/// @param precomputed_world_normal Optional borrowed normalized shading normal from the main pass.
/// @param precomputed_uvs Optional borrowed slot UVs from the main shading pass.
/// @param inout_r In/out fragment red component.
/// @param inout_g In/out fragment green component.
/// @param inout_b In/out fragment blue component.
static void sw_apply_environment_reflection(const vgfx3d_draw_cmd_t *cmd,
                                            const sw_pixels_view *normal_map,
                                            const sw_pixels_view *metallic_roughness_map,
                                            float attr_b0,
                                            float attr_b1,
                                            float attr_b2,
                                            float uv_b0,
                                            float uv_b1,
                                            float uv_b2,
                                            const screen_vert_t *v0,
                                            const screen_vert_t *v1,
                                            const screen_vert_t *v2,
                                            const sw_context_t *ctx,
                                            float normal_sign,
                                            const float *precomputed_world_normal,
                                            const struct sw_slot_uvs *precomputed_uvs,
                                            float *inout_r,
                                            float *inout_g,
                                            float *inout_b) {
    float wx;
    float wy;
    float wz;
    float pnx;
    float pny;
    float pnz;
    float vdx;
    float vdy;
    float vdz;
    float reflectivity;
    float roughness;
    float normal_u = 0.0f;
    float normal_v = 0.0f;
    float mr_u = 0.0f;
    float mr_v = 0.0f;

    if (!cmd || !ctx || !inout_r || !inout_g || !inout_b || !cmd->env_map ||
        cmd->reflectivity <= 0.0001f)
        return;

    wx = attr_b0 * v0->wx + attr_b1 * v1->wx + attr_b2 * v2->wx;
    wy = attr_b0 * v0->wy + attr_b1 * v1->wy + attr_b2 * v2->wy;
    wz = attr_b0 * v0->wz + attr_b1 * v1->wz + attr_b2 * v2->wz;
    pnx = attr_b0 * v0->nx + attr_b1 * v1->nx + attr_b2 * v2->nx;
    pny = attr_b0 * v0->ny + attr_b1 * v1->ny + attr_b2 * v2->ny;
    pnz = attr_b0 * v0->nz + attr_b1 * v1->nz + attr_b2 * v2->nz;
    normalize3f(&pnx, &pny, &pnz);
    pnx *= normal_sign;
    pny *= normal_sign;
    pnz *= normal_sign;

    /* Reuse the slot UVs the main shading path already interpolated for this
     * fragment; only recompute them for fragments that skipped that path. */
    if (precomputed_uvs) {
        normal_u = precomputed_uvs->normal_u;
        normal_v = precomputed_uvs->normal_v;
        mr_u = precomputed_uvs->mr_u;
        mr_v = precomputed_uvs->mr_v;
    } else {
        sw_interpolate_uv_for_slot(cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
                                   uv_b0,
                                   uv_b1,
                                   uv_b2,
                                   v0,
                                   v1,
                                   v2,
                                   &normal_u,
                                   &normal_v);
        sw_interpolate_uv_for_slot(cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
                                   uv_b0,
                                   uv_b1,
                                   uv_b2,
                                   v0,
                                   v1,
                                   v2,
                                   &mr_u,
                                   &mr_v);
    }

    /* When the main shading path already produced this fragment's perturbed
     * world normal, reuse it (below) and skip the redundant per-pixel normal-map
     * sample + TBN rebuild here. */
    if (!precomputed_world_normal && normal_map) {
        float ptx = attr_b0 * v0->tx + attr_b1 * v1->tx + attr_b2 * v2->tx;
        float pty = attr_b0 * v0->ty + attr_b1 * v1->ty + attr_b2 * v2->ty;
        float ptz = attr_b0 * v0->tz + attr_b1 * v1->tz + attr_b2 * v2->tz;
        float ptw = attr_b0 * v0->tw + attr_b1 * v1->tw + attr_b2 * v2->tw;
        float tlen = sw_length3(ptx, pty, ptz);

        if (tlen > 1e-7f) {
            float tnr;
            float tng;
            float tnb;
            float tna;
            float map_x;
            float map_y;
            float map_z;
            float tdotn;
            float tangent_sign;
            float bbx;
            float bby;
            float bbz;

            ptx /= tlen;
            pty /= tlen;
            ptz /= tlen;
            tdotn = ptx * pnx + pty * pny + ptz * pnz;
            ptx -= tdotn * pnx;
            pty -= tdotn * pny;
            ptz -= tdotn * pnz;
            normalize3f(&ptx, &pty, &ptz);

            sample_texture_slot_ex(normal_map,
                                   cmd,
                                   RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
                                   normal_u,
                                   normal_v,
                                   &tnr,
                                   &tng,
                                   &tnb,
                                   &tna);
            map_x = (tnr * 2.0f - 1.0f) * cmd->normal_scale;
            map_y = (tng * 2.0f - 1.0f) * cmd->normal_scale;
            map_z = tnb * 2.0f - 1.0f;
            tangent_sign = ptw < 0.0f ? -1.0f : 1.0f;
            bbx = (pny * ptz - pnz * pty) * tangent_sign;
            bby = (pnz * ptx - pnx * ptz) * tangent_sign;
            bbz = (pnx * pty - pny * ptx) * tangent_sign;
            pnx = ptx * map_x + bbx * map_y + pnx * map_z;
            pny = pty * map_x + bby * map_y + pny * map_z;
            pnz = ptz * map_x + bbz * map_y + pnz * map_z;
            normalize3f(&pnx, &pny, &pnz);
        }
    }
    if (precomputed_world_normal) {
        pnx = precomputed_world_normal[0];
        pny = precomputed_world_normal[1];
        pnz = precomputed_world_normal[2];
    }

    sw_compute_view_vector(ctx, wx, wy, wz, &vdx, &vdy, &vdz);
    roughness = clamp01f(cmd->roughness);
    if (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR && metallic_roughness_map) {
        float mrr;
        float mrg;
        float mrb;
        float mra;
        sample_texture_slot_ex(metallic_roughness_map,
                               cmd,
                               RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
                               mr_u,
                               mr_v,
                               &mrr,
                               &mrg,
                               &mrb,
                               &mra);
        roughness = clamp01f(roughness * mrg);
        if (roughness < 0.045f)
            roughness = 0.045f;
    }
    reflectivity = clamp01f(cmd->reflectivity);
    if (reflectivity > 0.0f) {
        float dot_nv = dot3f(pnx, pny, pnz, vdx, vdy, vdz);
        float rx = 2.0f * dot_nv * pnx - vdx;
        float ry = 2.0f * dot_nv * pny - vdy;
        float rz = 2.0f * dot_nv * pnz - vdz;
        float env_r;
        float env_g;
        float env_b;

        normalize3f(&rx, &ry, &rz);
        rt_cubemap_sample_roughness(
            (const rt_cubemap3d *)cmd->env_map, rx, ry, rz, roughness, &env_r, &env_g, &env_b);
        *inout_r = *inout_r * (1.0f - reflectivity) + env_r * reflectivity;
        *inout_g = *inout_g * (1.0f - reflectivity) + env_g * reflectivity;
        *inout_b = *inout_b * (1.0f - reflectivity) + env_b * reflectivity;
    }
}

/// @brief PBR D term with the material-invariant alpha-squared value precomputed.
/// @param ndh Clamped dot product between surface normal and half vector.
/// @param alpha_squared Fourth power of perceptual roughness.
/// @return GGX normal-distribution value with a small denominator guard.
static inline float pbr_distribution_ggx_alpha(float ndh, float alpha_squared) {
    const float kPi = 3.14159265358979323846f;
    float denom = ndh * ndh * (alpha_squared - 1.0f) + 1.0f;
    return alpha_squared / (kPi * denom * denom + 1e-6f);
}

/// @brief PBR G1 term with the material-invariant geometry factor precomputed.
/// @param nd Clamped normal-direction dot product.
/// @param geometry_k Schlick geometry factor `(roughness + 1)^2 / 8`.
/// @return Schlick-GGX single-direction visibility term.
static inline float pbr_geometry_schlick_ggx_k(float nd, float geometry_k) {
    return nd / (nd * (1.0f - geometry_k) + geometry_k + 1e-6f);
}

/// @brief Per-triangle invariants shared by every fragment of one raster_triangle
///   call. Built once before the pixel loop; sw_shade_fragment reads it per pixel
///   so the rasterizer's inner loop stays a thin barycentric/depth-test skeleton.
typedef struct {
    uint8_t *pixels;
    float *zbuf;
    int32_t stride;
    const screen_vert_t *v0;
    const screen_vert_t *v1;
    const screen_vert_t *v2;
    const sw_pixels_view *tex;
    const sw_pixels_view *emissive_tex;
    const float *emissive_color;
    const sw_pixels_view *normal_map;
    const sw_pixels_view *specular_map;
    const sw_pixels_view *metallic_roughness_map;
    const sw_pixels_view *ao_map;
    const sw_pixels_view *lightmap_tex; /* baked GI atlas view (TEXCOORD_1) or NULL */
    const sw_pixels_view *decal_tex;    /* ADR 0312 decal view or NULL */
    const vgfx3d_draw_cmd_t *cmd;
    const vgfx3d_light_params_t *lights;
    int32_t light_count;
    const vgfx3d_cluster_table_t *cluster_table;
    int32_t framebuffer_width;
    int32_t framebuffer_height;
    const float *ambient;
    const sw_context_t *fog_ctx;
    int depth_disabled;
    int have_splat;
    /* 1 when any light carries a resolvable shadow slot: legacy materials must
     * take the per-pixel lighting path so shadows land (parity with the GPU
     * backends, which always shade per pixel). */
    int per_pixel_shadows;
    /* ZB-21: -1 when this triangle rasterized back-facing (only reachable on
     * cull-off draws), +1 otherwise. Shading normals multiply by this so
     * double-sided geometry lights with the outward normal on both sides —
     * parity with the GPU backends' front-facing flip. */
    float normal_sign;
    const sw_pixels_view *splat_view;
    const sw_pixels_view *layer_views;
} sw_fragment_ctx;

typedef struct sw_slot_uvs sw_slot_uvs;

/// @brief Per-fragment geometric inputs shared by the PBR and Phong lighting
///   stages: the (possibly normal-mapped) shading normal, world position, view
///   direction, and clamped N·V.
typedef struct {
    float nx, ny, nz;
    float wx, wy, wz;
    float vx, vy, vz;
    float ndv;
    int32_t global_light_count;
    const uint16_t *local_light_indices;
    int32_t local_light_count;
} sw_shade_geom;

/// @brief Resolve one selected global-prefix or cluster-local light index.
/// @param g Borrowed fragment geometry carrying the clustered-light selection.
/// @param selection_index Index in the compact global-plus-local selection.
/// @return Index into the draw's flattened light array.
static int32_t sw_shade_light_index(const sw_shade_geom *g, int32_t selection_index) {
    if (selection_index < g->global_light_count)
        return selection_index;
    return (int32_t)g->local_light_indices[selection_index - g->global_light_count];
}

/// @brief Resolve the pre-lighting albedo: interpolated vertex color modulated by
///   the base-color texture, the optional terrain-splat layer blend, and the
///   legacy emissive map. Writes the albedo to (*fr,*fg,*fb_c) and the per-texel
///   base-color alpha to *tex_alpha.
/// @param fc Borrowed per-triangle fragment context.
/// @param attr_b0 Perspective-correct first attribute weight.
/// @param attr_b1 Perspective-correct second attribute weight.
/// @param attr_b2 Perspective-correct third attribute weight.
/// @param uv_b0 First screen-space weight used for slot UV interpolation.
/// @param uv_b1 Second screen-space weight used for slot UV interpolation.
/// @param uv_b2 Third screen-space weight used for slot UV interpolation.
/// @param fr Receives the pre-lighting red component.
/// @param fg Receives the pre-lighting green component.
/// @param fb_c Receives the pre-lighting blue component.
/// @param tex_alpha Receives sampled base-color alpha.
static void sw_shade_resolve_albedo(const sw_fragment_ctx *fc,
                                    float attr_b0,
                                    float attr_b1,
                                    float attr_b2,
                                    float uv_b0,
                                    float uv_b1,
                                    float uv_b2,
                                    float *fr,
                                    float *fg,
                                    float *fb_c,
                                    float *tex_alpha) {
    const screen_vert_t *v0 = fc->v0;
    const screen_vert_t *v1 = fc->v1;
    const screen_vert_t *v2 = fc->v2;
    const sw_pixels_view *tex = fc->tex;
    const sw_pixels_view *emissive_tex = fc->emissive_tex;
    const float *emissive_color = fc->emissive_color;
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;
    int have_splat = fc->have_splat;
    const sw_pixels_view *layer_views = fc->layer_views;

    float cr = attr_b0 * v0->r + attr_b1 * v1->r + attr_b2 * v2->r;
    float cg = attr_b0 * v0->g + attr_b1 * v1->g + attr_b2 * v2->g;
    float cb = attr_b0 * v0->b + attr_b1 * v1->b + attr_b2 * v2->b;
    float material_r = cr;
    float material_g = cg;
    float material_b = cb;
    float ta_out = 1.0f; /* per-texel alpha (for foliage, fences) */
    if (tex) {
        float u;
        float vc;
        sw_interpolate_uv_for_slot(
            cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR, uv_b0, uv_b1, uv_b2, v0, v1, v2, &u, &vc);
        {
            float tr, tg, tb, ta;
            if (cmd && cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR)
                sample_texture_slot_srgb_ex(
                    tex, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR, u, vc, &tr, &tg, &tb, &ta);
            else
                sample_texture_slot_ex(
                    tex, cmd, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR, u, vc, &tr, &tg, &tb, &ta);
            cr *= tr;
            cg *= tg;
            cb *= tb;
            ta_out = ta;
        }
    }
    /* Terrain splat: replace diffuse with per-pixel layer blend. Views were
     * resolved once per triangle (splat_view / layer_views). */
    if (have_splat) {
        float iw = uv_b0 * v0->inv_w + uv_b1 * v1->inv_w + uv_b2 * v2->inv_w;
        if (isfinite(iw) && fabsf(iw) > 1e-7f) {
            float sp_u = (uv_b0 * v0->u_over_w + uv_b1 * v1->u_over_w + uv_b2 * v2->u_over_w) / iw;
            float sp_v = (uv_b0 * v0->v_over_w + uv_b1 * v1->v_over_w + uv_b2 * v2->v_over_w) / iw;
            {
                float sr, sg, sb, sa;
                sample_texture(fc->splat_view, sp_u, sp_v, &sr, &sg, &sb, &sa);
                float w[4] = {sr, sg, sb, sa};
                float wsum = w[0] + w[1] + w[2] + w[3];
                if (wsum > 0.001f) {
                    for (int wi = 0; wi < 4; wi++)
                        w[wi] /= wsum;
                } else {
                    w[0] = 1.0f;
                    w[1] = w[2] = w[3] = 0.0f;
                }
                float blr = 0, blg = 0, blb = 0;
                for (int L = 0; L < 4; L++) {
                    if (w[L] < 0.001f)
                        continue;
                    float lu = sp_u * cmd->splat_layer_scales[L];
                    float lvc = sp_v * cmd->splat_layer_scales[L];
                    float pr, pg, pb, pa;
                    sample_texture(&layer_views[L], lu, lvc, &pr, &pg, &pb, &pa);
                    if (cmd && cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR) {
                        pr = sw_srgb_to_linear(pr);
                        pg = sw_srgb_to_linear(pg);
                        pb = sw_srgb_to_linear(pb);
                    }
                    blr += pr * w[L];
                    blg += pg * w[L];
                    blb += pb * w[L];
                }
                cr = blr * material_r;
                cg = blg * material_g;
                cb = blb * material_b;
            }
        }
    }

    /* ADR 0312 projected decal: composite over the albedo before lighting so
     * the decal is lit, shadowed and normal-mapped exactly like the cloth it
     * sits on. Coordinates come from the BIND position through the material's
     * projector; the facing test keeps the far side of a torso clean. */
    if (fc->decal_tex && cmd && cmd->has_decal) {
        float ds = attr_b0 * v0->ds + attr_b1 * v1->ds + attr_b2 * v2->ds;
        float dt = attr_b0 * v0->dt + attr_b1 * v1->dt + attr_b2 * v2->dt;
        float dd = attr_b0 * v0->dd + attr_b1 * v1->dd + attr_b2 * v2->dd;
        float df = attr_b0 * v0->dfacing + attr_b1 * v1->dfacing + attr_b2 * v2->dfacing;
        if (df < 0.0f && ds >= 0.0f && ds <= 1.0f && dt >= 0.0f && dt <= 1.0f && dd >= 0.0f &&
            dd <= 1.0f) {
            float dcr, dcg, dcb, dca;
            sample_texture_ex(fc->decal_tex,
                              ds,
                              dt,
                              RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE,
                              RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE,
                              RT_MATERIAL3D_TEXTURE_FILTER_LINEAR,
                              &dcr,
                              &dcg,
                              &dcb,
                              &dca);
            if (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR) {
                dcr = sw_srgb_to_linear(dcr);
                dcg = sw_srgb_to_linear(dcg);
                dcb = sw_srgb_to_linear(dcb);
            }
            float w = clamp01f(dca * cmd->decal_opacity);
            cr = cr + (dcr - cr) * w;
            cg = cg + (dcg - cg) * w;
            cb = cb + (dcb - cb) * w;
        }
    }

    /* Emissive map sampling (legacy path only; PBR handles it in the lighting
     * branch so emissiveIntensity can scale both the color and the map.) The
     * legacy pipeline lights in gamma space (albedo is sampled raw), so the
     * emissive map is sampled raw as well — matching the GPU shaders, which
     * only sRGB-decode emissive for the PBR workflow. */
    if (emissive_tex && !(cmd && cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR)) {
        float u;
        float vc;
        float er, eg, eb, ea;
        sw_interpolate_uv_for_slot(
            cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE, uv_b0, uv_b1, uv_b2, v0, v1, v2, &u, &vc);
        sample_texture_slot_ex(
            emissive_tex, cmd, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE, u, vc, &er, &eg, &eb, &ea);
        float emissive_scale = (cmd ? cmd->emissive_intensity : 1.0f);
        cr += er * emissive_color[0] * emissive_scale;
        cg += eg * emissive_color[1] * emissive_scale;
        cb += eb * emissive_color[2] * emissive_scale;
        (void)ea;
    }

    *fr = cr;
    *fg = cg;
    *fb_c = cb;
    *tex_alpha = ta_out;
}

/// @brief Interpolate the world-space vertex normal and, when a normal map is
///   bound and the tangent is non-degenerate, perturb it through the TBN basis.
///   Writes the final shading normal to (*out_nx,*out_ny,*out_nz).
/// @param fc Borrowed per-triangle fragment context.
/// @param b0 Perspective-correct first attribute weight.
/// @param b1 Perspective-correct second attribute weight.
/// @param b2 Perspective-correct third attribute weight.
/// @param normal_u U coordinate for the normal-map slot.
/// @param normal_v V coordinate for the normal-map slot.
/// @param out_nx Receives the normalized world-space X component.
/// @param out_ny Receives the normalized world-space Y component.
/// @param out_nz Receives the normalized world-space Z component.
static void sw_shade_perturb_normal(const sw_fragment_ctx *fc,
                                    float b0,
                                    float b1,
                                    float b2,
                                    float normal_u,
                                    float normal_v,
                                    float *out_nx,
                                    float *out_ny,
                                    float *out_nz) {
    const screen_vert_t *v0 = fc->v0;
    const screen_vert_t *v1 = fc->v1;
    const screen_vert_t *v2 = fc->v2;
    const sw_pixels_view *normal_map = fc->normal_map;
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;

    /* Interpolate world normal */
    float pnx = b0 * v0->nx + b1 * v1->nx + b2 * v2->nx;
    float pny = b0 * v0->ny + b1 * v1->ny + b2 * v2->ny;
    float pnz = b0 * v0->nz + b1 * v1->nz + b2 * v2->nz;
    normalize3f(&pnx, &pny, &pnz);
    /* ZB-21: back faces of cull-off geometry shade with the outward normal;
     * the TBN below composes on the flipped base, matching the GPU flip. */
    pnx *= fc->normal_sign;
    pny *= fc->normal_sign;
    pnz *= fc->normal_sign;

    /* Interpolate tangent + build TBN */
    float ptx = b0 * v0->tx + b1 * v1->tx + b2 * v2->tx;
    float pty = b0 * v0->ty + b1 * v1->ty + b2 * v2->ty;
    float ptz = b0 * v0->tz + b1 * v1->tz + b2 * v2->tz;
    float ptw = b0 * v0->tw + b1 * v1->tw + b2 * v2->tw;
    float tlen = sw_length3(ptx, pty, ptz);

    if (normal_map && tlen > 1e-7f) {
        ptx /= tlen;
        pty /= tlen;
        ptz /= tlen;
        /* Gram-Schmidt orthogonalize T against N */
        float tdotn = ptx * pnx + pty * pny + ptz * pnz;
        ptx -= tdotn * pnx;
        pty -= tdotn * pny;
        ptz -= tdotn * pnz;
        tlen = sw_length3(ptx, pty, ptz);
        if (tlen > 1e-7f) {
            ptx /= tlen;
            pty /= tlen;
            ptz /= tlen;
        }

        /* Sample normal map: [0,1] → [-1,1] */
        float tnr, tng, tnb, tna;
        sample_texture_slot_ex(normal_map,
                               cmd,
                               RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
                               normal_u,
                               normal_v,
                               &tnr,
                               &tng,
                               &tnb,
                               &tna);
        float map_x = (tnr * 2.0f - 1.0f) * cmd->normal_scale;
        float map_y = (tng * 2.0f - 1.0f) * cmd->normal_scale;
        float map_z = tnb * 2.0f - 1.0f;

        /* Bitangent = sign * (N × T) */
        float tangent_sign = ptw < 0.0f ? -1.0f : 1.0f;
        float bbx = (pny * ptz - pnz * pty) * tangent_sign;
        float bby = (pnz * ptx - pnx * ptz) * tangent_sign;
        float bbz = (pnx * pty - pny * ptx) * tangent_sign;

        /* TBN transform: tangent-space → world-space */
        float wn_x = ptx * map_x + bbx * map_y + pnx * map_z;
        float wn_y = pty * map_x + bby * map_y + pny * map_z;
        float wn_z = ptz * map_x + bbz * map_y + pnz * map_z;
        float wlen = sw_length3(wn_x, wn_y, wn_z);
        if (wlen > 1e-7f) {
            pnx = wn_x / wlen;
            pny = wn_y / wlen;
            pnz = wn_z / wlen;
        }
    }
    /* else: degenerate tangent → use unperturbed normal */

    *out_nx = pnx;
    *out_ny = pny;
    *out_nz = pnz;
}

/// @brief Preserve the historical punctual-light path and its cross-compiler snapshots.
/// @param lt Borrowed sanitized light.
/// @param wx World-space receiver X coordinate.
/// @param wy World-space receiver Y coordinate.
/// @param wz World-space receiver Z coordinate.
/// @param llx Receives the fragment-to-light X component.
/// @param lly Receives the fragment-to-light Y component.
/// @param llz Receives the fragment-to-light Z component.
/// @param la Receives scalar attenuation.
/// @return Non-zero only when @p lt produces directional BRDF lighting at the receiver.
static int sw_eval_light_dir(const vgfx3d_light_params_t *lt,
                             float wx,
                             float wy,
                             float wz,
                             float *llx,
                             float *lly,
                             float *llz,
                             float *la) {
    return sw_eval_analytic_light(lt, wx, wy, wz, llx, lly, llz, la) == 1;
}

/// @brief Cook-Torrance PBR lighting for one fragment. Reads the albedo from
///   (*fr,*fg,*fb_c) and overwrites it with the lit color. Samples the
///   metallic-roughness, AO, and emissive maps; accumulates ambient, per-light
///   direct, and emissive contributions; applies the toon/emissive shading models.
/// @param fc Borrowed per-triangle fragment context containing material, maps, and lights.
/// @param g Borrowed per-fragment world-space geometry.
/// @param uv Borrowed perspective-correct UVs for each material texture slot.
/// @param fr In/out linear red component.
/// @param fg In/out linear green component.
/// @param fb_c In/out linear blue component.
static void sw_shade_lighting_pbr(const sw_fragment_ctx *fc,
                                  const sw_shade_geom *g,
                                  const sw_slot_uvs *uv,
                                  float *fr,
                                  float *fg,
                                  float *fb_c) {
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;
    const sw_pixels_view *emissive_tex = fc->emissive_tex;
    const sw_pixels_view *metallic_roughness_map = fc->metallic_roughness_map;
    const sw_pixels_view *ao_map = fc->ao_map;
    const vgfx3d_light_params_t *lights = fc->lights;
    int32_t selected_light_count = g->global_light_count + g->local_light_count;
    const float *ambient = fc->ambient;
    const sw_context_t *fog_ctx = fc->fog_ctx;
    float pnx = g->nx, pny = g->ny, pnz = g->nz;
    float wx = g->wx, wy = g->wy, wz = g->wz;
    float vdx = g->vx, vdy = g->vy, vdz = g->vz;

    float base_r = *fr;
    float base_g = *fg;
    float base_b = *fb_c;
    float metallic = clamp01f(cmd->metallic);
    float roughness = clamp01f(cmd->roughness);
    float ao = clamp01f(cmd->ao);
    if (metallic_roughness_map) {
        float mrr, mrg, mrb, mra;
        sample_texture_slot_ex(metallic_roughness_map,
                               cmd,
                               RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
                               uv->mr_u,
                               uv->mr_v,
                               &mrr,
                               &mrg,
                               &mrb,
                               &mra);
        roughness *= mrg;
        metallic *= mrb;
    }
    if (ao_map) {
        float aor, aog, aob, aoa;
        sample_texture_slot_ex(
            ao_map, cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO, uv->ao_u, uv->ao_v, &aor, &aog, &aob, &aoa);
        ao *= aor;
    }
    metallic = clamp01f(metallic);
    roughness = roughness < 0.045f ? 0.045f : clamp01f(roughness);
    ao = clamp01f(ao);

    float ndv = g->ndv;
    if (ndv < 0.001f)
        ndv = 0.001f;

    float lit_r;
    float lit_g;
    float lit_b;
    /* KHR_materials_ior / clearcoat / volume ride the PBR custom params
     * (1 = coat factor, 2 = coat roughness, 4 = ior, 5 = thickness,
     * 6 = folded Beer-Lambert absorption). Toon and Fresnel models own
     * params 0/1 for their controls, so the lobes gate on the model. */
    int pbr_param_model = cmd->shading_model != 1 && cmd->shading_model != 5;
    float dielectric_f0 = 0.04f;
    float coat = 0.0f;
    float coat_roughness = 0.045f;
    if (pbr_param_model) {
        if (cmd->custom_params[4] > 0.0f) {
            float ior = cmd->custom_params[4];
            float root = (ior - 1.0f) / (ior + 1.0f);
            dielectric_f0 = clamp01f(root * root);
        }
        coat = clamp01f(cmd->custom_params[1]);
        coat_roughness = cmd->custom_params[2] < 0.045f ? 0.045f : clamp01f(cmd->custom_params[2]);
    }
    float sheen = pbr_param_model ? clamp01f(cmd->custom_params[0]) : 0.0f;
    float sheen_roughness = cmd->custom_params[7] < 0.07f ? 0.07f : clamp01f(cmd->custom_params[7]);
    float one_minus_metallic = 1.0f - metallic;
    float f0_r = dielectric_f0 + (base_r - dielectric_f0) * metallic;
    float f0_g = dielectric_f0 + (base_g - dielectric_f0) * metallic;
    float f0_b = dielectric_f0 + (base_b - dielectric_f0) * metallic;
    float rough_alpha = roughness * roughness;
    float rough_alpha_squared = rough_alpha * rough_alpha;
    float rough_geometry_r = roughness + 1.0f;
    float rough_geometry_k = (rough_geometry_r * rough_geometry_r) * 0.125f;
    float geometry_view = pbr_geometry_schlick_ggx_k(ndv, rough_geometry_k);
    float coat_alpha = coat_roughness * coat_roughness;
    float coat_alpha_squared = coat_alpha * coat_alpha;
    float sheen_inv_alpha = 1.0f / (sheen_roughness * sheen_roughness);
    const float inv_pi = 0.31830988618f;
    if (cmd->ibl_env && cmd->env_map && fog_ctx && fog_ctx->ibl_enabled) {
        /* Image-based ambient: SH diffuse + prefiltered specular (split-sum).
         * Replaces the flat ambient term; direct lights below are unchanged. */
        float irr[3];
        float gloss = 1.0f - roughness;
        float w = pow5f_fast(1.0f - ndv);
        /* Roughness-aware Schlick: rough surfaces lose grazing Fresnel gain. */
        float fa_r = f0_r + ((gloss > f0_r ? gloss : f0_r) - f0_r) * w;
        float fa_g = f0_g + ((gloss > f0_g ? gloss : f0_g) - f0_g) * w;
        float fa_b = f0_b + ((gloss > f0_b ? gloss : f0_b) - f0_b) * w;
        float kd_r = (1.0f - fa_r) * one_minus_metallic;
        float kd_g = (1.0f - fa_g) * one_minus_metallic;
        float kd_b = (1.0f - fa_b) * one_minus_metallic;
        /* Reflection vector R = 2(N.V)N - V. */
        float rvx = 2.0f * ndv * pnx - vdx;
        float rvy = 2.0f * ndv * pny - vdy;
        float rvz = 2.0f * ndv * pnz - vdz;
        float pre_r;
        float pre_g;
        float pre_b;
        rt_cubemap_sample_ibl(
            (const rt_cubemap3d *)cmd->env_map, rvx, rvy, rvz, roughness, &pre_r, &pre_g, &pre_b);
        /* Precomputed split-sum environment BRDF (64x64 CPU-integrated table;
         * shared with the GPU backends' LUT texture). */
        float env_ab[2];
        vgfx3d_brdf_lut_sample(ndv, roughness, env_ab);
        float ab_x = env_ab[0];
        float ab_y = env_ab[1];
        float intensity = fog_ctx->ibl_intensity;
        rt_sh9_eval_irradiance(fog_ctx->ibl_sh, pnx, pny, pnz, irr);
        lit_r = (kd_r * irr[0] * base_r + pre_r * (f0_r * ab_x + ab_y)) * ao * intensity;
        lit_g = (kd_g * irr[1] * base_g + pre_g * (f0_g * ab_x + ab_y)) * ao * intensity;
        lit_b = (kd_b * irr[2] * base_b + pre_b * (f0_b * ab_x + ab_y)) * ao * intensity;
        if (coat > 0.0f) {
            /* Clearcoat ambient lobe: dielectric F0 0.04 at the coat roughness;
             * the base layer loses the energy the coat reflects. */
            float coat_pre_r;
            float coat_pre_g;
            float coat_pre_b;
            float coat_ab[2];
            float coat_fresnel;
            rt_cubemap_sample_ibl((const rt_cubemap3d *)cmd->env_map,
                                  rvx,
                                  rvy,
                                  rvz,
                                  coat_roughness,
                                  &coat_pre_r,
                                  &coat_pre_g,
                                  &coat_pre_b);
            vgfx3d_brdf_lut_sample(ndv, coat_roughness, coat_ab);
            coat_fresnel = 0.04f + 0.96f * w;
            {
                float coat_spec = 0.04f * coat_ab[0] + coat_ab[1];
                float keep = 1.0f - coat * coat_fresnel;
                lit_r = lit_r * keep + coat_pre_r * coat_spec * coat * ao * intensity;
                lit_g = lit_g * keep + coat_pre_g * coat_spec * coat * ao * intensity;
                lit_b = lit_b * keep + coat_pre_b * coat_spec * coat * ao * intensity;
            }
        }
        if (sheen > 0.0f) {
            /* Ambient sheen: irradiance-scaled rim energy (quarter-strength
             * matches the direct lobe's average reflectance). */
            float sheen_amb = sheen * 0.25f * ao * intensity;
            lit_r += irr[0] * sheen_amb;
            lit_g += irr[1] * sheen_amb;
            lit_b += irr[2] * sheen_amb;
        }
    } else if (fc->lightmap_tex) {
        /* Baked GI: lightmap radiance (stored with 2x headroom) x albedo replaces
         * the flat ambient term; analytic lights below still add direct light. */
        float lmr, lmg, lmb, lma;
        sample_texture_slot_ex(
            fc->lightmap_tex, cmd, -1, uv->lm_u, uv->lm_v, &lmr, &lmg, &lmb, &lma);
        lit_r = lmr * 2.0f * base_r * ao;
        lit_g = lmg * 2.0f * base_g * ao;
        lit_b = lmb * 2.0f * base_b * ao;
    } else {
        lit_r = (ambient ? ambient[0] : 0.0f) * base_r * ao;
        lit_g = (ambient ? ambient[1] : 0.0f) * base_g * ao;
        lit_b = (ambient ? ambient[2] : 0.0f) * base_b * ao;
    }

    for (int32_t selection = 0; selection < selected_light_count; selection++) {
        int32_t li = sw_shade_light_index(g, selection);
        const vgfx3d_light_params_t *lt = &lights[li];
        float llx, lly, llz, la = 1.0f;

        if (lt->type != 0 && lt->type != 1 && lt->type != 3) {
            int light_mode;
            if (lt->type == 2) {
                lit_r += lt->color[0] * lt->intensity * base_r * ao;
                lit_g += lt->color[1] * lt->intensity * base_g * ao;
                lit_b += lt->color[2] * lt->intensity * base_b * ao;
                continue;
            }
            light_mode = sw_eval_analytic_light(lt, wx, wy, wz, &llx, &lly, &llz, &la);
            if (light_mode == 0)
                continue;
            if (light_mode == 2) {
                lit_r += lt->color[0] * lt->intensity * base_r * ao * la;
                lit_g += lt->color[1] * lt->intensity * base_g * ao * la;
                lit_b += lt->color[2] * lt->intensity * base_b * ao * la;
                continue;
            }
        } else if (!sw_eval_light_dir(lt, wx, wy, wz, &llx, &lly, &llz, &la)) {
            continue;
        }

        if ((lt->type == 0 || lt->type == 1 || lt->type == 3 || lt->type == 4 || lt->type == 5) &&
            lt->shadow_index >= 0 && la > 0.0f) {
            la *= sw_sample_shadow_light(fog_ctx, lt, wx, wy, wz, pnx, pny, pnz);
        }

        float ndl = dot3f(pnx, pny, pnz, llx, lly, llz);
        if (ndl <= 0.0f)
            continue;

        float hx = llx + vdx;
        float hy = lly + vdy;
        float hz = llz + vdz;
        normalize3f(&hx, &hy, &hz);
        float ndh = clamp01f(dot3f(pnx, pny, pnz, hx, hy, hz));
        float vdh = clamp01f(dot3f(vdx, vdy, vdz, hx, hy, hz));

        float fresnel_w = pow5f_fast(1.0f - vdh);
        float f_r = f0_r + (1.0f - f0_r) * fresnel_w;
        float f_g = f0_g + (1.0f - f0_g) * fresnel_w;
        float f_b = f0_b + (1.0f - f0_b) * fresnel_w;

        float d = pbr_distribution_ggx_alpha(ndh, rough_alpha_squared);
        float gg = geometry_view * pbr_geometry_schlick_ggx_k(ndl, rough_geometry_k);
        float spec_denom = 4.0f * ndv * ndl + 1e-4f;
        float spec_r = (d * gg * f_r) / spec_denom;
        float spec_g = (d * gg * f_g) / spec_denom;
        float spec_b = (d * gg * f_b) / spec_denom;

        float kd_r = (1.0f - f_r) * one_minus_metallic;
        float kd_g = (1.0f - f_g) * one_minus_metallic;
        float kd_b = (1.0f - f_b) * one_minus_metallic;
        float diff_r = kd_r * base_r * inv_pi;
        float diff_g = kd_g * base_g * inv_pi;
        float diff_b = kd_b * base_b * inv_pi;

        float radiance_r = lt->color[0] * lt->intensity * la;
        float radiance_g = lt->color[1] * lt->intensity * la;
        float radiance_b = lt->color[2] * lt->intensity * la;
        if (sheen > 0.0f) {
            /* Charlie sheen lobe (KHR_materials_sheen): inverted-GGX style
             * distribution with the Neubelt-Pettineo visibility term. */
            float sin2 = 1.0f - ndh * ndh;
            float d_charlie = (2.0f + sheen_inv_alpha) *
                              powf(sin2 > 0.0f ? sin2 : 0.0f, sheen_inv_alpha * 0.5f) *
                              0.15915494309f; /* 1 / (2 pi) */
            float vis_sheen = 1.0f / (4.0f * (ndl + ndv - ndl * ndv) + 1e-4f);
            float sheen_spec = sheen * d_charlie * vis_sheen;
            spec_r += sheen_spec;
            spec_g += sheen_spec;
            spec_b += sheen_spec;
        }
        if (coat > 0.0f) {
            /* Second specular lobe: GGX D at the coat roughness, Schlick F at
             * F0 0.04, Kelemen visibility; base layer attenuated by the coat. */
            float coat_d = pbr_distribution_ggx_alpha(ndh, coat_alpha_squared);
            float coat_f = 0.04f + 0.96f * fresnel_w;
            float coat_vis = 0.25f / (vdh * vdh + 1e-4f);
            float coat_spec = coat * coat_d * coat_f * coat_vis;
            float keep = 1.0f - coat * coat_f;
            diff_r *= keep;
            diff_g *= keep;
            diff_b *= keep;
            spec_r = spec_r * keep + coat_spec;
            spec_g = spec_g * keep + coat_spec;
            spec_b = spec_b * keep + coat_spec;
        }
        lit_r += (diff_r + spec_r) * radiance_r * ndl;
        lit_g += (diff_g + spec_g) * radiance_g * ndl;
        lit_b += (diff_b + spec_b) * radiance_b * ndl;
    }

    float emissive_r = cmd->emissive_color[0] * cmd->emissive_intensity;
    float emissive_g = cmd->emissive_color[1] * cmd->emissive_intensity;
    float emissive_b = cmd->emissive_color[2] * cmd->emissive_intensity;
    if (emissive_tex) {
        float er, eg, eb, ea;
        sample_texture_slot_srgb_ex(emissive_tex,
                                    cmd,
                                    RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE,
                                    uv->emissive_u,
                                    uv->emissive_v,
                                    &er,
                                    &eg,
                                    &eb,
                                    &ea);
        emissive_r *= er;
        emissive_g *= eg;
        emissive_b *= eb;
    }
    lit_r += emissive_r;
    lit_g += emissive_g;
    lit_b += emissive_b;

    if (pbr_param_model && cmd->custom_params[3] > 0.0f && cmd->custom_params[6] > 0.0f &&
        cmd->custom_params[5] > 0.0f) {
        /* KHR_materials_volume: Beer-Lambert absorption through the folded
         * scalar coefficient over the thickness, scaled by transmission. */
        float absorb = expf(-cmd->custom_params[6] * cmd->custom_params[5]);
        float tint = 1.0f + clamp01f(cmd->custom_params[3]) * (absorb - 1.0f);
        lit_r *= tint;
        lit_g *= tint;
        lit_b *= tint;
    }

    if (cmd->shading_model == 1) {
        /* Same banding domain and defaults as the GPU shaders' PBR toon pass. */
        float bands = cmd->custom_params[0] > 2.0f ? cmd->custom_params[0] : 2.0f;
        lit_r = floorf(lit_r * bands) / bands;
        lit_g = floorf(lit_g * bands) / bands;
        lit_b = floorf(lit_b * bands) / bands;
    } else if (cmd->shading_model == 5) {
        float strength = cmd->custom_params[0] > 0.0f ? cmd->custom_params[0] : 2.0f;
        lit_r += emissive_r * (strength - 1.0f);
        lit_g += emissive_g * (strength - 1.0f);
        lit_b += emissive_b * (strength - 1.0f);
    }

    *fr = lit_r;
    *fg = lit_g;
    *fb_c = lit_b;
}

/// @brief Blinn-Phong lighting for one normal-mapped legacy fragment. Reads the
///   albedo from (*fr,*fg,*fb_c) and overwrites it with the lit color. Samples the
///   specular map; accumulates ambient, per-light diffuse+specular, and emissive
///   contributions; applies the toon/emissive shading models.
/// @param fc Borrowed per-triangle fragment context containing material, maps, and lights.
/// @param g Borrowed per-fragment world-space geometry.
/// @param uv Borrowed perspective-correct UVs for each material texture slot.
/// @param fr In/out red component.
/// @param fg In/out green component.
/// @param fb_c In/out blue component.
static void sw_shade_lighting_phong(const sw_fragment_ctx *fc,
                                    const sw_shade_geom *g,
                                    const sw_slot_uvs *uv,
                                    float *fr,
                                    float *fg,
                                    float *fb_c) {
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;
    const sw_pixels_view *specular_map = fc->specular_map;
    const vgfx3d_light_params_t *lights = fc->lights;
    int32_t selected_light_count = g->global_light_count + g->local_light_count;
    const float *ambient = fc->ambient;
    const sw_context_t *fog_ctx = fc->fog_ctx;
    float pnx = g->nx, pny = g->ny, pnz = g->nz;
    float wx = g->wx, wy = g->wy, wz = g->wz;
    float vdx = g->vx, vdy = g->vy, vdz = g->vz;

    float base_r = *fr;
    float base_g = *fg;
    float base_b = *fb_c;

    /* Ambient */
    float lit_r = (ambient ? ambient[0] : 0.0f) * base_r;
    float lit_g = (ambient ? ambient[1] : 0.0f) * base_g;
    float lit_b = (ambient ? ambient[2] : 0.0f) * base_b;

    /* Specular properties (possibly from specular map) */
    float sp_r = cmd->specular[0];
    float sp_g = cmd->specular[1];
    float sp_b = cmd->specular[2];
    if (specular_map) {
        float smr, smg, smb, sma;
        sample_texture_slot_ex(specular_map,
                               cmd,
                               RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR,
                               uv->specular_u,
                               uv->specular_v,
                               &smr,
                               &smg,
                               &smb,
                               &sma);
        sp_r *= smr;
        sp_g *= smg;
        sp_b *= smb;
    }

    for (int32_t selection = 0; selection < selected_light_count; selection++) {
        int32_t li = sw_shade_light_index(g, selection);
        const vgfx3d_light_params_t *lt = &lights[li];
        float llx, lly, llz, la = 1.0f;

        if (lt->type != 0 && lt->type != 1 && lt->type != 3) {
            int light_mode;
            if (lt->type == 2) {
                lit_r += lt->color[0] * lt->intensity * base_r;
                lit_g += lt->color[1] * lt->intensity * base_g;
                lit_b += lt->color[2] * lt->intensity * base_b;
                continue;
            }
            light_mode = sw_eval_analytic_light(lt, wx, wy, wz, &llx, &lly, &llz, &la);
            if (light_mode == 0)
                continue;
            if (light_mode == 2) {
                lit_r += lt->color[0] * lt->intensity * base_r * la;
                lit_g += lt->color[1] * lt->intensity * base_g * la;
                lit_b += lt->color[2] * lt->intensity * base_b * la;
                continue;
            }
        } else if (!sw_eval_light_dir(lt, wx, wy, wz, &llx, &lly, &llz, &la)) {
            continue;
        }

        if ((lt->type == 0 || lt->type == 1 || lt->type == 3 || lt->type == 4 || lt->type == 5) &&
            lt->shadow_index >= 0 && la > 0.0f) {
            la *= sw_sample_shadow_light(fog_ctx, lt, wx, wy, wz, pnx, pny, pnz);
        }

        float ndl = pnx * llx + pny * lly + pnz * llz;
        if (ndl < 0.0f)
            ndl = 0.0f;
        /* Toon (legacy workflow): band the per-light diffuse term and threshold
         * the specular highlight, matching the GPU shaders. The final color is
         * NOT banded on the legacy path (that final-band pass is PBR-only). */
        if (cmd->shading_model == 1) {
            float bands = cmd->custom_params[0] > 2.0f ? cmd->custom_params[0] : 2.0f;
            float denom = bands - 1.0f > 1.0f ? bands - 1.0f : 1.0f;
            ndl = floorf(ndl * bands) / denom;
        }
        float li_i = lt->intensity;
        lit_r += lt->color[0] * li_i * ndl * base_r * la;
        lit_g += lt->color[1] * li_i * ndl * base_g * la;
        lit_b += lt->color[2] * li_i * ndl * base_b * la;

        if (ndl > 0.0f && cmd->shininess > 0.0f) {
            float hx = llx + vdx, hy = lly + vdy, hz = llz + vdz;
            normalize3f(&hx, &hy, &hz);
            float ndh = pnx * hx + pny * hy + pnz * hz;
            if (ndh < 0.0f)
                ndh = 0.0f;
            float spec = sw_material_powf(ndh, cmd->shininess);
            if (cmd->shading_model == 1) {
                float cut = cmd->custom_params[1] > 0.5f ? cmd->custom_params[1] : 0.5f;
                spec = spec >= cut ? 1.0f : 0.0f;
            }
            lit_r += lt->color[0] * li_i * spec * sp_r * la;
            lit_g += lt->color[1] * li_i * spec * sp_g * la;
            lit_b += lt->color[2] * li_i * spec * sp_b * la;
        }
    }

    /* Emissive color base */
    lit_r += cmd->emissive_color[0] * cmd->emissive_intensity;
    lit_g += cmd->emissive_color[1] * cmd->emissive_intensity;
    lit_b += cmd->emissive_color[2] * cmd->emissive_intensity;

    if (cmd->shading_model == 5) {
        float strength = cmd->custom_params[0] > 0.0f ? cmd->custom_params[0] : 2.0f;
        lit_r += cmd->emissive_color[0] * cmd->emissive_intensity * (strength - 1.0f);
        lit_g += cmd->emissive_color[1] * cmd->emissive_intensity * (strength - 1.0f);
        lit_b += cmd->emissive_color[2] * cmd->emissive_intensity * (strength - 1.0f);
    }

    *fr = lit_r;
    *fg = lit_g;
    *fb_c = lit_b;
}

/// @brief Blend the fragment color toward the fog color by interpolated camera
///   distance. Caller guards on fog_ctx->fog_enabled.
/// @param fog_ctx Borrowed software context containing distance and height-fog settings.
/// @param b0 Perspective-correct first attribute weight.
/// @param b1 Perspective-correct second attribute weight.
/// @param b2 Perspective-correct third attribute weight.
/// @param v0 Borrowed first triangle vertex.
/// @param v1 Borrowed second triangle vertex.
/// @param v2 Borrowed third triangle vertex.
/// @param fr In/out red component.
/// @param fg In/out green component.
/// @param fb_c In/out blue component.
static void sw_shade_apply_fog(const sw_context_t *fog_ctx,
                               float b0,
                               float b1,
                               float b2,
                               const screen_vert_t *v0,
                               const screen_vert_t *v1,
                               const screen_vert_t *v2,
                               float *fr,
                               float *fg,
                               float *fb_c) {
    float wx = b0 * v0->wx + b1 * v1->wx + b2 * v2->wx;
    float wy = b0 * v0->wy + b1 * v1->wy + b2 * v2->wy;
    float wz = b0 * v0->wz + b1 * v1->wz + b2 * v2->wz;
    float dist = sw_compute_fog_distance(fog_ctx, wx, wy, wz);
    float fog_f = 0.0f;
    if (fog_ctx->fog_enabled) {
        float fog_range = fog_ctx->fog_far - fog_ctx->fog_near;
        fog_f = (fog_range > 1e-6f) ? (dist - fog_ctx->fog_near) / fog_range : 0.0f;
        fog_f = fog_f < 0.0f ? 0.0f : (fog_f > 1.0f ? 1.0f : fog_f);
    }
    float fog_r = fog_ctx->fog_color[0];
    float fog_g = fog_ctx->fog_color[1];
    float fog_b = fog_ctx->fog_color[2];
    if (fog_ctx->height_fog_enabled) {
        /* Exponential height fog: density pools below the base height; combine
         * with distance fog via combined transmittance. Clamps mirror the
         * D3D11/GL twins: an unclamped exponent overflows expf() to Inf ~88
         * units below the fog base (step-function fog instead of a gradient),
         * and Inf * 0 at zero view distance produces NaN fog. */
        float fog_exponent = -(wy - fog_ctx->height_fog_base) * fog_ctx->height_fog_falloff;
        if (!(fog_exponent >= -80.0f))
            fog_exponent = -80.0f; /* also catches NaN */
        if (fog_exponent > 80.0f)
            fog_exponent = 80.0f;
        float hd = fog_ctx->height_fog_density * expf(fog_exponent);
        if (!(hd >= 0.0f))
            hd = 0.0f;
        if (hd > 1e20f)
            hd = 1e20f;
        float optical_depth = hd * (dist > 0.0f ? dist : 0.0f);
        if (optical_depth > 80.0f)
            optical_depth = 80.0f;
        float hf = 1.0f - expf(-optical_depth);
        if (!isfinite(hf))
            hf = 1.0f;
        hf = (hf < 0.0f ? 0.0f : (hf > 1.0f ? 1.0f : hf)) * fog_ctx->height_fog_blend;
        fog_f = 1.0f - (1.0f - fog_f) * (1.0f - hf);
        if (fog_ctx->height_fog_sun_amount > 0.0f) {
            /* Sun inscattering: shift the fog color toward the sun tint by
             * view-sun alignment (silver lining looking toward the sun). */
            float vx = wx - fog_ctx->cam_pos[0];
            float vy = wy - fog_ctx->cam_pos[1];
            float vz = wz - fog_ctx->cam_pos[2];
            float vlen = sw_length3(vx, vy, vz);
            if (isfinite(vlen) && vlen > 1e-6f) {
                float align =
                    (vx * fog_ctx->height_fog_sun_dir[0] + vy * fog_ctx->height_fog_sun_dir[1] +
                     vz * fog_ctx->height_fog_sun_dir[2]) /
                    vlen;
                if (align > 0.0f) {
                    float sun_w =
                        fog_ctx->height_fog_sun_amount * powf(align, fog_ctx->height_fog_sun_power);
                    sun_w = clamp01f(sun_w);
                    fog_r += (fog_ctx->height_fog_sun_color[0] - fog_r) * sun_w;
                    fog_g += (fog_ctx->height_fog_sun_color[1] - fog_g) * sun_w;
                    fog_b += (fog_ctx->height_fog_sun_color[2] - fog_b) * sun_w;
                }
            }
        }
    }
    fog_f = clamp01f(fog_f);
    *fr = *fr * (1.0f - fog_f) + fog_r * fog_f;
    *fg = *fg * (1.0f - fog_f) + fog_g * fog_f;
    *fb_c = *fb_c * (1.0f - fog_f) + fog_b * fog_f;
}

/// @brief Convert an interpolated NDC depth (sz, GL-style [-1,1]) to view-space
///   depth using the frame's clip planes. Out-of-range/empty (FLT_MAX) inputs
///   clamp to the far plane so "nothing behind" reads as maximally distant.
/// @param ctx Borrowed software context containing near/far planes and projection type.
/// @param ndc_z Normalized-device depth to linearize.
/// @return Positive view-space depth, the far plane for cleared input, or zero when clip planes
///         are invalid.
static float sw_linearize_ndc_depth(const sw_context_t *ctx, float ndc_z) {
    float zn = ctx->cam_znear;
    float zf = ctx->cam_zfar;
    if (!(zn > 0.0f) || !(zf > zn))
        return 0.0f;
    if (!isfinite(ndc_z) || ndc_z > 1.0f)
        return zf;
    if (ndc_z < -1.0f)
        ndc_z = -1.0f;
    if (ctx->cam_is_ortho)
        return zn + (ndc_z + 1.0f) * 0.5f * (zf - zn);
    return (2.0f * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

/// @brief Resolve a fragment's flattened clustered-light cell.
/// @param fc Borrowed triangle context with validated table and framebuffer dimensions.
/// @param x Fragment pixel X coordinate.
/// @param y Fragment pixel Y coordinate.
/// @param ndc_z Interpolated normalized-device depth.
/// @return Clamped flattened cluster index.
static int32_t sw_fragment_cluster_index(const sw_fragment_ctx *fc, int x, int y, float ndc_z) {
    const vgfx3d_cluster_table_t *table = fc->cluster_table;
    float u = ((float)x + 0.5f) / (float)fc->framebuffer_width;
    float v = ((float)y + 0.5f) / (float)fc->framebuffer_height;
    float depth = sw_linearize_ndc_depth(fc->fog_ctx, ndc_z);
    float z_scale;
    int32_t cluster_x = (int32_t)(u * (float)VGFX3D_CLUSTER_DIM_X);
    int32_t cluster_y = (int32_t)(v * (float)VGFX3D_CLUSTER_DIM_Y);
    int32_t cluster_z = 0;

    if (depth > table->znear && depth < table->zfar) {
        z_scale = logf(depth / table->znear) / logf(table->zfar / table->znear);
        cluster_z = (int32_t)(z_scale * (float)VGFX3D_CLUSTER_DIM_Z);
    } else if (depth >= table->zfar) {
        cluster_z = VGFX3D_CLUSTER_DIM_Z - 1;
    }
    if (cluster_x < 0)
        cluster_x = 0;
    if (cluster_x >= VGFX3D_CLUSTER_DIM_X)
        cluster_x = VGFX3D_CLUSTER_DIM_X - 1;
    if (cluster_y < 0)
        cluster_y = 0;
    if (cluster_y >= VGFX3D_CLUSTER_DIM_Y)
        cluster_y = VGFX3D_CLUSTER_DIM_Y - 1;
    if (cluster_z < 0)
        cluster_z = 0;
    if (cluster_z >= VGFX3D_CLUSTER_DIM_Z)
        cluster_z = VGFX3D_CLUSTER_DIM_Z - 1;
    return cluster_x + cluster_y * VGFX3D_CLUSTER_DIM_X +
           cluster_z * VGFX3D_CLUSTER_DIM_X * VGFX3D_CLUSTER_DIM_Y;
}

/// @brief Decide whether a resolved fragment may use the opaque color/depth write path.
/// @details The draw's blend state is invariant across its fragments. In particular, an explicit
///          BLEND or additive material remains non-depth-writing even when one sampled fragment
///          happens to have alpha one.
/// @param cmd Borrowed sanitized draw command.
/// @param fragment_alpha Resolved fragment alpha after material effects.
/// @return Non-zero only for fully opaque fragments from an opaque/masked draw.
static int sw_fragment_uses_opaque_write(const vgfx3d_draw_cmd_t *cmd, float fragment_alpha) {
    return fragment_alpha >= 1.0f && !vgfx3d_draw_cmd_uses_transparent_blend(cmd);
}

/// @brief Apply alpha mode (mask/opaque/blend), the shading_model==4 fresnel-alpha
///   term, then write or blend the fragment into the framebuffer (with optional
///   depth write). Returns 1 if a pixel was written, 0 if discarded.
/// @param fc Borrowed per-triangle context containing framebuffer, depth, and material state.
/// @param x Destination pixel X coordinate.
/// @param y Destination pixel Y coordinate.
/// @param idx Linear index into the depth buffer.
/// @param z Interpolated normalized-device depth.
/// @param b0 Perspective-correct first attribute weight.
/// @param b1 Perspective-correct second attribute weight.
/// @param b2 Perspective-correct third attribute weight.
/// @param fr Shaded red component.
/// @param fg Shaded green component.
/// @param fb_c Shaded blue component.
/// @param tex_alpha Sampled base-color alpha.
/// @param pixel_ndv Clamped normal-view dot product used by Fresnel alpha.
/// @return 1 when an opaque or blended pixel is written, otherwise 0 when discarded.
static int sw_blend_and_write(const sw_fragment_ctx *fc,
                              int x,
                              int y,
                              size_t idx,
                              float z,
                              float b0,
                              float b1,
                              float b2,
                              float fr,
                              float fg,
                              float fb_c,
                              float tex_alpha,
                              float pixel_ndv) {
    uint8_t *pixels = fc->pixels;
    float *zbuf = fc->zbuf;
    int32_t stride = fc->stride;
    const screen_vert_t *v0 = fc->v0;
    const screen_vert_t *v1 = fc->v1;
    const screen_vert_t *v2 = fc->v2;
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;
    const sw_pixels_view *normal_map = fc->normal_map;
    int depth_disabled = fc->depth_disabled;

    /* Interpolate alpha, respecting material alpha modes. */
    float material_alpha = b0 * v0->a + b1 * v1->a + b2 * v2->a;
    float fa = clamp01f(material_alpha * tex_alpha);
    int discard_fragment = 0;
    if (cmd && cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_MASK) {
        if (fa < cmd->alpha_cutoff)
            discard_fragment = 1;
        fa = 1.0f;
    } else if (cmd && cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE) {
        fa = 1.0f;
    }
    if (cmd && (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR || normal_map) &&
        cmd->shading_model == 4) {
        float ndv = pixel_ndv;
        float power = cmd->custom_params[0] > 0.1f ? cmd->custom_params[0] : 3.0f;
        float bias = cmd->custom_params[1];
        float fresnel = sw_material_powf(1.0f - ndv, power) + bias;
        fa *= clamp01f(fresnel);
    }
    /* Plan 10 soft particles: fade blend fragments as they approach the opaque
     * depth snapshot, removing the hard intersection line where a particle quad
     * crosses geometry. Only meaningful for depth-tested blend-mode draws. The
     * snapshot is indexed by its own dimensions, not the framebuffer's, so a
     * stale snapshot captured at a different size cannot alias wrong texels. */
    if (cmd && cmd->soft_particle_fade > 0.0f && !depth_disabled && fc->fog_ctx &&
        fc->fog_ctx->opaque_zbuf_valid && fc->fog_ctx->opaque_zbuf &&
        (cmd->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND || cmd->additive_blend) && x >= 0 &&
        y >= 0 && x < fc->fog_ctx->opaque_zbuf_w && y < fc->fog_ctx->opaque_zbuf_h) {
        const sw_context_t *sc = fc->fog_ctx;
        size_t snap_idx = (size_t)y * (size_t)sc->opaque_zbuf_w + (size_t)x;
        float lin_scene = sw_linearize_ndc_depth(sc, sc->opaque_zbuf[snap_idx]);
        float lin_frag = sw_linearize_ndc_depth(sc, z);
        if (lin_scene > 0.0f && lin_frag > 0.0f)
            fa *= clamp01f((lin_scene - lin_frag) / cmd->soft_particle_fade);
    }

    uint8_t *dst = pixels + (size_t)y * (size_t)stride + (size_t)x * 4u;
    if (!discard_fragment && sw_fragment_uses_opaque_write(cmd, fa)) {
        /* Opaque: overwrite pixel; screen-space overlays skip Z writes.
         * Round to nearest (+0.5) like GPU backends so quantization does not
         * carry a consistent half-LSB darkening bias versus native output. */
        if (!depth_disabled)
            zbuf[idx] = z;
        dst[0] = (uint8_t)(clamp01f(fr) * 255.0f + 0.5f);
        dst[1] = (uint8_t)(clamp01f(fg) * 255.0f + 0.5f);
        dst[2] = (uint8_t)(clamp01f(fb_c) * 255.0f + 0.5f);
        dst[3] = 0xFF;
        return 1;
    } else if (!discard_fragment && fa > 0.0f) {
        /* Transparent: additive keeps the destination, alpha uses source-over.
         * Both skip Z writes so they don't occlude later transparent draws. */
        if (cmd && cmd->additive_blend) {
            dst[0] = (uint8_t)fminf(255.0f, clamp01f(fr) * 255.0f * fa + (float)dst[0] + 0.5f);
            dst[1] = (uint8_t)fminf(255.0f, clamp01f(fg) * 255.0f * fa + (float)dst[1] + 0.5f);
            dst[2] = (uint8_t)fminf(255.0f, clamp01f(fb_c) * 255.0f * fa + (float)dst[2] + 0.5f);
        } else {
            float inv_a = 1.0f - fa;
            dst[0] = (uint8_t)(clamp01f(fr) * 255.0f * fa + (float)dst[0] * inv_a + 0.5f);
            dst[1] = (uint8_t)(clamp01f(fg) * 255.0f * fa + (float)dst[1] * inv_a + 0.5f);
            dst[2] = (uint8_t)(clamp01f(fb_c) * 255.0f * fa + (float)dst[2] * inv_a + 0.5f);
        }
        dst[3] = 0xFF;
        return 1;
    }
    /* else: alpha <= 0 → fully invisible, skip */

    return 0;
}

/// @brief Shade and write one fragment that already passed the in-triangle and
///   depth tests. Resolves albedo (base/splat/emissive), then per-pixel normal
///   mapping and PBR or Phong lighting with shadows, environment reflection,
///   distance fog, and alpha-mode-aware blending into the framebuffer.
/// @param fc Per-triangle invariants (see sw_fragment_ctx).
/// @param x Destination pixel X coordinate.
/// @param y Destination pixel Y coordinate.
/// @param idx Linear depth-buffer index for the pixel.
/// @param z Interpolated normalized-device depth.
/// @param b0 First screen-space barycentric weight.
/// @param b1 Second screen-space barycentric weight.
/// @param b2 Third screen-space barycentric weight.
/// @return 1 if a pixel was written (opaque or blended), 0 if discarded.
static int sw_shade_fragment(
    const sw_fragment_ctx *fc, int x, int y, size_t idx, float z, float b0, float b1, float b2) {
    const screen_vert_t *v0 = fc->v0;
    const screen_vert_t *v1 = fc->v1;
    const screen_vert_t *v2 = fc->v2;
    const sw_pixels_view *emissive_tex = fc->emissive_tex;
    const sw_pixels_view *normal_map = fc->normal_map;
    const sw_pixels_view *specular_map = fc->specular_map;
    const sw_pixels_view *metallic_roughness_map = fc->metallic_roughness_map;
    const sw_pixels_view *ao_map = fc->ao_map;
    const vgfx3d_draw_cmd_t *cmd = fc->cmd;
    const sw_context_t *fog_ctx = fc->fog_ctx;

    float attr_b0;
    float attr_b1;
    float attr_b2;
    if (!sw_perspective_attribute_weights(
            b0, b1, b2, v0->inv_w, v1->inv_w, v2->inv_w, &attr_b0, &attr_b1, &attr_b2))
        return 0;

    float fr, fg, fb_c, tex_alpha;
    sw_shade_resolve_albedo(fc, attr_b0, attr_b1, attr_b2, b0, b1, b2, &fr, &fg, &fb_c, &tex_alpha);

    /* Per-pixel lighting for PBR or normal-mapped legacy materials. */
    float pixel_ndv = 1.0f;
    /* Final shading normal; reused by the env-reflection pass below to avoid
     * recomputing the TBN a second time. */
    float shading_normal[3] = {0.0f, 0.0f, 0.0f};
    int shading_normal_valid = 0;
    sw_slot_uvs uv = {0};
    int uv_valid = 0;
    if (cmd && !cmd->unlit &&
        (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR || normal_map || fc->per_pixel_shadows)) {
        float pp_iw = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
        if (fabsf(pp_iw) > 1e-7f) {
            uv_valid = 1;
            sw_interpolate_uv_for_slot(cmd,
                                       RT_MATERIAL3D_TEXTURE_SLOT_NORMAL,
                                       b0,
                                       b1,
                                       b2,
                                       v0,
                                       v1,
                                       v2,
                                       &uv.normal_u,
                                       &uv.normal_v);
            if (specular_map)
                sw_interpolate_uv_for_slot(cmd,
                                           RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR,
                                           b0,
                                           b1,
                                           b2,
                                           v0,
                                           v1,
                                           v2,
                                           &uv.specular_u,
                                           &uv.specular_v);
            if (emissive_tex)
                sw_interpolate_uv_for_slot(cmd,
                                           RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE,
                                           b0,
                                           b1,
                                           b2,
                                           v0,
                                           v1,
                                           v2,
                                           &uv.emissive_u,
                                           &uv.emissive_v);
            if (metallic_roughness_map)
                sw_interpolate_uv_for_slot(cmd,
                                           RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS,
                                           b0,
                                           b1,
                                           b2,
                                           v0,
                                           v1,
                                           v2,
                                           &uv.mr_u,
                                           &uv.mr_v);
            if (ao_map)
                sw_interpolate_uv_for_slot(
                    cmd, RT_MATERIAL3D_TEXTURE_SLOT_AO, b0, b1, b2, v0, v1, v2, &uv.ao_u, &uv.ao_v);
            if (cmd->lightmap) {
                /* Raw TEXCOORD_1 (perspective-correct), no slot transform. */
                uv.lm_u = (b0 * v0->u1_over_w + b1 * v1->u1_over_w + b2 * v2->u1_over_w) / pp_iw;
                uv.lm_v = (b0 * v0->v1_over_w + b1 * v1->v1_over_w + b2 * v2->v1_over_w) / pp_iw;
            }

            sw_shade_geom geom;
            sw_shade_perturb_normal(fc,
                                    attr_b0,
                                    attr_b1,
                                    attr_b2,
                                    uv.normal_u,
                                    uv.normal_v,
                                    &geom.nx,
                                    &geom.ny,
                                    &geom.nz);
            shading_normal[0] = geom.nx;
            shading_normal[1] = geom.ny;
            shading_normal[2] = geom.nz;
            shading_normal_valid = 1;

            geom.wx = attr_b0 * v0->wx + attr_b1 * v1->wx + attr_b2 * v2->wx;
            geom.wy = attr_b0 * v0->wy + attr_b1 * v1->wy + attr_b2 * v2->wy;
            geom.wz = attr_b0 * v0->wz + attr_b1 * v1->wz + attr_b2 * v2->wz;
            sw_compute_view_vector(
                fog_ctx, geom.wx, geom.wy, geom.wz, &geom.vx, &geom.vy, &geom.vz);
            geom.ndv = clamp01f(dot3f(geom.nx, geom.ny, geom.nz, geom.vx, geom.vy, geom.vz));
            geom.global_light_count = fc->light_count;
            geom.local_light_indices = NULL;
            geom.local_light_count = 0;
            if (fc->cluster_table) {
                int32_t cluster_index = sw_fragment_cluster_index(fc, x, y, z);
                uint16_t begin = fc->cluster_table->offsets[cluster_index];
                uint16_t end = fc->cluster_table->offsets[cluster_index + 1];
                geom.global_light_count = fc->cluster_table->global_light_count;
                geom.local_light_indices = &fc->cluster_table->indices[begin];
                geom.local_light_count = (int32_t)(end - begin);
            }
            pixel_ndv = geom.ndv;

            if (cmd->workflow == RT_MATERIAL3D_WORKFLOW_PBR)
                sw_shade_lighting_pbr(fc, &geom, &uv, &fr, &fg, &fb_c);
            else
                sw_shade_lighting_phong(fc, &geom, &uv, &fr, &fg, &fb_c);
        }
    }

    if (cmd && cmd->env_map && cmd->reflectivity > 0.0001f) {
        sw_apply_environment_reflection(cmd,
                                        normal_map,
                                        metallic_roughness_map,
                                        attr_b0,
                                        attr_b1,
                                        attr_b2,
                                        b0,
                                        b1,
                                        b2,
                                        v0,
                                        v1,
                                        v2,
                                        fog_ctx,
                                        fc->normal_sign,
                                        shading_normal_valid ? shading_normal : NULL,
                                        uv_valid ? &uv : NULL,
                                        &fr,
                                        &fg,
                                        &fb_c);
    }

    /* Fog — interpolate world position, compute camera distance (distance
     * and/or exponential height fog; both share the fog color). */
    if (fog_ctx && (fog_ctx->fog_enabled || fog_ctx->height_fog_enabled))
        sw_shade_apply_fog(fog_ctx, attr_b0, attr_b1, attr_b2, v0, v1, v2, &fr, &fg, &fb_c);

    return sw_blend_and_write(
        fc, x, y, idx, z, attr_b0, attr_b1, attr_b2, fr, fg, fb_c, tex_alpha, pixel_ndv);
}

/// @brief Edge-function triangle rasterizer with PBR + Phong + shadow + fog support.
///
/// Centerpiece of the software backend. Steps:
///   1. Backface cull / winding flip via signed triangle area.
///   2. Compute screen-space bounding box, clip to viewport.
///   3. For each pixel: barycentric weights → in-triangle test → Z
///      compare → texture sample → lighting (Phong or PBR Cook-Torrance)
///      → shadow lookup → fog blend → alpha blend → write.
/// Honours every flag on `cmd` (unlit, alpha mode, normal map, etc.).
/// Optimization: perspective-correct attribute interpolation via
/// `u/w`, `v/w`, `1/w` vertex attributes.
/// @brief Computed rasterization setup for one screen-space triangle: the
///   (possibly winding-swapped) vertices, clipped bounding box, inverse area, and
///   half-space edge-function deltas + first-pixel values.
typedef struct {
    const screen_vert_t *v0, *v1, *v2;
    int min_x, max_x, min_y, max_y;
    float area, original_area, inv_area;
    float e12_dx, e12_dy, e20_dx, e20_dy, e01_dx, e01_dy;
    float row_w0, row_w1, row_w2;
    uint8_t edge12_top_left, edge20_top_left, edge01_top_left;
} sw_raster_geom;

/// @brief Conservatively test a screen triangle against one rectangle of pixel centers.
/// @details The triangle bounding box is cheap but grossly over-bins long thin triangles. This
/// applies the three remaining triangle/box SAT axes in double precision; the rectangle is expanded
/// by half a pixel so numerical edge cases can only add work, never drop covered samples.
static int sw_triangle_overlaps_pixel_rect(float x0,
                                           float y0,
                                           float x1,
                                           float y1,
                                           float x2,
                                           float y2,
                                           int32_t min_x,
                                           int32_t min_y,
                                           int32_t max_x,
                                           int32_t max_y) {
    double xs[3] = {(double)x0, (double)x1, (double)x2};
    double ys[3] = {(double)y0, (double)y1, (double)y2};
    double center_x = ((double)min_x + (double)max_x) * 0.5;
    double center_y = ((double)min_y + (double)max_y) * 0.5;
    double half_x = ((double)max_x - (double)min_x) * 0.5 + 0.5;
    double half_y = ((double)max_y - (double)min_y) * 0.5 + 0.5;
    for (int edge = 0; edge < 3; ++edge) {
        int next = edge == 2 ? 0 : edge + 1;
        double axis_x = -(ys[next] - ys[edge]);
        double axis_y = xs[next] - xs[edge];
        double p0 = xs[0] * axis_x + ys[0] * axis_y;
        double p1 = xs[1] * axis_x + ys[1] * axis_y;
        double p2 = xs[2] * axis_x + ys[2] * axis_y;
        double tri_min = fmin(p0, fmin(p1, p2));
        double tri_max = fmax(p0, fmax(p1, p2));
        double rect_center = center_x * axis_x + center_y * axis_y;
        double rect_radius = half_x * fabs(axis_x) + half_y * fabs(axis_y);
        double epsilon = (fabs(rect_center) + rect_radius + 1.0) * 1e-12;
        if (tri_max < rect_center - rect_radius - epsilon ||
            tri_min > rect_center + rect_radius + epsilon)
            return 0;
    }
    return 1;
}

/// @brief Return whether an oriented edge owns samples exactly on that edge.
/// @details Implements the standard top-left fill convention so adjacent triangles sharing an edge
///          do not both shade it or both leave a crack. The software backend uses screen-space
///          coordinates after viewport transform, so this deterministic ownership rule is based on
///          the final rasterized edge direction.
/// @param a Borrowed oriented edge start vertex.
/// @param b Borrowed oriented edge end vertex.
/// @return Non-zero when the edge is top or left under the rasterizer's screen-space convention.
static int sw_edge_is_top_left(const screen_vert_t *a, const screen_vert_t *b) {
    float dx;
    float dy;
    if (!a || !b)
        return 0;
    dx = b->sx - a->sx;
    dy = b->sy - a->sy;
    return (dy > 0.0f || (dy == 0.0f && dx < 0.0f)) ? 1 : 0;
}

/// @brief Test one edge function value using top-left sample ownership.
/// @param value Signed half-space edge-function value at the sample.
/// @param top_left Non-zero when exact-zero samples belong to this edge.
/// @return Non-zero when the sample lies inside or on an owned edge.
static int sw_edge_accepts_sample(float value, uint8_t top_left) {
    return value > 0.0f || (top_left && value == 0.0f);
}

/// @brief Backface-cull, fix winding, reject degenerate/offscreen triangles, and
///   compute the bounding box + half-space edge functions. Returns 1 with *g
///   filled when the triangle should be rasterized, or 0 (emitting the matching
///   reject debug line when emit_debug is set) when it is rejected.
/// @param v0 Borrowed first screen-space vertex.
/// @param v1 Borrowed second screen-space vertex.
/// @param v2 Borrowed third screen-space vertex.
/// @param fb_w Positive framebuffer width in pixels.
/// @param fb_h Positive framebuffer height in pixels.
/// @param backface_cull Non-zero to reject back-facing triangles.
/// @param emit_debug Non-zero to print a bounded diagnostic for rejected geometry.
/// @param debug_tri_count Optional in/out diagnostic counter.
/// @param g Receives reordered vertices, clipped bounds, and edge-function increments.
/// @return 1 when @p g describes a rasterizable triangle, otherwise 0.
static int sw_raster_prepare(const screen_vert_t *v0,
                             const screen_vert_t *v1,
                             const screen_vert_t *v2,
                             int32_t fb_w,
                             int32_t fb_h,
                             int8_t backface_cull,
                             int emit_debug,
                             int32_t *debug_tri_count,
                             sw_raster_geom *g) {
    if (!v0 || !v1 || !v2 || !g || fb_w <= 0 || fb_h <= 0 || !isfinite(v0->sx) ||
        !isfinite(v0->sy) || !isfinite(v1->sx) || !isfinite(v1->sy) || !isfinite(v2->sx) ||
        !isfinite(v2->sy)) {
        if (emit_debug) {
            fprintf(stderr,
                    "[sw3d] tri %d rejected invalid raster inputs\n",
                    debug_tri_count ? (*debug_tri_count)++ : 0);
        }
        return 0;
    }
    float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v2->sx - v0->sx) * (v1->sy - v0->sy);
    float original_area = area;
    float span_x = fmaxf(fmaxf(v0->sx, v1->sx), v2->sx) - fminf(fminf(v0->sx, v1->sx), v2->sx);
    float span_y = fmaxf(fmaxf(v0->sy, v1->sy), v2->sy) - fminf(fminf(v0->sy, v1->sy), v2->sy);
    float scale = fmaxf(1.0f, fmaxf(fabsf(span_x), fabsf(span_y)));
    const float area_epsilon = fmaxf(1e-12f, scale * scale * 1e-12f);

    if (fabsf(area) <= area_epsilon) {
        if (emit_debug) {
            fprintf(stderr,
                    "[sw3d] tri %d degenerate area=%.6f orig=%.6f\n",
                    debug_tri_count ? (*debug_tri_count)++ : 0,
                    fabsf(area),
                    original_area);
        }
        return 0;
    }

    /* After viewport Y-flip, CCW world-space triangles have NEGATIVE screen-space
     * area. So negative area = front face, positive area = back face.
     * Cull back faces (positive area) when backface culling is enabled. */
    if (backface_cull && area > area_epsilon) {
        if (emit_debug) {
            fprintf(stderr,
                    "[sw3d] tri %d culled area=%.3f v0=(%.2f %.2f %.3f) v1=(%.2f %.2f %.3f) "
                    "v2=(%.2f %.2f %.3f)\n",
                    debug_tri_count ? (*debug_tri_count)++ : 0,
                    original_area,
                    v0->sx,
                    v0->sy,
                    v0->sz,
                    v1->sx,
                    v1->sy,
                    v1->sz,
                    v2->sx,
                    v2->sy,
                    v2->sz);
        }
        return 0;
    }
    if (area < 0.0f) {
        const screen_vert_t *tmp = v1;
        v1 = v2;
        v2 = tmp;
        area = -area;
    }

    if (!isfinite(area)) {
        if (emit_debug) {
            fprintf(stderr,
                    "[sw3d] tri %d rejected non-finite screen area\n",
                    debug_tri_count ? (*debug_tri_count)++ : 0);
        }
        return 0;
    }

    int min_x = sw_floor_pixel_coord(fminf(fminf(v0->sx, v1->sx), v2->sx), fb_w - 1);
    int max_x = sw_ceil_pixel_coord(fmaxf(fmaxf(v0->sx, v1->sx), v2->sx), fb_w - 1);
    int min_y = sw_floor_pixel_coord(fminf(fminf(v0->sy, v1->sy), v2->sy), fb_h - 1);
    int max_y = sw_ceil_pixel_coord(fmaxf(fmaxf(v0->sy, v1->sy), v2->sy), fb_h - 1);
    if (min_x > max_x || min_y > max_y) {
        if (emit_debug) {
            fprintf(stderr,
                    "[sw3d] tri %d clipped bbox=(%d..%d,%d..%d) area=%.3f orig=%.3f\n",
                    debug_tri_count ? (*debug_tri_count)++ : 0,
                    min_x,
                    max_x,
                    min_y,
                    max_y,
                    area,
                    original_area);
        }
        return 0;
    }

    /* Half-space edge functions for rasterization.
     * w0/w1/w2 are the signed distances from pixel center to each edge.
     * A pixel is inside when all three are >= 0. The edge function increments
     * by -dy/+dx per pixel step, enabling efficient scanline traversal. */
    float e12_dx = v2->sx - v1->sx, e12_dy = v2->sy - v1->sy;
    float e20_dx = v0->sx - v2->sx, e20_dy = v0->sy - v2->sy;
    float e01_dx = v1->sx - v0->sx, e01_dy = v1->sy - v0->sy;
    float px0 = (float)min_x + 0.5f, py0 = (float)min_y + 0.5f;

    g->v0 = v0;
    g->v1 = v1;
    g->v2 = v2;
    g->min_x = min_x;
    g->max_x = max_x;
    g->min_y = min_y;
    g->max_y = max_y;
    g->area = area;
    g->original_area = original_area;
    g->inv_area = 1.0f / area;
    g->e12_dx = e12_dx;
    g->e12_dy = e12_dy;
    g->e20_dx = e20_dx;
    g->e20_dy = e20_dy;
    g->e01_dx = e01_dx;
    g->e01_dy = e01_dy;
    g->row_w0 = e12_dx * (py0 - v1->sy) - e12_dy * (px0 - v1->sx);
    g->row_w1 = e20_dx * (py0 - v2->sy) - e20_dy * (px0 - v2->sx);
    g->row_w2 = e01_dx * (py0 - v0->sy) - e01_dy * (px0 - v0->sx);
    g->edge12_top_left = (uint8_t)sw_edge_is_top_left(v1, v2);
    g->edge20_top_left = (uint8_t)sw_edge_is_top_left(v2, v0);
    g->edge01_top_left = (uint8_t)sw_edge_is_top_left(v0, v1);
    return 1;
}

/// @brief Advance an edge-function accumulator by a signed number of steps.
/// @param value Edge-function value at the starting sample.
/// @param delta Per-step edge-function increment.
/// @param steps Signed number of pixel or scanline steps.
/// @return The advanced edge-function value.
static float sw_advance_edge_value(float value, float delta, int steps) {
    return value + delta * (float)steps;
}

/// @brief Rasterize and shade one triangle within an inclusive pixel scissor rectangle.
/// @details Prepares top-left-rule edge functions once, resolves triangle-constant texture views,
///          and walks only the intersection of the triangle bounds, framebuffer, and supplied
///          scissor. The caller must ensure parallel jobs use disjoint scissor regions.
/// @param pixels Borrowed mutable RGBA8 framebuffer.
/// @param zbuf Borrowed mutable linear depth buffer matching @p fb_w and @p fb_h.
/// @param fb_w Positive framebuffer width in pixels.
/// @param fb_h Positive framebuffer height in pixels.
/// @param stride Framebuffer row stride in bytes.
/// @param v0 Borrowed first screen-space vertex.
/// @param v1 Borrowed second screen-space vertex.
/// @param v2 Borrowed third screen-space vertex.
/// @param tex Optional borrowed base-color texture view.
/// @param emissive_tex Optional borrowed emissive texture view.
/// @param emissive_color Borrowed three-component legacy emissive multiplier.
/// @param normal_map Optional borrowed tangent-space normal-map view.
/// @param specular_map Optional borrowed legacy specular-map view.
/// @param metallic_roughness_map Optional borrowed PBR metallic-roughness-map view.
/// @param ao_map Optional borrowed ambient-occlusion-map view.
/// @param lightmap Optional borrowed baked-lightmap view.
/// @param cmd Borrowed sanitized draw command.
/// @param lights Optional borrowed sanitized light array.
/// @param light_count Number of entries supplied in @p lights.
/// @param ambient Borrowed three-component ambient-light color.
/// @param backface_cull Non-zero to reject back-facing triangles.
/// @param fog_ctx Borrowed software context containing camera, fog, shadow, and debug state.
/// @param clip_min_x Inclusive scissor minimum X coordinate.
/// @param clip_min_y Inclusive scissor minimum Y coordinate.
/// @param clip_max_x Inclusive scissor maximum X coordinate.
/// @param clip_max_y Inclusive scissor maximum Y coordinate.
static void raster_triangle_scissored(uint8_t *pixels,
                                      float *zbuf,
                                      int32_t fb_w,
                                      int32_t fb_h,
                                      int32_t stride,
                                      const screen_vert_t *v0,
                                      const screen_vert_t *v1,
                                      const screen_vert_t *v2,
                                      const sw_pixels_view *tex,
                                      const sw_pixels_view *emissive_tex,
                                      const float *emissive_color,
                                      const sw_pixels_view *normal_map,
                                      const sw_pixels_view *specular_map,
                                      const sw_pixels_view *metallic_roughness_map,
                                      const sw_pixels_view *ao_map,
                                      const sw_pixels_view *lightmap,
                                      const sw_pixels_view *decal_tex,
                                      const vgfx3d_draw_cmd_t *cmd,
                                      const vgfx3d_light_params_t *lights,
                                      int32_t light_count,
                                      const float *ambient,
                                      int8_t backface_cull,
                                      sw_context_t *fog_ctx,
                                      int32_t clip_min_x,
                                      int32_t clip_min_y,
                                      int32_t clip_max_x,
                                      int32_t clip_max_y) {
    if (!zbuf || !sw_rgba_surface_is_valid(pixels, fb_w, fb_h, stride) || !cmd || !v0 || !v1 || !v2)
        return;
    light_count = sw_sanitize_light_count(lights, light_count);
    int emit_debug = sw_debug_enabled() && fog_ctx && fog_ctx->debug_tri_count < 16;
    sw_raster_geom g;
    if (!sw_raster_prepare(v0,
                           v1,
                           v2,
                           fb_w,
                           fb_h,
                           backface_cull,
                           emit_debug,
                           fog_ctx ? &fog_ctx->debug_tri_count : NULL,
                           &g))
        return;
    int min_x = g.min_x > clip_min_x ? g.min_x : clip_min_x;
    int max_x = g.max_x < clip_max_x ? g.max_x : clip_max_x;
    int min_y = g.min_y > clip_min_y ? g.min_y : clip_min_y;
    int max_y = g.max_y < clip_max_y ? g.max_y : clip_max_y;
    if (min_x > max_x || min_y > max_y)
        return;

    const int depth_disabled = cmd->disable_depth_test;
    const float depth_bias = vgfx3d_clamp_float_param(cmd->depth_bias, -0.05f, 0.05f, 0.0f);
    float slope_depth_bias = 0.0f;
    if (fabsf(cmd->slope_scaled_depth_bias) > 1e-8f) {
        float denom = g.v0->sx * (g.v1->sy - g.v2->sy) + g.v1->sx * (g.v2->sy - g.v0->sy) +
                      g.v2->sx * (g.v0->sy - g.v1->sy);
        if (fabsf(denom) > 1e-8f) {
            float dzdx = (g.v0->sz * (g.v1->sy - g.v2->sy) + g.v1->sz * (g.v2->sy - g.v0->sy) +
                          g.v2->sz * (g.v0->sy - g.v1->sy)) /
                         denom;
            float dzdy = (g.v0->sz * (g.v2->sx - g.v1->sx) + g.v1->sz * (g.v0->sx - g.v2->sx) +
                          g.v2->sz * (g.v1->sx - g.v0->sx)) /
                         denom;
            slope_depth_bias = cmd->slope_scaled_depth_bias * fmaxf(fabsf(dzdx), fabsf(dzdy));
            slope_depth_bias = fmaxf(-0.05f, fminf(0.05f, slope_depth_bias));
        }
    }
    size_t inside_samples = 0;
    size_t depth_passes = 0;
    size_t pixels_written = 0;

    /* Terrain-splat views are constant across the whole triangle; resolve them
     * once here instead of re-opening the five Pixels views for every pixel. */
    sw_pixels_view splat_view;
    sw_pixels_view layer_views[4];
    int have_splat = (cmd->has_splat && cmd->splat_map)
                         ? sw_setup_complete_splat(cmd, &splat_view, layer_views)
                         : 0;

    int per_pixel_shadows =
        sw_draw_requires_per_pixel_lighting(fog_ctx, cmd, lights, light_count) &&
        cmd->workflow != RT_MATERIAL3D_WORKFLOW_PBR && !normal_map;
    const vgfx3d_cluster_table_t *cluster_table =
        (fog_ctx && fb_w > 0 && fb_h > 0 &&
         vgfx3d_cluster_table_is_usable(
             (const vgfx3d_cluster_table_t *)cmd->cluster_table, cmd->lights_revision, light_count))
            ? (const vgfx3d_cluster_table_t *)cmd->cluster_table
            : NULL;
    sw_fragment_ctx fc = {.pixels = pixels,
                          .zbuf = zbuf,
                          .stride = stride,
                          .v0 = g.v0,
                          .v1 = g.v1,
                          .v2 = g.v2,
                          .tex = tex,
                          .emissive_tex = emissive_tex,
                          .emissive_color = emissive_color,
                          .normal_map = normal_map,
                          .specular_map = specular_map,
                          .metallic_roughness_map = metallic_roughness_map,
                          .ao_map = ao_map,
                          .lightmap_tex = lightmap,
                          .cmd = cmd,
                          .lights = lights,
                          .light_count = light_count,
                          .cluster_table = cluster_table,
                          .framebuffer_width = fb_w,
                          .framebuffer_height = fb_h,
                          .ambient = ambient,
                          .decal_tex = decal_tex,
                          .fog_ctx = fog_ctx,
                          .depth_disabled = depth_disabled,
                          .have_splat = have_splat,
                          .per_pixel_shadows = per_pixel_shadows,
                          .normal_sign = (g.original_area > 0.0f) ? -1.0f : 1.0f,
                          .splat_view = have_splat ? &splat_view : NULL,
                          .layer_views = have_splat ? layer_views : NULL};

    float row_w0 = sw_advance_edge_value(g.row_w0, g.e12_dx, min_y - g.min_y);
    float row_w1 = sw_advance_edge_value(g.row_w1, g.e20_dx, min_y - g.min_y);
    float row_w2 = sw_advance_edge_value(g.row_w2, g.e01_dx, min_y - g.min_y);
    for (int y = min_y; y <= max_y; y++) {
        float w0 = sw_advance_edge_value(row_w0, -g.e12_dy, min_x - g.min_x);
        float w1 = sw_advance_edge_value(row_w1, -g.e20_dy, min_x - g.min_x);
        float w2 = sw_advance_edge_value(row_w2, -g.e01_dy, min_x - g.min_x);
        for (int x = min_x; x <= max_x; x++) {
            if (sw_edge_accepts_sample(w0, g.edge12_top_left) &&
                sw_edge_accepts_sample(w1, g.edge20_top_left) &&
                sw_edge_accepts_sample(w2, g.edge01_top_left)) {
                if (emit_debug)
                    inside_samples++;
                /* Barycentric weights from edge functions. sz is the post-perspective-divide
                 * NDC depth (clip.z * 1/w, set in the vertex stage), which is *linear in screen
                 * space* — so this screen-space barycentric interpolation is exactly the depth
                 * value GPU hardware produces, and matches the GL/D3D11 backends. (Perspective-
                 * correct interpolation — z/w then divide by interpolated 1/w — is for vertex
                 * *attributes* such as UVs, not for the depth buffer.) */
                float b0 = w0 * g.inv_area, b1 = w1 * g.inv_area, b2 = w2 * g.inv_area;
                float z = b0 * g.v0->sz + b1 * g.v1->sz + b2 * g.v2->sz;
                z += depth_bias + slope_depth_bias;
                size_t idx = (size_t)y * (size_t)fb_w + (size_t)x;
                if (isfinite(z)) {
                    if (z < -1.0f)
                        z = -1.0f;
                    else if (z > 1.0f)
                        z = 1.0f;
                }
                if (isfinite(z) && (depth_disabled || !isfinite(zbuf[idx]) || z < zbuf[idx])) {
                    if (fog_ctx && fog_ctx->depth_only_shading) {
                        /* ADR 0311 depth-only shading: keep the depth test and the
                         * opaque depth write (occlusion and later draws see the same
                         * scene), skip the fragment shader and the colour write. */
                        if (!depth_disabled && cmd &&
                            cmd->alpha_mode != RT_MATERIAL3D_ALPHA_MODE_BLEND &&
                            !cmd->additive_blend)
                            zbuf[idx] = z;
                        if (emit_debug)
                            depth_passes++;
                    } else {
                        int wrote_pixel = sw_shade_fragment(&fc, x, y, idx, z, b0, b1, b2);
                        if (emit_debug) {
                            depth_passes++;
                            pixels_written += (size_t)(wrote_pixel != 0);
                        }
                    }
                }
            }
            w0 -= g.e12_dy;
            w1 -= g.e20_dy;
            w2 -= g.e01_dy;
        }
        row_w0 += g.e12_dx;
        row_w1 += g.e20_dx;
        row_w2 += g.e01_dx;
    }

    if (emit_debug) {
        fprintf(stderr,
                "[sw3d] tri %d area=%.3f orig=%.3f bbox=(%d..%d,%d..%d) inside=%zu depth=%zu "
                "written=%zu v0=(%.2f %.2f %.3f) v1=(%.2f %.2f %.3f) v2=(%.2f %.2f %.3f)\n",
                fog_ctx ? fog_ctx->debug_tri_count++ : 0,
                g.area,
                g.original_area,
                g.min_x,
                g.max_x,
                g.min_y,
                g.max_y,
                inside_samples,
                depth_passes,
                pixels_written,
                g.v0->sx,
                g.v0->sy,
                g.v0->sz,
                g.v1->sx,
                g.v1->sy,
                g.v1->sz,
                g.v2->sx,
                g.v2->sy,
                g.v2->sz);
    }
}

/// @brief Rasterize and shade one triangle across the complete framebuffer.
/// @details Convenience wrapper around @c raster_triangle_scissored() using the full framebuffer
///          bounds.
/// @param pixels Borrowed mutable RGBA8 framebuffer.
/// @param zbuf Borrowed mutable linear depth buffer matching @p fb_w and @p fb_h.
/// @param fb_w Positive framebuffer width in pixels.
/// @param fb_h Positive framebuffer height in pixels.
/// @param stride Framebuffer row stride in bytes.
/// @param v0 Borrowed first screen-space vertex.
/// @param v1 Borrowed second screen-space vertex.
/// @param v2 Borrowed third screen-space vertex.
/// @param tex Optional borrowed base-color texture view.
/// @param emissive_tex Optional borrowed emissive texture view.
/// @param emissive_color Borrowed three-component legacy emissive multiplier.
/// @param normal_map Optional borrowed tangent-space normal-map view.
/// @param specular_map Optional borrowed legacy specular-map view.
/// @param metallic_roughness_map Optional borrowed PBR metallic-roughness-map view.
/// @param ao_map Optional borrowed ambient-occlusion-map view.
/// @param lightmap Optional borrowed baked-lightmap view.
/// @param cmd Borrowed sanitized draw command.
/// @param lights Optional borrowed sanitized light array.
/// @param light_count Number of entries supplied in @p lights.
/// @param ambient Borrowed three-component ambient-light color.
/// @param backface_cull Non-zero to reject back-facing triangles.
/// @param fog_ctx Borrowed software context containing camera, fog, shadow, and debug state.
static void raster_triangle(uint8_t *pixels,
                            float *zbuf,
                            int32_t fb_w,
                            int32_t fb_h,
                            int32_t stride,
                            const screen_vert_t *v0,
                            const screen_vert_t *v1,
                            const screen_vert_t *v2,
                            const sw_pixels_view *tex,
                            const sw_pixels_view *emissive_tex,
                            const float *emissive_color,
                            const sw_pixels_view *normal_map,
                            const sw_pixels_view *specular_map,
                            const sw_pixels_view *metallic_roughness_map,
                            const sw_pixels_view *ao_map,
                            const sw_pixels_view *lightmap,
                            const sw_pixels_view *decal_tex,
                            const vgfx3d_draw_cmd_t *cmd,
                            const vgfx3d_light_params_t *lights,
                            int32_t light_count,
                            const float *ambient,
                            int8_t backface_cull,
                            sw_context_t *fog_ctx) {
    raster_triangle_scissored(pixels,
                              zbuf,
                              fb_w,
                              fb_h,
                              stride,
                              v0,
                              v1,
                              v2,
                              tex,
                              emissive_tex,
                              emissive_color,
                              normal_map,
                              specular_map,
                              metallic_roughness_map,
                              ao_map,
                              lightmap,
                              decal_tex,
                              cmd,
                              lights,
                              light_count,
                              ambient,
                              backface_cull,
                              fog_ctx,
                              0,
                              0,
                              fb_w - 1,
                              fb_h - 1);
}
