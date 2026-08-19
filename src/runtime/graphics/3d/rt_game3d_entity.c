//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_entity.c
// Purpose: Entity3D for the Zanna.Game3D layer — node/mesh/material/body/animator
//   composition, parent/child hierarchy, transform, and collision layer/mask.
//   Split out of rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Key invariants:
//   - Public Entity3D entry points validate class id and liveness before touching
//     retained node/body/animator state.
//   - Stale entity handles return neutral values or no-op and increment
//     Game3D.Diagnostics.StaleEntityCalls.
// Ownership/Lifetime:
//   - Entity3D handles are GC-managed and retain owned node/mesh/material/body/
//     animator/name/child slots until finalization.
//   - World spawn/despawn controls the alive/spawned/world back-pointer state; a
//     retained entity never owns its World3D.
// Links: rt_game3d_internal.h, rt_scene3d.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Game3D Entity3D composition, hierarchy, transforms, and physics binding.
/// @details Entity3D coordinates retained scene, render, animation, behavior,
///          combat, and physics components around one SceneNode3D. The API
///          preserves fluent return semantics, propagates selected properties to
///          the node/body, rejects stale handles, and performs transactional
///          rollback when hierarchy or physics registration cannot complete.

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_ragdoll3d.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_skeleton3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Push an opaque SceneNode3D handle onto a growable traversal stack.
/// @param[in,out] stack_io Address of the heap stack pointer.
/// @param[in,out] count_io Address of the occupied-slot count.
/// @param[in,out] capacity_io Address of the allocated-slot count.
/// @param node Node to append; NULL is treated as a successful no-op.
/// @return Non-zero on success, or zero when capacity growth overflows or allocation fails.
static int game3d_node_stack_push(void ***stack_io,
                                  size_t *count_io,
                                  size_t *capacity_io,
                                  void *node) {
    void **stack = *stack_io;
    size_t count = *count_io;
    size_t capacity = *capacity_io;
    if (!node)
        return 1;
    if (count >= capacity) {
        size_t new_capacity = capacity > 0 ? capacity * 2u : 32u;
        void **grown;
        if (new_capacity <= capacity || new_capacity > SIZE_MAX / sizeof(void *))
            return 0;
        grown = (void **)realloc(stack, new_capacity * sizeof(void *));
        if (!grown)
            return 0;
        stack = grown;
        capacity = new_capacity;
    }
    stack[count++] = node;
    *stack_io = stack;
    *count_io = count;
    *capacity_io = capacity;
    return 1;
}

/// @brief Hash-set sidecar used while preflighting a scene-node traversal.
typedef struct game3d_node_seen_set {
    void **slots;
    size_t count;
    size_t capacity;
} game3d_node_seen_set;

/// @brief Produce a stable cross-platform hash for an aligned node pointer.
/// @param node Non-null SceneNode3D pointer.
/// @return Mixed pointer bits suitable for a power-of-two open-addressed table.
static size_t game3d_node_pointer_hash(void *node) {
    uintptr_t bits = (uintptr_t)node;
    bits ^= bits >> 16;
    bits *= (uintptr_t)UINT32_C(0x7FEB352D);
    bits ^= bits >> 15;
    return (size_t)bits;
}

/// @brief Grow and rehash a traversal's seen-node table.
/// @param[in,out] seen Seen-set storage to grow.
/// @param minimum_capacity Minimum requested power-of-two capacity.
/// @return Non-zero on success, otherwise zero without altering the old table.
static int game3d_node_seen_reserve(game3d_node_seen_set *seen, size_t minimum_capacity) {
    size_t capacity;
    void **slots;
    if (!seen)
        return 0;
    capacity = seen->capacity > 0 ? seen->capacity : 64u;
    while (capacity < minimum_capacity) {
        if (capacity > SIZE_MAX / 2u)
            return 0;
        capacity *= 2u;
    }
    if (capacity == seen->capacity)
        return 1;
    if (capacity > SIZE_MAX / sizeof(void *))
        return 0;
    slots = (void **)calloc(capacity, sizeof(void *));
    if (!slots)
        return 0;
    for (size_t i = 0; i < seen->capacity; ++i) {
        void *node = seen->slots[i];
        if (!node)
            continue;
        size_t index = game3d_node_pointer_hash(node) & (capacity - 1u);
        while (slots[index])
            index = (index + 1u) & (capacity - 1u);
        slots[index] = node;
    }
    free(seen->slots);
    seen->slots = slots;
    seen->capacity = capacity;
    return 1;
}

/// @brief Record a node once in a traversal seen set.
/// @param[in,out] seen Seen set receiving @p node.
/// @param node Non-null SceneNode3D handle.
/// @param[out] inserted Set to one for a new node, zero for a duplicate.
/// @return Non-zero on success, or zero on allocation/overflow failure.
static int game3d_node_seen_insert(game3d_node_seen_set *seen, void *node, int *inserted) {
    size_t index;
    if (!seen || !node || !inserted)
        return 0;
    *inserted = 0;
    if (seen->capacity == 0 || seen->count >= seen->capacity / 2u) {
        size_t requested = seen->capacity > 0 ? seen->capacity * 2u : 64u;
        if (requested < seen->capacity || !game3d_node_seen_reserve(seen, requested))
            return 0;
    }
    index = game3d_node_pointer_hash(node) & (seen->capacity - 1u);
    while (seen->slots[index]) {
        if (seen->slots[index] == node)
            return 1;
        index = (index + 1u) & (seen->capacity - 1u);
    }
    seen->slots[index] = node;
    seen->count++;
    *inserted = 1;
    return 1;
}

/// @brief Collect a complete, duplicate-free SceneNode3D subtree before mutation.
/// @details Breadth-first growth makes allocation failure transactional: callers
///          receive every node or none and can then perform an infallible apply pass.
/// @param root Valid SceneNode3D root to traverse.
/// @param[out] nodes_out Owned node-pointer array on success.
/// @param[out] count_out Number of collected nodes.
/// @return Non-zero on success; zero for malformed graphs or allocation failure.
static int game3d_entity_collect_nodes(void *root, void ***nodes_out, size_t *count_out) {
    void **nodes = NULL;
    size_t count = 0;
    size_t capacity = 0;
    game3d_node_seen_set seen;
    if (!nodes_out || !count_out ||
        !rt_obj_is_instance(root, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d)))
        return 0;
    memset(&seen, 0, sizeof(seen));
    int inserted = 0;
    if (!game3d_node_seen_insert(&seen, root, &inserted) ||
        !game3d_node_stack_push(&nodes, &count, &capacity, root))
        goto fail;
    for (size_t read = 0; read < count; ++read) {
        int64_t child_count = rt_scene_node3d_child_count(nodes[read]);
        if (child_count < 0)
            goto fail;
        for (int64_t i = 0; i < child_count; ++i) {
            void *child = rt_scene_node3d_get_child(nodes[read], i);
            if (!rt_obj_is_instance(child, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d)))
                goto fail;
            if (!game3d_node_seen_insert(&seen, child, &inserted))
                goto fail;
            if (inserted && !game3d_node_stack_push(&nodes, &count, &capacity, child))
                goto fail;
        }
    }
    free(seen.slots);
    *nodes_out = nodes;
    *count_out = count;
    return 1;

fail:
    free(seen.slots);
    free(nodes);
    return 0;
}

/// @brief Release a retained runtime-string slot, clearing wrong-kind corruption unowned.
/// @param[in,out] slot Address of an owned runtime-string slot.
static void game3d_entity_release_string_slot(rt_string *slot) {
    if (!slot || !*slot)
        return;
    if (!rt_string_is_handle(*slot)) {
        *slot = NULL;
        return;
    }
    game3d_release_ref((void **)slot);
}

/// @brief Repair an entity name to a valid retained runtime string.
/// @param entity Entity whose private name slot is normalized.
/// @return Valid retained name, or the shared empty string for a NULL entity.
static rt_string game3d_entity_repair_name(rt_game3d_entity *entity) {
    if (!entity)
        return rt_const_cstr("");
    if (entity->name && rt_string_is_handle(entity->name))
        return entity->name;
    entity->name = NULL;
    rt_string empty = rt_const_cstr("");
    rt_obj_retain_maybe(empty);
    entity->name = empty;
    if (entity->spawned &&
        rt_obj_is_instance(entity->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world)))
        ((rt_game3d_world *)entity->world)->name_index_valid = 0;
    return entity->name;
}

