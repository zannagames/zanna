//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_vscn_internal.h
// Purpose: Shared limits and value sanitizers for the Scene3D .vscn save/load
//   pair. The serializer (rt_scene3d_vscn_save.c) and loader
//   (rt_scene3d_vscn_load.c, including its material-parse .inc) both clamp
//   floating-point values and validate material enum ids, so those pure
//   helpers live here as static inline functions (one internal-linkage copy
//   per translation unit: no exported symbol, no source duplication).
//
// Key invariants:
//   - Helpers are pure: finite/range clamping, vector normalization, and
//     enum-id validation only.
//   - VSCN_ABS_MAX bounds serialized/parsed coordinate magnitudes.
//
// Ownership/Lifetime:
//   - No state; operates only on caller-provided values.
//
// Links: rt_scene3d_vscn_save.c, rt_scene3d_vscn_load.c, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared VSCN asset views, limits, and numeric/enum sanitizers.
/// @details The serializer and loader include this private header to enforce the
///   same depth, file-size, coordinate, quaternion, direction, and material-mode
///   invariants without exporting helper symbols.

#pragma once

#include "rt_canvas3d.h" // RT_MATERIAL3D_* enum ids used by the validators below

#include <math.h>
#include <stdint.h>

struct rt_scene_node3d;

/// @brief Borrowed immutable-scene input for the model-aware VSCN v5 serializer.
typedef struct rt_vscn_asset_scene_view {
    /// Borrowed scene root.
    struct rt_scene_node3d *root;
    /// Borrowed UTF-8 scene name.
    const char *name;
    /// Borrowed array of Camera3D handles referenced by this scene.
    void *const *cameras;
    /// Number of readable camera handles.
    int32_t camera_count;
} rt_vscn_asset_scene_view;

/// @brief Borrowed complete SceneAsset inventory consumed synchronously by VSCN v5 save.
/// @details Every array remains owned by Model3D. The serializer validates class ids/counts and
/// copies all output before this call returns.
typedef struct rt_vscn_asset_save_view {
    /// Borrowed array of Mesh3D handles.
    void *const *meshes;
    /// Number of readable mesh handles.
    int32_t mesh_count;
    /// Borrowed array of Material3D handles.
    void *const *materials;
    /// Number of readable material handles.
    int32_t material_count;
    /// Borrowed array of Skeleton3D handles.
    void *const *skeletons;
    /// Number of readable skeleton handles.
    int32_t skeleton_count;
    /// Borrowed array of skeletal Animation3D handles.
    void *const *animations;
    /// Number of readable skeletal animation handles.
    int32_t animation_count;
    /// Borrowed array of NodeAnimation3D handles.
    void *const *node_animations;
    /// Number of readable node-animation handles.
    int32_t node_animation_count;
    /// Borrowed complete asset camera array.
    void *const *cameras;
    /// Number of readable asset camera handles.
    int32_t camera_count;
    /// Borrowed array of scene serialization views.
    const rt_vscn_asset_scene_view *scenes;
    /// Number of readable scene views.
    int32_t scene_count;
    /// Borrowed array of UTF-8 material-variant names.
    const char *const *variant_names;
    /// Number of readable variant names.
    int32_t variant_count;
} rt_vscn_asset_save_view;

/// @brief Save a complete imported asset as VSCN v5.
/// @param view Borrowed complete asset inventory consumed synchronously.
/// @param path Borrowed runtime-string output path.
/// @return Nonzero on successful serialization, otherwise zero.
int64_t rt_vscn_save_asset_view(const rt_vscn_asset_save_view *view, rt_string path);

/* A VSCN node adds an object and, except at the leaf, a children array to the shared JSON parser's
 * 200-level nesting budget. Ninety-eight node levels leave room for the document root plus the
 * deepest node's optional light/LOD/auto-LOD objects. Save and load enforce this same limit. */
#define VSCN_MAX_NODE_DEPTH 98
#define VSCN_ABS_MAX 1.0e12
#define VSCN_MAX_FILE_BYTES (256u * 1024u * 1024u)

/// @brief Return @p value if finite, else @p fallback. Base sanitizer for loaded JSON numbers.
/// @param value Candidate parsed/serialized scalar.
/// @param fallback Replacement used for non-finite input.
/// @return @p value when finite, otherwise @p fallback.
static inline double vscn_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Sanitize @p value (fallback if non-finite) and clamp to ±VSCN_ABS_MAX.
/// @param value Candidate parsed/serialized scalar.
/// @param fallback Replacement used for non-finite input.
/// @return Finite scalar in `[-VSCN_ABS_MAX, VSCN_ABS_MAX]`.
static inline double vscn_clamp_abs_or(double value, double fallback) {
    value = vscn_finite_or(value, fallback);
    if (value > VSCN_ABS_MAX)
        return VSCN_ABS_MAX;
    if (value < -VSCN_ABS_MAX)
        return -VSCN_ABS_MAX;
    return value;
}

