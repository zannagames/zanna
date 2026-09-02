//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader.c
// Purpose: Dependency-free binary/ASCII FBX parser and complete scene asset extractor for
//   geometry, materials, skeletons, cameras, lights, object animation, and morph animation.
//
// Key invariants:
//   - Supports FBX versions 7100-7700 (both 32-bit and 64-bit offsets).
//   - Array properties with zlib encoding: strip 2-byte header + 4-byte
//     Adler-32 trailer, then call rt_compress_inflate on raw DEFLATE.
//   - Negative polygon indices mark end-of-polygon (bitwise NOT to decode).
//   - Coordinate system correction applied if source is Z-up.
//   - Ear-clipping triangulation for quads/n-gons, with fan fallback only for
//     degenerate projected polygons.
//   - Skinning palette is reduced to the top 4 (bone, weight) influences per
//     vertex and renormalized to sum to 1.
//
// Ownership/Lifetime:
//   - rt_fbx_asset is GC-managed; finalizer releases every owned mesh, material, skeletal/node
//     animation, camera, morph target, skeleton, and scene root.
//   - Parser scratch state (node tree, connection table, binding tables,
//     mesh remaps) is freed before returning from rt_fbx_load.
//   - Texture references loaded from disk are released after assignment to
//     the materials that retain them.
//
// Links: rt_fbx_loader.h, rt_fbx_loader_morph.inc, rt_fbx_loader_nodeanim.inc,
//   plans/3d/15-fbx-loader.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fbx_loader.c
 * @brief Assembles the dependency-free FBX parser and exposes imported assets.
 *
 * This translation unit defines the shared limits, numeric sanitizers,
 * allocation-budget accounting, retained asset container, and public accessors
 * used by the binary and ASCII parsing fragments included below. A successful
 * asset owns its meshes, materials, skeleton, animations, cameras, morph
 * targets, and scene root; accessor object pointers are borrowed.
 */
#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_fbx_loader.h"
#include "rt_alloc_size.h"
#include "rt_asset_error.h"
#include "rt_bytes.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_compress.h"
#include "rt_fbx_loader_internal.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_gif.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_morphtarget3d_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_quat.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_trap.h"
#include "rt_untrusted_count.h"
#include "rt_vec3.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Aim a runtime camera using explicit eye, target, and up components.
/// @param[in,out] obj Camera to orient.
/// @param[in] eye_x Eye x coordinate.
/// @param[in] eye_y Eye y coordinate.
/// @param[in] eye_z Eye z coordinate.
/// @param[in] target_x Target x coordinate.
/// @param[in] target_y Target y coordinate.
/// @param[in] target_z Target z coordinate.
/// @param[in] up_x Up-vector x component.
/// @param[in] up_y Up-vector y component.
/// @param[in] up_z Up-vector z component.
extern void rt_camera3d_look_at_components(void *obj,
                                           double eye_x,
                                           double eye_y,
                                           double eye_z,
                                           double target_x,
                                           double target_y,
                                           double target_z,
                                           double up_x,
                                           double up_y,
                                           double up_z);
/// @brief Decode typed image bytes for an externally referenced FBX texture.
/// @param[in] name Source name used for format selection and diagnostics.
/// @param[in] data Encoded bytes.
/// @param[in] size Encoded byte count.
/// @return A new decoded runtime asset, or `NULL` on failure.
extern void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size);

/// Absolute audited file-size ceiling accepted from configuration.
#define RT_FBX_HARD_MAX_FILE_BYTES (1024ull * 1024ull * 1024ull)
/// Default maximum source FBX size.
#define RT_FBX_DEFAULT_MAX_FILE_BYTES (256ull * 1024ull * 1024ull)
/// Default aggregate charged allocation budget for one load.
#define RT_FBX_DEFAULT_LOAD_BUDGET_BYTES (1024ull * 1024ull * 1024ull)
/// Maximum encoded byte length accepted for an external texture path.
#define RT_FBX_MAX_TEXTURE_PATH_BYTES (1024u * 1024u)
/// Maximum external texture file size.
#define RT_FBX_MAX_TEXTURE_FILE_BYTES (256u * 1024u * 1024u)

#if defined(_MSC_VER)
/// Platform spelling for FBX loader thread-local state.
#define RT_FBX_THREAD_LOCAL __declspec(thread)
#else
/// Platform spelling for FBX loader thread-local state.
#define RT_FBX_THREAD_LOCAL _Thread_local
#endif