/// @brief GC finalizer for an entity: release child entities, node/mesh/material/body/anim/name
///   references it owns, then free the child array.
/// @param obj Entity3D storage being finalized; NULL is ignored.
static void game3d_entity_finalize(void *obj) {
    rt_game3d_entity *entity = (rt_game3d_entity *)obj;
    if (!entity)
        return;
    int storage_is_owned =
        entity->children && entity->child_storage_capacity > 0 &&
        entity->child_storage_cookie == game3d_entity_child_storage_cookie_value(
                                            entity->children, entity->child_storage_capacity);
    if (storage_is_owned) {
        for (int32_t i = 0; i < entity->child_storage_capacity; ++i) {
            rt_game3d_entity *child = rt_obj_is_instance(entity->children[i],
                                                         RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                         sizeof(rt_game3d_entity))
                                          ? entity->children[i]
                                          : NULL;
            if (!child || child->parent != entity) {
                entity->children[i] = NULL;
                continue;
            }
            child->parent = NULL;
            game3d_release_typed_ref((void **)&entity->children[i], RT_G3D_GAME3D_ENTITY_CLASS_ID);
        }
        free(entity->children);
    }
    entity->children = NULL;
    entity->child_count = 0;
    entity->child_capacity = 0;
    entity->child_storage_capacity = 0;
    entity->child_storage_cookie = 0;
    game3d_release_typed_ref(&entity->node, RT_G3D_SCENENODE3D_CLASS_ID);
    game3d_release_typed_ref(&entity->mesh, RT_G3D_MESH3D_CLASS_ID);
    game3d_release_typed_ref(&entity->material, RT_G3D_MATERIAL3D_CLASS_ID);
    game3d_release_typed_ref(&entity->body, RT_G3D_BODY3D_CLASS_ID);
    game3d_release_typed_ref(&entity->anim, RT_G3D_GAME3D_ANIMATOR3D_CLASS_ID);
    game3d_release_typed_ref(&entity->behavior, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID);
    game3d_entity_release_combat_slots(entity);
    game3d_release_typed_ref(&entity->ragdoll, RT_G3D_RAGDOLL3D_CLASS_ID);
    {
        rt_game3d_lipsync *lipsync_slot = rt_obj_is_instance(entity->lipsync,
                                                             RT_G3D_GAME3D_LIPSYNC_CLASS_ID,
                                                             sizeof(rt_game3d_lipsync))
                                              ? (rt_game3d_lipsync *)entity->lipsync
                                              : NULL;
        if (lipsync_slot)
            lipsync_slot->entity = NULL;
    }
    game3d_release_typed_ref(&entity->lipsync, RT_G3D_GAME3D_LIPSYNC_CLASS_ID);
    game3d_release_typed_ref(&entity->footsteps, RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID);
    game3d_release_typed_ref(&entity->interactable, RT_G3D_GAME3D_INTERACTABLE_CLASS_ID);
    game3d_release_typed_ref(&entity->interactor, RT_G3D_GAME3D_INTERACTOR_CLASS_ID);
    game3d_release_typed_ref(&entity->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
    game3d_release_typed_ref(&entity->btree, RT_G3D_GAME3D_BTINSTANCE_CLASS_ID);
    game3d_entity_release_string_slot(&entity->persistent_key);
    game3d_entity_release_string_slot(&entity->name);
}

/// @brief Allocate a bare entity wrapping a fresh scene node, on the DYNAMIC layer
///   colliding with everything; installs the finalizer and traps on OOM.
/// @return A newly allocated live Entity3D retaining a SceneNode3D and empty
///         name, or NULL after entity/node allocation failure.
void *rt_game3d_entity_new(void) {
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, (int64_t)sizeof(*entity));
    if (!entity) {
        rt_trap("Game3D.Entity3D.New: allocation failed");
        return NULL;
    }
    memset(entity, 0, sizeof(*entity));
    rt_obj_set_finalizer(entity, game3d_entity_finalize);
    entity->node = rt_scene_node3d_new();
    if (!entity->node) {
        if (rt_obj_release_check0(entity))
            rt_obj_free(entity);
        rt_trap("Game3D.Entity3D.New: node allocation failed");
        return NULL;
    }
    entity->layer = RT_GAME3D_LAYER_DYNAMIC;
    entity->collision_mask_bits = ~(int64_t)0;
    entity->registry_index = -1;
    entity->name = rt_const_cstr("");
    if (!entity->name) {
        if (rt_obj_release_check0(entity))
            rt_obj_free(entity);
        rt_trap("Game3D.Entity3D.New: name allocation failed");
        return NULL;
    }
    rt_obj_retain_maybe(entity->name);
    entity->alive = 1;
    return entity;
}

/// @brief Allocate an entity and assign the given mesh and material. See header.
/// @param mesh Mesh3D to retain and mirror onto the new node, or NULL.
/// @param material Material3D to retain and mirror onto the new node, or NULL.
/// @return A newly allocated Entity3D, or NULL when base entity allocation fails.
void *rt_game3d_entity_of(void *mesh, void *material) {
    if (mesh && !rt_obj_is_instance(mesh, RT_G3D_MESH3D_CLASS_ID, sizeof(rt_mesh3d))) {
        rt_trap("Game3D.Entity3D.Of: mesh must be Mesh3D");
        return NULL;
    }
    if (material &&
        !rt_obj_is_instance(material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))) {
        rt_trap("Game3D.Entity3D.Of: material must be Material3D");
        return NULL;
    }
    rt_game3d_entity *entity = (rt_game3d_entity *)rt_game3d_entity_new();
    if (!entity)
        return NULL;
    rt_game3d_entity_set_mesh(entity, mesh);
    rt_game3d_entity_set_material(entity, material);
    return entity;
}