/// @brief Sanitize @p value (fallback if non-finite) and clamp to [lo, hi].
/// @param value Candidate parsed/serialized scalar.
/// @param fallback Replacement used for non-finite input.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return Finite scalar clamped to `[`@p lo`, `@p hi`]`.
static inline double vscn_clamp_or(double value, double fallback, double lo, double hi) {
    value = vscn_finite_or(value, fallback);
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Sanitize @p value (fallback if non-finite) and clamp negatives to 0.
/// @param value Candidate parsed/serialized scalar.
/// @param fallback Replacement used for non-finite input.
/// @return Finite non-negative scalar.
static inline double vscn_nonnegative_or(double value, double fallback) {
    value = vscn_finite_or(value, fallback);
    return value < 0.0 ? 0.0 : value;
}

/// @brief Accept a material workflow id (legacy/PBR) from JSON, else use @p fallback.
/// @param value Candidate serialized `RT_MATERIAL3D_WORKFLOW_*` identifier.
/// @param fallback Previously validated workflow used for an unknown identifier.
/// @return Valid material workflow identifier.
static inline int32_t vscn_material_workflow_or(int64_t value, int32_t fallback) {
    if (value == RT_MATERIAL3D_WORKFLOW_LEGACY || value == RT_MATERIAL3D_WORKFLOW_PBR)
        return (int32_t)value;
    return fallback;
}

/// @brief Accept an alpha-mode id (opaque/mask/blend) from JSON, else use @p fallback.
/// @param value Candidate serialized `RT_MATERIAL3D_ALPHA_MODE_*` identifier.
/// @param fallback Previously validated alpha mode used for an unknown identifier.
/// @return Valid material alpha-mode identifier.
static inline int32_t vscn_alpha_mode_or(int64_t value, int32_t fallback) {
    if (value >= RT_MATERIAL3D_ALPHA_MODE_OPAQUE && value <= RT_MATERIAL3D_ALPHA_MODE_BLEND)
        return (int32_t)value;
    return fallback;
}

/// @brief Accept a texture-wrap mode (repeat/clamp/mirror) from JSON, else use @p fallback.
/// @param value Candidate serialized `RT_MATERIAL3D_TEXTURE_WRAP_*` identifier.
/// @param fallback Previously validated wrap mode used for an unknown identifier.
/// @return Valid texture-wrap identifier.
static inline int32_t vscn_wrap_or(int64_t value, int32_t fallback) {
    if (value == RT_MATERIAL3D_TEXTURE_WRAP_REPEAT ||
        value == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
        value == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return (int32_t)value;
    return fallback;
}

/// @brief Accept a texture-filter mode (nearest/linear) from JSON, else use @p fallback.
/// @param value Candidate serialized `RT_MATERIAL3D_TEXTURE_FILTER_*` identifier.
/// @param fallback Previously validated filter used for an unknown identifier.
/// @return Valid texture-filter identifier.
static inline int32_t vscn_filter_or(int64_t value, int32_t fallback) {
    if (value == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ||
        value == RT_MATERIAL3D_TEXTURE_FILTER_LINEAR)
        return (int32_t)value;
    return fallback;
}

/// @brief Accept a texture mip-selection mode (none/nearest/linear), else use @p fallback.
/// @param value Candidate serialized `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` value.
/// @param fallback Previously validated value used when @p value is outside the enum domain.
/// @return A valid three-state mip-selection mode.
static inline int32_t vscn_mip_filter_or(int64_t value, int32_t fallback) {
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE ||
        value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST ||
        value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return (int32_t)value;
    return fallback;
}

/// @brief Normalize a loaded quaternion in place; non-finite or near-zero-length values
///   reset to the identity quaternion (0,0,0,1).
/// @param q Mutable four-component quaternion.
static inline void vscn_normalize_quat(double q[4]) {
    if (!q)
        return;
    if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    double len_sq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!isfinite(len_sq) || len_sq < 1e-20) {
        q[0] = 0.0;
        q[1] = 0.0;
        q[2] = 0.0;
        q[3] = 1.0;
        return;
    }
    double inv_len = 1.0 / sqrt(len_sq);
    q[0] *= inv_len;
    q[1] *= inv_len;
    q[2] *= inv_len;
    q[3] *= inv_len;
}

/// @brief Normalize a loaded vec3 in place; a near-zero or non-finite length falls back
///   to the (fx, fy, fz) direction.
/// @param v Mutable three-component vector.
/// @param fx Fallback X direction component.
/// @param fy Fallback Y direction component.
/// @param fz Fallback Z direction component.
static inline void vscn_normalize_vec3(double v[3], double fx, double fy, double fz) {
    if (!v)
        return;
    double len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(len_sq) || len_sq < 1e-20) {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
        return;
    }
    double inv_len = 1.0 / sqrt(len_sq);
    v[0] *= inv_len;
    v[1] *= inv_len;
    v[2] *= inv_len;
}
