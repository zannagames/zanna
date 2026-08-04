//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_texture_stream.inc
// Purpose: Automatic TextureAsset3D mip-residency streaming (#included from
//   rt_canvas3d.c; not a standalone translation unit). Estimates each draw's
//   on-screen texel demand from its local bounds and the frame camera,
//   aggregates a per-asset minimum desired mip per frame, and moves each
//   asset's resident mip window through rt_textureasset3d_set_resident_mip_range.
// Key invariants:
//   - Windows change only at an asset's first draw-time touch of a frame, so
//     every draw command snapshot within one frame sees one consistent window.
//   - Promotions (more resolution) apply immediately; demotions require
//     CANVAS3D_TEXTURE_STREAM_DEMOTE_FRAMES consecutive decision frames and
//     drop one mip level at a time.
//   - The tracking table stores identities and counters only, never object
//     references, so it cannot extend TextureAsset3D lifetimes.
// Ownership/Lifetime:
//   - texture_stream_entries is owned by the canvas and freed in its finalizer.
// Links: src/runtime/graphics/3d/assets/rt_textureasset3d.h,
//   src/runtime/graphics/3d/render/rt_canvas3d_deferred.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements draw-driven TextureAsset3D mip-residency streaming for Canvas3D.
/// @details Per-frame projected-size demand is aggregated by stable asset identity,
///   then applied on the next frame's first material touch with immediate promotion
///   and deliberately hysteretic one-level demotion.

/* Consecutive decision frames a lower-resolution demand must hold before one
 * mip level is dropped (~0.5 s at 60 FPS). Promotions are immediate. */
#define CANVAS3D_TEXTURE_STREAM_DEMOTE_FRAMES 30
/* Textures whose largest dimension is below this never stream — the memory
 * savings cannot justify re-upload churn. */
#define CANVAS3D_TEXTURE_STREAM_MIN_DIM 128
/* Entries untouched for this many frames are recycled during table growth. */
#define CANVAS3D_TEXTURE_STREAM_STALE_FRAMES 600

/// @brief Per-asset streaming state, keyed by the asset's stable cache identity.
typedef struct {
    uint64_t identity;         /* 0 = empty slot */
    int64_t last_touch_serial; /* frame_serial of the last first-touch decision */
    int64_t demand_serial;     /* frame_serial the accumulating demand belongs to */
    int32_t frame_min_desired; /* min desired mip seen during demand_serial */
    int32_t demote_streak;     /* consecutive decision frames wanting less detail */
} canvas3d_texture_stream_entry_t;

/// @brief Return whether two nonnegative frame stamps are separated by more than a bound.
/// @details Ordering is checked before unsigned subtraction, avoiding signed addition/subtraction
///   overflow near the frame-serial limit. Invalid or future prior stamps are treated as stale.
/// @param current Current positive canvas frame serial.
/// @param previous Earlier entry stamp.
/// @param maximum_gap Largest consecutive gap that remains live.
/// @return Nonzero when the entry is invalid, from the future, or older than @p maximum_gap.
static int canvas3d_texture_stream_gap_exceeds(int64_t current,
                                               int64_t previous,
                                               uint64_t maximum_gap) {
    if (current <= 0 || previous < 0 || previous > current)
        return 1;
    return (uint64_t)current - (uint64_t)previous > maximum_gap;
}

/// @brief Find the table slot for @p identity, or the empty slot to insert into.
/// @param entries Open-addressed entry array.
/// @param capacity Positive power-of-two number of slots in @p entries.
/// @param identity Nonzero stable asset identity to find.
/// @return Slot pointer, or NULL when the table has no capacity.
static canvas3d_texture_stream_entry_t *canvas3d_texture_stream_slot(
    canvas3d_texture_stream_entry_t *entries, int32_t capacity, uint64_t identity) {
    uint64_t hash;
    if (!entries || capacity <= 0 || identity == 0)
        return NULL;
    hash = identity * 0x9e3779b97f4a7c15ull;
    for (int32_t probe = 0; probe < capacity; probe++) {
        int32_t index = (int32_t)((hash + (uint64_t)probe) & (uint64_t)(capacity - 1));
        canvas3d_texture_stream_entry_t *entry = &entries[index];
        if (entry->identity == identity || entry->identity == 0)
            return entry;
    }
    return NULL;
}

