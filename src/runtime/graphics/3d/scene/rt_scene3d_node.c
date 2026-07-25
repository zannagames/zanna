//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_node.c
// Purpose: SceneNode3D lifecycle + accessors (transform, mesh/material/light/body,
//   hierarchy, animator binding, world-space queries). Split out of rt_scene3d.c;
//   shares private structs/helpers via rt_scene3d_internal.h.
// Key invariants:
//   - Scene roots cannot be reparented or detached through node hierarchy APIs.
//   - Reparenting preflights subtree owner propagation before topology mutation.
//   - Preserve-world reparenting rejects before mutation unless local TRS can
//     reproduce the complete prior world matrix.
//   - Sibling reordering validates the complete direct-child table before
//     moving one retained slot and never changes ownership or parent links.
// Ownership/Lifetime:
//   - Parent nodes retain each child exactly once while it is attached.
//   - Mesh, material, animator, physics, and name slots are runtime-managed references.
//   - Each node releases its native gameplay-metadata table during finalization.
// Links: rt_scene3d_internal.h, rt_scene3d.h, rt_scene3d_metadata.c,
//   docs/adr/0161-stable-scenenode-sibling-reordering.md,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md
//
//===----------------------------------------------------------------------===//

#ifdef ZANNA_ENABLE_GRAPHICS

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
#include "rt_quat.h"
#include "rt_scene3d.h"
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

#ifndef RT_SCENE3D_MAX_ANCESTOR_DEPTH
#define RT_SCENE3D_MAX_ANCESTOR_DEPTH 1000000
#endif

/// @brief Release a retained Graphics3D slot only if it still has the expected class.
static void scene_node_release_class_slot(void **slot, int64_t class_id) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, class_id)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    scene3d_release_ref(slot);
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
static void scene_node_release_pixels_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_pixels_checked_impl_or_null(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    scene3d_release_ref(slot);
}

/// @brief Release a retained rt_string slot only if it still points at an rt_string handle.
static void scene_node_release_string_slot(rt_string *slot) {
    if (!slot || !*slot)
        return;
    if (!rt_string_is_handle(*slot)) {
        rt_g3d_ref_slot_clear_unowned((void **)slot);
        return;
    }
    scene3d_release_ref((void **)slot);
}

/// @brief Retain-then-release assignment for a Graphics3D typed slot.
static void scene_node_assign_class_ref(void **slot, void *value, int64_t class_id) {
    if (!slot || *slot == value)
        return;
    if (value && !rt_g3d_has_class(value, class_id))
        return;
    rt_obj_retain_maybe(value);
    scene_node_release_class_slot(slot, class_id);
    *slot = value;
}

/// @brief Clear one slot if it is non-null but no longer has the expected class.
static void scene_node_repair_class_slot(void **slot, int64_t class_id) {
    if (slot && *slot && !rt_g3d_has_class(*slot, class_id))
        scene_node_release_class_slot(slot, class_id);
}

/// @brief Clear one string slot if it no longer points at an rt_string handle.
static void scene_node_repair_string_slot(rt_string *slot) {
    if (slot && *slot && !rt_string_is_handle(*slot))
        rt_g3d_ref_slot_clear_unowned((void **)slot);
}