/// @brief Wrap an existing SceneNode3D hierarchy as a group entity.
/// @details A detached node, or a non-root node already owned by a scene, is retained normally.
///   When @p root is a Scene3D's implicit root, the operation first allocates and preflights a
///   replacement empty root. It then atomically transfers the old root's scene-owned reference to
///   the new entity, clears the transferred subtree's owner links without further allocation, and
///   leaves the source scene valid and empty. This preserves the invariant that an implicit scene
///   root is never reparented while retaining the complete imported hierarchy and its bindings.
/// @param root SceneNode3D hierarchy root to wrap; must be a valid runtime SceneNode3D handle.
/// @return Newly allocated group Entity3D owning @p root, or NULL after a runtime trap on invalid
///   input or allocation/preflight failure.
void *rt_game3d_entity_from_node(void *root) {
    rt_scene_node3d *node;
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_scene3d *source_scene = NULL;
    rt_scene_node3d *replacement_root = NULL;
    scene_owner_transaction owner_transaction;
    int transfer_scene_root = 0;
    memset(&owner_transaction, 0, sizeof(owner_transaction));
#endif
    if (!rt_obj_is_instance(root, RT_G3D_SCENENODE3D_CLASS_ID, sizeof(rt_scene_node3d))) {
        rt_trap("Game3D.Entity3D.FromNode: root must be a SceneNode3D");
        return NULL;
    }
    node = (rt_scene_node3d *)root;
#ifdef ZANNA_ENABLE_GRAPHICS
    source_scene = scene3d_checked(node->owner_scene);
    transfer_scene_root = source_scene && scene_node3d_checked(source_scene->root) == node;
    if (transfer_scene_root) {
        if (node->parent) {
            rt_trap("Game3D.Entity3D.FromNode: scene root has an invalid parent");
            return NULL;
        }
        replacement_root = (rt_scene_node3d *)rt_scene_node3d_new();
        if (!replacement_root)
            return NULL;
        if (!scene_node_owner_transaction_prepare(node, &owner_transaction)) {
            game3d_release_ref((void **)&replacement_root);
            rt_trap("Game3D.Entity3D.FromNode: owner traversal preflight failed");
            return NULL;
        }
    }
#endif
    rt_game3d_entity *entity =
        (rt_game3d_entity *)rt_obj_new_i64(RT_G3D_GAME3D_ENTITY_CLASS_ID, (int64_t)sizeof(*entity));
    if (!entity) {
#ifdef ZANNA_ENABLE_GRAPHICS
        scene_node_owner_transaction_discard(&owner_transaction);
        game3d_release_ref((void **)&replacement_root);
#endif
        rt_trap("Game3D.Entity3D.FromNode: allocation failed");
        return NULL;
    }
    memset(entity, 0, sizeof(*entity));
    rt_obj_set_finalizer(entity, game3d_entity_finalize);
#ifdef ZANNA_ENABLE_GRAPHICS
    if (transfer_scene_root) {
        /* Every fallible operation completed above. Publish the replacement root and transfer the
         * source scene's existing ownership reference directly into the entity without a retain /
         * release window. */
        source_scene->root = replacement_root;
        replacement_root->owner_scene = source_scene;
        replacement_root = NULL;
        entity->node = node;
        scene_node_owner_transaction_clear(&owner_transaction, source_scene);
        scene_node_owner_transaction_discard(&owner_transaction);
        node->parent = NULL;
        source_scene->node_count = 1;
        source_scene->last_culled_count = 0;
        source_scene->last_visible_node_count = 0;
        source_scene->last_pvs_culled_count = 0;
        source_scene->last_portal_traversal_count = 0;
        scene3d_mark_spatial_dirty(source_scene);
        mark_dirty(node);
    } else
#endif
    {
        game3d_assign_typed_ref(&entity->node, root, RT_G3D_SCENENODE3D_CLASS_ID);
    }
    entity->layer = RT_GAME3D_LAYER_DYNAMIC;
    entity->collision_mask_bits = ~(int64_t)0;
    entity->registry_index = -1;
    entity->name = rt_const_cstr("");
    rt_obj_retain_maybe(entity->name);
    entity->alive = 1;
    {
        rt_string root_name = rt_scene_node3d_get_name(root);
        const char *root_name_cstr = root_name ? rt_string_cstr(root_name) : NULL;
        if (root_name_cstr && root_name_cstr[0] != '\0')
            game3d_assign_ref((void **)&entity->name, root_name);
        rt_string_unref(root_name);
    }
    entity->group = 1;
    return entity;
}

/// @brief Get the entity's stable id (0 if invalid or stale).
/// @param obj Entity3D runtime handle.
/// @return The positive world-assigned identifier, or zero before assignment or
///         for an invalid/stale entity.
int64_t rt_game3d_entity_get_id(void *obj) {
    rt_game3d_entity *entity = game3d_entity_checked(obj, "Game3D.Entity3D.get_Id: invalid entity");
    return entity && entity->id > 0 ? entity->id : 0;
}

/// @brief Get the entity's scene node (NULL if invalid).
/// @param obj Entity3D runtime handle.
/// @return The validated retained SceneNode3D pointer, or NULL.
void *rt_game3d_entity_get_node(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Node: invalid entity");
    return game3d_entity_node_ref(entity);
}

/// @brief Get the entity's mesh (NULL if none/invalid).
/// @param obj Entity3D runtime handle.
/// @return The validated retained Mesh3D pointer, or NULL.
void *rt_game3d_entity_get_mesh(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Mesh: invalid entity");
    return game3d_entity_mesh_ref(entity);
}

/// @brief Property setter for the mesh (delegates to the fluent setMesh).
/// @param obj Entity3D runtime handle.
/// @param mesh Mesh3D to retain, or NULL to clear the mesh.
void rt_game3d_entity_set_mesh_prop(void *obj, void *mesh) {
    (void)rt_game3d_entity_set_mesh(obj, mesh);
}

/// @brief Get the entity's material (NULL if none/invalid).
/// @param obj Entity3D runtime handle.
/// @return The validated retained Material3D pointer, or NULL.
void *rt_game3d_entity_get_material(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Material: invalid entity");
    return game3d_entity_material_ref(entity);
}

/// @brief Property setter for the material (delegates to the fluent setMaterial).
/// @param obj Entity3D runtime handle.
/// @param material Material3D to retain, or NULL to clear the material.
void rt_game3d_entity_set_material_prop(void *obj, void *material) {
    (void)rt_game3d_entity_set_material(obj, material);
}

/// @brief Get the entity's physics body (NULL if unattached/invalid).
/// @param obj Entity3D runtime handle.
/// @return The validated retained Physics3DBody pointer, or NULL.
void *rt_game3d_entity_get_body(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Body: invalid entity");
    return game3d_entity_body_ref(entity);
}

/// @brief Get the entity's animator (NULL if none/invalid).
/// @param obj Entity3D runtime handle.
/// @return The validated retained Animator3D pointer, or NULL.
void *rt_game3d_entity_get_anim(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Anim: invalid entity");
    return game3d_entity_anim_ref(entity);
}

/// @brief Get the entity's collision layer (0 if invalid).
/// @param obj Entity3D runtime handle.
/// @return The stored single-bit layer, the dynamic fallback for corrupt state,
///         or zero for an invalid/stale entity.
int64_t rt_game3d_entity_get_layer(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Layer: invalid entity");
    if (!entity)
        return 0;
    if (!game3d_valid_layer(entity->layer)) {
        entity->layer = RT_GAME3D_LAYER_DYNAMIC;
        void *body = game3d_entity_body_ref(entity);
        if (body)
            rt_body3d_set_collision_layer(body, entity->layer);
    }
    return entity->layer;
}

/// @brief Property setter for the collision layer (delegates to setLayer).
/// @param obj Entity3D runtime handle.
/// @param layer Positive single-bit collision layer.
void rt_game3d_entity_set_layer_prop(void *obj, int64_t layer) {
    (void)rt_game3d_entity_set_layer(obj, layer);
}

/// @brief Get a fresh LayerMask reflecting the entity's collision mask bits.
/// @param obj Entity3D runtime handle.
/// @return A newly allocated LayerMask copy, or NULL for an invalid/stale entity
///         or allocation failure.
void *rt_game3d_entity_get_collision_mask(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_CollisionMask: invalid entity");
    return entity ? game3d_layermask_new_bits(entity->collision_mask_bits) : NULL;
}

/// @brief Property setter for the collision mask (delegates to setCollisionMask).
/// @param obj Entity3D runtime handle.
/// @param mask LayerMask whose bits are copied into the entity and body.
void rt_game3d_entity_set_collision_mask_prop(void *obj, void *mask) {
    (void)rt_game3d_entity_set_collision_mask(obj, mask);
}

/// @brief Get the entity's name, or "" if unset/invalid.
/// @param obj Entity3D runtime handle.
/// @return The retained runtime name string, or an empty runtime string.
rt_string rt_game3d_entity_get_name(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_Name: invalid entity");
    return game3d_entity_repair_name(entity);
}

/// @brief Property setter for the name (delegates to the fluent setName).
/// @param obj Entity3D runtime handle.
/// @param name Runtime string to retain; NULL is normalized to empty.
void rt_game3d_entity_set_name_prop(void *obj, rt_string name) {
    (void)rt_game3d_entity_set_name(obj, name);
}

/// @brief Fluent: set local position (updating the node and any attached body) and
///   return the entity.
/// @param obj Entity3D runtime handle.
/// @param x Requested local X coordinate.
/// @param y Requested local Y coordinate.
/// @param z Requested local Z coordinate.
/// @return @p obj for fluent chaining, including invalid/stale inputs.
void *rt_game3d_entity_set_position(void *obj, double x, double y, double z) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setPosition: invalid entity");
    void *node = game3d_entity_node_ref(entity);
    if (node)
        rt_scene_node3d_set_position(node,
                                     game3d_clamp_coord_or(x, 0.0),
                                     game3d_clamp_coord_or(y, 0.0),
                                     game3d_clamp_coord_or(z, 0.0));
    game3d_sync_body_from_entity_node(entity, 0);
    return obj;
}