/// @brief Thread-local original path used when a temp FBX file should resolve external textures
/// beside the source asset rather than beside the temp spill file.
static RT_FBX_THREAD_LOCAL rt_string g_fbx_texture_base_override = NULL;
/// @brief Optional borrowed source bytes for the synchronous internal memory-load bridge.
static RT_FBX_THREAD_LOCAL const uint8_t *g_fbx_source_bytes_override = NULL;
static RT_FBX_THREAD_LOCAL size_t g_fbx_source_size_override = 0;

/// @brief Per-load accounting and diagnostics shared by binary/ASCII FBX parsing and extraction.
/// @details Every retained allocation class named by ADR 0173 is charged before allocation. The
///          context also accumulates hash/adjacency probe telemetry without introducing global
///          mutable parser state. A context lives only until its load either publishes an asset or
///          rolls back.
typedef struct fbx_load_context {
    uint64_t budget_limit;  ///< Maximum aggregate charged bytes for this load.
    uint64_t budget_used;   ///< Saturating aggregate charged bytes.
    uint64_t lookup_probes; ///< Object-id and connection-endpoint hash probes.
    int budget_exhausted;   ///< Nonzero after overflow or a charge beyond the limit.
    /// Nonzero when a hard per-record structural cap (child/property count, nesting depth) was
    /// reached. Distinguishes a well-formed document that outgrew a parser bound from genuinely
    /// malformed bytes, which would otherwise share the same failure path.
    int structural_limit_exceeded;
} fbx_load_context_t;

/// @brief One-shot thread-local budget override used only by deterministic CTests.
static RT_FBX_THREAD_LOCAL uint64_t g_fbx_next_load_budget_bytes = 0;
/// @brief Charged bytes observed for the most recent load on the calling thread.
static RT_FBX_THREAD_LOCAL uint64_t g_fbx_last_budget_used_bytes = 0;
/// @brief Hash/adjacency probes observed for the most recent load on the calling thread.
static RT_FBX_THREAD_LOCAL uint64_t g_fbx_last_lookup_probe_count = 0;

/*==========================================================================
 * FBX asset container
 *=========================================================================*/

/// @brief GC-managed collection of all resources published by one FBX load.
/// @details Each growable pointer array owns its retained elements. Morph
/// targets are indexed in parallel with meshes, while the optional skeleton
/// and scene root occupy dedicated retained slots.
typedef struct {
    /// Reserved runtime dispatch pointer.
    void *vptr;
    /// Owned mesh-reference array.
    void **meshes;
    /// Logical mesh count.
    int32_t mesh_count;
    /// Allocated mesh-reference capacity.
    int32_t mesh_capacity;
    /// Optional retained `Skeleton3D`.
    void *skeleton;
    /// Owned skeletal-animation-reference array.
    void **animations;
    /// Logical skeletal animation count.
    int32_t animation_count;
    /// Allocated skeletal animation capacity.
    int32_t animation_capacity;
    /// Owned object/morph-animation-reference array.
    void **node_animations;
    /// Logical object/morph animation count.
    int32_t node_animation_count;
    /// Allocated object/morph animation capacity.
    int32_t node_animation_capacity;
    /// Owned camera-reference array.
    void **cameras;
    /// Logical camera count.
    int32_t camera_count;
    /// Allocated camera capacity.
    int32_t camera_capacity;
    /// Owned material-reference array.
    void **materials;
    /// Logical material count.
    int32_t material_count;
    /// Allocated material capacity.
    int32_t material_capacity;
    /// Owned morph targets parallel to `meshes`.
    void **morph_targets; // rt_morphtarget3d*[] parallel to meshes[]
    /// Logical parallel morph-target slot count.
    int32_t morph_count;
    /// Allocated morph-target slot capacity.
    int32_t morph_capacity;
    /// Optional retained imported scene root.
    void *scene_root;
} rt_fbx_asset;

/// @brief Per-mesh mapping from generated triangle index to material slot.
typedef struct {
    /// Owned material slot for each generated triangle.
    int32_t *triangle_slots;
    /// Number of generated triangles.
    uint32_t triangle_count;
    /// Number of referenced material slots.
    int32_t slot_count;
    /// Nonzero when explicit slot data was imported.
    int8_t has_slots;
} fbx_mesh_material_map_t;

/// @brief Associate an FBX geometry identifier with its runtime mesh and materials.
typedef struct {
    /// FBX object identifier.
    int64_t id;
    /// Borrowed mesh during extraction.
    void *mesh;
    /// Owned triangle-to-material mapping.
    fbx_mesh_material_map_t material_map;
} fbx_mesh_binding_t;

