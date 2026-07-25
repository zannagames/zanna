//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_backend_sw_texture.inc
// Purpose: Validated software texture views, coordinate wrapping, and filtered sampling.
// Key invariants:
//   - Pixels extents fit signed sampler indices and every computed texel index remains in range.
//   - Non-finite coordinates/colors fall back to deterministic finite samples.
// Ownership/Lifetime:
//   - sw_pixels_view borrows Pixels storage retained by the enclosing draw snapshot.
//   - Sampling allocates no per-fragment memory and never mutates source images.
// Links: vgfx3d_backend_sw.c, vgfx3d_backend_sw_raster.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_backend_sw_texture.inc
 * @brief Implements validated Pixels views, texture addressing, filtering, and UV interpolation
 *        for the software Graphics3D backend.
 *
 * Sampling is allocation-free and supports repeat, mirrored-repeat, and clamp addressing with
 * nearest or bilinear base-level filtering. Per-material slots may select either UV channel,
 * apply independent affine transforms, and opt into sRGB-to-linear conversion before lighting.
 */

/*==========================================================================
 * Texture sampling
 *=========================================================================*/

typedef struct {
    int64_t width;
    int64_t height;
    uint32_t *data;
} sw_pixels_view;

/// @brief Cast an `rt_pixels` opaque pointer to a `sw_pixels_view` for sampling.
///
/// The underlying struct layout is duplicated locally (matches the
/// public ABI of `rt_pixels_new`). Returns 1 if the view is sampleable
/// (non-null + non-empty), 0 otherwise.
/// @param pixels_obj Borrowed opaque @c rt_pixels object.
/// @param out Receives the validated sampling view.
/// @return 1 when the dimensions, storage, and signed sampler indexing are valid, otherwise 0.
static int setup_pixels_view(const void *pixels_obj, sw_pixels_view *out) {
    if (!pixels_obj || !out)
        return 0;
    const sw_pixels_view *pv = (const sw_pixels_view *)pixels_obj;
    *out = *pv;
    if (out->width <= 0 || out->height <= 0 || out->width > INT32_MAX || out->height > INT32_MAX ||
        !out->data)
        return 0;
    if (out->width > INT32_MAX / out->height)
        return 0;
    return 1;
}

/// @brief Validate that a draw has full terrain-splat data (weight map + all 4 layers)
///   and open Pixels views over each; returns 0 if splatting is inactive or incomplete.
/// @param cmd Borrowed draw command containing the splat map and four layer identities.
/// @param splat_view Receives the validated weight-map view.
/// @param layer_views Receives four validated layer texture views.
/// @return 1 when every required texture is present and sampleable, otherwise 0.
static int sw_setup_complete_splat(const vgfx3d_draw_cmd_t *cmd,
                                   sw_pixels_view *splat_view,
                                   sw_pixels_view layer_views[4]) {
    if (!cmd || !cmd->has_splat || !cmd->splat_map || !cmd->splat_layers[0] ||
        !cmd->splat_layers[1] || !cmd->splat_layers[2] || !cmd->splat_layers[3]) {
        return 0;
    }
    if (!setup_pixels_view(cmd->splat_map, splat_view))
        return 0;
    for (int i = 0; i < 4; i++) {
        if (!setup_pixels_view(cmd->splat_layers[i], &layer_views[i]))
            return 0;
    }
    return 1;
}