/// @brief Fluent: set local position from a Vec3; traps if `position` is not a Vec3.
/// @param obj Entity3D runtime handle.
/// @param position Vec3 supplying local coordinates.
/// @return @p obj for fluent chaining; invalid vectors leave the transform unchanged.
void *rt_game3d_entity_set_position_v(void *obj, void *position) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setPositionV: invalid entity");
    if (!entity)
        return obj;
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Game3D.Entity3D.setPositionV: position must be Vec3");
        return obj;
    }
    void *node = game3d_entity_node_ref(entity);
    if (node)
        rt_scene_node3d_set_position(node,
                                     game3d_clamp_coord_or(rt_vec3_x(position), 0.0),
                                     game3d_clamp_coord_or(rt_vec3_y(position), 0.0),
                                     game3d_clamp_coord_or(rt_vec3_z(position), 0.0));
    game3d_sync_body_from_entity_node(entity, 0);
    return obj;
}

/// @brief Fluent: set a uniform scale and return the entity.
/// @param obj Entity3D runtime handle.
/// @param scale Uniform local scale; invalid/near-zero values normalize through
///              the shared scale sanitizer.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_scale(void *obj, double scale) {
    return rt_game3d_entity_set_scale_xyz(obj, scale, scale, scale);
}

/// @brief Fluent: set a non-uniform XYZ scale on the node and return the entity.
/// @param obj Entity3D runtime handle.
/// @param x Local X scale.
/// @param y Local Y scale.
/// @param z Local Z scale.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_scale_xyz(void *obj, double x, double y, double z) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setScaleXYZ: invalid entity");
    x = game3d_scale_or_unit(x);
    y = game3d_scale_or_unit(y);
    z = game3d_scale_or_unit(z);
    void *node = game3d_entity_node_ref(entity);
    if (node)
        rt_scene_node3d_set_scale(node, x, y, z);
    game3d_sync_body_from_entity_node(entity, 0);
    return obj;
}

/// @brief Fluent: set rotation from Euler angles (degrees), converting to a quaternion;
///   returns the entity.
/// @param obj Entity3D runtime handle.
/// @param x_deg Local X-axis rotation in degrees.
/// @param y_deg Local Y-axis rotation in degrees.
/// @param z_deg Local Z-axis rotation in degrees.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_rotation_euler(void *obj, double x_deg, double y_deg, double z_deg) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setRotationEuler: invalid entity");
    void *node = game3d_entity_node_ref(entity);
    if (node) {
        const double deg = 3.14159265358979323846 / 180.0;
        x_deg = game3d_clamp_abs_or(x_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        y_deg = game3d_clamp_abs_or(y_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        z_deg = game3d_clamp_abs_or(z_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        void *quat = rt_quat_from_euler(x_deg * deg, y_deg * deg, z_deg * deg);
        if (quat)
            rt_scene_node3d_set_rotation(node, quat);
        game3d_release_ref(&quat);
    }
    game3d_sync_body_from_entity_node(entity, 0);
    return obj;
}

/// @brief Fluent: assign the mesh (validated as Mesh3D), mirror it onto the node, and
///   return the entity.
/// @param obj Entity3D runtime handle.
/// @param mesh Mesh3D to retain, or NULL to clear the entity and node mesh.
/// @return @p obj for fluent chaining; wrong runtime classes trap without mutation.
void *rt_game3d_entity_set_mesh(void *obj, void *mesh) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setMesh: invalid entity");
    if (!entity)
        return obj;
    if (mesh && !rt_obj_is_instance(mesh, RT_G3D_MESH3D_CLASS_ID, sizeof(rt_mesh3d))) {
        rt_trap("Game3D.Entity3D.setMesh: mesh must be Mesh3D");
        return obj;
    }
    if (entity) {
        void *node = game3d_entity_node_ref(entity);
        game3d_assign_typed_ref(&entity->mesh, mesh, RT_G3D_MESH3D_CLASS_ID);
        if (node)
            rt_scene_node3d_set_mesh(node, mesh);
    }
    return obj;
}

/// @brief Fluent: assign the material (validated as Material3D), mirror it onto the
///   node, and return the entity.
/// @param obj Entity3D runtime handle.
/// @param material Material3D to retain, or NULL to clear the entity and node material.
/// @return @p obj for fluent chaining; wrong runtime classes trap without mutation.
void *rt_game3d_entity_set_material(void *obj, void *material) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setMaterial: invalid entity");
    if (!entity)
        return obj;
    if (material &&
        !rt_obj_is_instance(material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))) {
        rt_trap("Game3D.Entity3D.setMaterial: material must be Material3D");
        return obj;
    }
    if (entity) {
        void *node = game3d_entity_node_ref(entity);
        game3d_assign_typed_ref(&entity->material, material, RT_G3D_MATERIAL3D_CLASS_ID);
        if (node)
            rt_scene_node3d_set_material(node, material);
    }
    return obj;
}

/// @brief Assign `mesh` to every mesh-bearing node in the subtree rooted at `root`,
///   walked as an iterative depth-first traversal over an explicit heap stack (avoids
///   C-stack overflow on deep hierarchies). If no node in the subtree carries a mesh,
///   the mesh is assigned to `root` as a fallback. Traps when traversal preflight fails.
/// @param root SceneNode3D at the root of the traversal; NULL is a no-op.
/// @param mesh Mesh3D value assigned to matching nodes; may be NULL to clear them.
/// @return Count of nodes that received the mesh (>= 1 when `root` is non-NULL).
static int game3d_entity_set_mesh_subtree(void *root, void *mesh) {
    void **nodes = NULL;
    size_t count = 0;
    int assigned = 0;
    if (!root)
        return 0;
    if (!game3d_entity_collect_nodes(root, &nodes, &count)) {
        rt_trap("Game3D.Entity3D.setMeshRecursive: traversal preflight failed");
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (rt_scene_node3d_get_mesh(nodes[i])) {
            rt_scene_node3d_set_mesh(nodes[i], mesh);
            assigned++;
        }
    }
    if (!assigned) {
        rt_scene_node3d_set_mesh(root, mesh);
        assigned = 1;
    }
    free(nodes);
    return assigned;
}

/// @brief Assign `material` to every node in the subtree rooted at `root`, walked as an
///   iterative depth-first traversal over an explicit heap stack (avoids C-stack overflow
///   on deep hierarchies). Traps when traversal preflight fails.
/// @param root SceneNode3D at the root of the traversal; NULL is a no-op.
/// @param material Material3D value assigned to every visited node; may be NULL.
static int game3d_entity_set_material_subtree(void *root, void *material) {
    void **nodes = NULL;
    size_t count = 0;
    if (!root)
        return 1;
    if (!game3d_entity_collect_nodes(root, &nodes, &count)) {
        rt_trap("Game3D.Entity3D.setMaterialRecursive: traversal preflight failed");
        return 0;
    }
    for (size_t i = 0; i < count; ++i)
        rt_scene_node3d_set_material(nodes[i], material);
    free(nodes);
    return 1;
}

/// @brief Fluent: assign the mesh (validated as Mesh3D) to the entity and propagate it
///   to every mesh-bearing node of its scene-node subtree; returns the entity.
/// @param obj Entity3D runtime handle.
/// @param mesh Mesh3D to retain and propagate, or NULL to clear matching nodes.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_mesh_recursive(void *obj, void *mesh) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setMeshRecursive: invalid entity");
    if (!entity)
        return obj;
    if (mesh && !rt_obj_is_instance(mesh, RT_G3D_MESH3D_CLASS_ID, sizeof(rt_mesh3d))) {
        rt_trap("Game3D.Entity3D.setMeshRecursive: mesh must be Mesh3D");
        return obj;
    }
    if (game3d_entity_set_mesh_subtree(game3d_entity_node_ref(entity), mesh) >= 0)
        game3d_assign_typed_ref(&entity->mesh, mesh, RT_G3D_MESH3D_CLASS_ID);
    return obj;
}