/// @brief Class-checked cast of an opaque handle to a SceneNode3D, or NULL on mismatch.
static rt_scene_node3d *scene_node_ref(void *ref) {
    return (rt_scene_node3d *)rt_g3d_checked_or_null(ref, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Class-checked cast of an opaque handle to a NodeAnimator3D, or NULL on mismatch.
static rt_node_animator3d *scene_node_animator_ref(void *ref) {
    return (rt_node_animator3d *)rt_g3d_checked_or_null(ref, RT_G3D_NODEANIMATOR3D_CLASS_ID);
}

/// @brief Length of a 3D basis vector without overflowing on large finite matrix terms.
static double scene_node_basis_length(double x, double y, double z) {
    double max_abs = fmax(fabs(x), fmax(fabs(y), fabs(z)));
    if (!isfinite(max_abs) || max_abs <= 0.0)
        return max_abs == 0.0 ? 0.0 : 1.0;
    x /= max_abs;
    y /= max_abs;
    z /= max_abs;
    return max_abs * sqrt(x * x + y * y + z * z);
}

/*==========================================================================
 * SceneNode3D — lifecycle
 *=========================================================================*/

/// @brief GC finalizer for a SceneNode — release mesh/material/animator/body refs and the children
/// array.
static void rt_scene_node3d_finalize(void *obj) {
    rt_scene_node3d *node = (rt_scene_node3d *)obj;
    int32_t child_count;
    int32_t lod_count;
    if (!node)
        return;

    child_count = scene3d_node_child_count(node);
    for (int32_t i = 0; i < child_count; i++) {
        rt_scene_node3d *child = scene_node_ref(node->children[i]);
        if (child)
            child->parent = NULL;
        scene_node_release_class_slot((void **)&node->children[i], RT_G3D_SCENENODE3D_CLASS_ID);
    }
    free(node->children);
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    lod_count = scene3d_node_lod_count(node);
    for (int32_t i = 0; i < lod_count; i++)
        scene_node_release_class_slot(&node->lod_levels[i].mesh, RT_G3D_MESH3D_CLASS_ID);
    free(node->lod_levels);
    node->lod_levels = NULL;
    node->lod_count = 0;
    node->lod_capacity = 0;
    scene_node_release_class_slot(&node->mesh, RT_G3D_MESH3D_CLASS_ID);
    scene_node_release_class_slot(&node->material, RT_G3D_MATERIAL3D_CLASS_ID);
    scene_node_release_class_slot(&node->light, RT_G3D_LIGHT3D_CLASS_ID);
    scene_node_release_class_slot(&node->camera, RT_G3D_CAMERA3D_CLASS_ID);
    scene_node_release_class_slot(&node->bound_body, RT_G3D_BODY3D_CLASS_ID);
    scene_node_release_class_slot(&node->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    {
        rt_node_animator3d *node_animator = scene_node_animator_ref(node->bound_node_animator);
        if (node_animator)
            node_animator->root = NULL;
    }
    scene_node_release_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
    scene_node_release_class_slot(&node->socket_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    scene_node_release_pixels_slot(&node->impostor_pixels);
    scene_node_release_class_slot(&node->impostor_mesh, RT_G3D_MESH3D_CLASS_ID);
    scene_node_release_class_slot(&node->impostor_material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (node->impostor_frame_meshes) {
        for (int32_t fi = 0; fi < node->impostor_frame_count; ++fi)
            scene_node_release_class_slot(&node->impostor_frame_meshes[fi], RT_G3D_MESH3D_CLASS_ID);
        free(node->impostor_frame_meshes);
        node->impostor_frame_meshes = NULL;
    }
    node->impostor_frame_count = 0;
    node->impostor_frame_index = 0;
    for (int32_t i = 0; i < node->variant_material_count && node->variant_materials; i++)
        scene_node_release_class_slot(&node->variant_materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
    free(node->variant_materials);
    node->variant_materials = NULL;
    node->variant_material_count = 0;
    scene_node_release_string_slot(&node->name);
    scene_node_release_string_slot(&node->prefab_path);
    rt_scene_node3d_metadata_clear_internal(node);
}

/// @brief Install a variant-indexed material table on @p node (importer/clone hook).
/// @details Retains each non-NULL entry before releasing any previous table, so
///          re-assigning the same table is safe. NULL slots are preserved (they mean
///          "this variant leaves the node's material untouched").
int rt_scene_node3d_assign_variant_materials(rt_scene_node3d *node,
                                             void *const *materials,
                                             int32_t count) {
    void **table = NULL;
    if (!node)
        return 0;
    if (materials && count > 0) {
        if ((size_t)count > SIZE_MAX / sizeof(void *))
            return 0;
        table = (void **)calloc((size_t)count, sizeof(void *));
        if (!table)
            return 0;
        for (int32_t i = 0; i < count; i++) {
            void *material = rt_g3d_checked_or_null(materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
            if (material)
                rt_obj_retain_maybe(material);
            table[i] = material;
        }
    }
    for (int32_t i = 0; i < node->variant_material_count && node->variant_materials; i++)
        scene_node_release_class_slot(&node->variant_materials[i], RT_G3D_MATERIAL3D_CLASS_ID);
    free(node->variant_materials);
    node->variant_materials = table;
    node->variant_material_count = table ? count : 0;
    return 1;
}

/// @brief Copy the variant-material table from @p src onto @p dst (clone helper).
int rt_scene_node3d_copy_variant_materials(rt_scene_node3d *dst, const rt_scene_node3d *src) {
    if (!dst)
        return 0;
    if (!src || !src->variant_materials || src->variant_material_count <= 0)
        return rt_scene_node3d_assign_variant_materials(dst, NULL, 0);
    return rt_scene_node3d_assign_variant_materials(
        dst, src->variant_materials, src->variant_material_count);
}

// ===========================================================================
// SceneNode public API
//
// A SceneNode is one transformable element in the scene graph: it
// carries a TRS (position / rotation / scale), an optional mesh +
// material to draw, an optional rigid body to drive physics from,
// an optional animator, and a list of child nodes. Each accessor
// is null-safe; setters skip if `obj` is NULL, getters return zero
// / identity / NULL.
// ===========================================================================

/// @brief Create an empty SceneNode at the origin (identity rotation, scale 1).
void *rt_scene_node3d_new(void) {
    rt_scene_node3d *node = (rt_scene_node3d *)rt_obj_new_i64(RT_G3D_SCENENODE3D_CLASS_ID,
                                                              (int64_t)sizeof(rt_scene_node3d));
    if (!node) {
        rt_trap("SceneNode3D.New: memory allocation failed");
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->vptr = NULL;
    node->identity_serial = rt_g3d_next_identity_serial();
    node->position[0] = node->position[1] = node->position[2] = 0.0;
    node->rotation[0] = node->rotation[1] = node->rotation[2] = 0.0;
    node->rotation[3] = 1.0; /* identity quaternion (0,0,0,1) */
    node->scale_xyz[0] = node->scale_xyz[1] = node->scale_xyz[2] = 1.0;

    /* Identity world matrix */
    memset(node->world_matrix, 0, sizeof(double) * 16);
    node->world_matrix[0] = node->world_matrix[5] = 1.0;
    node->world_matrix[10] = node->world_matrix[15] = 1.0;
    node->world_dirty = 1;
    node->world_revision = 1;
    node->parent_world_revision_seen = 0;

    node->parent = NULL;
    node->owner_scene = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;

    node->mesh = NULL;
    node->material = NULL;
    node->light = NULL;
    node->camera = NULL;
    node->bound_body = NULL;
    node->bound_animator = NULL;
    node->bound_node_animator = NULL;
    node->sync_mode = RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY;
    node->import_index = -1;
    node->visible = 1;
    node->name = NULL;
    node->metadata = NULL;
    node->metadata_count = 0;
    node->metadata_capacity = 0;
    node->socket_animator = NULL;
    node->socket_bone = -1;
    node->socket_offset_pos[0] = node->socket_offset_pos[1] = node->socket_offset_pos[2] = 0.0;
    node->socket_offset_quat[0] = node->socket_offset_quat[1] = node->socket_offset_quat[2] = 0.0;
    node->socket_offset_quat[3] = 1.0;

    memset(node->aabb_min, 0, sizeof(float) * 3);
    memset(node->aabb_max, 0, sizeof(float) * 3);
    node->bsphere_radius = 0.0f;

    node->lod_levels = NULL;
    node->lod_count = 0;
    node->lod_capacity = 0;
    node->auto_lod_enabled = 0;
    node->auto_lod_screen_error_px = 8.0;
    node->lod_selected_index = 0;
    node->lod_selection_valid = 0;
    node->is_static = 0;
    node->has_impostor = 0;
    node->impostor_selected = 0;
    node->impostor_distance = 0.0;
    node->impostor_pixels = NULL;
    node->impostor_mesh = NULL;
    node->impostor_material = NULL;
    node->impostor_frame_count = 0;
    node->impostor_frame_index = 0;
    node->impostor_frame_meshes = NULL;
    node->variant_materials = NULL;
    node->variant_material_count = 0;

    rt_obj_set_finalizer(node, rt_scene_node3d_finalize);
    return node;
}

/*==========================================================================
 * SceneNode3D — transform
 *=========================================================================*/

/// @brief Set the local-space position component of the node's TRS.
void rt_scene_node3d_set_position(void *obj, double x, double y, double z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double next[3];
    if (!n)
        return;
    next[0] = scene3d_clamp_abs_or(x, 0.0);
    next[1] = scene3d_clamp_abs_or(y, 0.0);
    next[2] = scene3d_clamp_abs_or(z, 0.0);
    if (n->position[0] == next[0] && n->position[1] == next[1] && n->position[2] == next[2])
        return;
    n->position[0] = next[0];
    n->position[1] = next[1];
    n->position[2] = next[2];
    mark_dirty(n);
}

/// @brief Read the local position as a Vec3 (origin if `obj` is NULL).
void *rt_scene_node3d_get_position(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return rt_vec3_new(0, 0, 0);
    return rt_vec3_new(n->position[0], n->position[1], n->position[2]);
}

/// @brief Replace the local rotation with the given Quat (re-normalised on store).
void rt_scene_node3d_set_rotation(void *obj, void *quat) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double next[4];
    if (!n || !rt_g3d_is_quat(quat))
        return;
    next[0] = rt_quat_x(quat);
    next[1] = rt_quat_y(quat);
    next[2] = rt_quat_z(quat);
    next[3] = rt_quat_w(quat);
    scene3d_quat_normalize_local(next);
    if (n->rotation[0] == next[0] && n->rotation[1] == next[1] && n->rotation[2] == next[2] &&
        n->rotation[3] == next[3])
        return;
    n->rotation[0] = next[0];
    n->rotation[1] = next[1];
    n->rotation[2] = next[2];
    n->rotation[3] = next[3];
    mark_dirty(n);
}

/// @brief Read the local rotation as a Quat (identity if `obj` is NULL).
void *rt_scene_node3d_get_rotation(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return rt_quat_new(0, 0, 0, 1);
    return rt_quat_new(n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]);
}

/// @brief Set the per-axis scale (uniform or non-uniform).
void rt_scene_node3d_set_scale(void *obj, double x, double y, double z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double next[3];
    if (!n)
        return;
    next[0] = scene3d_scale_or_unit(x);
    next[1] = scene3d_scale_or_unit(y);
    next[2] = scene3d_scale_or_unit(z);
    if (n->scale_xyz[0] == next[0] && n->scale_xyz[1] == next[1] && n->scale_xyz[2] == next[2])
        return;
    n->scale_xyz[0] = next[0];
    n->scale_xyz[1] = next[1];
    n->scale_xyz[2] = next[2];
    mark_dirty(n);
}

/// @brief Read the local scale as a Vec3 (1,1,1 if `obj` is NULL).
void *rt_scene_node3d_get_scale(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return rt_vec3_new(1, 1, 1);
    return rt_vec3_new(n->scale_xyz[0], n->scale_xyz[1], n->scale_xyz[2]);
}

/// @brief Compose this node's local TRS with all ancestors and return the world matrix as a Mat4.
void *rt_scene_node3d_get_world_matrix(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return NULL;
    recompute_world_matrix(n);
    const double *m = n->world_matrix;
    return rt_mat4_new(m[0],
                       m[1],
                       m[2],
                       m[3],
                       m[4],
                       m[5],
                       m[6],
                       m[7],
                       m[8],
                       m[9],
                       m[10],
                       m[11],
                       m[12],
                       m[13],
                       m[14],
                       m[15]);
}

/// @brief Assign one complete world matrix only when parent-relative TRS is exact.
/// @details The requested Mat4 and current parent chain are validated before any
///          transform lane changes. Singular parents, projective matrices,
///          degenerate bases, shear, and decomposition drift reject atomically.
int8_t rt_scene_node3d_try_set_world_matrix(void *obj, void *world_matrix) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    rt_scene_node3d *parent;
    double desired_world[16];
    double position[3];
    double rotation[4];
    double scale[3];

    if (!node || !rt_obj_is_instance(world_matrix, RT_MAT4_CLASS_ID, 16 * sizeof(double)))
        return 0;
    for (int64_t row = 0; row < 4; ++row) {
        for (int64_t column = 0; column < 4; ++column)
            desired_world[row * 4 + column] = rt_mat4_get(world_matrix, row, column);
    }
    if (!scene3d_preserve_matrix_equal(desired_world, desired_world))
        return 0;
    recompute_world_matrix(node);
    if (scene3d_preserve_matrix_equal(node->world_matrix, desired_world))
        return 1;

    parent = scene_node3d_checked(node->parent);
    if (!scene_node_compute_local_trs_for_world_matrix(
            parent, desired_world, position, rotation, scale))
        return 0;

    memcpy(node->position, position, sizeof(position));
    memcpy(node->rotation, rotation, sizeof(rotation));
    memcpy(node->scale_xyz, scale, sizeof(scale));
    mark_dirty(node);
    return 1;
}

/// @brief Read the world-space translation as a Vec3.
void *rt_scene_node3d_get_world_position(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    scene_node_get_world_position(n, &x, &y, &z);
    x = scene3d_clamp_abs_or(x, 0.0);
    y = scene3d_clamp_abs_or(y, 0.0);
    z = scene3d_clamp_abs_or(z, 0.0);
    return rt_vec3_new(x, y, z);
}

/// @brief Read a node's world-space position into @p x / @p y / @p z (resolving its world
/// transform).
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_world_position_components(void *obj, double *x, double *y, double *z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n || !x || !y || !z)
        return 0;
    scene_node_get_world_position(n, x, y, z);
    *x = scene3d_clamp_abs_or(*x, 0.0);
    *y = scene3d_clamp_abs_or(*y, 0.0);
    *z = scene3d_clamp_abs_or(*z, 0.0);
    return 1;
}

/// @brief Read the world-space orientation as a Quat.
void *rt_scene_node3d_get_world_rotation(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double q[4];
    scene_node_get_world_rotation(n, q);
    return rt_quat_new(q[0], q[1], q[2], q[3]);
}

/// @brief Read world-space scale magnitudes from the composed matrix basis vectors.
void *rt_scene_node3d_get_world_scale(void *obj) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    const double *m;
    double sx;
    double sy;
    double sz;
    if (!n)
        return rt_vec3_new(1.0, 1.0, 1.0);
    recompute_world_matrix(n);
    m = n->world_matrix;
    sx = scene_node_basis_length(m[0], m[4], m[8]);
    sy = scene_node_basis_length(m[1], m[5], m[9]);
    sz = scene_node_basis_length(m[2], m[6], m[10]);
    if (!isfinite(sx))
        sx = 1.0;
    if (!isfinite(sy))
        sy = 1.0;
    if (!isfinite(sz))
        sz = 1.0;
    sx = scene3d_clamp_abs_or(sx, 1.0);
    sy = scene3d_clamp_abs_or(sy, 1.0);
    sz = scene3d_clamp_abs_or(sz, 1.0);
    return rt_vec3_new(sx, sy, sz);
}

/*==========================================================================
 * SceneNode3D — allocation-free transform access + combined/batch setters
 *
 * Per-frame game loops reading or writing many node transforms through the
 * object-returning getters allocate a GC Vec3/Quat/Mat4 per call. These
 * out-parameter variants (mirroring get_world_position_components) and the
 * combined SetTransform/SetTransformBatch entry points keep hot loops
 * allocation-free and collapse three VM crossings per node into one.
 *=========================================================================*/

/// @brief Read the local position into out params without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_position_components(void *obj, double *x, double *y, double *z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n || !x || !y || !z)
        return 0;
    *x = n->position[0];
    *y = n->position[1];
    *z = n->position[2];
    return 1;
}

/// @brief Read the local rotation quaternion into out params without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_rotation_components(
    void *obj, double *x, double *y, double *z, double *w) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n || !x || !y || !z || !w)
        return 0;
    *x = n->rotation[0];
    *y = n->rotation[1];
    *z = n->rotation[2];
    *w = n->rotation[3];
    return 1;
}