/// @brief Wrap a continuous UV coordinate into [0, 1] according to the material wrap mode.
/// @details CLAMP_TO_EDGE saturates to [0, 1]. MIRRORED_REPEAT folds across even/odd periods
///          so the texture reads forward then backward. Default (repeat) uses fract().
/// @param value Continuous texture coordinate.
/// @param mode One of the @c RT_MATERIAL3D_TEXTURE_WRAP_* values.
/// @return Wrapped coordinate in `[0, 1]`, with non-finite input mapped to zero.
static float sw_wrap_coord(float value, int32_t mode) {
    if (!isfinite(value))
        return 0.0f;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE) {
        if (value < 0.0f)
            return 0.0f;
        if (value > 1.0f)
            return 1.0f;
        return value;
    }
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT) {
        double period = floor((double)value);
        double frac = (double)value - period;
        double parity = fmod(period, 2.0);
        if (frac < 0.0f) {
            frac += 1.0f;
            parity = fmod(period - 1.0, 2.0);
        }
        if (parity < 0.0)
            parity += 2.0;
        return parity >= 1.0 ? (float)(1.0 - frac) : (float)frac;
    }
    return (float)((double)value - floor((double)value));
}

/// @brief Convert a potentially-out-of-range texel coordinate to a valid array index.
/// @details Implements all three material wrap modes on integer texel indices after
///   the continuous UV coordinate has been mapped to the texel grid:
///     - `RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE`: saturates to `[0, size-1]`.
///     - `RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT`: maps the index into `[0, size-1]`
///       by folding across the period `2*size`; odd periods are mirrored so the
///       sequence reads `0,1,...,size-1,size-1,...,1,0,1,...` etc.
///     - Default (repeat): standard modulo with `((index % size) + size) % size`
///       so negative indices wrap correctly (plain `%` can return negative values in C).
///   Returns 0 immediately when @p size is zero or negative to prevent division by zero.
/// @param index  Texel coordinate, possibly negative or beyond `size`.
/// @param size   Texture dimension in texels along the relevant axis (must be > 0).
/// @param mode   One of the `RT_MATERIAL3D_TEXTURE_WRAP_*` constants.
/// @return Clamped/wrapped index in `[0, size-1]`.
static int sw_wrap_index(int64_t index, int size, int32_t mode) {
    if (size <= 0)
        return 0;
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE) {
        if (index < 0)
            return 0;
        if (index >= size)
            return size - 1;
        return (int)index;
    }
    if (mode == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT) {
        int64_t period = (int64_t)size * 2;
        int64_t wrapped = index % period;
        if (wrapped < 0)
            wrapped += period;
        return (int)(wrapped >= size ? period - 1 - wrapped : wrapped);
    }
    {
        int64_t wrapped = index % (int64_t)size;
        if (wrapped < 0)
            wrapped += size;
        return (int)wrapped;
    }
}