/// @brief Grow the streaming table, dropping entries stale for many frames.
/// @details Rehashes live identities into a zeroed power-of-two table, retaining
///   only entries touched or demanded within the configured stale-frame window.
///   The old table remains owned until replacement allocation succeeds.
/// @param c Valid canvas owning the table, live count, and current frame serial.
/// @return 1 on success, 0 when allocation fails (streaming quietly degrades).
static int canvas3d_texture_stream_grow(rt_canvas3d *c) {
    canvas3d_texture_stream_entry_t *old_entries =
        (canvas3d_texture_stream_entry_t *)c->texture_stream_entries;
    int32_t old_capacity = c->texture_stream_capacity;
    int32_t new_capacity = old_capacity < 64 ? 64 : old_capacity * 2;
    canvas3d_texture_stream_entry_t *new_entries;
    int32_t live = 0;

    if (old_capacity > INT32_MAX / 2)
        return 0;
    new_entries =
        (canvas3d_texture_stream_entry_t *)calloc((size_t)new_capacity, sizeof(*new_entries));
    if (!new_entries)
        return 0;
    for (int32_t i = 0; i < old_capacity; i++) {
        const canvas3d_texture_stream_entry_t *entry = &old_entries[i];
        canvas3d_texture_stream_entry_t *slot;
        int64_t last_seen;
        if (entry->identity == 0)
            continue;
        last_seen = entry->last_touch_serial > entry->demand_serial ? entry->last_touch_serial
                                                                    : entry->demand_serial;
        if (canvas3d_texture_stream_gap_exceeds(
                c->frame_serial, last_seen, CANVAS3D_TEXTURE_STREAM_STALE_FRAMES))
            continue;
        slot = canvas3d_texture_stream_slot(new_entries, new_capacity, entry->identity);
        if (slot) {
            *slot = *entry;
            live++;
        }
    }
    free(old_entries);
    c->texture_stream_entries = new_entries;
    c->texture_stream_capacity = new_capacity;
    c->texture_stream_live = live;
    return 1;
}

/// @brief Look up (creating on demand) the streaming entry for @p identity.
/// @details Creation grows at a three-quarter load threshold, initializes
///   demand state, and increments the canvas live-entry count.
/// @param c Canvas owning the streaming table.
/// @param identity Nonzero stable texture-asset cache identity.
/// @param create Nonzero to insert a missing entry; zero for lookup only.
/// @return Entry pointer, or NULL when @p identity is 0 or allocation fails.
static canvas3d_texture_stream_entry_t *canvas3d_texture_stream_entry(rt_canvas3d *c,
                                                                      uint64_t identity,
                                                                      int create) {
    canvas3d_texture_stream_entry_t *slot;

    if (!c || identity == 0)
        return NULL;
    slot =
        canvas3d_texture_stream_slot((canvas3d_texture_stream_entry_t *)c->texture_stream_entries,
                                     c->texture_stream_capacity,
                                     identity);
    if (slot && slot->identity == identity)
        return slot;
    if (!create)
        return NULL;
    /* Grow at 3/4 load (or when full/absent) before inserting. */
    if (!slot || (c->texture_stream_live + 1) * 4 > c->texture_stream_capacity * 3) {
        if (!canvas3d_texture_stream_grow(c))
            return NULL;
        slot = canvas3d_texture_stream_slot(
            (canvas3d_texture_stream_entry_t *)c->texture_stream_entries,
            c->texture_stream_capacity,
            identity);
        if (!slot)
            return NULL;
    }
    slot->identity = identity;
    slot->last_touch_serial = 0;
    slot->demand_serial = 0;
    slot->frame_min_desired = INT32_MAX;
    slot->demote_streak = 0;
    c->texture_stream_live++;
    return slot;
}