/// @brief Fluent: assign the material (validated as Material3D) to the entity and
///   propagate it to every node of its scene-node subtree; returns the entity.
/// @param obj Entity3D runtime handle.
/// @param material Material3D to retain and propagate, or NULL to clear the subtree.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_material_recursive(void *obj, void *material) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setMaterialRecursive: invalid entity");
    if (!entity)
        return obj;
    if (material &&
        !rt_obj_is_instance(material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))) {
        rt_trap("Game3D.Entity3D.setMaterialRecursive: material must be Material3D");
        return obj;
    }
    if (game3d_entity_set_material_subtree(game3d_entity_node_ref(entity), material))
        game3d_assign_typed_ref(&entity->material, material, RT_G3D_MATERIAL3D_CLASS_ID);
    return obj;
}

/// @brief Restore a previously valid body after attachBody failed partway through.
/// @details Reinstates layer/mask and node binding, and for spawned entities
///          ensures both the physics world and body index contain the prior body.
///          A failed rollback clears the body slot and any node binding.
/// @param entity Entity whose prior binding is restored.
/// @param world Owning World3D, used only for spawned physics registration.
/// @param old_body Previously retained Physics3DBody, or NULL to restore no body.
/// @return Non-zero when the old state was restored, otherwise zero.
static int game3d_entity_restore_body_binding(rt_game3d_entity *entity,
                                              rt_game3d_world *world,
                                              void *old_body) {
    void *node = game3d_entity_node_ref(entity);
    if (!entity)
        return 0;
    game3d_assign_typed_ref(&entity->body, old_body, RT_G3D_BODY3D_CLASS_ID);
    if (!old_body) {
        if (node)
            rt_scene_node3d_clear_body_binding(node);
        return 1;
    }
    rt_body3d_set_collision_layer(old_body, entity->layer);
    rt_body3d_set_collision_mask(old_body, entity->collision_mask_bits);
    if (node)
        rt_scene_node3d_bind_body(node, old_body);
    if (entity->spawned && world && world->physics) {
        int old_body_already_in_world = rt_world3d_contains_body(world->physics, old_body) ? 1 : 0;
        int restored_old_body = old_body_already_in_world;
        if (!restored_old_body)
            restored_old_body = rt_world3d_try_add(world->physics, old_body);
        if (!restored_old_body || !game3d_world_body_index_add(world, entity)) {
            if (!old_body_already_in_world)
                rt_world3d_remove(world->physics, old_body);
            if (node)
                rt_scene_node3d_clear_body_binding(node);
            game3d_assign_typed_ref(&entity->body, NULL, RT_G3D_BODY3D_CLASS_ID);
            return 0;
        }
    }
    return 1;
}

/// @brief Restore a child's previous entity/node parent after a failed spawn step.
/// @details The retain currently owned by @p new_parent's child slot is transferred
///          back to the old parent when possible, or released exactly once.
/// @param new_parent Parent whose just-added relationship is rolled back.
/// @param child Child being restored.
/// @param old_parent Previously consistent Entity3D parent, or NULL.
/// @param old_index Previous dense-array position within @p old_parent.
/// @param old_scene_parent Previous SceneNode3D parent, or NULL for a detached node.
static void game3d_entity_restore_reparent(rt_game3d_entity *new_parent,
                                           rt_game3d_entity *child,
                                           rt_game3d_entity *old_parent,
                                           int32_t old_index,
                                           void *old_scene_parent) {
    int transferred = 0;
    if (!new_parent || !child)
        return;
    int32_t new_index = game3d_entity_find_child_index(new_parent, child);
    if (new_index >= 0) {
        int32_t count = game3d_entity_child_count(new_parent);
        new_parent->child_count = count;
        for (int32_t i = new_index; i < count - 1; ++i)
            new_parent->children[i] = new_parent->children[i + 1];
        new_parent->children[--new_parent->child_count] = NULL;
    }
    child->parent = NULL;

    void *new_node = game3d_entity_node_ref(new_parent);
    void *child_node = game3d_entity_node_ref(child);
    if (new_node && child_node && rt_scene_node3d_get_parent(child_node) == new_node)
        rt_scene_node3d_remove_child(new_node, child_node);
    if (old_scene_parent && child_node &&
        !rt_scene_node3d_try_add_child(old_scene_parent, child_node))
        rt_trap("Game3D.Entity3D.addChild: failed to restore previous scene parent");

    if (old_parent && old_index >= 0 &&
        rt_obj_is_instance(old_parent, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity))) {
        int32_t count = game3d_entity_child_count(old_parent);
        if (count < INT32_MAX && game3d_entity_grow_children(old_parent, count + 1)) {
            if (old_index > count)
                old_index = count;
            for (int32_t i = count; i > old_index; --i)
                old_parent->children[i] = old_parent->children[i - 1];
            old_parent->children[old_index] = child;
            old_parent->child_count = count + 1;
            child->parent = old_parent;
            transferred = 1;
        }
    }
    if (!transferred) {
        void *owned_child = child;
        game3d_release_ref(&owned_child);
    }
}

/// @brief Fluent: parent `child_obj` under this entity (retaining it and linking the
///   nodes), mark this entity a group, and return it.
/// @details Rejects self-parenting, cycles, destroyed entities, and inconsistent
///          spawned-world relationships. When the parent is spawned, an unspawned
///          child subtree is registered transactionally; any node or world-spawn
///          failure rolls back the entity and scene-node links.
/// @param obj Parent Entity3D runtime handle.
/// @param child_obj Child Entity3D to retain and reparent.
/// @return @p obj for fluent chaining, including rejected operations.
void *rt_game3d_entity_add_child(void *obj, void *child_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.addChild: invalid entity");
    if (!entity)
        return obj;
    rt_game3d_entity *child =
        game3d_entity_checked(child_obj, "Game3D.Entity3D.addChild: child must be Entity3D");
    if (!child)
        return obj;
    if (entity == child) {
        rt_trap("Game3D.Entity3D.addChild: entity cannot be its own child");
        return obj;
    }
    if (!entity->alive || !child->alive) {
        rt_trap("Game3D.Entity3D.addChild: destroyed entities cannot be parented");
        return obj;
    }
    if (game3d_entity_has_ancestor(entity, child)) {
        rt_trap("Game3D.Entity3D.addChild: parenting would create a cycle");
        return obj;
    }
    if (child->parent == entity || game3d_entity_find_child_index(entity, child) >= 0)
        return obj;
    entity->spawned = entity->spawned ? 1 : 0;
    child->spawned = child->spawned ? 1 : 0;
    if ((entity->spawned && !rt_obj_is_instance(entity->world,
                                                RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                sizeof(rt_game3d_world))) ||
        (child->spawned && !rt_obj_is_instance(child->world,
                                               RT_G3D_GAME3D_WORLD_CLASS_ID,
                                               sizeof(rt_game3d_world)))) {
        rt_trap("Game3D.Entity3D.addChild: spawned entity has an invalid world");
        return obj;
    }
    if (entity->spawned && child->spawned && child->world != entity->world) {
        rt_trap("Game3D.Entity3D.addChild: child already belongs to another world");
        return obj;
    }
    if (!entity->spawned && child->spawned) {
        rt_trap(
            "Game3D.Entity3D.addChild: spawned child cannot be parented under an unspawned entity");
        return obj;
    }
    void *entity_node = game3d_entity_node_ref(entity);
    void *child_node = game3d_entity_node_ref(child);
    if (!entity_node || !child_node) {
        rt_trap("Game3D.Entity3D.addChild: both entities need valid scene nodes");
        return obj;
    }
    rt_game3d_entity *old_parent = NULL;
    int32_t old_index = -1;
    if (child->parent && rt_obj_is_instance(child->parent,
                                            RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                            sizeof(rt_game3d_entity))) {
        old_parent = child->parent;
        old_index = game3d_entity_find_child_index(old_parent, child);
        if (old_index < 0)
            old_parent = NULL;
    }
    if (!old_parent)
        child->parent = NULL;
    void *old_scene_parent = rt_scene_node3d_get_parent(child_node);

    entity->child_count = game3d_entity_child_count(entity);
    if (entity->child_count == INT32_MAX ||
        !game3d_entity_grow_children(entity, entity->child_count + 1)) {
        rt_trap("Game3D.Entity3D.addChild: allocation failed");
        return obj;
    }
    if (!rt_scene_node3d_try_add_child(entity_node, child_node)) {
        rt_trap("Game3D.Entity3D.addChild: scene-node parenting failed");
        return obj;
    }
    rt_obj_retain_maybe(child);
    game3d_entity_detach_from_parent(child);
    entity->children[entity->child_count++] = child;
    child->parent = entity;
    if (entity->spawned && entity->world && !child->spawned) {
        rt_game3d_world *world = (rt_game3d_world *)entity->world;
        int64_t next_id = world->next_entity_id;
        if (!game3d_world_spawn_entity_tree(world, child, 0, &next_id)) {
            game3d_entity_restore_reparent(entity, child, old_parent, old_index, old_scene_parent);
            return obj;
        }
        world->next_entity_id = next_id;
    }
    entity->group = 1;
    return obj;
}