/// @brief Texture sampler with imported wrap/filter state.
///
/// Maps UV → texel center (subtract 0.5), looks up the 4 surrounding
/// texels with selected wrap indexing, then weights them by the
/// fractional component. Produces normalized [0,1] RGBA.
///
/// @note Accepted fallback limitation: this sampler is base-level only — it does
///   no LOD/mip selection, so minified textures alias/shimmer where the GL/Metal/
///   D3D11 backends use mipmapped trilinear minification. The software rasterizer
///   is a fallback renderer, not the VM-vs-native determinism reference (the GPU
///   backends are), so this parity gap is tolerated rather than closed with a full
///   screen-space-derivative mip pipeline (which would carry its own determinism
///   cost). Revisit if the SW path is ever promoted to the reference.
/// @param tex Borrowed validated pixel view; invalid input produces opaque white.
/// @param u Horizontal texture coordinate.
/// @param v Vertical texture coordinate.
/// @param wrap_s Horizontal addressing mode.
/// @param wrap_t Vertical addressing mode.
/// @param filter Nearest or linear material filter mode.
/// @param r Receives normalized red.
/// @param g Receives normalized green.
/// @param b Receives normalized blue.
/// @param a Receives normalized alpha.
static void sample_texture_ex(const sw_pixels_view *tex,
                              float u,
                              float v,
                              int32_t wrap_s,
                              int32_t wrap_t,
                              int32_t filter,
                              float *r,
                              float *g,
                              float *b,
                              float *a) {
    if (!r || !g || !b || !a)
        return;
    if (!tex || !tex->data || tex->width <= 0 || tex->height <= 0 || tex->width > INT32_MAX ||
        tex->height > INT32_MAX || tex->width > INT32_MAX / tex->height) {
        *r = *g = *b = 1.0f;
        *a = 1.0f;
        return;
    }
    int w = (int)tex->width, h = (int)tex->height;

    u = sw_wrap_coord(u, wrap_s);
    v = sw_wrap_coord(v, wrap_t);

    if (filter == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST) {
        int64_t raw_x = (int64_t)floor((double)u * (double)w);
        int64_t raw_y = (int64_t)floor((double)v * (double)h);
        int x = sw_wrap_index(raw_x, w, wrap_s);
        int y = sw_wrap_index(raw_y, h, wrap_t);
        uint32_t p = tex->data[(size_t)y * (size_t)w + (size_t)x];
        *r = (float)((p >> 24) & 0xFF) / 255.0f;
        *g = (float)((p >> 16) & 0xFF) / 255.0f;
        *b = (float)((p >> 8) & 0xFF) / 255.0f;
        *a = (float)(p & 0xFF) / 255.0f;
        return;
    }

    /* Bilinear: map UV to texel center, then interpolate the 4 neighbors */
    double fx = (double)u * (double)w - 0.5;
    double fy = (double)v * (double)h - 0.5;
    int64_t raw_x0 = (int64_t)floor(fx);
    int64_t raw_y0 = (int64_t)floor(fy);
    float xf = (float)(fx - (double)raw_x0); /* fractional part [0,1) */
    float yf = (float)(fy - (double)raw_y0);

    /* Wrap coordinates for the material sampler mode */
    int x0 = sw_wrap_index(raw_x0, w, wrap_s);
    int y0 = sw_wrap_index(raw_y0, h, wrap_t);
    int x1 = sw_wrap_index(raw_x0 + 1, w, wrap_s);
    int y1 = sw_wrap_index(raw_y0 + 1, h, wrap_t);

    /* Sample 4 texels */
    uint32_t p00 = tex->data[(size_t)y0 * (size_t)w + (size_t)x0];
    uint32_t p10 = tex->data[(size_t)y0 * (size_t)w + (size_t)x1];
    uint32_t p01 = tex->data[(size_t)y1 * (size_t)w + (size_t)x0];
    uint32_t p11 = tex->data[(size_t)y1 * (size_t)w + (size_t)x1];

    /* Bilinear weights */
    float w00 = (1.0f - xf) * (1.0f - yf);
    float w10 = xf * (1.0f - yf);
    float w01 = (1.0f - xf) * yf;
    float w11 = xf * yf;

    *r = (((p00 >> 24) & 0xFF) * w00 + ((p10 >> 24) & 0xFF) * w10 + ((p01 >> 24) & 0xFF) * w01 +
          ((p11 >> 24) & 0xFF) * w11) /
         255.0f;
    *g = (((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10 + ((p01 >> 16) & 0xFF) * w01 +
          ((p11 >> 16) & 0xFF) * w11) /
         255.0f;
    *b = (((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10 + ((p01 >> 8) & 0xFF) * w01 +
          ((p11 >> 8) & 0xFF) * w11) /
         255.0f;
    *a = ((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 + (p01 & 0xFF) * w01 + (p11 & 0xFF) * w11) /
         255.0f;
}

/// @brief Sample a texture with default repeat wrap and linear filter.
/// @details Convenience wrapper around `sample_texture_ex` that supplies
///   `RT_MATERIAL3D_TEXTURE_WRAP_REPEAT` for both axes and
///   `RT_MATERIAL3D_TEXTURE_FILTER_LINEAR` as the filter mode. Used for
///   internal texture lookups (e.g., environment cubemap faces) that are not
///   driven by per-material draw command state.
/// @param tex  Pixel view to sample.
/// @param u    Horizontal texture coordinate (unnormalized; repeat wraps to [0,1)).
/// @param v    Vertical texture coordinate.
/// @param r    Output red channel in [0, 1].
/// @param g    Output green channel in [0, 1].
/// @param b    Output blue channel in [0, 1].
/// @param a    Output alpha channel in [0, 1].
static void sample_texture(
    const sw_pixels_view *tex, float u, float v, float *r, float *g, float *b, float *a) {
    sample_texture_ex(tex,
                      u,
                      v,
                      RT_MATERIAL3D_TEXTURE_WRAP_REPEAT,
                      RT_MATERIAL3D_TEXTURE_WRAP_REPEAT,
                      RT_MATERIAL3D_TEXTURE_FILTER_LINEAR,
                      r,
                      g,
                      b,
                      a);
}

/// @brief Convert one channel from sRGB-encoded to linear space (IEC 61966-2-1 curve).
/// @details Standard two-segment sRGB EOTF: linear for dark values, `((v+0.055)/1.055)^2.4`
///   otherwise. Used by `sample_texture_srgb` so textures authored in sRGB are lit
///   correctly — lighting math is linear, so we have to undo the storage gamma first.
///   Alpha is intentionally *not* converted, matching the sRGB spec.
/// @param value Normalized sRGB-encoded channel.
/// @return Corresponding normalized linear-light channel with out-of-range input clamped.
static float sw_srgb_to_linear(float value) {
    if (!(value > 0.0f))
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    if (value <= 0.04045f)
        return value / 12.92f;
    return powf((value + 0.055f) / 1.055f, 2.4f);
}

/// @brief Return the primary texture S-axis wrap mode from @p cmd, defaulting to REPEAT.
/// @param cmd Optional borrowed draw command.
/// @return Command S addressing mode, or repeat when @p cmd is null.
static int32_t sw_cmd_wrap_s(const vgfx3d_draw_cmd_t *cmd) {
    return cmd ? cmd->texture_wrap_s : RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Return the primary texture T-axis wrap mode from @p cmd, defaulting to REPEAT.
/// @param cmd Optional borrowed draw command.
/// @return Command T addressing mode, or repeat when @p cmd is null.
static int32_t sw_cmd_wrap_t(const vgfx3d_draw_cmd_t *cmd) {
    return cmd ? cmd->texture_wrap_t : RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Return the primary texture filter mode from @p cmd, defaulting to LINEAR.
/// @param cmd Optional borrowed draw command.
/// @return Command texture filter, or linear when @p cmd is null.
static int32_t sw_cmd_filter(const vgfx3d_draw_cmd_t *cmd) {
    return cmd ? cmd->texture_filter : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

typedef struct screen_vert_t {
    float sx, sy, sz;
    float r, g, b, a;
    float u_over_w, v_over_w, inv_w;
    float u1_over_w, v1_over_w;
    float wx, wy, wz;     /* world position (for fog distance computation) */
    float nx, ny, nz;     /* world normal (for per-pixel lighting with normal maps) */
    float tx, ty, tz, tw; /* world tangent plus handedness sign (for TBN construction) */
} screen_vert_t;

/// @brief Return the S-axis wrap mode for a specific texture slot, falling back to the primary wrap
/// when out of range.
/// @param cmd Optional borrowed draw command.
/// @param slot Material texture-slot index.
/// @return Slot-specific S addressing mode, or the primary/default mode for an invalid slot.
static int32_t sw_cmd_slot_wrap_s(const vgfx3d_draw_cmd_t *cmd, int32_t slot) {
    if (!cmd || slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return sw_cmd_wrap_s(cmd);
    return cmd->texture_slot_wrap_s[slot];
}

/// @brief Return the T-axis wrap mode for a specific texture slot, falling back to the primary wrap
/// when out of range.
/// @param cmd Optional borrowed draw command.
/// @param slot Material texture-slot index.
/// @return Slot-specific T addressing mode, or the primary/default mode for an invalid slot.
static int32_t sw_cmd_slot_wrap_t(const vgfx3d_draw_cmd_t *cmd, int32_t slot) {
    if (!cmd || slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return sw_cmd_wrap_t(cmd);
    return cmd->texture_slot_wrap_t[slot];
}

/// @brief Return the filter mode for a specific texture slot, falling back to the primary filter
/// when out of range.
/// @param cmd Optional borrowed draw command.
/// @param slot Material texture-slot index.
/// @return Slot-specific filter mode, or the primary/default mode for an invalid slot.
static int32_t sw_cmd_slot_filter(const vgfx3d_draw_cmd_t *cmd, int32_t slot) {
    if (!cmd || slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return sw_cmd_filter(cmd);
    return cmd->texture_slot_filter[slot];
}

/// @brief Perspective-correct UV interpolation with per-slot UV-set selection and transform.
/// @details Performs perspective-correct barycentric UV interpolation by dividing the
///   interpolated `u_over_w` / `v_over_w` quantities by the interpolated `inv_w`. This
///   avoids the affine texture distortion visible on large triangles when using plain
///   linear barycentric interpolation. The denominator guard (`fabsf(iw) <= 1e-7f`) skips
///   degenerate near-zero-W fragments.
///
///   After interpolation, the UV is optionally transformed by the per-slot 2×3 affine
///   matrix stored in `cmd->texture_slot_uv_transform[slot]` as `[a,b,c,d,tx,ty]`,
///   where `[0..3]` are the 2x2 linear row-major affine part and `[4..5]` are
///   translation. This allows per-slot UV scroll, scale, and rotation without
///   modifying geometry.
///
///   UV-set selection: when `cmd->texture_slot_uv_set[slot] > 0` the secondary UV channel
///   (`u1_over_w` / `v1_over_w`) is used instead of the primary, matching the glTF
///   `TEXCOORD_1` convention for lightmap or detail textures.
/// @param cmd     Draw command containing per-slot UV-set and transform state.
/// @param slot    Material texture slot index; negative or out-of-range disables per-slot
/// overrides.
/// @param b0 Barycentric weight for @p v0.
/// @param b1 Barycentric weight for @p v1.
/// @param b2 Barycentric weight for @p v2.
/// @param v0 Borrowed first screen-space vertex.
/// @param v1 Borrowed second screen-space vertex.
/// @param v2 Borrowed third screen-space vertex.
/// @param out_u   Receives the perspective-corrected, transformed U coordinate.
/// @param out_v   Receives the perspective-corrected, transformed V coordinate.
static void sw_interpolate_uv_for_slot(const vgfx3d_draw_cmd_t *cmd,
                                       int32_t slot,
                                       float b0,
                                       float b1,
                                       float b2,
                                       const screen_vert_t *v0,
                                       const screen_vert_t *v1,
                                       const screen_vert_t *v2,
                                       float *out_u,
                                       float *out_v) {
    float iw;
    float u;
    float v;
    const float *m;
    int use_uv1 = cmd && slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT &&
                  cmd->texture_slot_uv_set[slot] > 0;
    if (!out_u || !out_v)
        return;
    *out_u = 0.0f;
    *out_v = 0.0f;
    if (!v0 || !v1 || !v2)
        return;
    iw = b0 * v0->inv_w + b1 * v1->inv_w + b2 * v2->inv_w;
    if (!isfinite(iw) || fabsf(iw) <= 1e-7f)
        return;
    if (use_uv1) {
        u = (b0 * v0->u1_over_w + b1 * v1->u1_over_w + b2 * v2->u1_over_w) / iw;
        v = (b0 * v0->v1_over_w + b1 * v1->v1_over_w + b2 * v2->v1_over_w) / iw;
    } else {
        u = (b0 * v0->u_over_w + b1 * v1->u_over_w + b2 * v2->u_over_w) / iw;
        v = (b0 * v0->v_over_w + b1 * v1->v_over_w + b2 * v2->v_over_w) / iw;
    }
    if (cmd && slot >= 0 && slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT) {
        m = cmd->texture_slot_uv_transform[slot];
        *out_u = u * m[0] + v * m[1] + m[4];
        *out_v = u * m[2] + v * m[3] + m[5];
    } else {
        *out_u = u;
        *out_v = v;
    }
    if (!isfinite(*out_u) || !isfinite(*out_v))
        *out_u = *out_v = 0.0f;
}

/// @brief Sample a texture using the wrap and filter parameters from one material slot.
/// @details Routes to `sample_texture_ex` using per-slot wrap/filter accessors
///   (`sw_cmd_slot_wrap_s`, `sw_cmd_slot_wrap_t`, `sw_cmd_slot_filter`) that
///   gracefully fall back to the command's global wrap/filter when @p slot is
///   out of range or @p cmd is NULL. This ensures consistent sampling state
///   regardless of whether the draw command has per-slot overrides.
///   Does NOT apply sRGB linearisation — see `sample_texture_slot_srgb_ex` for that.
/// @param tex   Pixel view to sample.
/// @param cmd   Draw command carrying per-slot wrap/filter state; may be NULL.
/// @param slot  Material texture slot index; out-of-range falls back to global fields.
/// @param u     Texture U coordinate (will be wrapped per the slot's wrap_s mode).
/// @param v     Texture V coordinate (will be wrapped per the slot's wrap_t mode).
/// @param r     Output red channel in [0, 1].
/// @param g     Output green channel in [0, 1].
/// @param b     Output blue channel in [0, 1].
/// @param a     Output alpha channel in [0, 1].
static void sample_texture_slot_ex(const sw_pixels_view *tex,
                                   const vgfx3d_draw_cmd_t *cmd,
                                   int32_t slot,
                                   float u,
                                   float v,
                                   float *r,
                                   float *g,
                                   float *b,
                                   float *a) {
    sample_texture_ex(tex,
                      u,
                      v,
                      sw_cmd_slot_wrap_s(cmd, slot),
                      sw_cmd_slot_wrap_t(cmd, slot),
                      sw_cmd_slot_filter(cmd, slot),
                      r,
                      g,
                      b,
                      a);
}

/// @brief Sample a texture and linearise the RGB channels from sRGB storage.
/// @details Calls `sample_texture_slot_ex` to obtain raw [0,1] RGBA values, then
///   applies `sw_srgb_to_linear` to each of the three colour channels. Alpha is
///   intentionally left in its stored form (sRGB spec does not gamma-encode alpha).
///   Used for base-colour / albedo textures whose pixel data was authored in sRGB
///   space; lighting and PBR math must operate in linear light, so we undo the
///   storage gamma here before those computations.
/// @param tex   Pixel view to sample.
/// @param cmd   Draw command carrying per-slot wrap/filter state; may be NULL.
/// @param slot  Material texture slot index.
/// @param u     Texture U coordinate.
/// @param v     Texture V coordinate.
/// @param r     Output linearised red channel in [0, 1].
/// @param g     Output linearised green channel in [0, 1].
/// @param b     Output linearised blue channel in [0, 1].
/// @param a     Output alpha channel in [0, 1] (not linearised).
static void sample_texture_slot_srgb_ex(const sw_pixels_view *tex,
                                        const vgfx3d_draw_cmd_t *cmd,
                                        int32_t slot,
                                        float u,
                                        float v,
                                        float *r,
                                        float *g,
                                        float *b,
                                        float *a) {
    sample_texture_slot_ex(tex, cmd, slot, u, v, r, g, b, a);
    *r = sw_srgb_to_linear(*r);
    *g = sw_srgb_to_linear(*g);
    *b = sw_srgb_to_linear(*b);
}