/// @brief Estimate a draw's projected bounding-sphere diameter in output pixels.
/// @details Mirrors the Scene3D auto-LOD estimate but works from the draw
///          command's own model matrix and local AABB, so it holds for direct
///          Canvas3D draws. The projection scale is recovered from the cached
///          VP matrix's second row: the view rotation is orthonormal, so the
///          row's 3-vector norm equals the projection's [1][1] term for both
///          perspective and orthographic cameras, independent of any
///          camera-relative rebasing applied to model matrices.
/// @param c Canvas supplying cached camera, projection, and output dimensions.
/// @param model_matrix Sixteen-element row-major local-to-world matrix.
/// @param bounds_min Three-element local-space minimum AABB corner.
/// @param bounds_max Three-element local-space maximum AABB corner.
/// @return Projected diameter in pixels, or DBL_MAX when it cannot be
///         estimated (callers then keep full resolution — the safe default).
static double canvas3d_texture_stream_projected_px(const rt_canvas3d *c,
                                                   const float *model_matrix,
                                                   const float *bounds_min,
                                                   const float *bounds_max) {
    double cx, cy, cz, ex, ey, ez, radius_local;
    double wx, wy, wz;
    double sx, sy, sz, scale, radius;
    double proj_m11, output_h, projected;

    if (!c || !model_matrix || !bounds_min || !bounds_max)
        return DBL_MAX;
    if (!canvas3d_bounds_are_valid(bounds_min, bounds_max))
        return DBL_MAX;
    cx = ((double)bounds_min[0] + (double)bounds_max[0]) * 0.5;
    cy = ((double)bounds_min[1] + (double)bounds_max[1]) * 0.5;
    cz = ((double)bounds_min[2] + (double)bounds_max[2]) * 0.5;
    ex = ((double)bounds_max[0] - (double)bounds_min[0]) * 0.5;
    ey = ((double)bounds_max[1] - (double)bounds_min[1]) * 0.5;
    ez = ((double)bounds_max[2] - (double)bounds_min[2]) * 0.5;
    radius_local = sqrt(ex * ex + ey * ey + ez * ez);
    if (!isfinite(radius_local) || radius_local <= 0.0)
        return DBL_MAX;

    /* Row-major p' = M*p: world center and the largest basis-column scale. */
    wx = (double)model_matrix[0] * cx + (double)model_matrix[1] * cy +
         (double)model_matrix[2] * cz + (double)model_matrix[3];
    wy = (double)model_matrix[4] * cx + (double)model_matrix[5] * cy +
         (double)model_matrix[6] * cz + (double)model_matrix[7];
    wz = (double)model_matrix[8] * cx + (double)model_matrix[9] * cy +
         (double)model_matrix[10] * cz + (double)model_matrix[11];
    sx = sqrt((double)model_matrix[0] * (double)model_matrix[0] +
              (double)model_matrix[4] * (double)model_matrix[4] +
              (double)model_matrix[8] * (double)model_matrix[8]);
    sy = sqrt((double)model_matrix[1] * (double)model_matrix[1] +
              (double)model_matrix[5] * (double)model_matrix[5] +
              (double)model_matrix[9] * (double)model_matrix[9]);
    sz = sqrt((double)model_matrix[2] * (double)model_matrix[2] +
              (double)model_matrix[6] * (double)model_matrix[6] +
              (double)model_matrix[10] * (double)model_matrix[10]);
    scale = sx > sy ? sx : sy;
    if (sz > scale)
        scale = sz;
    if (!isfinite(scale) || scale <= 0.0)
        scale = 1.0;
    radius = radius_local * scale;

    proj_m11 = sqrt((double)c->cached_vp[4] * (double)c->cached_vp[4] +
                    (double)c->cached_vp[5] * (double)c->cached_vp[5] +
                    (double)c->cached_vp[6] * (double)c->cached_vp[6]);
    if (!isfinite(proj_m11) || proj_m11 <= 0.0)
        return DBL_MAX;
    output_h = c->render_target ? (double)c->render_target->height : (double)c->height;
    if (!(output_h > 0.0))
        output_h = 720.0;

    if (c->cached_cam_is_ortho) {
        projected = 2.0 * radius * 0.5 * output_h * proj_m11;
    } else {
        double dx = wx - (double)c->cached_render_cam_pos[0];
        double dy = wy - (double)c->cached_render_cam_pos[1];
        double dz = wz - (double)c->cached_render_cam_pos[2];
        double distance = sqrt(dx * dx + dy * dy + dz * dz);
        if (!isfinite(distance) || distance < 1e-4)
            distance = 1e-4;
        projected = 2.0 * radius * 0.5 * output_h * proj_m11 / distance;
    }
    return isfinite(projected) ? projected : DBL_MAX;
}