/// @brief True if the entity is a group (explicitly flagged or has children).
/// @param obj Entity3D runtime handle.
/// @return Non-zero when explicitly grouped or currently owning children.
int8_t rt_game3d_entity_is_group(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.isGroup: invalid entity");
    return entity && (entity->group || game3d_entity_child_count(entity) > 0) ? 1 : 0;
}

/// @brief Fluent: assign the display name (NULL becomes ""), mirror it onto the node,
///   and return the entity.
/// @param obj Entity3D runtime handle.
/// @param name Runtime string to retain; NULL is normalized to empty.
/// @return @p obj for fluent chaining.
/// @post A spawned entity invalidates its world's name index.
void *rt_game3d_entity_set_name(void *obj, rt_string name) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setName: invalid entity");
    if (!name)
        name = rt_const_cstr("");
    if (!name || !rt_string_is_handle(name)) {
        rt_trap("Game3D.Entity3D.setName: name must be a runtime string");
        return obj;
    }
    if (entity) {
        if (entity->name && !rt_string_is_handle(entity->name))
            entity->name = NULL;
        game3d_assign_ref((void **)&entity->name, name);
        void *node = game3d_entity_node_ref(entity);
        if (node)
            rt_scene_node3d_set_name(node, name);
        if (entity->spawned && rt_obj_is_instance(entity->world,
                                                  RT_G3D_GAME3D_WORLD_CLASS_ID,
                                                  sizeof(rt_game3d_world)))
            ((rt_game3d_world *)entity->world)->name_index_valid = 0;
    }
    return obj;
}

/// @brief Fluent: set the collision layer (must be a single bit), propagate to the
///   body if any, and return the entity.
/// @param obj Entity3D runtime handle.
/// @param layer Positive single-bit collision layer.
/// @return @p obj for fluent chaining; invalid layers trap without mutation.
void *rt_game3d_entity_set_layer(void *obj, int64_t layer) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setLayer: invalid entity");
    if (!entity)
        return obj;
    if (!game3d_valid_layer(layer)) {
        rt_trap("Game3D.Entity3D.setLayer: layer must be a single positive bit");
        return obj;
    }
    if (entity) {
        entity->layer = layer;
        void *body = game3d_entity_body_ref(entity);
        if (body)
            rt_body3d_set_collision_layer(body, layer);
    }
    return obj;
}

/// @brief Fluent: copy a LayerMask's bits into the entity's collision mask, propagate
///   to the body if any, and return the entity.
/// @param obj Entity3D runtime handle.
/// @param mask_obj LayerMask whose bits are copied.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_set_collision_mask(void *obj, void *mask_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setCollisionMask: invalid entity");
    if (!entity)
        return obj;
    rt_game3d_layermask *mask =
        game3d_layermask_checked(mask_obj, "Game3D.Entity3D.setCollisionMask: invalid mask");
    if (entity && mask) {
        entity->collision_mask_bits = mask->bits;
        void *body = game3d_entity_body_ref(entity);
        if (body)
            rt_body3d_set_collision_mask(body, entity->collision_mask_bits);
    }
    return obj;
}

/// @brief Fluent: attach a physics body to the entity and return it.
/// @details Accepts either a ready Physics3DBody or a BodyDef (which is materialized
///   into a body here). Swaps out any previously attached body from the physics world,
///   adopts the def's layer/mask if present, seeds the body position from the node's
///   world position, binds node↔body with the def's sync mode, and re-adds the body to
///   the physics world when the entity is already spawned. Passing NULL clears the body
///   binding. Traps if `body_or_def` is neither a Physics3DBody nor a BodyDef.
/// @param obj Entity3D runtime handle.
/// @param body_or_def Existing Physics3DBody, BodyDef to materialize, or NULL
///                    to detach the current body.
/// @return @p obj for fluent chaining. Failed physics insertion or index
///         allocation restores the prior body, layer, mask, and node binding.
void *rt_game3d_entity_attach_body(void *obj, void *body_or_def) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.attachBody: invalid entity");
    if (!entity)
        return obj;
    void *body = body_or_def;
    void *created_body = NULL;
    rt_game3d_body_def *def =
        rt_obj_is_instance(body_or_def, RT_G3D_GAME3D_BODYDEF_CLASS_ID, sizeof(rt_game3d_body_def))
            ? (rt_game3d_body_def *)body_or_def
            : NULL;
    if (def) {
        created_body = game3d_body_def_create_body(def);
        body = created_body;
    } else if (body && !rt_g3d_has_class(body, RT_G3D_BODY3D_CLASS_ID)) {
        rt_trap("Game3D.Entity3D.attachBody: expected Physics3DBody or BodyDef");
        return obj;
    }
    if (entity) {
        rt_game3d_world *world =
            rt_obj_is_instance(entity->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world))
                ? (rt_game3d_world *)entity->world
                : NULL;
        if (entity->spawned && !world) {
            game3d_release_ref(&created_body);
            rt_trap("Game3D.Entity3D.attachBody: spawned entity has an invalid world");
            return obj;
        }
        void *old_body = game3d_entity_body_ref(entity);
        void *node = game3d_entity_node_ref(entity);
        int64_t old_layer = entity->layer;
        int64_t old_mask_bits = entity->collision_mask_bits;
        int body_is_old = body && body == old_body;
        rt_obj_retain_maybe(old_body);
        if (body && body != old_body && entity->spawned && world && world->physics) {
            rt_game3d_entity *owner = game3d_world_find_entity_by_body(world, body);
            if (owner && owner != entity) {
                game3d_release_ref(&old_body);
                game3d_release_ref(&created_body);
                rt_trap("Game3D.Entity3D.attachBody: body is already attached to another entity");
                return obj;
            }
        }
        if (old_body && old_body != body && entity->spawned && world && world->physics) {
            rt_world3d_remove(world->physics, old_body);
            game3d_world_body_index_remove(world, old_body);
        }
        if (def && def->has_layer)
            entity->layer = def->layer;
        if (def && def->has_mask)
            entity->collision_mask_bits = def->mask_bits;
        game3d_assign_typed_ref(&entity->body, body, RT_G3D_BODY3D_CLASS_ID);
        if (body) {
            rt_body3d_set_collision_layer(body, entity->layer);
            rt_body3d_set_collision_mask(body, entity->collision_mask_bits);
            if (node) {
                if (def)
                    rt_scene_node3d_set_sync_mode(node, def->sync_mode);
                rt_scene_node3d_bind_body(node, body);
                game3d_sync_body_from_entity_node(entity, 1);
            }
            if (entity->spawned && world && world->physics) {
                int body_in_world =
                    body_is_old && rt_world3d_contains_body(world->physics, body) ? 1 : 0;
                if (!body_in_world && !rt_world3d_try_add(world->physics, body)) {
                    if (node)
                        rt_scene_node3d_clear_body_binding(node);
                    entity->layer = old_layer;
                    entity->collision_mask_bits = old_mask_bits;
                    (void)game3d_entity_restore_body_binding(entity, world, old_body);
                    game3d_release_ref(&old_body);
                    game3d_release_ref(&created_body);
                    rt_trap("Game3D.Entity3D.attachBody: physics world add failed");
                    return obj;
                }
                if (!game3d_world_body_index_add(world, entity)) {
                    if (!body_in_world)
                        rt_world3d_remove(world->physics, body);
                    if (node)
                        rt_scene_node3d_clear_body_binding(node);
                    entity->layer = old_layer;
                    entity->collision_mask_bits = old_mask_bits;
                    (void)game3d_entity_restore_body_binding(entity, world, old_body);
                    game3d_release_ref(&old_body);
                    game3d_release_ref(&created_body);
                    rt_trap("Game3D.Entity3D.attachBody: body index allocation failed");
                    return obj;
                }
            }
        } else if (node) {
            rt_scene_node3d_clear_body_binding(node);
        }
        game3d_release_ref(&old_body);
    }
    game3d_release_ref(&created_body);
    return obj;
}