/// @brief Read the world-space rotation quaternion into out params without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_world_rotation_components(
    void *obj, double *x, double *y, double *z, double *w) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double q[4];
    if (!n || !x || !y || !z || !w)
        return 0;
    scene_node_get_world_rotation(n, q);
    *x = q[0];
    *y = q[1];
    *z = q[2];
    *w = q[3];
    return 1;
}

/// @brief Read the local scale into out params without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_scale_components(void *obj, double *x, double *y, double *z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n || !x || !y || !z)
        return 0;
    *x = n->scale_xyz[0];
    *y = n->scale_xyz[1];
    *z = n->scale_xyz[2];
    return 1;
}

/// @brief Read the world-space scale magnitudes into out params without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle.
int8_t rt_scene_node3d_get_world_scale_components(void *obj, double *x, double *y, double *z) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    const double *m;
    double sx;
    double sy;
    double sz;
    if (!n || !x || !y || !z)
        return 0;
    recompute_world_matrix(n);
    m = n->world_matrix;
    sx = scene_node_basis_length(m[0], m[4], m[8]);
    sy = scene_node_basis_length(m[1], m[5], m[9]);
    sz = scene_node_basis_length(m[2], m[6], m[10]);
    *x = scene3d_clamp_abs_or(isfinite(sx) ? sx : 1.0, 1.0);
    *y = scene3d_clamp_abs_or(isfinite(sy) ? sy : 1.0, 1.0);
    *z = scene3d_clamp_abs_or(isfinite(sz) ? sz : 1.0, 1.0);
    return 1;
}