/// @brief Associate an FBX material identifier with a runtime material.
typedef struct {
    /// FBX object identifier.
    int64_t id;
    /// Borrowed material during extraction.
    void *material;
} fbx_material_binding_t;

/// @brief Growable list of tessellated vertex indices for one control point.
typedef struct {
    /// Owned index array.
    int32_t *vertices;
    /// Logical index count.
    int32_t count;
    /// Allocated index capacity.
    int32_t capacity;
} fbx_vertex_index_list_t;

/// @brief Sparse control-point contribution range for one tessellated surface vertex.
typedef struct {
    /// Offset of the first contributing control point.
    uint32_t offset;
    /// Number of contributing control points.
    uint32_t count;
} fbx_surface_vertex_remap_t;

/// @brief Bidirectional remapping between FBX control points and surface vertices.
typedef struct {
    /// Geometry object identifier.
    int64_t id;
    /// Per-control-point generated vertex lists.
    fbx_vertex_index_list_t *control_vertices;
    /// Number of control-point lists.
    int32_t control_count;
    /// Per-surface-vertex sparse contribution ranges.
    fbx_surface_vertex_remap_t *surface_vertices;
    /// Packed contributing control-point indices.
    int32_t *surface_control_indices;
    /// Packed contribution weights parallel to `surface_control_indices`.
    double *surface_control_weights;
    /// Number of surface remap records.
    uint32_t surface_vertex_count;
    /// Number of packed sparse contributions.
    uint32_t surface_contribution_count;
} fbx_mesh_remap_t;

/// @brief Reduced four-bone influence set for one imported control point.
typedef struct {
    /// Skeleton indices ordered with their retained influences.
    int32_t bone_indices[4];
    /// Corresponding normalized or accumulating weights.
    double weights[4];
} fbx_skin_influence_t;

/// @brief Map one FBX model identifier to its runtime skeleton index.
typedef struct {
    /// FBX model object identifier.
    int64_t model_id;
    /// Zero-based skeleton bone index.
    int32_t bone_index;
} fbx_bone_binding_t;

/// @brief One FBX BlendShapeChannel mapped into a mesh-local MorphTarget3D shape range.
typedef struct {
    /// FBX BlendShapeChannel identifier.
    int64_t channel_id;
    /// Owning geometry identifier.
    int64_t geometry_id;
    /// Target mesh index in the asset.
    int32_t mesh_index;
    /// Number of progressive shapes in the channel.
    int32_t shape_count;
    /// Owned runtime morph-shape indices.
    int32_t *shape_indices;
    /// Owned FBX full-weight thresholds.
    double *full_weights;
    /// Default channel deformation percentage.
    double default_percent;
} fbx_morph_channel_binding_t;

/// General coordinate and transform magnitude clamp.
#define FBX_NUMERIC_ABS_MAX 1000000000000.0
/// Imported texture-coordinate magnitude clamp.
#define FBX_UV_ABS_MAX 1000000.0
/// Imported Euler-angle magnitude clamp in degrees.
#define FBX_ROTATION_DEG_ABS_MAX 1000000.0
/// Per-influence weight magnitude clamp used before normalization.
#define FBX_SKIN_WEIGHT_MAX 1000000.0
/// Maximum absolute converted animation time in seconds.
#define FBX_ANIM_TIME_SECONDS_MAX 100000000.0
/// Maximum key count accepted for one imported curve.
#define FBX_ANIM_CURVE_KEYS_MAX 1000000u
/* Keep importer capacity aligned with Skeleton3D. Individual draw palettes remain capped at
 * VGFX3D_MAX_BONES (256) and use per-mesh remapping; the asset-level hierarchy supports 1024. */
/// FBX importer skeleton limit, aligned with `Skeleton3D`.
#define FBX_MAX_SKELETON_BONES 1024

/// @brief Parse an unsigned decimal byte limit without adding libc conversion dependencies.
/// @details Accepts only ASCII digits, rejects zero, and validates the entire input. Values above
///          @ref RT_FBX_HARD_MAX_FILE_BYTES are reported as that hard cap so administrators can
///          safely provide oversized values without raising the audited maximum.
/// @param[in] text Environment variable text.
/// @param[out] out_limit Receives the parsed and clamped byte limit.
/// @return 1 when @p text is a valid decimal limit, otherwise 0.
static int fbx_parse_file_byte_limit(const char *text, uint64_t *out_limit) {
    if (!text || !*text || !out_limit)
        return 0;

    uint64_t value = 0;
    int clamped = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        uint64_t digit = (uint64_t)(*p - '0');
        if (!clamped) {
            if (value > (RT_FBX_HARD_MAX_FILE_BYTES - digit) / 10u) {
                value = RT_FBX_HARD_MAX_FILE_BYTES;
                clamped = 1;
            } else {
                value = value * 10u + digit;
            }
        }
    }

    if (value == 0)
        return 0;
    *out_limit = value > RT_FBX_HARD_MAX_FILE_BYTES ? RT_FBX_HARD_MAX_FILE_BYTES : value;
    return 1;
}