/// @brief Apply a linear impulse to the entity's body; traps if it has no body.
/// @param obj Entity3D runtime handle.
/// @param x World-space impulse X component.
/// @param y World-space impulse Y component.
/// @param z World-space impulse Z component.
/// @details Components are finite-clamped against a mass-scaled limit so the
///          resulting velocity change stays within the controller speed envelope.
void rt_game3d_entity_apply_impulse(void *obj, double x, double y, double z) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.applyImpulse: invalid entity");
    if (!entity)
        return;
    void *body = game3d_entity_body_ref(entity);
    if (!body) {
        rt_trap("Game3D.Entity3D.applyImpulse: entity has no body");
        return;
    }
    /* Bound the impulse in velocity-change terms (impulse = mass * delta-v), so
     * heavy bodies can receive proportionally larger momentum while the
     * resulting velocity change stays inside the same finite envelope as the
     * controller speed cap. */
    double mass = rt_body3d_get_mass(body);
    if (!isfinite(mass) || mass < 1.0)
        mass = 1.0;
    double impulse_max = mass * RT_GAME3D_CONTROLLER_SPEED_MAX;
    rt_body3d_apply_impulse(body,
                            game3d_clamp_abs_or(x, 0.0, impulse_max),
                            game3d_clamp_abs_or(y, 0.0, impulse_max),
                            game3d_clamp_abs_or(z, 0.0, impulse_max));
}

/// @brief Set the entity body's linear velocity; traps if it has no body.
/// @param obj Entity3D runtime handle.
/// @param x World-space velocity X component.
/// @param y World-space velocity Y component.
/// @param z World-space velocity Z component.
void rt_game3d_entity_set_velocity(void *obj, double x, double y, double z) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.setVelocity: invalid entity");
    if (!entity)
        return;
    void *body = game3d_entity_body_ref(entity);
    if (!body) {
        rt_trap("Game3D.Entity3D.setVelocity: entity has no body");
        return;
    }
    rt_body3d_set_velocity(body,
                           game3d_clamp_abs_or(x, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX),
                           game3d_clamp_abs_or(y, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX),
                           game3d_clamp_abs_or(z, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX));
}

/// @brief Get the entity's local position as a Vec3 (origin if no node).
/// @param obj Entity3D runtime handle.
/// @return A position Vec3 supplied by the node, or a newly allocated origin
///         vector when no valid node is available.
void *rt_game3d_entity_position(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.position: invalid entity");
    void *node = game3d_entity_node_ref(entity);
    return node ? rt_scene_node3d_get_position(node) : rt_vec3_new(0, 0, 0);
}

/// @brief Get the entity's world-space position as a Vec3 (origin if no node).
/// @param obj Entity3D runtime handle.
/// @return A world-position Vec3 supplied by the node, or a newly allocated
///         origin vector when no valid node is available.
void *rt_game3d_entity_world_position(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.worldPosition: invalid entity");
    void *node = game3d_entity_node_ref(entity);
    return node ? rt_scene_node3d_get_world_position(node) : rt_vec3_new(0, 0, 0);
}

/// @brief Read an entity's world-space position into out x/y/z (resolving body or node as
/// appropriate).
/// @param entity Entity whose live SceneNode3D transform is queried.
/// @param[out] out_pos Three-element destination, initialized to the origin
///                     before validation and finite-clamped on success.
/// @return 1 on success, 0 if the entity has no resolvable transform.
int game3d_entity_world_position_components(rt_game3d_entity *entity, double out_pos[3]) {
    int8_t ok;
    if (out_pos) {
        out_pos[0] = 0.0;
        out_pos[1] = 0.0;
        out_pos[2] = 0.0;
    }
    if (entity && !game3d_entity_alive_or_record(entity))
        return 0;
    void *node = game3d_entity_node_ref(entity);
    if (!node || !out_pos)
        return 0;
    ok = rt_scene_node3d_get_world_position_components(node, &out_pos[0], &out_pos[1], &out_pos[2]);
    if (!ok)
        return 0;
    out_pos[0] = game3d_clamp_coord_or(out_pos[0], 0.0);
    out_pos[1] = game3d_clamp_coord_or(out_pos[1], 0.0);
    out_pos[2] = game3d_clamp_coord_or(out_pos[2], 0.0);
    return 1;
}

/// @brief True if the entity is currently spawned into a world.
/// @param obj Entity3D runtime handle; destroyed entities remain queryable.
/// @return Non-zero when the spawned flag is set, otherwise zero.
int8_t rt_game3d_entity_is_spawned(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked_allow_destroyed(obj, "Game3D.Entity3D.isSpawned: invalid entity");
    if (entity)
        entity->spawned = entity->spawned ? 1 : 0;
    return entity ? entity->spawned : 0;
}

/// @brief True if the entity has been despawned/destroyed.
/// @param obj Entity3D runtime handle; destroyed entities remain queryable.
/// @return Non-zero when the destroyed flag is set, otherwise zero.
int8_t rt_game3d_entity_is_destroyed(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked_allow_destroyed(obj, "Game3D.Entity3D.isDestroyed: invalid entity");
    if (entity)
        entity->destroyed = entity->destroyed ? 1 : 0;
    return entity ? entity->destroyed : 0;
}

/// @brief Attach a child entity to a named bone of this entity's animated
///   skeleton with a positional offset in bone space.
/// @details Parents @p child_obj under this entity (scene-node parenting
///   included), then installs a bone socket on the child's node so every
///   simulation step drives the child's world transform from the bone's
///   composited pose. Requires this entity to have an attached Animator3D
///   whose controller has a skeleton containing @p bone_name; traps otherwise.
/// @param obj Parent Entity3D whose animator supplies the skeleton.
/// @param child_obj Child Entity3D to parent and socket.
/// @param bone_name Runtime string naming a skeleton bone.
/// @param offset_x Bone-space X offset.
/// @param offset_y Bone-space Y offset.
/// @param offset_z Bone-space Z offset.
/// @return @p obj for fluent chaining. Parenting or skeleton validation
///         failures leave the socket unattached.
void *rt_game3d_entity_attach_to_bone_offset(void *obj,
                                             void *child_obj,
                                             rt_string bone_name,
                                             double offset_x,
                                             double offset_y,
                                             double offset_z) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.attachToBone: invalid entity");
    rt_game3d_entity *child =
        game3d_entity_checked(child_obj, "Game3D.Entity3D.attachToBone: child must be Entity3D");
    void *animator;
    void *controller;
    void *skeleton;
    void *child_node;
    int64_t bone_index;

    if (!entity || !child)
        return obj;
    animator = game3d_entity_anim_ref(entity);
    controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
    skeleton = controller ? rt_anim_controller3d_get_skeleton(controller) : NULL;
    if (!skeleton) {
        rt_trap("Game3D.Entity3D.attachToBone: entity needs an animator with a skeleton");
        return obj;
    }
    bone_index = rt_skeleton3d_find_bone(skeleton, bone_name);
    if (bone_index < 0) {
        rt_trap("Game3D.Entity3D.attachToBone: bone name not found in skeleton");
        return obj;
    }
    rt_game3d_entity_add_child(obj, child_obj);
    if (child->parent != entity)
        return obj; /* add_child rejected the parenting (already trapped/reported) */
    child_node = game3d_entity_node_ref(child);
    if (!child_node) {
        rt_trap("Game3D.Entity3D.attachToBone: child entity has no scene node");
        return obj;
    }
    rt_scene_node3d_attach_to_bone(
        child_node, controller, bone_index, offset_x, offset_y, offset_z);
    return obj;
}