/// @brief Copy the composed world matrix into @p out (row-major, 16 doubles) without allocating.
/// @return 1 on success, 0 (no writes) for an invalid handle or NULL @p out.
int8_t rt_scene_node3d_get_world_matrix_components(void *obj, double out[16]) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n || !out)
        return 0;
    recompute_world_matrix(n);
    memcpy(out, n->world_matrix, 16 * sizeof(double));
    return 1;
}

/// @brief Set position, rotation (quaternion components), and scale in one call.
/// @details Equivalent to SetPosition + SetRotation + SetScale but without the intermediate
///          Quat allocation and with a single VM crossing — the hot-loop form for moving
///          many nodes per frame.
void rt_scene_node3d_set_transform(void *obj,
                                   double px,
                                   double py,
                                   double pz,
                                   double qx,
                                   double qy,
                                   double qz,
                                   double qw,
                                   double sx,
                                   double sy,
                                   double sz) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    double q[4];
    if (!n)
        return;
    rt_scene_node3d_set_position(obj, px, py, pz);
    q[0] = qx;
    q[1] = qy;
    q[2] = qz;
    q[3] = qw;
    scene3d_quat_normalize_local(q);
    if (n->rotation[0] != q[0] || n->rotation[1] != q[1] || n->rotation[2] != q[2] ||
        n->rotation[3] != q[3]) {
        n->rotation[0] = q[0];
        n->rotation[1] = q[1];
        n->rotation[2] = q[2];
        n->rotation[3] = q[3];
        mark_dirty(n);
    }
    rt_scene_node3d_set_scale(obj, sx, sy, sz);
}

/// @brief SceneGraph.SetNodeTransforms: apply packed TRS values to a list of nodes.
/// @details The scene handle anchors the call as an instance method (the nodes need not
///          belong to it); the work is delegated to the internal batch helper.
void rt_scene3d_set_node_transforms(void *scene, void *nodes, void *values) {
    if (!scene3d_checked(scene))
        return;
    rt_scene_node3d_set_transform_batch(nodes, values);
}

/// @brief Apply packed TRS values to a list of nodes in one runtime call.
/// @details @p values packs 10 floats per node: px,py,pz, qx,qy,qz,qw, sx,sy,sz. Traps when
///          the value count does not match the node count. Null node entries are skipped so
///          callers can keep sparse lists.
void rt_scene_node3d_set_transform_batch(void *nodes, void *values) {
    int64_t node_count;
    int64_t value_count;
    int64_t i;
    if (!nodes || !values) {
        rt_trap("SceneNode3D.SetTransformBatch: nodes and values must be non-null lists");
        return;
    }
    node_count = rt_seq_len(nodes);
    value_count = rt_seq_len(values);
    if (value_count != node_count * 10) {
        rt_trap("SceneNode3D.SetTransformBatch: values must pack 10 floats per node "
                "(px,py,pz, qx,qy,qz,qw, sx,sy,sz)");
        return;
    }
    for (i = 0; i < node_count; i++) {
        void *node = rt_seq_get(nodes, i);
        double v[10];
        int k;
        if (!node)
            continue;
        for (k = 0; k < 10; k++)
            v[k] = rt_unbox_f64(rt_seq_get(values, i * 10 + k));
        rt_scene_node3d_set_transform(
            node, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    }
}

/*==========================================================================
 * SceneNode3D — hierarchy
 *=========================================================================*/

/// @brief Shared hierarchy attach implementation for AddChild and TryAddChild.
/// @details The try-style public API must remain a non-trapping boolean probe for invalid
///          parent/child relationships, while AddChild should surface programmer errors through
///          runtime traps. Allocation and overflow failures trap in both modes because they are
///          runtime failures rather than ordinary relationship rejections.
/// @param parent Already-validated parent node, or NULL for an invalid parent handle.
/// @param child Already-validated child node, or NULL for an invalid child handle.
/// @param trap_on_reject Non-zero to trap for invalid hierarchy relationships.
/// @return 1 when @p child is attached to @p parent, otherwise 0.
static int8_t scene_node3d_try_add_child_impl(rt_scene_node3d *parent,
                                              rt_scene_node3d *child,
                                              int trap_on_reject) {
    scene_owner_transaction owner_transaction;
    rt_scene_node3d *old_parent;
    rt_scene3d *old_owner;
    rt_scene3d *new_owner;
    int32_t old_child_index = -1;
    int owner_change;

    if (!parent || !child || parent == child)
        return 0;
    if (child->owner_scene && child->owner_scene->root == child) {
        if (trap_on_reject)
            rt_trap("SceneNode3D.AddChild: cannot reparent a scene root");
        return 0;
    }
    if (child->parent == parent)
        return 1;

    /* Reject cycle formation by walking the proposed parent's ancestor chain.
     * The hierarchy is a tree, so this is equivalent to subtree search and avoids
     * scanning every descendant of large child subtrees on reparent. */
    int32_t ancestor_guard = 0;
    for (rt_scene_node3d *ancestor = parent; ancestor;
         ancestor = scene_node_ref(ancestor->parent)) {
        if (++ancestor_guard > RT_SCENE3D_MAX_ANCESTOR_DEPTH) {
            if (trap_on_reject)
                rt_trap("SceneNode3D.AddChild: hierarchy is too deep");
            return 0;
        }
        if (ancestor == child)
            return 0;
    }

    old_parent = scene_node_ref(child->parent);
    old_owner = child->owner_scene;
    new_owner = parent->owner_scene;
    owner_change = old_owner != new_owner;
    /* Reserve the target parent's ownership reference before allocating the
       owner transaction. A refcount-overflow trap therefore cannot strand a
       prepared native buffer, and every later preflight failure can restore
       the original retain count before it traps. */
    rt_obj_retain_maybe(child);
    memset(&owner_transaction, 0, sizeof(owner_transaction));
    if (owner_change && !scene_node_owner_transaction_prepare(child, &owner_transaction)) {
        if (rt_obj_release_check0(child))
            rt_obj_free(child);
        rt_trap("Scene3D: owner traversal stack allocation failed");
        return 0;
    }

    if (old_parent) {
        old_parent->child_count = scene3d_node_child_count(old_parent);
        for (int32_t i = 0; i < old_parent->child_count; ++i) {
            if (scene_node_ref(old_parent->children[i]) == child) {
                old_child_index = i;
                break;
            }
        }
    }

    parent->child_count = scene3d_node_child_count(parent);
    if (!parent->children)
        parent->child_capacity = 0;

    /* Grow children array if needed */
    if (parent->child_count >= parent->child_capacity) {
        int32_t new_cap;
        if (parent->child_capacity < 0 || parent->child_capacity > INT32_MAX / 2) {
            scene_node_owner_transaction_discard(&owner_transaction);
            if (rt_obj_release_check0(child))
                rt_obj_free(child);
            rt_trap("SceneNode3D.AddChild: too many children");
            return 0;
        }
        new_cap = parent->child_capacity == 0 ? NODE_INIT_CHILDREN : parent->child_capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(rt_scene_node3d *)) {
            scene_node_owner_transaction_discard(&owner_transaction);
            if (rt_obj_release_check0(child))
                rt_obj_free(child);
            rt_trap("SceneNode3D.AddChild: too many children");
            return 0;
        }
        rt_scene_node3d **nc = (rt_scene_node3d **)realloc(
            parent->children, (size_t)new_cap * sizeof(rt_scene_node3d *));
        if (!nc) {
            scene_node_owner_transaction_discard(&owner_transaction);
            if (rt_obj_release_check0(child))
                rt_obj_free(child);
            rt_trap("SceneNode3D.AddChild: allocation failed");
            return 0;
        }
        parent->children = nc;
        memset(parent->children + parent->child_capacity,
               0,
               (size_t)(new_cap - parent->child_capacity) * sizeof(parent->children[0]));
        parent->child_capacity = new_cap;
    }

    /* Transfer the old parent's retained slot to the already-reserved target
       slot. No fallible work remains after this topology mutation begins. */
    if (old_parent && old_child_index >= 0) {
        for (int32_t i = old_child_index; i < old_parent->child_count - 1; ++i)
            old_parent->children[i] = old_parent->children[i + 1];
        old_parent->child_count--;
        old_parent->children[old_parent->child_count] = NULL;
    } else if (child->parent) {
        child->parent = NULL;
    }

    parent->children[parent->child_count++] = child;
    child->parent = parent;
    if (owner_change)
        scene_node_owner_transaction_assign(&owner_transaction, new_owner);
    scene_node_owner_transaction_discard(&owner_transaction);
    if (old_parent && old_child_index >= 0 && rt_obj_release_check0(child))
        rt_obj_free(child);
    if (old_owner)
        scene3d_mark_spatial_dirty(old_owner);
    if (new_owner)
        scene3d_mark_spatial_dirty(new_owner);
    mark_dirty(child);
    return 1;
}