/// @brief Translate a projected size into the desired first resident mip.
/// @details desired = floor(log2(texture max dimension / projected pixels))
///          plus the canvas bias, clamped to the asset's mip chain. Assets
///          below the streaming minimum dimension or without a mip chain
///          always want mip 0.
/// @param c Canvas supplying the finite-or-fallback streaming bias.
/// @param asset Borrowed TextureAsset3D native handle queried for dimensions and mip count.
/// @param projected_px Estimated on-screen diameter in pixels; nonfinite values
///   conservatively request full resolution.
/// @return First desired resident mip, clamped from zero through the final mip.
static int32_t canvas3d_texture_stream_desired_mip(const rt_canvas3d *c,
                                                   void *asset,
                                                   double projected_px) {
    int64_t width = rt_textureasset3d_get_width(asset);
    int64_t height = rt_textureasset3d_get_height(asset);
    int64_t mip_count = rt_textureasset3d_get_mip_count(asset);
    double max_dim = (double)(width > height ? width : height);
    double desired;

    if (mip_count <= 1 || max_dim < (double)CANVAS3D_TEXTURE_STREAM_MIN_DIM)
        return 0;
    if (!isfinite(projected_px))
        return 0;
    if (projected_px < 1.0)
        projected_px = 1.0;
    if (max_dim <= projected_px)
        desired = 0.0;
    else
        desired = floor(log2(max_dim / projected_px));
    desired += isfinite(c->texture_stream_bias) ? c->texture_stream_bias : 0.0;
    if (desired <= 0.0)
        return 0;
    if (desired >= (double)(mip_count - 1))
        return (int32_t)(mip_count - 1);
    return (int32_t)desired;
}

/// @brief First-touch-per-frame decision: apply the last frame's aggregated demand.
/// @details Each asset is decided at most once per canvas frame. Higher-resolution
///   demand promotes immediately; lower-resolution demand advances a consecutive
///   streak and demotes by one level only after the threshold. Draw gaps reset the
///   streak, and the accumulator is prepared for the current frame after decision.
/// @param c Canvas containing streaming history and the current frame serial.
/// @param asset Borrowed TextureAsset3D handle; `NULL` is ignored.
/// @return 1 when the asset's resident window changed, 0 otherwise.
static int canvas3d_texture_stream_touch(rt_canvas3d *c, void *asset) {
    canvas3d_texture_stream_entry_t *entry;
    uint64_t identity;
    int64_t previous_touch;
    int64_t mip_count;
    int64_t current_start;
    int32_t target;

    if (!asset)
        return 0;
    identity = rt_textureasset3d_get_cache_identity(asset);
    /* Only assets that ever recorded demand are tracked; everything else keeps
     * its window untouched (loads default to full residency). */
    entry = canvas3d_texture_stream_entry(c, identity, 0);
    if (!entry || entry->last_touch_serial == c->frame_serial)
        return 0;
    previous_touch = entry->last_touch_serial;
    entry->last_touch_serial = c->frame_serial;
    /* A draw gap makes the accumulated demand stale — restart demotion pacing. */
    if (canvas3d_texture_stream_gap_exceeds(c->frame_serial, previous_touch, UINT64_C(2)))
        entry->demote_streak = 0;

    target = entry->frame_min_desired == INT32_MAX ? 0 : entry->frame_min_desired;
    /* Reset the accumulator for the frame that is now beginning for this asset. */
    entry->demand_serial = c->frame_serial;
    entry->frame_min_desired = INT32_MAX;

    mip_count = rt_textureasset3d_get_mip_count(asset);
    if (mip_count <= 1)
        return 0;
    if (target > (int32_t)(mip_count - 1))
        target = (int32_t)(mip_count - 1);
    current_start = rt_textureasset3d_get_resident_mip_start(asset);

    if ((int64_t)target < current_start) {
        /* Promotion: restore resolution immediately; the budgeted upload path
         * spreads the re-upload across frames. */
        rt_textureasset3d_set_resident_mip_range(asset, target, mip_count - target);
        entry->demote_streak = 0;
        return 1;
    }
    if ((int64_t)target > current_start) {
        entry->demote_streak++;
        if (entry->demote_streak >= CANVAS3D_TEXTURE_STREAM_DEMOTE_FRAMES) {
            rt_textureasset3d_set_resident_mip_range(
                asset, current_start + 1, mip_count - (current_start + 1));
            entry->demote_streak = 0;
            c->texture_stream_demotions++;
            return 1;
        }
        return 0;
    }
    entry->demote_streak = 0;
    return 0;
}