/// @brief Return the process-configured FBX file-size ceiling in bytes.
/// @details `ZANNA_FBX_MAX_FILE_BYTES` may lower the default ceiling for hosts that process
///          untrusted assets under tighter memory budgets. The hard upper bound remains
///          @c RT_FBX_HARD_MAX_FILE_BYTES so a misconfigured environment cannot raise the
///          loader above its audited allocation limit.
/// @return Maximum readable FBX file size in bytes.
static uint64_t fbx_max_file_bytes(void) {
    const char *env = getenv("ZANNA_FBX_MAX_FILE_BYTES");
    uint64_t parsed = 0;
    if (!env || !*env)
        return RT_FBX_DEFAULT_MAX_FILE_BYTES;
    if (!fbx_parse_file_byte_limit(env, &parsed))
        return RT_FBX_DEFAULT_MAX_FILE_BYTES;
    return parsed;
}

/// @brief Resolve the next load's aggregate memory budget.
/// @details A CTest override is consumed once. Otherwise `ZANNA_FBX_MAX_LOAD_BYTES` may lower, but
///          never raise, the 1 GiB default. Invalid or zero environment text is ignored. Keeping
///          the ceiling independent from the file-size ceiling ensures compressed expansion and
///          parser metadata remain bounded even for a small source file.
/// @return Positive byte ceiling for one FBX load.
static uint64_t fbx_next_load_budget_bytes(void) {
    const char *env;
    uint64_t parsed = 0;
    uint64_t override = g_fbx_next_load_budget_bytes;

    g_fbx_next_load_budget_bytes = 0;
    if (override > 0)
        return override < RT_FBX_DEFAULT_LOAD_BUDGET_BYTES ? override
                                                           : RT_FBX_DEFAULT_LOAD_BUDGET_BYTES;
    env = getenv("ZANNA_FBX_MAX_LOAD_BYTES");
    if (env && *env && fbx_parse_file_byte_limit(env, &parsed) &&
        parsed < RT_FBX_DEFAULT_LOAD_BUDGET_BYTES) {
        return parsed;
    }
    return RT_FBX_DEFAULT_LOAD_BUDGET_BYTES;
}

/// @brief Initialize a per-load budget and reset thread-local test telemetry.
/// @param[out] context Caller-owned load context.
/// @details The aggregate byte limit is selected from a one-shot override,
/// environment configuration, or the production default.
static void fbx_load_context_init(fbx_load_context_t *context) {
    if (!context)
        return;
    memset(context, 0, sizeof(*context));
    context->budget_limit = fbx_next_load_budget_bytes();
    g_fbx_last_budget_used_bytes = 0;
    g_fbx_last_lookup_probe_count = 0;
}

/// @brief Charge a checked element-count allocation to one FBX load before allocating it.
/// @details Both multiplication and addition are overflow checked. Charges are conservative and
///          monotonic: memory released during a failed parse does not restore budget, preventing a
///          malicious file from cycling allocations to bypass the aggregate work/memory ceiling.
/// @param[in,out] context Active load context.
/// @param[in] count Number of elements to charge.
/// @param[in] element_size Bytes per element.
/// @return Nonzero when the charge fits; zero after marking the context exhausted.
static int fbx_budget_charge(fbx_load_context_t *context, uint64_t count, uint64_t element_size) {
    uint64_t bytes;
    if (!context || context->budget_exhausted)
        return 0;
    if (count != 0 && element_size > UINT64_MAX / count) {
        context->budget_exhausted = 1;
        return 0;
    }
    bytes = count * element_size;
    if (bytes > context->budget_limit || context->budget_used > context->budget_limit - bytes) {
        context->budget_exhausted = 1;
        return 0;
    }
    context->budget_used += bytes;
    g_fbx_last_budget_used_bytes = context->budget_used;
    return 1;
}

/// @brief Record one or more load-local lookup probes with saturation.
/// @param[in,out] context Active load context; NULL is ignored.
/// @param[in] count Probe increment.
static void fbx_record_lookup_probes(fbx_load_context_t *context, uint64_t count) {
    if (!context)
        return;
    if (UINT64_MAX - context->lookup_probes < count)
        context->lookup_probes = UINT64_MAX;
    else
        context->lookup_probes += count;
    g_fbx_last_lookup_probe_count = context->lookup_probes;
}