/// @brief Reparent `child` under `obj`. Detaches from previous parent first; rejects cycles.
int8_t rt_scene_node3d_try_add_child(void *obj, void *child_obj) {
    rt_scene_node3d *parent = scene_node3d_checked(obj);
    rt_scene_node3d *child = scene_node3d_checked(child_obj);
    return scene_node3d_try_add_child_impl(parent, child, 0);
}

/// @brief Reparent one node while retaining its complete world-space matrix.
/// @details The prospective local matrix is derived and proven representable as
///          finite non-degenerate TRS before hierarchy mutation. Singular
///          destination transforms, conversions requiring shear, cycles, roots,
///          and invalid handles return 0 without changing either hierarchy or
///          transform. Once the normal attach transaction succeeds, publishing
///          the precomputed local scalars cannot fail.
int8_t rt_scene_node3d_try_add_child_preserve_world(void *obj, void *child_obj) {
    rt_scene_node3d *parent = scene_node3d_checked(obj);
    rt_scene_node3d *child = scene_node3d_checked(child_obj);
    double position[3];
    double rotation[4];
    double scale[3];
    int32_t ancestor_guard = 0;

    if (!parent || !child || parent == child)
        return 0;
    if (child->owner_scene && child->owner_scene->root == child)
        return 0;
    if (scene_node_ref(child->parent) == parent)
        return 1;
    for (rt_scene_node3d *ancestor = parent; ancestor;
         ancestor = scene_node_ref(ancestor->parent)) {
        if (++ancestor_guard > RT_SCENE3D_MAX_ANCESTOR_DEPTH || ancestor == child)
            return 0;
    }
    if (!scene_node_compute_preserved_local_trs(parent, child, position, rotation, scale))
        return 0;
    if (!scene_node3d_try_add_child_impl(parent, child, 0))
        return 0;

    memcpy(child->position, position, sizeof(position));
    memcpy(child->rotation, rotation, sizeof(rotation));
    memcpy(child->scale_xyz, scale, sizeof(scale));
    mark_dirty(child);
    return 1;
}

/// @brief Move one existing direct child to a zero-based sibling index.
/// @details The complete retained child table is validated before mutation.
///          The move is stable for every other sibling and does not change
///          references, parent links, or scene ownership. An already-satisfied
///          index succeeds. Invalid relationships, indexes, or corrupt private
///          child slots fail without changing the hierarchy.
int8_t rt_scene_node3d_try_move_child(void *obj, void *child_obj, int64_t index) {
    rt_scene_node3d *parent = scene_node3d_checked(obj);
    rt_scene_node3d *child = scene_node3d_checked(child_obj);
    int32_t child_count;
    int32_t source_index = -1;
    int32_t target_index;

    if (!parent || !child || scene_node_ref(child->parent) != parent)
        return 0;
    child_count = scene3d_node_child_count(parent);
    parent->child_count = child_count;
    if (index < 0 || index >= child_count)
        return 0;
    target_index = (int32_t)index;

    for (int32_t i = 0; i < child_count; ++i) {
        rt_scene_node3d *candidate = scene_node_ref(parent->children[i]);
        if (!candidate || scene_node_ref(candidate->parent) != parent)
            return 0;
        if (candidate == child) {
            if (source_index >= 0)
                return 0;
            source_index = i;
        }
    }
    if (source_index < 0)
        return 0;
    if (source_index == target_index)
        return 1;

    rt_scene_node3d *retained_child = parent->children[source_index];
    if (source_index < target_index) {
        memmove(&parent->children[source_index],
                &parent->children[source_index + 1],
                (size_t)(target_index - source_index) * sizeof(parent->children[0]));
    } else {
        memmove(&parent->children[target_index + 1],
                &parent->children[target_index],
                (size_t)(source_index - target_index) * sizeof(parent->children[0]));
    }
    parent->children[target_index] = retained_child;
    if (parent->owner_scene)
        scene3d_mark_spatial_dirty(parent->owner_scene);
    mark_dirty(parent);
    return 1;
}