/// @brief Record one draw's desired mip for @p asset in this frame's aggregate.
/// @details Keeps the smallest desired mip observed for the asset because that
///   represents the highest detail required by any draw. Already-full-resident
///   assets with full-resolution demand do not create unnecessary table entries.
/// @param c Canvas owning the per-identity demand table.
/// @param asset Borrowed TextureAsset3D handle; `NULL` is ignored.
/// @param projected_px Draw coverage estimate in pixels, or a conservative
///   full-resolution sentinel.
static void canvas3d_texture_stream_demand(rt_canvas3d *c, void *asset, double projected_px) {
    canvas3d_texture_stream_entry_t *entry;
    uint64_t identity;
    int32_t desired;

    if (!asset)
        return;
    desired = canvas3d_texture_stream_desired_mip(c, asset, projected_px);
    if (desired <= 0 && rt_textureasset3d_get_resident_mip_start(asset) <= 0) {
        /* Full resolution demanded and already resident: only track assets we
         * may still need to promote, keeping the table to streamable state. */
        identity = rt_textureasset3d_get_cache_identity(asset);
        entry = canvas3d_texture_stream_entry(c, identity, 0);
        if (!entry)
            return;
    } else {
        identity = rt_textureasset3d_get_cache_identity(asset);
        entry = canvas3d_texture_stream_entry(c, identity, 1);
        if (!entry)
            return;
    }
    if (entry->demand_serial != c->frame_serial) {
        entry->demand_serial = c->frame_serial;
        entry->frame_min_desired = desired;
    } else if (desired < entry->frame_min_desired) {
        entry->frame_min_desired = desired;
    }
}

/// @brief Apply pending streaming decisions for every TextureAsset3D a material binds.
/// @details Must run before canvas3d_fill_material_cmd so the draw command's
///          residency snapshot captures any window applied this frame.
/// @param c Canvas whose streaming mode and frame state are inspected.
/// @param mat Borrowed material whose six supported texture slots are resolved;
///   null, disabled, and 2D-frame calls are ignored.
static void canvas3d_texture_stream_touch_material(rt_canvas3d *c, const rt_material3d *mat) {
    if (!c || !c->texture_stream_enabled || c->frame_is_2d || !mat)
        return;
    canvas3d_texture_stream_touch(c, rt_material3d_resolve_texture_native_asset(mat->texture));
    canvas3d_texture_stream_touch(c, rt_material3d_resolve_texture_native_asset(mat->normal_map));
    canvas3d_texture_stream_touch(c, rt_material3d_resolve_texture_native_asset(mat->specular_map));
    canvas3d_texture_stream_touch(c, rt_material3d_resolve_texture_native_asset(mat->emissive_map));
    canvas3d_texture_stream_touch(
        c, rt_material3d_resolve_texture_native_asset(mat->metallic_roughness_map));
    canvas3d_texture_stream_touch(c, rt_material3d_resolve_texture_native_asset(mat->ao_map));
}

/// @brief Record the frame's texel demand for every asset a filled draw command binds.
/// @param c Canvas owning demand aggregation and cached camera state.
/// @param cmd Borrowed filled draw command containing model state and native assets.
/// @param bounds_min Three-element local-space minimum bound; invalid bounds
///   trigger the conservative full-resolution estimate.
/// @param bounds_max Three-element local-space maximum bound; invalid bounds
///   trigger the conservative full-resolution estimate.
static void canvas3d_texture_stream_record_cmd(rt_canvas3d *c,
                                               const vgfx3d_draw_cmd_t *cmd,
                                               const float *bounds_min,
                                               const float *bounds_max) {
    double projected_px;
    if (!c || !c->texture_stream_enabled || c->frame_is_2d || !cmd)
        return;
    projected_px =
        canvas3d_texture_stream_projected_px(c, cmd->model_matrix, bounds_min, bounds_max);
    canvas3d_texture_stream_demand(c, cmd->texture_asset, projected_px);
    canvas3d_texture_stream_demand(c, cmd->normal_map_asset, projected_px);
    canvas3d_texture_stream_demand(c, cmd->specular_map_asset, projected_px);
    canvas3d_texture_stream_demand(c, cmd->emissive_map_asset, projected_px);
    canvas3d_texture_stream_demand(c, cmd->metallic_roughness_map_asset, projected_px);
    canvas3d_texture_stream_demand(c, cmd->ao_map_asset, projected_px);
}