/// @brief Lower the aggregate budget of the next FBX load on this thread for CTest injection.
/// @details Zero clears the override. Values above the production default are clamped so this hook
///          cannot weaken the runtime resource limit.
/// @param[in] bytes Requested one-shot byte budget, or zero to restore normal selection.
void rt_fbx_test_set_load_budget_bytes(uint64_t bytes) {
    g_fbx_next_load_budget_bytes = bytes;
}

/// @brief Return aggregate bytes charged by the most recent FBX load on this thread.
/// @return Most recent monotonic charged-byte total, or zero before charging.
uint64_t rt_fbx_test_get_last_budget_used_bytes(void) {
    return g_fbx_last_budget_used_bytes;
}

/// @brief Return object-id and connection-endpoint hash probes from the most recent FBX load.
/// @return Saturating probe total recorded by the most recent load.
uint64_t rt_fbx_test_get_last_lookup_probe_count(void) {
    return g_fbx_last_lookup_probe_count;
}

/// @brief Release and clear one retained FBX extraction reference.
/// @param[in,out] slot Address of the owned reference slot.
static void fbx_release_ref(void **slot);

/// @brief Return @p value when finite, else @p fallback (scalar sanitizer).
/// @param[in] value Candidate numeric value.
/// @param[in] fallback Replacement for NaN or infinity.
/// @return `value` when finite, otherwise `fallback`.
static double fbx_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp @p value into [lo, hi], substituting @p fallback when non-finite.
/// @param[in] value Candidate numeric value.
/// @param[in] lo Inclusive lower bound.
/// @param[in] hi Inclusive upper bound.
/// @param[in] fallback Replacement applied before clamping when non-finite.
/// @return The sanitized bounded value.
static double fbx_clamp_double(double value, double lo, double hi, double fallback) {
    value = fbx_finite_or(value, fallback);
    if (value < lo)
        value = lo;
    if (value > hi)
        value = hi;
    return value;
}

/// @brief Clamp @p value into [-limit, limit], substituting @p fallback when non-finite.
/// @param[in] value Candidate numeric value.
/// @param[in] fallback Replacement for non-finite input.
/// @param[in] limit Nonnegative absolute magnitude bound.
/// @return The sanitized bounded value.
static double fbx_clamp_abs_or(double value, double fallback, double limit) {
    value = fbx_finite_or(value, fallback);
    if (value > limit)
        value = limit;
    if (value < -limit)
        value = -limit;
    return value;
}

/// @brief Sanitize a scale factor to a finite, bounded value, replacing ~zero magnitudes with 1.0.
/// @param[in] value Candidate scale component.
/// @return A finite value within `FBX_NUMERIC_ABS_MAX`, with magnitudes below
/// `1e-12` replaced by one.
static double fbx_scale_or_unit(double value) {
    value = fbx_clamp_abs_or(value, 1.0, FBX_NUMERIC_ABS_MAX);
    if (fabs(value) < 1e-12)
        value = 1.0;
    return value;
}

/// @brief Clamp a position triple into the FBX numeric bound; returns 0 (leaving the values
///   untouched) when any lane is non-finite.
/// @param[in,out] x Position x component.
/// @param[in,out] y Position y component.
/// @param[in,out] z Position z component.
/// @return One after all three components are clamped, or zero for a null
/// pointer or any non-finite component.
static int fbx_sanitize_position3(double *x, double *y, double *z) {
    if (!x || !y || !z || !isfinite(*x) || !isfinite(*y) || !isfinite(*z))
        return 0;
    *x = fbx_clamp_abs_or(*x, 0.0, FBX_NUMERIC_ABS_MAX);
    *y = fbx_clamp_abs_or(*y, 0.0, FBX_NUMERIC_ABS_MAX);
    *z = fbx_clamp_abs_or(*z, 0.0, FBX_NUMERIC_ABS_MAX);
    return 1;
}

/// @brief Normalize a normal triple in place, falling back to +Y when it is non-finite or
///   of ~zero length.
/// @param[in,out] x Normal x component.
/// @param[in,out] y Normal y component.
/// @param[in,out] z Normal z component.
/// @details If any pointer or value is invalid, every available destination is
/// set to the corresponding component of `(0, 1, 0)`.
static void fbx_sanitize_normal3(double *x, double *y, double *z) {
    double len2;
    double inv_len;
    if (!x || !y || !z || !isfinite(*x) || !isfinite(*y) || !isfinite(*z)) {
        if (x)
            *x = 0.0;
        if (y)
            *y = 1.0;
        if (z)
            *z = 0.0;
        return;
    }
    len2 = (*x) * (*x) + (*y) * (*y) + (*z) * (*z);
    if (!isfinite(len2) || len2 <= 1e-20) {
        *x = 0.0;
        *y = 1.0;
        *z = 0.0;
        return;
    }
    inv_len = 1.0 / sqrt(len2);
    *x *= inv_len;
    *y *= inv_len;
    *z *= inv_len;
}