/// @brief Attach a child entity to a named bone with no offset.
/// @param obj Parent Entity3D whose animator supplies the skeleton.
/// @param child_obj Child Entity3D to parent and socket.
/// @param bone_name Runtime string naming a skeleton bone.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_attach_to_bone(void *obj, void *child_obj, rt_string bone_name) {
    return rt_game3d_entity_attach_to_bone_offset(obj, child_obj, bone_name, 0.0, 0.0, 0.0);
}

/// @brief Attach a child entity to a named bone with a bone-space offset AND a
///   bone-space rotation (Euler degrees) — ADR 0274.
/// @details The rotation composes after the bone pose during socket sync, so a
///   held prop can cock or roll in the hand without baking the tilt into its
///   mesh (the pre-0274 workaround; socket sync overwrites the child's own
///   local rotation every frame, so SetRotationEuler on the child never
///   worked). Angles follow the SetRotationEuler convention (degrees,
///   pitch/yaw/roll) and are clamped to the same magnitude bound. On any
///   attach failure the socket (and its rotation) is left unset.
/// @param obj Parent Entity3D whose animator supplies the skeleton.
/// @param child_obj Child Entity3D to parent and socket.
/// @param bone_name Runtime string naming a skeleton bone.
/// @param offset_x Bone-space X offset.
/// @param offset_y Bone-space Y offset.
/// @param offset_z Bone-space Z offset.
/// @param rot_x_deg Bone-space X rotation in degrees.
/// @param rot_y_deg Bone-space Y rotation in degrees.
/// @param rot_z_deg Bone-space Z rotation in degrees.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_attach_to_bone_offset_rotated(void *obj,
                                                     void *child_obj,
                                                     rt_string bone_name,
                                                     double offset_x,
                                                     double offset_y,
                                                     double offset_z,
                                                     double rot_x_deg,
                                                     double rot_y_deg,
                                                     double rot_z_deg) {
    rt_game3d_entity *entity;
    rt_game3d_entity *child;
    void *child_node;
    void *quat;

    rt_game3d_entity_attach_to_bone_offset(obj, child_obj, bone_name, offset_x, offset_y, offset_z);
    entity = game3d_entity_checked(obj, "Game3D.Entity3D.attachToBone: invalid entity");
    child =
        game3d_entity_checked(child_obj, "Game3D.Entity3D.attachToBone: child must be Entity3D");
    if (!entity || !child || child->parent != entity)
        return obj; /* attach failed and already trapped/reported */
    child_node = game3d_entity_node_ref(child);
    if (!child_node)
        return obj;
    {
        const double deg = 3.14159265358979323846 / 180.0;
        rot_x_deg = game3d_clamp_abs_or(rot_x_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        rot_y_deg = game3d_clamp_abs_or(rot_y_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        rot_z_deg = game3d_clamp_abs_or(rot_z_deg, 0.0, RT_GAME3D_ANGLE_DEG_ABS_MAX);
        quat = rt_quat_from_euler(rot_x_deg * deg, rot_y_deg * deg, rot_z_deg * deg);
        if (quat) {
            rt_scene_node3d_set_bone_socket_rotation(
                child_node, rt_quat_x(quat), rt_quat_y(quat), rt_quat_z(quat), rt_quat_w(quat));
        }
        game3d_release_ref(&quat);
    }
    return obj;
}

/// @brief Build (lazily, cached) and activate a ragdoll from the entity's
///   animator skeleton against the owning world's physics. Returns the
///   Ragdoll3D handle, or NULL when the entity lacks an animator/skeleton,
///   node, or spawned world.
/// @param obj Entity3D runtime handle.
/// @return The cached active Ragdoll3D, or NULL when prerequisites or
///         allocation/activation fail.
void *rt_game3d_entity_enable_ragdoll(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.enableRagdoll: invalid entity");
    if (!entity)
        return NULL;
    rt_game3d_world *world =
        rt_obj_is_instance(entity->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world))
            ? (rt_game3d_world *)entity->world
            : NULL;
    void *animator = game3d_entity_anim_ref(entity);
    void *controller = animator ? rt_game3d_animator_get_controller(animator) : NULL;
    void *skeleton = controller ? rt_anim_controller3d_get_skeleton(controller) : NULL;
    void *node = game3d_entity_node_ref(entity);
    if (!world || !world->physics || !controller || !skeleton || !node) {
        rt_trap("Game3D.Entity3D.enableRagdoll: entity needs a spawned world, an animator "
                "with a skeleton, and a scene node");
        return NULL;
    }
    void *ragdoll = rt_g3d_checked_or_null(entity->ragdoll, RT_G3D_RAGDOLL3D_CLASS_ID);
    if (!ragdoll) {
        ragdoll = rt_ragdoll3d_from_skeleton(skeleton);
        if (!ragdoll)
            return NULL;
        game3d_assign_typed_ref(&entity->ragdoll, ragdoll, RT_G3D_RAGDOLL3D_CLASS_ID);
        game3d_release_ref(&ragdoll);
        ragdoll = entity->ragdoll;
    }
    if (!rt_ragdoll3d_get_active(ragdoll))
        rt_ragdoll3d_activate(ragdoll, world->physics, controller, node);
    return ragdoll;
}

/// @brief Deactivate the entity's ragdoll (if any), blending back to animation
///   over @p blend_seconds. Returns true when a ragdoll was active.
/// @param obj Entity3D runtime handle.
/// @param blend_seconds Requested animation-recovery blend duration, forwarded
///                      to Ragdoll3D.
/// @return Non-zero when an active ragdoll was deactivated, otherwise zero.
int8_t rt_game3d_entity_disable_ragdoll(void *obj, double blend_seconds) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.disableRagdoll: invalid entity");
    if (!entity)
        return 0;
    void *ragdoll = rt_g3d_checked_or_null(entity->ragdoll, RT_G3D_RAGDOLL3D_CLASS_ID);
    if (!ragdoll || !rt_ragdoll3d_get_active(ragdoll))
        return 0;
    rt_ragdoll3d_deactivate(ragdoll, blend_seconds);
    return 1;
}

/// @brief Get the entity's cached Ragdoll3D (NULL before enableRagdoll).
/// @param obj Entity3D runtime handle.
/// @return The validated cached Ragdoll3D pointer, or NULL.
void *rt_game3d_entity_get_ragdoll(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_ragdoll: invalid entity");
    return entity ? rt_g3d_checked_or_null(entity->ragdoll, RT_G3D_RAGDOLL3D_CLASS_ID) : NULL;
}

/// @brief Remove this entity's bone-socket binding (installed by a parent's
///   AttachToBone); the entity keeps its last transform and stays parented.
/// @param obj Entity3D runtime handle.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_detach_from_bone(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.detachFromBone: invalid entity");
    void *node = entity ? game3d_entity_node_ref(entity) : NULL;
    if (node)
        rt_scene_node3d_detach_bone_socket(node);
    return obj;
}

/// @brief Fluent: attach a Behavior3D that the world ticks each simulation
///   step, or pass null to detach the current behavior.
/// @param obj Entity3D runtime handle.
/// @param behavior Behavior3D to retain, or NULL to detach. Other runtime
///                 classes trap without replacing the current behavior.
/// @return @p obj for fluent chaining.
void *rt_game3d_entity_attach_behavior(void *obj, void *behavior) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.attachBehavior: invalid entity");
    if (!entity)
        return obj;
    if (behavior && !rt_g3d_has_class(behavior, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID)) {
        rt_trap("Game3D.Entity3D.attachBehavior: expected Behavior3D or null");
        return obj;
    }
    game3d_assign_typed_ref(&entity->behavior, behavior, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID);
    return obj;
}

/// @brief The entity's attached Behavior3D (NULL if none).
/// @param obj Entity3D runtime handle.
/// @return The validated retained Behavior3D pointer, or NULL.
void *rt_game3d_entity_get_behavior(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.getBehavior: invalid entity");
    return entity ? rt_g3d_checked_or_null(entity->behavior, RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID)
                  : NULL;
}