/// @brief Record full-resolution demand for a draw whose per-instance extents are unknown.
/// @details Instanced batches share one command but span many transforms, so
///          streaming keeps their textures at full resolution rather than
///          risking a visibly soft near instance.
/// @param c Canvas owning demand aggregation.
/// @param cmd Borrowed filled instanced draw command and its native asset slots.
static void canvas3d_texture_stream_record_cmd_full(rt_canvas3d *c, const vgfx3d_draw_cmd_t *cmd) {
    if (!c || !c->texture_stream_enabled || c->frame_is_2d || !cmd)
        return;
    canvas3d_texture_stream_demand(c, cmd->texture_asset, DBL_MAX);
    canvas3d_texture_stream_demand(c, cmd->normal_map_asset, DBL_MAX);
    canvas3d_texture_stream_demand(c, cmd->specular_map_asset, DBL_MAX);
    canvas3d_texture_stream_demand(c, cmd->emissive_map_asset, DBL_MAX);
    canvas3d_texture_stream_demand(c, cmd->metallic_roughness_map_asset, DBL_MAX);
    canvas3d_texture_stream_demand(c, cmd->ao_map_asset, DBL_MAX);
}

/// @brief Enable or disable automatic texture mip-residency streaming.
/// @details While enabled, Canvas3D manages the resident mip window of every
///          multi-mip TextureAsset3D drawn through it, overriding manual
///          SetResidentMipRange calls at the asset's next draw. Disabling
///          stops further window changes but does not restore prior windows;
///          call SetResidentMipRange to pin a window afterwards.
/// @param obj Canvas3D receiver or supported stack-wrapper handle; invalid
///   receivers are ignored.
/// @param enabled Nonzero to enable automatic decisions, zero to stop them.
void rt_canvas3d_set_texture_streaming(void *obj, int8_t enabled) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    c->texture_stream_enabled = enabled ? 1 : 0;
}

/// @brief `Canvas3D.TextureStreaming` — read the retained streaming flag (ADR 0233).
/// @param obj Canvas3D receiver or supported stack-wrapper handle.
/// @return Nonzero while automatic texture streaming is enabled.
int8_t rt_canvas3d_get_texture_streaming(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c && c->texture_stream_enabled ? 1 : 0;
}

/// @brief Bias automatic streaming's desired mip (positive drops more detail).
/// @details Applied after the screen-coverage estimate and before clamping to
///          the mip chain; fractional values act as a soft threshold shift.
///          Non-finite inputs reset the bias to 0. Clamped to [-16, 16].
/// @param obj Canvas3D receiver or supported stack-wrapper handle; invalid
///   receivers are ignored.
/// @param bias Desired mip offset; positive values favor lower resolution.
void rt_canvas3d_set_texture_streaming_bias(void *obj, double bias) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    if (!c)
        return;
    if (!isfinite(bias))
        bias = 0.0;
    if (bias < -16.0)
        bias = -16.0;
    if (bias > 16.0)
        bias = 16.0;
    c->texture_stream_bias = bias;
}

/// @brief `Canvas3D.TextureStreamingBias` — read the retained mip bias (ADR 0233).
/// @param obj Canvas3D receiver or supported stack-wrapper handle.
/// @return Retained bias in [-16, 16], or zero for an invalid handle.
double rt_canvas3d_get_texture_streaming_bias(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->texture_stream_bias : 0.0;
}

/// @brief Lifetime count of resident-window demotions applied by streaming.
/// @param obj Canvas3D receiver or supported stack-wrapper handle.
/// @return Saturation-unchecked cumulative demotion count, or zero for an invalid receiver.
int64_t rt_canvas3d_get_texture_streaming_demotions(void *obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(obj);
    return c ? c->texture_stream_demotions : 0;
}