/// @brief Clamp a rotation angle in degrees to ±FBX_ROTATION_DEG_ABS_MAX (non-finite → 0).
/// @param[in] value Candidate angle in degrees.
/// @return A finite bounded angle.
static double fbx_sanitize_rotation_degrees(double value) {
    return fbx_clamp_abs_or(value, 0.0, FBX_ROTATION_DEG_ABS_MAX);
}

/// @brief Clamp a (count, capacity) pair to a safe element count (0 when invalid, else min).
/// @param[in] items Backing pointer array.
/// @param[in] count Stored logical count.
/// @param[in] capacity Stored allocation capacity.
/// @return Zero for missing storage or nonpositive metadata; otherwise
/// `min(count, capacity)`.
static int32_t fbx_asset_safe_count(void **items, int32_t count, int32_t capacity) {
    if (!items || count <= 0 || capacity <= 0)
        return 0;
    if (count > capacity)
        return capacity;
    return count;
}

/// @brief Ensure a growable reference array holds @p needed slots, doubling capacity and
///   zero-filling the new slots; returns 0 on overflow or allocation failure.
/// @param[in,out] items Address of the owned pointer-array allocation.
/// @param[in,out] capacity Address of its stored capacity.
/// @param[in] needed Required slot count.
/// @return Nonzero when sufficient capacity already exists or allocation
/// succeeds; otherwise zero.
static int fbx_asset_reserve_ref_array(void ***items, int32_t *capacity, int32_t needed) {
    int32_t old_capacity;
    int32_t new_capacity;
    void **grown;
    if (!items || !capacity || needed < 0)
        return 0;
    if (!*items && *capacity > 0)
        *capacity = 0;
    if (*capacity < 0)
        *capacity = 0;
    if (needed <= *capacity)
        return 1;
    old_capacity = *capacity;
    new_capacity = old_capacity > 0 ? old_capacity : 4;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(void *))
        return 0;
    grown = (void **)realloc(*items, (size_t)new_capacity * sizeof(void *));
    if (!grown)
        return 0;
    if (new_capacity > old_capacity)
        memset(grown + old_capacity, 0, (size_t)(new_capacity - old_capacity) * sizeof(*grown));
    *items = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Release every reference in a ref array (over its safe count) and free the backing
///   storage, resetting the count/capacity to zero.
/// @param[in,out] items Address of the owned pointer-array allocation.
/// @param[in,out] count Address of its logical count.
/// @param[in,out] capacity Address of its allocation capacity.
static void fbx_asset_release_ref_array(void ***items, int32_t *count, int32_t *capacity) {
    void **array = items ? *items : NULL;
    int32_t safe_count = fbx_asset_safe_count(array, count ? *count : 0, capacity ? *capacity : 0);
    if (array) {
        for (int32_t i = 0; i < safe_count; i++)
            fbx_release_ref(&array[i]);
        free(array);
    }
    if (items)
        *items = NULL;
    if (count)
        *count = 0;
    if (capacity)
        *capacity = 0;
}

// clang-format off
typedef struct fbx_constraint_pose fbx_constraint_pose_t;
/// @brief Find a constraint pose's static global transform by model identifier.
/// @param[in] pose Constraint-pose lookup data.
/// @param[in] model_id FBX model identifier.
/// @return Borrowed row-major 4-by-4 matrix, or `NULL` when absent.
static const double *fbx_constraint_static_global_for_id(const fbx_constraint_pose_t *pose,
                                                         int64_t model_id);

#include "rt_fbx_loader_parse.inc"
#include "rt_fbx_loader_ascii.inc"
#include "rt_fbx_loader_geometry.inc"
#include "rt_fbx_loader_scene.inc"
#include "rt_fbx_loader_constraints.inc"
#include "rt_fbx_loader_skeleton.inc"
#include "rt_fbx_loader_anim.inc"
#include "rt_fbx_loader_nodeanim.inc"
#include "rt_fbx_loader_loader.inc"