/// @brief Attach @p child_obj under @p obj, retaining it and propagating owner/dirty state.
/// @details Updates the child's parent link and owning scene, and marks the spatial index topology
///          dirty so the BVH rebuilds. Ignores cycles and invalid handles.
void rt_scene_node3d_add_child(void *obj, void *child_obj) {
    rt_scene_node3d *parent = scene_node3d_checked(obj);
    rt_scene_node3d *child = scene_node3d_checked(child_obj);
    (void)scene_node3d_try_add_child_impl(parent, child, 1);
}

/// @brief Detach `child` from `obj`. Decrements the GC refcount. No-op if not actually a child.
void rt_scene_node3d_remove_child(void *obj, void *child_obj) {
    rt_scene_node3d *parent = scene_node3d_checked(obj);
    rt_scene_node3d *child = scene_node3d_checked(child_obj);
    rt_scene3d *owner = parent ? parent->owner_scene : NULL;
    if (!parent || !child)
        return;

    parent->child_count = scene3d_node_child_count(parent);
    for (int32_t i = 0; i < parent->child_count; i++) {
        if (scene_node_ref(parent->children[i]) == child) {
            scene_owner_transaction owner_transaction;
            memset(&owner_transaction, 0, sizeof(owner_transaction));
            if (owner && !scene_node_owner_transaction_prepare(child, &owner_transaction)) {
                rt_trap("Scene3D: owner traversal stack allocation failed");
                return;
            }
            /* Shift remaining children down */
            for (int32_t j = i; j < parent->child_count - 1; j++)
                parent->children[j] = parent->children[j + 1];
            parent->child_count--;
            parent->children[parent->child_count] = NULL;
            child->parent = NULL;
            if (owner) {
                scene_node_owner_transaction_clear(&owner_transaction, owner);
                scene3d_mark_spatial_dirty(owner);
            }
            scene_node_owner_transaction_discard(&owner_transaction);
            mark_dirty(child);
            if (rt_obj_release_check0(child))
                rt_obj_free(child);
            return;
        }
    }
}

/// @brief Number of immediate (non-recursive) children attached to this node.
int64_t rt_scene_node3d_child_count(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return 0;
    node->child_count = scene3d_node_child_count(node);
    return node->child_count;
}

/// @brief Return the `index`-th child handle (NULL on out-of-range or NULL `obj`).
void *rt_scene_node3d_get_child(void *obj, int64_t index) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    int32_t child_count;
    if (!n)
        return NULL;
    child_count = scene3d_node_child_count(n);
    n->child_count = child_count;
    if (index < 0 || index >= child_count)
        return NULL;
    return scene_node_ref(n->children[(int32_t)index]);
}

/// @brief Parent node handle (NULL for root or detached nodes).
void *rt_scene_node3d_get_parent(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? scene_node_ref(node->parent) : NULL;
}

/// @brief Recursive depth-first search of the subtree for a node with the given name.
void *rt_scene_node3d_find(void *obj, rt_string name) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node || !name)
        return NULL;
    if (!rt_string_is_handle(name))
        return NULL;
    const char *s = rt_string_cstr(name);
    if (!s)
        return NULL;
    return find_by_name(node, s);
}

/// @brief Search a subtree by name and wrap the nullable result in an Option.
/// @details Keeps the same traversal and validation behavior as
///          `rt_scene_node3d_find`, while exposing absence through `None` for
///          modern public runtime callers.
/// @param obj SceneNode3D subtree root.
/// @param name Exact node name to match.
/// @return `Some(SceneNode3D)` when a matching descendant exists, otherwise `None`.
void *rt_scene_node3d_find_option(void *obj, rt_string name) {
    void *node = rt_scene_node3d_find(obj, name);
    return node ? rt_option_some(node) : rt_option_none();
}

/*==========================================================================
 * SceneNode3D — renderable / visibility / name
 *=========================================================================*/

/// @brief Bind a mesh to this node (replaces previous; null clears).
/// The mesh is referenced (not copied) so multiple nodes can share it.
void rt_scene_node3d_set_mesh(void *obj, void *mesh) {
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return;
    scene_node_repair_class_slot(&n->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (mesh && !rt_g3d_has_class(mesh, RT_G3D_MESH3D_CLASS_ID))
        return;
    if (n->mesh == mesh) {
        if (mesh)
            scene_node_refresh_mesh_bounds(n);
        scene3d_mark_spatial_dirty(n->owner_scene);
        return;
    }
    scene_node_assign_class_ref(&n->mesh, mesh, RT_G3D_MESH3D_CLASS_ID);

    /* Compute object-space AABB from mesh vertices */
    scene_node_refresh_mesh_bounds(n);
    scene3d_mark_spatial_dirty(n->owner_scene);
}

/// @brief Currently bound mesh handle (NULL if none).
void *rt_scene_node3d_get_mesh(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID) : NULL;
}

/// @brief Bind a material to this node (replaces previous; null clears).
void rt_scene_node3d_set_material(void *obj, void *material) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_class_slot(&node->material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (material && !rt_g3d_has_class(material, RT_G3D_MATERIAL3D_CLASS_ID))
        return;
    if (node->material == material)
        return;
    scene_node_assign_class_ref(&node->material, material, RT_G3D_MATERIAL3D_CLASS_ID);
}

/// @brief Currently bound material handle (NULL if none).
void *rt_scene_node3d_get_material(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->material, RT_G3D_MATERIAL3D_CLASS_ID) : NULL;
}

/// @brief Attach a Light3D to this node; Scene3D.Draw transforms it by the node world pose.
void rt_scene_node3d_set_light(void *obj, void *light) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_class_slot(&node->light, RT_G3D_LIGHT3D_CLASS_ID);
    if (light && !rt_g3d_has_class(light, RT_G3D_LIGHT3D_CLASS_ID))
        return;
    if (node->light == light)
        return;
    scene_node_assign_class_ref(&node->light, light, RT_G3D_LIGHT3D_CLASS_ID);
}

/// @brief Currently attached Light3D handle (NULL if this node has no imported/local light).
void *rt_scene_node3d_get_light(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->light, RT_G3D_LIGHT3D_CLASS_ID) : NULL;
}

