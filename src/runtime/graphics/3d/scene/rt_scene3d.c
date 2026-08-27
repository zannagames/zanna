//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d.c
// Purpose: Zanna.Graphics3D.Scene3D / SceneNode3D — 3D scene graph with
//   parent-child transform propagation. Each node holds local TRS, and the
//   world matrix is lazily recomputed on access or draw.
//
// Key invariants:
//   - TRS order: world = parent_world * Translate * Rotate * Scale
//   - Dirty state is lazy: changing a parent bumps its world revision, and
//     descendants refresh when their cached parent revision no longer matches.
//   - Children array is heap-allocated (not GC-managed); freed in finalizer.
//   - Mesh/material/name and LOD meshes are retained by the node.
//   - Iterative traversal stacks avoid recursion stack overflow for draw/count
//     walks; transform invalidation itself is allocation-free.
//   - LOD levels are kept sorted by distance ascending so the draw path picks
//     the highest threshold that does not exceed camera distance.
//
// Ownership/Lifetime:
//   - Scene3D / SceneNode3D / NodeAnimation3D / NodeAnimator3D are GC-managed.
//   - Scene3D retains the root subtree; finalizer releases the root.
//   - SceneNode3D retains mesh, material, light, name, animator, body binding,
//     and per-LOD meshes; finalizer releases all of them.
//
// Links: rt_scene3d.h, rt_quat.h, rt_mat4.h, plans/3d/12-scene-graph.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared Scene3D validation, numeric, allocation, and spatial-dirty helpers.
/// @details The public scene implementation is split across included modules;
/// this translation unit establishes the common finite-value, transform,
/// ownership, growth, and spatial-index invariants those modules rely on.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_scene3d.h"
#include "rt_animcontroller3d.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_physics3d.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_quat.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d_internal.h"
#include "rt_sound3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include "vgfx3d_frustum.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Conservative process-wide epoch for parent/child topology mutations.
static volatile uint64_t g_scene3d_hierarchy_epoch = 1;

/// @brief Record an add/remove/reparent so hierarchy-dependent caches revalidate once.
void scene3d_note_hierarchy_change(void) {
    uint64_t previous =
        rt_atomic_fetch_add_u64(&g_scene3d_hierarchy_epoch, UINT64_C(1), __ATOMIC_RELEASE);
    if (previous == UINT64_MAX)
        rt_atomic_store_u64(&g_scene3d_hierarchy_epoch, UINT64_C(1), __ATOMIC_RELEASE);
}

/// @brief Return the current nonzero hierarchy epoch.
uint64_t scene3d_hierarchy_epoch(void) {
    return rt_atomic_load_u64(&g_scene3d_hierarchy_epoch, __ATOMIC_ACQUIRE);
}

/// @brief Validate @p obj as a Scene3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Borrowed opaque runtime handle.
/// @return Borrowed Scene3D payload on a class match, otherwise `NULL`.
rt_scene3d *scene3d_checked(void *obj) {
#if defined(RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE) && RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE
    return (rt_scene3d *)obj;
#else
    return rt_obj_is_instance(obj, RT_G3D_SCENE3D_CLASS_ID, sizeof(rt_scene3d)) ? (rt_scene3d *)obj
                                                                                : NULL;
#endif
}

/// @brief Validate @p obj as a SceneNode3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Borrowed opaque runtime handle.
/// @return Borrowed SceneNode3D payload on a class match, otherwise `NULL`.
rt_scene_node3d *scene_node3d_checked(void *obj) {
#if defined(RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE) && RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE
    return (rt_scene_node3d *)obj;
#else
    return rt_obj_is_instance(obj, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d))
               ? (rt_scene_node3d *)obj
               : NULL;
#endif
}

/// @brief Validate an immutable SceneNode3D payload without discarding const qualification.
/// @param obj Borrowed candidate node payload.
/// @return The unchanged node on a class match, otherwise `NULL`.
static const rt_scene_node3d *scene_node3d_checked_const(const rt_scene_node3d *obj) {
#if defined(RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE) && RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE
    return obj;
#else
    void *raw = (void *)(uintptr_t)obj;
    return rt_obj_is_instance(raw, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d)) ? obj
                                                                                         : NULL;