/// @brief Compare a specialized translation/scale compose against the general 4x4 product.
/// @details Builds the same matrix the general path would have multiplied by, runs both, and
///          compares elementwise. Equality is IEEE `==`, so a positive and negative zero in the
///          same slot compare equal: the specialized path skips the zero-valued product terms the
///          general path sums, which can only ever differ in the sign of an exact zero.
/// @param[in] acc Row-major accumulator to compose onto.
/// @param[in] x First component of the translation or scale.
/// @param[in] y Second component.
/// @param[in] z Third component.
/// @param[in] scale Nonzero to test the scale compose, zero for the translation compose.
/// @return Nonzero when both paths agree on all sixteen elements.
int rt_fbx_test_append_matches_general_product(
    const double acc[16], double x, double y, double z, int scale) {
    double specialized[16];
    double general[16];
    double step[16];
    if (!acc)
        return 0;
    memcpy(specialized, acc, sizeof(specialized));
    memcpy(general, acc, sizeof(general));
    if (scale) {
        fbx_mat4_scale_local(x, y, z, step);
        fbx_mat4_append_scale_local(
            specialized, fbx_scale_or_unit(x), fbx_scale_or_unit(y), fbx_scale_or_unit(z));
    } else {
        fbx_mat4_translate_local(x, y, z, step);
        fbx_mat4_append_translation_local(specialized,
                                          fbx_clamp_abs_or(x, 0.0, FBX_NUMERIC_ABS_MAX),
                                          fbx_clamp_abs_or(y, 0.0, FBX_NUMERIC_ABS_MAX),
                                          fbx_clamp_abs_or(z, 0.0, FBX_NUMERIC_ABS_MAX));
    }
    fbx_mat4_append_local(general, step);
    for (int i = 0; i < 16; i++) {
        if (!(specialized[i] == general[i]))
            return 0;
    }
    return 1;
}

// clang-format on

/*==========================================================================
 * FBX asset accessors
 *=========================================================================*/

/// @brief Get the number of meshes extracted from the FBX file.
/// @param[in] obj Loaded FBX asset.
/// @return Its safe readable mesh count, or zero for an invalid object.
int64_t rt_fbx_mesh_count(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? fbx_asset_safe_count(a->meshes, a->mesh_count, a->mesh_capacity) : 0;
}

/// @brief Get a mesh by index from the loaded FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based mesh index.
/// @return The borrowed validated `Mesh3D`, or `NULL` for invalid input, an
/// out-of-range slot, or corrupt stored class.
void *rt_fbx_get_mesh(void *obj, int64_t index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    if (!a)
        return NULL;
    int32_t mesh_count = fbx_asset_safe_count(a->meshes, a->mesh_count, a->mesh_capacity);
    if (index < 0 || index >= mesh_count)
        return NULL;
    return rt_g3d_checked_or_null(a->meshes[index], RT_G3D_MESH3D_CLASS_ID);
}

/// @brief Get the skeleton extracted from the FBX file (NULL if no skeleton).
/// @param[in] obj Loaded FBX asset.
/// @return Its borrowed validated `Skeleton3D`, or `NULL` when absent or
/// invalid.
void *rt_fbx_get_skeleton(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? rt_g3d_checked_or_null(a->skeleton, RT_G3D_SKELETON3D_CLASS_ID) : NULL;
}

/// @brief Get the `SceneNode3D` root of the imported scene graph — the tree of models
/// the FBX author created, with their world transforms and mesh/material bindings.
/// Returned reference is borrowed; the asset owns the lifetime. Distinct from the flat
/// `mesh_count` / `material_count` lists which expose every shared resource the scene
/// uses, regardless of whether it's actually attached to a node.
/// @param[in] obj Loaded FBX asset.
/// @return Its borrowed validated scene root, or `NULL` when absent or invalid.
void *rt_fbx_get_scene_root(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? rt_g3d_checked_or_null(a->scene_root, RT_G3D_SCENENODE3D_CLASS_ID) : NULL;
}

/// @brief Get the number of animation clips in the FBX file.
/// @param[in] obj Loaded FBX asset.
/// @return Its safe readable skeletal-animation count, or zero for invalid input.
int64_t rt_fbx_animation_count(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? fbx_asset_safe_count(a->animations, a->animation_count, a->animation_capacity) : 0;
}

/// @brief Get the number of object/morph animation clips in the FBX file.
/// @param[in] obj Loaded FBX asset.
/// @return Its safe readable node-animation count, or zero for invalid input.
int64_t rt_fbx_node_animation_count(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? fbx_asset_safe_count(
                   a->node_animations, a->node_animation_count, a->node_animation_capacity)
             : 0;
}