/// @brief Attach a retained Camera3D whose view pose follows this node's world transform.
void rt_scene_node3d_set_camera(void *obj, void *camera) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_class_slot(&node->camera, RT_G3D_CAMERA3D_CLASS_ID);
    if (camera && !rt_g3d_has_class(camera, RT_G3D_CAMERA3D_CLASS_ID))
        return;
    if (node->camera == camera)
        return;
    scene_node_assign_class_ref(&node->camera, camera, RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Return the Camera3D attached to this node, or NULL on absence/class corruption.
void *rt_scene_node3d_get_camera(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->camera, RT_G3D_CAMERA3D_CLASS_ID) : NULL;
}

/// @brief Toggle whether this node participates in rendering.
void rt_scene_node3d_set_visible(void *obj, int8_t visible) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    int8_t next = visible ? 1 : 0;
    if (node) {
        if (node->visible == next)
            return;
        node->visible = next;
        /* Visibility is a per-entry filter in the spatial index, not topology:
         * request a refit so toggles stay O(changed paths) instead of forcing a
         * full O(n log n) rebuild per blink. */
        scene3d_mark_spatial_visibility_dirty(node->owner_scene);
    }
}

/// @brief Mark a node as static bake input (lightmaps/probes/reflection captures).
void rt_scene_node3d_set_static(void *obj, int8_t is_static) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (node)
        node->is_static = is_static ? 1 : 0;
}

/// @brief True when the node is flagged static for baking.
int8_t rt_scene_node3d_get_static(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node && node->is_static ? 1 : 0;
}

/// @brief Read the visibility flag (0 or 1; 0 if `obj` is NULL).
int8_t rt_scene_node3d_get_visible(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node && node->visible ? 1 : 0;
}

/// @brief Set the node's identifier name (used by `rt_scene_node3d_find`).
void rt_scene_node3d_set_name(void *obj, rt_string name) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_string_slot(&node->name);
    if (!name)
        name = rt_const_cstr("");
    if (!rt_string_is_handle(name))
        return;
    if (node->name == name)
        return;
    rt_obj_retain_maybe(name);
    scene_node_release_string_slot(&node->name);
    node->name = name;
}

/// @brief Read the node's name as an owned runtime-string reference.
/// @details Public string-return ABI callers release their result. Retaining the
///          stored slot here keeps repeated property reads from consuming the
///          node's own reference and turning later names into dangling handles.
rt_string rt_scene_node3d_get_name(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (node)
        scene_node_repair_string_slot(&node->name);
    if (node && node->name)
        return rt_string_ref(node->name);
    return rt_const_cstr("");
}

/// @brief VSCN v7 prefab source path; empty for ordinary nodes (ADR 0187).
rt_string rt_scene_node3d_get_prefab_path(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (node && node->prefab_path)
        return rt_string_ref(node->prefab_path);
    return rt_const_cstr("");
}

/// @brief True for nodes grafted from a prefab reference (ADR 0187).
int8_t rt_scene_node3d_get_is_instance_content(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node && node->is_instance_content ? 1 : 0;
}

/// @brief True for "/..." (POSIX) and "X:/" / "X:\\" / "\\\\server" (Windows) paths.
static int scene_node_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return 1;
    return 0;
}

/// @brief Author one portable prefab reference on a node (ADR 0187).
/// @details Absolute paths are rejected at authoring time so references stay
///          portable; loading still tolerates them with a diagnostic. The
///          call never grafts — reloading the owning scene resolves content.
int8_t rt_scene_node3d_set_prefab_reference(void *obj, rt_string path) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    const char *path_cstr = path ? rt_string_cstr(path) : NULL;
    if (!node || !path_cstr || !path_cstr[0])
        return 0;
    if (scene_node_path_is_absolute(path_cstr))
        return 0;
    scene_node_release_string_slot(&node->prefab_path);
    node->prefab_path = rt_string_ref(path);
    return 1;
}

/// @brief Recursively clear the transient instance-content flag (unpack).
static void scene_node_clear_instance_flags(rt_scene_node3d *node) {
    if (!node)
        return;
    node->is_instance_content = 0;
    for (int32_t i = 0; i < scene3d_node_child_count(node); ++i)
        scene_node_clear_instance_flags(scene_node3d_checked(node->children[i]));
}

/// @brief Remove a node's prefab reference, unpacking grafted content into
///        plain editable nodes (ADR 0187 Unpack).
int8_t rt_scene_node3d_clear_prefab_reference(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node || !node->prefab_path)
        return 0;
    scene_node_release_string_slot(&node->prefab_path);
    scene_node_clear_instance_flags(node);
    return 1;
}

/// @brief Local-space minimum corner of this node subtree's AABB (origin if empty).
void *rt_scene_node3d_get_aabb_min(void *obj) {
    double identity[16];
    float bounds_min[3];
    float bounds_max[3];
    int has_bounds;
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return rt_vec3_new(0, 0, 0);
    scene_bounds_reset(bounds_min, bounds_max);
    mat4d_identity(identity);
    has_bounds = scene_node_collect_subtree_bounds(n, identity, bounds_min, bounds_max);
    if (!has_bounds) {
        bounds_min[0] = bounds_min[1] = bounds_min[2] = 0.0f;
    }
    return rt_vec3_new(bounds_min[0], bounds_min[1], bounds_min[2]);
}

/// @brief Local-space maximum corner of this node subtree's AABB.
void *rt_scene_node3d_get_aabb_max(void *obj) {
    double identity[16];
    float bounds_min[3];
    float bounds_max[3];
    int has_bounds;
    rt_scene_node3d *n = scene_node3d_checked(obj);
    if (!n)
        return rt_vec3_new(0, 0, 0);
    scene_bounds_reset(bounds_min, bounds_max);
    mat4d_identity(identity);
    has_bounds = scene_node_collect_subtree_bounds(n, identity, bounds_min, bounds_max);
    if (!has_bounds) {
        bounds_max[0] = bounds_max[1] = bounds_max[2] = 0.0f;
    }
    return rt_vec3_new(bounds_max[0], bounds_max[1], bounds_max[2]);
}

/// @brief Link a physics rigid body to this node so transforms stay in sync (see `set_sync_mode`).
void rt_scene_node3d_bind_body(void *obj, void *body) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_class_slot(&node->bound_body, RT_G3D_BODY3D_CLASS_ID);
    if (body && !rt_g3d_has_class(body, RT_G3D_BODY3D_CLASS_ID))
        return;
    if (node->bound_body == body)
        return;
    scene_node_assign_class_ref(&node->bound_body, body, RT_G3D_BODY3D_CLASS_ID);
}

/// @brief Detach any bound rigid body. Subsequent `sync` calls on this node become no-ops.
void rt_scene_node3d_clear_body_binding(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_release_class_slot(&node->bound_body, RT_G3D_BODY3D_CLASS_ID);
}