#endif
}

/// @brief Drop the GC reference in `*slot` and null the pointer (refcount-aware free).
/// @param slot Address of an owned runtime-reference slot.
void scene3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained Graphics3D slot only if it still has the expected class.
/// @details A stale or mismatched slot is cleared without releasing an object
///          whose ownership can no longer be proven.
/// @param slot Address of the retained opaque handle.
/// @param class_id Stable class identifier required before release.
static void scene3d_release_class_ref(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    scene3d_release_ref(slot);
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
/// @param slot Address of the retained Pixels handle.
static void scene3d_release_pixels_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_pixels_checked_impl_or_null(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    scene3d_release_ref(slot);
}

/// @brief Grow a traversal-stack buffer by doubling (min 64 elements), overflow-checked.
/// @param buffer Address of the caller-owned allocation pointer.
/// @param capacity In/out element capacity.
/// @param elem_size Nonzero size of one stack element in bytes.
/// @return 1 with @p buffer / @p capacity updated, 0 on overflow or allocation failure.
int scene3d_grow_stack_storage(void **buffer, size_t *capacity, size_t elem_size) {
    size_t new_capacity;
    void *grown;
    if (!buffer || !capacity || elem_size == 0)
        return 0;
    new_capacity = *capacity > 0 ? *capacity * 2u : 64u;
    if (new_capacity <= *capacity || new_capacity > SIZE_MAX / elem_size)
        return 0;
    grown = realloc(*buffer, new_capacity * elem_size);
    if (!grown)
        return 0;
    *buffer = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Grow an int32-indexed array to hold at least @p needed elements (floor
///   @p min_capacity, doubling thereafter), optionally zeroing the newly added tail.
/// @param buffer Address of the caller-owned allocation pointer.
/// @param capacity In/out signed element capacity.
/// @param needed Minimum required element count.
/// @param min_capacity Positive initial growth floor.
/// @param elem_size Nonzero size of one array element in bytes.
/// @param zero_new Nonzero to clear newly allocated elements.
/// @return 1 on success (including when already large enough), 0 on bad args or allocation failure.
int scene3d_grow_array_i32(void **buffer,
                           int32_t *capacity,
                           int32_t needed,
                           int32_t min_capacity,
                           size_t elem_size,
                           int zero_new) {
    int32_t old_capacity;
    int32_t new_capacity;
    void *grown;
    if (!buffer || !capacity || needed < 0 || min_capacity <= 0 || elem_size == 0)
        return 0;
    old_capacity = *capacity;
    if (old_capacity >= needed)
        return 1;
    if (old_capacity < 0)
        return 0;
    new_capacity = old_capacity < min_capacity ? min_capacity : old_capacity;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            new_capacity = needed;
        else
            new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / elem_size)
        return 0;
    grown = realloc(*buffer, (size_t)new_capacity * elem_size);
    if (!grown)
        return 0;
    if (zero_new && new_capacity > old_capacity)
        memset((char *)grown + (size_t)old_capacity * elem_size,
               0,
               (size_t)(new_capacity - old_capacity) * elem_size);
    *buffer = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Mark the spatial index fully stale: a topology change forces a full BVH rebuild.
/// @param scene Borrowed scene whose spatial index flags are invalidated.
void scene3d_mark_spatial_dirty(rt_scene3d *scene) {
    if (!scene)
        return;
    scene->spatial_index.dirty = 1;
    scene->spatial_index.valid = 0;
    scene->spatial_index.topology_dirty = 1;
    scene->spatial_index.dirty_node_count = 0;
    scene->spatial_index.dirty_all = 0;
}

/// @brief Queue one changed subtree for a cheaper BVH refit.
/// @details Exact duplicate roots are coalesced. Allocation failure sets a full-scan fallback flag
///   rather than losing the invalidation request.
static void scene3d_mark_spatial_refit_dirty(rt_scene3d *scene, rt_scene_node3d *node) {
    if (!scene)
        return;
    scene->spatial_index.dirty = 1;
    if (!scene->spatial_index.valid) {
        scene->spatial_index.topology_dirty = 1;
        return;
    }
    if (!node || node->owner_scene != scene || scene->spatial_index.dirty_all)
        scene->spatial_index.dirty_all = 1;
    else {
        for (int32_t i = 0; i < scene->spatial_index.dirty_node_count; i++) {
            if (scene->spatial_index.dirty_nodes[i] == node)
                return;
        }
        if (!scene3d_grow_array_i32((void **)&scene->spatial_index.dirty_nodes,
                                    &scene->spatial_index.dirty_node_capacity,
                                    scene->spatial_index.dirty_node_count + 1,
                                    16,
                                    sizeof(*scene->spatial_index.dirty_nodes),
                                    0)) {
            scene->spatial_index.dirty_node_count = 0;
            scene->spatial_index.dirty_all = 1;
            return;
        }
        scene->spatial_index.dirty_nodes[scene->spatial_index.dirty_node_count++] = node;
    }
}

/// @brief Mark the spatial index stale after a visibility toggle.
/// @details Hidden nodes stay in the BVH as filtered entries, so a visibility
///   change is a refit (per-entry flag refresh), NOT a topology rebuild — a
///   blinking or LOD-hidden object no longer costs O(n log n) per toggle.
/// @param node Borrowed node whose effective-visibility subtree changed.
void scene3d_mark_spatial_visibility_dirty(rt_scene_node3d *node) {
    if (node)
        scene3d_mark_spatial_refit_dirty(node->owner_scene, node);
}

/// @brief Return @p value if it is a finite number, otherwise return @p fallback.
/// @details Used to sanitize every numeric input that comes from external data (glTF
///   assets, caller-supplied transforms) before it can corrupt a matrix multiply or
///   a length calculation with NaN / Inf. The indirection through this helper rather
///   than an inline ternary makes the intent clear at each call site.
/// @param value   Candidate double — may be NaN, +Inf, or -Inf.
/// @param fallback Value to substitute when @p value is not finite.
/// @return @p value when finite, @p fallback otherwise.
static double scene3d_finite_or(double value, double fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Clamp `value` into `[-SCENE3D_ABS_MAX, SCENE3D_ABS_MAX]`, substituting `fallback` when
/// not finite.
/// @param value Candidate scene-space scalar.
/// @param fallback Replacement used when @p value is non-finite.
/// @return Finite value clamped to the scene numeric envelope.
double scene3d_clamp_abs_or(double value, double fallback) {
    value = scene3d_finite_or(value, fallback);
    if (value > SCENE3D_ABS_MAX)
        return SCENE3D_ABS_MAX;
    if (value < -SCENE3D_ABS_MAX)
        return -SCENE3D_ABS_MAX;
    return value;
}

/// @brief Narrow a double to float, returning 0.0f when non-finite or outside
/// ±SCENE3D_FLOAT_ABS_MAX.
/// @param value Candidate double-precision scalar.
/// @return Safely narrowed finite float, or zero when outside the supported envelope.
float scene3d_float_or_zero(double value) {
    if (!isfinite(value) || value < -SCENE3D_FLOAT_ABS_MAX || value > SCENE3D_FLOAT_ABS_MAX)
        return 0.0f;
    return (float)value;
}

/// @brief Return @p value if finite, or 1.0 as a safe scale factor.
/// @details Specialisation of `scene3d_finite_or` for scale components where a
///   NaN/Inf value would corrupt transform composition. Finite zero scale is
///   preserved so authored collapse animations and intentionally flattened nodes
///   remain representable; inverse-dependent consumers handle singular matrices
///   at their own boundary.
/// @param value Scale factor candidate — may be NaN or Inf.
/// @return @p value when finite, otherwise 1.0.
double scene3d_scale_or_unit(double value) {
    if (!isfinite(value))
        return 1.0;
    if (value > SCENE3D_ABS_MAX)
        return SCENE3D_ABS_MAX;
    if (value < -SCENE3D_ABS_MAX)
        return -SCENE3D_ABS_MAX;
    return value;
}

/// @brief Clamp a non-negative scene-space distance/radius to the runtime numeric envelope.
/// @param value Candidate distance or radius.
/// @return Positive finite value capped at `SCENE3D_ABS_MAX`, or zero when invalid.
double scene3d_distance_or_zero(double value) {
    if (!isfinite(value) || value <= 0.0)
        return 0.0;
    return value > SCENE3D_ABS_MAX ? SCENE3D_ABS_MAX : value;
}

/// @brief Sanitize a raw double[3] coordinate vector in place.
/// @param v Mutable three-component vector; `NULL` is accepted.
/// @param fallback Replacement used independently for non-finite components.
static void scene3d_sanitize_vec3d(double v[3], double fallback) {
    if (!v)
        return;
    v[0] = scene3d_clamp_abs_or(v[0], fallback);
    v[1] = scene3d_clamp_abs_or(v[1], fallback);
    v[2] = scene3d_clamp_abs_or(v[2], fallback);
}

/// @brief Sanitize a raw scale vector in place while preserving finite zero/negative scales.
/// @param v Mutable three-component scale vector; `NULL` is accepted.
static void scene3d_sanitize_scale3(double v[3]) {
    if (!v)
        return;
    v[0] = scene3d_scale_or_unit(v[0]);
    v[1] = scene3d_scale_or_unit(v[1]);
    v[2] = scene3d_scale_or_unit(v[2]);
}

/// @brief Robustly normalize a raw double[3] vector without overflowing on huge finite inputs.
/// @param v Mutable three-component vector.
/// @return Nonzero after successful unit normalization, otherwise zero with input unchanged.
int scene3d_normalize_vec3d(double v[3]) {
    double max_abs;
    double x;
    double y;
    double z;
    double len_sq;
    double inv_len;
    if (!v || !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]))
        return 0;
    max_abs = fmax(fabs(v[0]), fmax(fabs(v[1]), fabs(v[2])));
    if (!isfinite(max_abs) || max_abs < 1e-24)
        return 0;
    x = v[0] / max_abs;
    y = v[1] / max_abs;
    z = v[2] / max_abs;
    len_sq = x * x + y * y + z * z;
    if (!isfinite(len_sq) || len_sq < 1e-24)
        return 0;
    inv_len = 1.0 / sqrt(len_sq);
    v[0] = x * inv_len;
    v[1] = y * inv_len;
    v[2] = z * inv_len;
    return 1;
}

/// @brief Check that every matrix lane is finite.
/// @param m Borrowed row-major 16-element matrix.
/// @return Nonzero when the pointer and every lane are finite.
static int scene3d_matrix_is_finite(const double *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    return 1;
}

/// @brief Canonicalize and sanitize a double-precision AABB in place.
/// @param mn Mutable three-component minimum corner.
/// @param mx Mutable three-component maximum corner.
void scene3d_canonicalize_aabb_d(double mn[3], double mx[3]) {
    if (!mn || !mx)
        return;
    scene3d_sanitize_vec3d(mn, 0.0);
    scene3d_sanitize_vec3d(mx, 0.0);
    for (int i = 0; i < 3; i++) {
        if (mn[i] > mx[i]) {
            double tmp = mn[i];
            mn[i] = mx[i];
            mx[i] = tmp;
        }
    }
}

/// @brief Restore SceneNode3D local TRS invariants before matrix composition or getters.
/// @param node Borrowed node whose position, scale, and quaternion are repaired in place.
static void scene3d_repair_node_transform(rt_scene_node3d *node) {
    if (!node)
        return;
    scene3d_sanitize_vec3d(node->position, 0.0);
    scene3d_sanitize_scale3(node->scale_xyz);
    scene3d_quat_normalize_local(node->rotation);
}

/// @brief Consume the pending root-motion rotation from an animation controller.
/// @param obj Borrowed AnimController3D handle.
/// @return New quaternion handle representing the consumed rotation.
extern void *rt_anim_controller3d_consume_root_motion_rotation(void *obj);

// clang-format off
#include "rt_scene3d_helpers.inc"
#include "rt_scene3d_cull.inc"
#include "rt_scene3d_api.inc"
#include "rt_scene3d_lod.inc"
// clang-format on
//=============================================================================

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