/// @brief Get an object/morph animation clip by index from the loaded FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based node-animation index.
/// @return The borrowed validated `NodeAnimation3D`, or `NULL` for invalid or
/// out-of-range input.
void *rt_fbx_get_node_animation(void *obj, int64_t index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    int32_t count;
    if (!a)
        return NULL;
    count = fbx_asset_safe_count(
        a->node_animations, a->node_animation_count, a->node_animation_capacity);
    if (index < 0 || index >= count)
        return NULL;
    return rt_g3d_checked_or_null(a->node_animations[index], RT_G3D_NODEANIMATION3D_CLASS_ID);
}

/// @brief Get the name of an object/morph animation clip by index.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based node-animation index.
/// @return A caller-owned runtime name string, or an empty runtime string for
/// invalid input.
rt_string rt_fbx_get_node_animation_name(void *obj, int64_t index) {
    void *animation = rt_fbx_get_node_animation(obj, index);
    return animation ? rt_node_animation3d_get_name(animation) : rt_const_cstr("");
}

/// @brief Get the number of cameras extracted from the FBX file.
/// @param[in] obj Loaded FBX asset.
/// @return Its safe readable camera count, or zero for invalid input.
int64_t rt_fbx_camera_count(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? fbx_asset_safe_count(a->cameras, a->camera_count, a->camera_capacity) : 0;
}

/// @brief Get a camera by index from the loaded FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based camera index.
/// @return The borrowed validated `Camera3D`, or `NULL` for invalid or
/// out-of-range input.
void *rt_fbx_get_camera(void *obj, int64_t index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    int32_t count;
    if (!a)
        return NULL;
    count = fbx_asset_safe_count(a->cameras, a->camera_count, a->camera_capacity);
    if (index < 0 || index >= count)
        return NULL;
    return rt_g3d_checked_or_null(a->cameras[index], RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Get an animation clip by index from the loaded FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based skeletal-animation index.
/// @return The borrowed validated `Animation3D`, or `NULL` for invalid or
/// out-of-range input.
void *rt_fbx_get_animation(void *obj, int64_t index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    if (!a)
        return NULL;
    int32_t animation_count =
        fbx_asset_safe_count(a->animations, a->animation_count, a->animation_capacity);
    if (index < 0 || index >= animation_count)
        return NULL;
    return rt_g3d_checked_or_null(a->animations[index], RT_G3D_ANIMATION3D_CLASS_ID);
}

/// @brief Get the name of an animation clip by index.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based skeletal-animation index.
/// @return A caller-owned runtime name string, or an empty runtime string for
/// invalid input.
rt_string rt_fbx_get_animation_name(void *obj, int64_t index) {
    void *anim = rt_fbx_get_animation(obj, index);
    if (!anim)
        return rt_const_cstr("");
    return rt_animation3d_get_name(anim);
}

/// @brief Get the number of materials extracted from the FBX file.
/// @param[in] obj Loaded FBX asset.
/// @return Its safe readable material count, or zero for invalid input.
int64_t rt_fbx_material_count(void *obj) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    return a ? fbx_asset_safe_count(a->materials, a->material_count, a->material_capacity) : 0;
}

/// @brief Get a material by index from the loaded FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] index Zero-based material index.
/// @return The borrowed validated `Material3D`, or `NULL` for invalid or
/// out-of-range input.
void *rt_fbx_get_material(void *obj, int64_t index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    if (!a)
        return NULL;
    int32_t material_count =
        fbx_asset_safe_count(a->materials, a->material_count, a->material_capacity);
    if (index < 0 || index >= material_count)
        return NULL;
    return rt_g3d_checked_or_null(a->materials[index], RT_G3D_MATERIAL3D_CLASS_ID);
}

/// @brief Get the morph target data for a mesh by its index in the FBX asset.
/// @param[in] obj Loaded FBX asset.
/// @param[in] mesh_index Zero-based mesh index in the parallel morph table.
/// @return The borrowed validated `MorphTarget3D`, or `NULL` when that mesh has
/// no morph data or the input is invalid.
void *rt_fbx_get_morph_target(void *obj, int64_t mesh_index) {
    rt_fbx_asset *a = (rt_fbx_asset *)rt_g3d_checked_or_null(obj, RT_G3D_FBX_ASSET_CLASS_ID);
    if (!a)
        return NULL;
    int32_t morph_count = fbx_asset_safe_count(a->morph_targets, a->morph_count, a->morph_capacity);
    if (mesh_index < 0 || mesh_index >= morph_count)
        return NULL;
    return rt_g3d_checked_or_null(a->morph_targets[mesh_index], RT_G3D_MORPHTARGET3D_CLASS_ID);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