/// @brief Attach this node to a skeletal bone: each SyncBindings pass drives the
///   node's world transform from parentWorld x bonePose x offset.
/// @details The bone pose is model-space, so the node should be parented under
///   the node that renders the skinned model. Socket sync supersedes any body
///   binding on the same node. Pass a negative bone index or NULL animator to
///   trap; use `rt_scene_node3d_detach_bone_socket` to remove the binding.
void rt_scene_node3d_attach_to_bone(void *obj,
                                    void *animator,
                                    int64_t bone_index,
                                    double offset_x,
                                    double offset_y,
                                    double offset_z) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    if (!animator || !rt_g3d_has_class(animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)) {
        rt_trap("SceneNode3D.AttachToBone: animator must be an AnimController3D");
        return;
    }
    if (bone_index < 0 || bone_index > INT32_MAX) {
        rt_trap("SceneNode3D.AttachToBone: bone index out of range");
        return;
    }
    if (!isfinite(offset_x) || !isfinite(offset_y) || !isfinite(offset_z)) {
        rt_trap("SceneNode3D.AttachToBone: offset must contain finite values");
        return;
    }
    scene_node_assign_class_ref(&node->socket_animator, animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    node->socket_bone = (int32_t)bone_index;
    node->socket_offset_pos[0] = offset_x;
    node->socket_offset_pos[1] = offset_y;
    node->socket_offset_pos[2] = offset_z;
    node->socket_offset_quat[0] = node->socket_offset_quat[1] = node->socket_offset_quat[2] = 0.0;
    node->socket_offset_quat[3] = 1.0;
}

/// @brief Remove any bone-socket binding; the node keeps its last transform.
void rt_scene_node3d_detach_bone_socket(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_release_class_slot(&node->socket_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    node->socket_bone = -1;
}

/// @brief Currently bound rigid body handle (NULL if none).
void *rt_scene_node3d_get_body(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->bound_body, RT_G3D_BODY3D_CLASS_ID) : NULL;
}

/// @brief Choose how this node and its bound body stay in sync each frame.
///
/// Modes: `NODE_FROM_BODY` (default — rigid-body sim drives the node),
/// `BODY_FROM_NODE` (kinematic — node animates the body),
/// `NODE_FROM_ANIMATOR_ROOT_MOTION` (root-motion driven), or
/// `TWO_WAY_KINEMATIC` (sync both directions per frame).
void rt_scene_node3d_set_sync_mode(void *obj, int64_t sync_mode) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    switch (sync_mode) {
        case RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY:
        case RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE:
        case RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION:
        case RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC:
            node->sync_mode = (int32_t)sync_mode;
            break;
        default:
            node->sync_mode = RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY;
            break;
    }
}

/// @brief Current node/body sync mode (`NODE_FROM_BODY` if `obj` is NULL).
int64_t rt_scene_node3d_get_sync_mode(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY;
    switch (node->sync_mode) {
        case RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY:
        case RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE:
        case RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION:
        case RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC:
            return node->sync_mode;
        default:
            return RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY;
    }
}

/// @brief Bind an animation controller to drive this node's transform / skeleton.
void rt_scene_node3d_bind_animator(void *obj, void *controller) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    scene_node_repair_class_slot(&node->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    if (controller && !rt_g3d_has_class(controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID))
        return;
    if (node->bound_animator == controller) {
        node->bound_animator_scene_update = 0;
        return;
    }
    scene_node_assign_class_ref(
        &node->bound_animator, controller, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    node->bound_animator_scene_update = 0;
}

/// @brief Control whether Scene3D.SyncBindings advances this node's skeletal controller.
void rt_scene_node3d_set_animator_scene_update(void *obj, int8_t enabled) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    if (!node)
        return;
    node->bound_animator_scene_update = enabled ? 1 : 0;
}

/// @brief Bind a NodeAnimator3D to this scene node so its clip channels are applied
///        each frame during `rt_scene3d_update`.
/// @details Retains the new animator and releases the old one. Crucially, the old
///   animator's `root` pointer is cleared to NULL before release so it cannot hold
///   a dangling reference to this node after the swap. The new animator's `root` is
///   set to this node immediately so `node_animator_update` can navigate the subtree
///   on the very next update tick. Passing NULL detaches the current animator, matching
///   the node-animation half of `rt_scene_node3d_clear_animator_binding`.
/// @param obj      Scene node to drive; no-op if NULL.
/// @param animator NodeAnimator3D handle, or NULL to detach.
void rt_scene_node3d_bind_node_animator(void *obj, void *animator) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    rt_node_animator3d *node_animator;
    if (!node)
        return;
    scene_node_repair_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
    if (animator && !rt_g3d_has_class(animator, RT_G3D_NODEANIMATOR3D_CLASS_ID))
        return;
    if (node->bound_node_animator == animator)
        return;
    rt_obj_retain_maybe(animator);
    node_animator = scene_node_animator_ref(animator);
    if (node_animator && node_animator->root && node_animator->root != node) {
        rt_scene_node3d *old_root = scene_node_ref(node_animator->root);
        if (!old_root) {
            node_animator->root = NULL;
        } else if (old_root->bound_node_animator == animator) {
            scene_node_release_class_slot(&old_root->bound_node_animator,
                                          RT_G3D_NODEANIMATOR3D_CLASS_ID);
        } else {
            node_animator->root = NULL;
        }
    }
    {
        rt_node_animator3d *old_node_animator = scene_node_animator_ref(node->bound_node_animator);
        if (old_node_animator)
            old_node_animator->root = NULL;
    }
    scene_node_release_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
    node->bound_node_animator = animator;
    if (node_animator)
        node_animator->root = node;
}

/// @brief Detach only the bound NodeAnimator3D from this scene node.
void rt_scene_node3d_clear_node_animator_binding(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    rt_node_animator3d *node_animator;
    if (!node)
        return;
    scene_node_repair_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
    node_animator = scene_node_animator_ref(node->bound_node_animator);
    if (node_animator)
        node_animator->root = NULL;
    scene_node_release_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
}

/// @brief Detach any bound skeletal or node animator. Subsequent frames stop applying motion.
void rt_scene_node3d_clear_animator_binding(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    rt_node_animator3d *node_animator;
    if (!node)
        return;
    scene_node_release_class_slot(&node->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID);
    node->bound_animator_scene_update = 0;
    scene_node_repair_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
    node_animator = scene_node_animator_ref(node->bound_node_animator);
    if (node_animator)
        node_animator->root = NULL;
    scene_node_release_class_slot(&node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID);
}

/// @brief Currently bound animation controller handle (NULL if none).
void *rt_scene_node3d_get_animator(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->bound_animator, RT_G3D_ANIMCONTROLLER3D_CLASS_ID)
                : NULL;
}

/// @brief Currently bound node animator handle (NULL if none).
void *rt_scene_node3d_get_node_animator(void *obj) {
    rt_scene_node3d *node = scene_node3d_checked(obj);
    return node ? rt_g3d_checked_or_null(node->bound_node_animator, RT_G3D_NODEANIMATOR3D_CLASS_ID)
                : NULL;
}

#endif /* ZANNA_ENABLE_GRAPHICS */
