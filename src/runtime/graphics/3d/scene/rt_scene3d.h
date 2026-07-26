//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d.h
// Purpose: Zanna.Graphics3D.Scene3D and SceneNode3D — hierarchical scene graph
//   with parent-child transform propagation. Nodes hold local TRS (translate,
//   rotate, scale) and compute world matrices by multiplying up the hierarchy.
//
// Key invariants:
//   - Scene3D always has a root node (created in constructor).
//   - SceneNode3D world_matrix is recomputed lazily when dirty.
//   - Descendants notice parent transform changes through cached world revisions.
//   - Children are stored as a dynamic array (not GC-managed).
//   - Gameplay metadata is typed, bounded, and serialized only through VSCN.
//   - Nodes with both mesh and material are drawn; imported node lights contribute
//     to scene draw snapshots after world-transform evaluation.
//
// Links: rt_canvas3d.h, rt_quat.h, rt_mat4.h, plans/3d/12-scene-graph.md,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//   docs/adr/0161-stable-scenenode-sibling-reordering.md,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md,
//   docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares hierarchical Scene3D graphs, nodes, bindings, metadata, animation, and LOD.
/// @details Scene and node handles are GC-managed. Child links and bound
/// resources are retained, returned graph/resource pointers are borrowed unless
/// explicitly described as fresh, and value accessors return new boxed math
/// values. Mutations preserve lazy world-transform and spatial-index invariants.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Direction a SceneNode3D synchronizes transforms with its bound physics body:
///   physics→node, node→physics, animation-root-motion→node, or bidirectional kinematic.
enum {
    ///< Copy the physics body's pose into the scene node.
    RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY = 0,
    ///< Copy the scene node's pose into the physics body.
    RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE = 1,
    ///< Apply animator root motion to both the node and bound body.
    RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION = 2,
    ///< Reconcile node and body as a bidirectional kinematic binding.
    RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC = 3,
};

/// @brief Stable limits for gameplay-facing SceneNode metadata.
#define RT_SCENE_NODE3D_MAX_METADATA_ENTRIES 256
#define RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES 128
#define RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES (64 * 1024)

/* Scene3D */
/// @brief Create an empty 3D scene with a single root node.
/// @return New Scene3D handle, or `NULL` on allocation failure.
void *rt_scene3d_new(void);

/// @brief Get the implicit root node so callers can attach children directly.
/// @param scene Borrowed Scene3D handle.
/// @return Borrowed root SceneNode3D handle, or `NULL` for an invalid scene.
void *rt_scene3d_get_root(void *scene);

/// @brief Convenience: attach @p node beneath the scene root.
/// @param scene Borrowed destination Scene3D handle.
/// @param node Borrowed SceneNode3D retained by the root after attachment.
void rt_scene3d_add(void *scene, void *node);

/// @brief Convenience: attach @p node beneath the scene root; returns 1 on success.
/// @param scene Borrowed destination Scene3D handle.
/// @param node Borrowed SceneNode3D retained by the root after attachment.
/// @return Nonzero when the node is parented beneath the root.
int8_t rt_scene3d_try_add(void *scene, void *node);

/// @brief Convenience: detach @p node from the scene root.
/// @param scene Borrowed owning Scene3D handle.
/// @param node Borrowed direct child to detach.
void rt_scene3d_remove(void *scene, void *node);

/// @brief Recursively search the scene for a node whose name matches @p name (NULL if none).
/// @param scene Borrowed Scene3D handle.
/// @param name Borrowed exact node name.
/// @return Borrowed first matching SceneNode3D in traversal order, or `NULL`.
void *rt_scene3d_find(void *scene, rt_string name);

/// @brief Recursively search the scene and return Some(SceneNode3D) on match, None on absence.
/// @param scene Borrowed Scene3D handle.
/// @param name Borrowed exact node name.
/// @return New Option containing the borrowed matching node, or `None`.
void *rt_scene3d_find_option(void *scene, rt_string name);

/// @brief Return visible mesh nodes whose world-space AABB intersects @p min/@p max.
/// @param scene Borrowed Scene3D handle.
/// @param min Borrowed Vec3 query minimum corner.
/// @param max Borrowed Vec3 query maximum corner.
/// @return New sequence of borrowed matching SceneNode3D handles.
void *rt_scene3d_query_aabb(void *scene, void *min, void *max);

/// @brief Return visible mesh nodes whose world-space AABB intersects @p center/@p radius.
/// @param scene Borrowed Scene3D handle.
/// @param center Borrowed Vec3 query center.
/// @param radius Non-negative query radius in world units.
/// @return New sequence of borrowed matching SceneNode3D handles.
void *rt_scene3d_query_sphere(void *scene, void *center, double radius);

/// @brief Return the closest visible mesh node hit by the world-space ray, or NULL.
/// @param scene Borrowed Scene3D handle.
/// @param origin Borrowed Vec3 ray origin.
/// @param direction Borrowed Vec3 ray direction.
/// @param max_distance Maximum accepted world-space hit distance.
/// @return Borrowed nearest SceneNode3D handle, or `NULL` on a miss.
void *rt_scene3d_raycast_nodes(void *scene, void *origin, void *direction, double max_distance);

/// @brief Return the visible mesh node containing the nearest ray-intersected triangle, or NULL.
/// @param scene Borrowed Scene3D handle.
/// @param origin Borrowed Vec3 ray origin.
/// @param direction Borrowed Vec3 ray direction.
/// @param max_distance Maximum accepted world-space hit distance.
/// @return Borrowed nearest triangle-hit SceneNode3D handle, or `NULL` on a miss.
void *rt_scene3d_raycast_nodes_precise(void *scene,
                                       void *origin,
                                       void *direction,
                                       double max_distance);

/// @brief Return every visible mesh node with a ray-intersected triangle, nearest first.
/// @param scene Borrowed Scene3D handle.
/// @param origin Borrowed Vec3 ray origin.
/// @param direction Borrowed Vec3 ray direction.
/// @param max_distance Maximum accepted world-space hit distance.
/// @return New sequence of borrowed hit SceneNode3D handles sorted by hit distance.
void *rt_scene3d_raycast_nodes_precise_all(void *scene,
                                           void *origin,
                                           void *direction,
                                           double max_distance);

/// @brief Return the nearest triangle hit against the whole scene as a RayHit3D, or NULL.
/// @param scene Borrowed Scene3D handle.
/// @param origin Borrowed Vec3 ray origin.
/// @param direction Borrowed Vec3 ray direction.
/// @param max_distance Maximum accepted world-space hit distance.
/// @return New RayHit3D with world point, face normal, distance, and triangle index, or `NULL`.
void *rt_scene3d_raycast_precise_hit(void *scene,
                                     void *origin,
                                     void *direction,
                                     double max_distance);

/// @brief Add an authored interior visibility zone and return its zero-based index.
/// @param scene Borrowed Scene3D handle.
/// @param name Borrowed zone name retained by the scene.
/// @param min Borrowed Vec3 minimum world-space corner.
/// @param max Borrowed Vec3 maximum world-space corner.
/// @return New zero-based zone index, or `-1` on invalid input or allocation failure.
int64_t rt_scene3d_add_visibility_zone(void *scene, rt_string name, void *min, void *max);

/// @brief Add a directed or bidirectional portal between visibility zones.
/// @param scene Borrowed Scene3D handle.
/// @param from_zone Existing source-zone index.
/// @param to_zone Existing destination-zone index.
/// @param bidirectional Nonzero to add the reverse directed link as well.
/// @return Index of the first added directed portal, or `-1` on failure.
int64_t rt_scene3d_add_visibility_portal(void *scene,
                                         int64_t from_zone,
                                         int64_t to_zone,
                                         int8_t bidirectional);
/// @brief Render every visible drawable node in the scene from @p camera onto @p canvas3d.
/// @param scene Borrowed Scene3D handle.
/// @param canvas3d Borrowed destination Canvas3D handle.
/// @param camera Borrowed Camera3D defining view and projection.
void rt_scene3d_draw(void *scene, void *canvas3d, void *camera);

/// @brief Detach all children of the root (does not delete the root itself).
/// @param scene Borrowed Scene3D handle to clear.
void rt_scene3d_clear(void *scene);

/// @brief Total number of nodes in the scene (counts root + every descendant).
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative total node count, including the implicit root.
int64_t rt_scene3d_get_node_count(void *scene);

/// @brief Serialize the scene to a .vscn file. Returns 1 on success, 0 on failure.
/// @param scene Borrowed Scene3D handle.
/// @param path Borrowed output path.
/// @return Nonzero after complete canonical serialization, otherwise zero.
int64_t rt_scene3d_save(void *scene, rt_string path);

/// @brief Serialize the scene to canonical VSCN text in memory (ADR 0190).
///   Byte-identical to rt_scene3d_save output; empty string on failure.
/// @param scene Borrowed Scene3D handle.
/// @return New canonical VSCN string, or an owned empty string on failure.
rt_string rt_scene3d_save_text(void *scene);

/// @brief Deserialize a scene from a `.vscn` (JSON) file. NULL on failure.
///   (glTF/FBX scenes load through Zanna.Graphics3D.GLTF.Load / FBX.Load.)
/// @param path Borrowed VSCN file path.
/// @return New Scene3D handle, or `NULL` on read, parse, validation, or allocation failure.
void *rt_scene3d_load(rt_string path);

/// @brief Deserialize a scene from already-read `.vscn` JSON text (internal streaming path).
///   @p path is diagnostics-only; the caller keeps ownership of @p text. NULL on failure.
/// @param path Borrowed diagnostics-only source path.
/// @param text Borrowed UTF-8 JSON buffer; need not be null-terminated.
/// @param len Exact number of readable bytes in @p text.
/// @return New Scene3D handle, or `NULL` on parse, validation, or allocation failure.
void *rt_scene3d_load_from_memory(rt_string path, const char *text, size_t len);

/// @brief Push physics body, animator root-motion, and other bindings into node transforms.
/// Call once per frame after physics step but before draw.
/// @param scene Borrowed Scene3D handle.
/// @param dt Finite frame delta used by time-dependent bindings.
void rt_scene3d_sync_bindings(void *scene, double dt);

/// @brief Shift every root-level scene subtree by -delta for floating-origin rebasing.
/// @param scene Borrowed Scene3D handle.
/// @param dx X component of the origin shift.
/// @param dy Y component of the origin shift.
/// @param dz Z component of the origin shift.
void rt_scene3d_rebase_origin(void *scene, double dx, double dy, double dz);

/* SceneNode3D */
/// @brief Create a SceneNode3D at the origin with identity rotation, unit scale, no mesh.
/// @return New detached SceneNode3D handle, or `NULL` on allocation failure.
void *rt_scene_node3d_new(void);

/// @brief Set the node's local-space position (relative to its parent).
/// @param node Borrowed SceneNode3D handle.
/// @param x Local X coordinate.
/// @param y Local Y coordinate.
/// @param z Local Z coordinate.
void rt_scene_node3d_set_position(void *node, double x, double y, double z);

/// @brief Get the node's local-space position as a Vec3.
/// @param node Borrowed SceneNode3D handle.
/// @return New Vec3 containing the sanitized local position.
void *rt_scene_node3d_get_position(void *node);

/// @brief Set the node's local-space orientation from a Quaternion.
/// @param node Borrowed SceneNode3D handle.
/// @param quat Borrowed quaternion handle normalized before storage.
void rt_scene_node3d_set_rotation(void *node, void *quat);

/// @brief Get the node's local-space orientation as a Quaternion.
/// @param node Borrowed SceneNode3D handle.
/// @return New normalized quaternion containing the local orientation.
void *rt_scene_node3d_get_rotation(void *node);

/// @brief Set the node's local-space scale (independent x/y/z factors).
/// @param node Borrowed SceneNode3D handle.
/// @param x Local X scale.
/// @param y Local Y scale.
/// @param z Local Z scale.
void rt_scene_node3d_set_scale(void *node, double x, double y, double z);

/// @brief Get the node's local-space scale as a Vec3.
/// @param node Borrowed SceneNode3D handle.
/// @return New Vec3 containing the sanitized local scale.
void *rt_scene_node3d_get_scale(void *node);

/// @brief Get the node's world-space transform as a Mat4 (lazily recomputed when dirty).
/// @param node Borrowed SceneNode3D handle.
/// @return New Mat4 containing the complete world transform.
void *rt_scene_node3d_get_world_matrix(void *node);

/// @brief Assign a complete world matrix only when it has an exact parent-relative TRS.
/// @param node Borrowed SceneNode3D handle to mutate.
/// @param world_matrix Borrowed Mat4 requested as the complete world transform.
/// @return 1 on success; 0 without mutation for invalid, singular, sheared, or lossy requests.
int8_t rt_scene_node3d_try_set_world_matrix(void *node, void *world_matrix);

/// @brief Get the node's world-space position as a Vec3.
/// @param node Borrowed SceneNode3D handle.
/// @return New Vec3 containing the lazily evaluated world position.
void *rt_scene_node3d_get_world_position(void *node);

/// @brief Get the node's world-space position as raw components (1 on success, 0 on null node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving the world X coordinate.
/// @param y Output receiving the world Y coordinate.
/// @param z Output receiving the world Z coordinate.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_world_position_components(void *node, double *x, double *y, double *z);

/// @brief Get the node's world-space orientation as a Quaternion.
/// @param node Borrowed SceneNode3D handle.
/// @return New normalized quaternion containing the world orientation.
void *rt_scene_node3d_get_world_rotation(void *node);

/// @brief Get the node's world-space scale magnitudes as a Vec3.
/// @param node Borrowed SceneNode3D handle.
/// @return New Vec3 containing world-axis scale magnitudes.
void *rt_scene_node3d_get_world_scale(void *node);

/// @brief Get the local position as raw components (1 on success, 0 on invalid node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving the local X coordinate.
/// @param y Output receiving the local Y coordinate.
/// @param z Output receiving the local Z coordinate.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_position_components(void *node, double *x, double *y, double *z);

/// @brief Get the local rotation quaternion as raw components (1 on success, 0 on invalid node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving quaternion X.
/// @param y Output receiving quaternion Y.
/// @param z Output receiving quaternion Z.
/// @param w Output receiving quaternion W.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
/// @brief Get the world rotation quaternion as raw components (1 on success, 0 on invalid node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving quaternion X.
/// @param y Output receiving quaternion Y.
/// @param z Output receiving quaternion Z.
/// @param w Output receiving quaternion W.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_world_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
/// @brief Get the local scale as raw components (1 on success, 0 on invalid node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving local X scale.
/// @param y Output receiving local Y scale.
/// @param z Output receiving local Z scale.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_scale_components(void *node, double *x, double *y, double *z);

/// @brief Get the world scale magnitudes as raw components (1 on success, 0 on invalid node).
/// @param node Borrowed SceneNode3D handle.
/// @param x Output receiving world X scale magnitude.
/// @param y Output receiving world Y scale magnitude.
/// @param z Output receiving world Z scale magnitude.
/// @return Nonzero when every non-null output was populated from a valid node.
int8_t rt_scene_node3d_get_world_scale_components(void *node, double *x, double *y, double *z);

/// @brief Copy the world matrix into @p out (row-major 16 doubles); 1 on success.
/// @param node Borrowed SceneNode3D handle.
/// @param out Output array receiving sixteen row-major matrix lanes.
/// @return Nonzero when a valid finite world matrix was copied.
int8_t rt_scene_node3d_get_world_matrix_components(void *node, double out[16]);

/// @brief Set position, rotation (quaternion components), and scale in one call.
/// @param node Borrowed SceneNode3D handle.
/// @param px Local position X.
/// @param py Local position Y.
/// @param pz Local position Z.
/// @param qx Local quaternion X.
/// @param qy Local quaternion Y.
/// @param qz Local quaternion Z.
/// @param qw Local quaternion W.
/// @param sx Local scale X.
/// @param sy Local scale Y.
/// @param sz Local scale Z.
void rt_scene_node3d_set_transform(void *node,
                                   double px,
                                   double py,
                                   double pz,
                                   double qx,
                                   double qy,
                                   double qz,
                                   double qw,
                                   double sx,
                                   double sy,
                                   double sz);
/// @brief Apply packed TRS values (10 floats per node) to a list of nodes in one call.
/// @param nodes Borrowed sequence of SceneNode3D handles.
/// @param values Borrowed flat numeric sequence containing ten TRS lanes per node.
void rt_scene_node3d_set_transform_batch(void *nodes, void *values);

/// @brief SceneGraph.SetNodeTransforms: batch-apply packed TRS values through a scene handle.
/// @param scene Borrowed Scene3D handle validating the target graph.
/// @param nodes Borrowed sequence of nodes belonging to the scene.
/// @param values Borrowed flat numeric sequence containing ten TRS lanes per node.
void rt_scene3d_set_node_transforms(void *scene, void *nodes, void *values);
/// @brief Attach @p child as a child of @p node (detaches from any prior parent).
/// @param node Borrowed destination parent SceneNode3D.
/// @param child Borrowed child retained by the new parent.
void rt_scene_node3d_add_child(void *node, void *child);

/// @brief Attach @p child as a child of @p node, returning 1 when the child is parented there.
/// @param node Borrowed destination parent SceneNode3D.
/// @param child Borrowed child retained by the new parent.
/// @return Nonzero when attachment succeeds without creating a cycle.
int8_t rt_scene_node3d_try_add_child(void *node, void *child);

/// @brief Attach @p child while preserving its complete world matrix, or reject without mutation.
/// @param node Borrowed destination parent SceneNode3D.
/// @param child Borrowed child whose complete world transform must remain exact.
/// @return Nonzero on exact reparenting, otherwise zero with hierarchy unchanged.
int8_t rt_scene_node3d_try_add_child_preserve_world(void *node, void *child);

/// @brief Move an existing direct child to @p index without changing its parent.
/// @param node Borrowed parent SceneNode3D.
/// @param child Borrowed direct child to reorder.
/// @param index Destination sibling index.
/// @return Nonzero when the child was moved to the requested valid position.
int8_t rt_scene_node3d_try_move_child(void *node, void *child, int64_t index);

/// @brief Detach @p child from @p node (no-op if not actually a child).
/// @param node Borrowed parent SceneNode3D.
/// @param child Borrowed direct child to detach.
void rt_scene_node3d_remove_child(void *node, void *child);

/// @brief Number of direct children of @p node.
/// @param node Borrowed SceneNode3D handle.
/// @return Non-negative direct-child count.
int64_t rt_scene_node3d_child_count(void *node);

/// @brief Get the @p index-th child (NULL if out of range).
/// @param node Borrowed parent SceneNode3D.
/// @param index Zero-based direct-child index.
/// @return Borrowed child SceneNode3D handle, or `NULL` when out of range.
void *rt_scene_node3d_get_child(void *node, int64_t index);

/// @brief Get the node's parent (NULL if root or detached).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed parent SceneNode3D handle, or `NULL`.
void *rt_scene_node3d_get_parent(void *node);

/// @brief Recursively search this subtree for a node whose name matches @p name.
/// @param node Borrowed subtree root.
/// @param name Borrowed exact node name.
/// @return Borrowed first matching node in traversal order, or `NULL`.
void *rt_scene_node3d_find(void *node, rt_string name);

/// @brief Recursively search this subtree and return Some(SceneNode3D) on match, None on absence.
/// @param node Borrowed subtree root.
/// @param name Borrowed exact node name.
/// @return New Option containing the borrowed match, or `None`.
void *rt_scene_node3d_find_option(void *node, rt_string name);

/// @brief Bind a Mesh3D for rendering at this node's world transform.
/// @param node Borrowed SceneNode3D handle.
/// @param mesh Borrowed Mesh3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_set_mesh(void *node, void *mesh);

/// @brief Get the bound Mesh3D (NULL if this is a transform-only node).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed bound Mesh3D handle, or `NULL`.
void *rt_scene_node3d_get_mesh(void *node);

/// @brief Bind a Material3D for the node's mesh draw.
/// @param node Borrowed SceneNode3D handle.
/// @param material Borrowed Material3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_set_material(void *node, void *material);

/// @brief Get the bound Material3D (NULL if none).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed bound Material3D handle, or `NULL`.
void *rt_scene_node3d_get_material(void *node);

/// @brief Bind a Light3D transformed and submitted with this node; NULL clears.
/// @param node Borrowed SceneNode3D handle.
/// @param light Borrowed Light3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_set_light(void *node, void *light);

/// @brief Get the node-attached Light3D, or NULL.
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed attached Light3D handle, or `NULL`.
void *rt_scene_node3d_get_light(void *node);

/// @brief Bind a Camera3D whose view pose follows this node during SyncBindings.
/// @param node Borrowed SceneNode3D handle.
/// @param camera Borrowed Camera3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_set_camera(void *node, void *camera);

/// @brief Get the node-attached Camera3D, or NULL.
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed attached Camera3D handle, or `NULL`.
void *rt_scene_node3d_get_camera(void *node);

/// @brief Toggle visibility (hidden nodes and their descendants are skipped during draw).
/// @param node Borrowed SceneNode3D handle.
/// @param visible Nonzero to make the node subtree eligible for drawing.
void rt_scene_node3d_set_visible(void *node, int8_t visible);

/// @brief Get the visibility flag.
/// @param node Borrowed SceneNode3D handle.
/// @return Nonzero when the node's local visibility flag is enabled.
int8_t rt_scene_node3d_get_visible(void *node);

/// @brief Set the node's name (used by `_find` lookups).
/// @param node Borrowed SceneNode3D handle.
/// @param name Borrowed string retained by the node.
void rt_scene_node3d_set_name(void *node, rt_string name);

/// @brief Get the node's name as an owned string (empty if unset).
/// @param node Borrowed SceneNode3D handle.
/// @return New string reference containing the name, or an owned empty string.
rt_string rt_scene_node3d_get_name(void *node);

/// @brief Get the source prefab path recorded on an instance root.
/// @param node Borrowed SceneNode3D handle.
/// @return New string reference containing the prefab path, or an owned empty string.
rt_string rt_scene_node3d_get_prefab_path(void *node);

/// @brief Determine whether a node belongs to expanded prefab-instance content.
/// @param node Borrowed SceneNode3D handle.
/// @return Nonzero when the node is marked as instance-derived content.
int8_t rt_scene_node3d_get_is_instance_content(void *node);

/// @brief Mark a node as a prefab instance root referencing @p path.
/// @param node Borrowed SceneNode3D handle.
/// @param path Borrowed non-empty prefab path retained by the node.
/// @return Nonzero when the reference was stored successfully.
int8_t rt_scene_node3d_set_prefab_reference(void *node, rt_string path);

/// @brief Clear a prefab reference and related instance-content markers.
/// @param node Borrowed SceneNode3D handle.
/// @return Nonzero when a valid node was updated.
int8_t rt_scene_node3d_clear_prefab_reference(void *node);

/// @brief Return every gameplay-metadata key in deterministic lexicographic order.
/// @param node Borrowed SceneNode3D handle.
/// @return New sequence containing owned key strings.
void *rt_scene_node3d_metadata_keys(void *node);
/// @brief Return `null`, `bool`, `int`, `float`, or `string`; empty when @p key is absent.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return New string naming the exact stored type, or an owned empty string.
rt_string rt_scene_node3d_metadata_kind(void *node, rt_string key);

/// @brief Return whether the node has an explicit metadata value for @p key.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return Nonzero when the key exists, including when explicitly set to null.
int8_t rt_scene_node3d_metadata_has(void *node, rt_string key);

/// @brief Read exact integer metadata, or @p def when absent or differently typed.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Default returned for absence or a type mismatch.
/// @return Stored integer or @p def.
int64_t rt_scene_node3d_metadata_get_int(void *node, rt_string key, int64_t def);

/// @brief Read float metadata (or widen an integer), or @p def on mismatch.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Default returned for absence or a nonnumeric type.
/// @return Stored finite float, widened integer, or @p def.
double rt_scene_node3d_metadata_get_float(void *node, rt_string key, double def);

/// @brief Read boolean metadata, or @p def when absent or differently typed.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Default returned for absence or a type mismatch.
/// @return Normalized stored boolean or @p def.
int8_t rt_scene_node3d_metadata_get_bool(void *node, rt_string key, int8_t def);

/// @brief Read string metadata, or a retained copy of @p def on mismatch.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @param def Borrowed default string.
/// @return New reference to the stored string or @p def.
rt_string rt_scene_node3d_metadata_get_string(void *node, rt_string key, rt_string def);

/// @brief Set explicit null metadata; return 0 when the key or bounded table is invalid.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed bounded metadata key.
/// @return Nonzero when the typed entry was stored.
int8_t rt_scene_node3d_metadata_set_null(void *node, rt_string key);

/// @brief Set integer metadata; return 0 when the key or bounded table is invalid.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed bounded metadata key.
/// @param value Exact integer value to store.
/// @return Nonzero when the typed entry was stored.
int8_t rt_scene_node3d_metadata_set_int(void *node, rt_string key, int64_t value);

/// @brief Set finite float metadata; return 0 for invalid input or a full bounded table.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed bounded metadata key.
/// @param value Finite floating-point value to store.
/// @return Nonzero when the typed entry was stored.
int8_t rt_scene_node3d_metadata_set_float(void *node, rt_string key, double value);

/// @brief Set boolean metadata; return 0 when the key or bounded table is invalid.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed bounded metadata key.
/// @param value Boolean value normalized before storage.
/// @return Nonzero when the typed entry was stored.
int8_t rt_scene_node3d_metadata_set_bool(void *node, rt_string key, int8_t value);

/// @brief Set bounded string metadata; return 0 for invalid input or a full table.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed bounded metadata key.
/// @param value Borrowed bounded string retained by the entry.
/// @return Nonzero when the typed entry was stored.
int8_t rt_scene_node3d_metadata_set_string(void *node, rt_string key, rt_string value);

/// @brief Remove metadata and report whether the key existed.
/// @param node Borrowed SceneNode3D handle.
/// @param key Borrowed metadata key.
/// @return Nonzero when an existing entry was removed.
int8_t rt_scene_node3d_metadata_remove(void *node, rt_string key);

/// @brief Get the AABB minimum corner for the node subtree in this node's local space.
/// @param node Borrowed subtree root SceneNode3D.
/// @return New Vec3 containing the local-space subtree minimum corner.
void *rt_scene_node3d_get_aabb_min(void *node);

/// @brief Get the AABB maximum corner for the node subtree in this node's local space.
/// @param node Borrowed subtree root SceneNode3D.
/// @return New Vec3 containing the local-space subtree maximum corner.
void *rt_scene_node3d_get_aabb_max(void *node);
/// @brief Bind a physics Body3D so its transform syncs with this node's transform.
/// @param node Borrowed SceneNode3D handle.
/// @param body Borrowed Body3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_bind_body(void *node, void *body);

/// @brief Remove the body binding (transform updates stop affecting the body).
/// @param node Borrowed SceneNode3D handle.
void rt_scene_node3d_clear_body_binding(void *node);

/// @brief Attach this node to a skeletal bone: SyncBindings drives its world
///   transform from parentWorld x bonePose x positional offset each pass.
/// @param node Borrowed SceneNode3D socket node.
/// @param animator Borrowed skeletal AnimController3D retained by the binding.
/// @param bone_index Zero-based target bone index.
/// @param offset_x Local socket offset X.
/// @param offset_y Local socket offset Y.
/// @param offset_z Local socket offset Z.
void rt_scene_node3d_attach_to_bone(void *node,
                                    void *animator,
                                    int64_t bone_index,
                                    double offset_x,
                                    double offset_y,
                                    double offset_z);
/// @brief Remove any bone-socket binding; the node keeps its last transform.
/// @param node Borrowed SceneNode3D handle.
void rt_scene_node3d_detach_bone_socket(void *node);

/// @brief Get the bound Body3D (NULL if none).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed bound Body3D handle, or `NULL`.
void *rt_scene_node3d_get_body(void *node);

/// @brief Choose how node↔body sync flows (one of RT_SCENE_NODE3D_SYNC_*).
/// @param node Borrowed SceneNode3D handle.
/// @param sync_mode One `RT_SCENE_NODE3D_SYNC_*` constant.
void rt_scene_node3d_set_sync_mode(void *node, int64_t sync_mode);

/// @brief Get the current sync-mode constant.
/// @param node Borrowed SceneNode3D handle.
/// @return Current `RT_SCENE_NODE3D_SYNC_*` value.
int64_t rt_scene_node3d_get_sync_mode(void *node);

/// @brief Bind an AnimController3D so its evaluated bones drive this node's child meshes.
/// @param node Borrowed SceneNode3D handle.
/// @param controller Borrowed AnimController3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_bind_animator(void *node, void *controller);

/// @brief Remove the animator binding.
/// @param node Borrowed SceneNode3D handle.
void rt_scene_node3d_clear_animator_binding(void *node);

/// @brief Get the bound AnimController3D (NULL if none).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed bound AnimController3D handle, or `NULL`.
void *rt_scene_node3d_get_animator(void *node);

/// @brief Bind a NodeAnimator3D so imported node, morph-weight, object, or camera clips drive
/// this node's subtree during Scene3D.SyncBindings().
/// @param node Borrowed SceneNode3D handle.
/// @param animator Borrowed NodeAnimator3D retained by the node, or `NULL` to clear.
void rt_scene_node3d_bind_node_animator(void *node, void *animator);

/// @brief Remove only the bound NodeAnimator3D, leaving any skeletal AnimController3D intact.
/// @param node Borrowed SceneNode3D handle.
void rt_scene_node3d_clear_node_animator_binding(void *node);

/// @brief Get the bound NodeAnimator3D (NULL if no node-animation playback is attached).
/// @param node Borrowed SceneNode3D handle.
/// @return Borrowed bound NodeAnimator3D handle, or `NULL`.
void *rt_scene_node3d_get_node_animator(void *node);

/* NodeAnimation3D / NodeAnimator3D */
/// @brief Create a NodeAnimation3D clip that targets scene-node TRS or morph-weight channels.
/// @details Node animations mirror glTF node animation semantics and are separate from
/// skeletal Animation3D clips. They are useful for object motion, camera paths, and morph target
/// weight animation.
/// @param name Borrowed clip name retained by the animation.
/// @param duration Non-negative authored duration in seconds.
/// @return New NodeAnimation3D handle, or `NULL` on invalid input or allocation failure.
void *rt_node_animation3d_new(rt_string name, double duration);

/// @brief Get the authored NodeAnimation3D clip name, or an empty string for invalid handles.
/// @param animation Borrowed NodeAnimation3D handle.
/// @return New reference to the authored name, or an owned empty string.
rt_string rt_node_animation3d_get_name(void *animation);

/// @brief Get the NodeAnimation3D duration in seconds.
/// @param animation Borrowed NodeAnimation3D handle.
/// @return Finite non-negative clip duration, or zero for an invalid handle.
double rt_node_animation3d_get_duration(void *animation);

/// @brief Get the number of channels stored on a NodeAnimation3D clip.
/// @param animation Borrowed NodeAnimation3D handle.
/// @return Non-negative authored channel count.
int64_t rt_node_animation3d_get_channel_count(void *animation);

/// @brief Create a NodeAnimator3D that owns and plays a single NodeAnimation3D clip.
/// @param clip Borrowed NodeAnimation3D retained by the new animator.
/// @return New NodeAnimator3D handle, or `NULL` on invalid input or allocation failure.
void *rt_node_animator3d_new(void *clip);

/// @brief Get the number of clips retained by a NodeAnimator3D.
/// @param animator Borrowed NodeAnimator3D handle.
/// @return Non-negative retained clip count.
int64_t rt_node_animator3d_get_clip_count(void *animator);

/// @brief Borrow a clip from a NodeAnimator3D by index, or NULL when out of range.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param index Zero-based clip index.
/// @return Borrowed NodeAnimation3D handle, or `NULL` when out of range.
void *rt_node_animator3d_get_clip(void *animator, int64_t index);

/// @brief Get the clip name at @p index, or an empty string when out of range.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param index Zero-based clip index.
/// @return New reference to the clip name, or an owned empty string.
rt_string rt_node_animator3d_get_clip_name(void *animator, int64_t index);

/// @brief Get the currently active node-animation clip name, or an empty string when none.
/// @param animator Borrowed NodeAnimator3D handle.
/// @return New reference to the active clip name, or an owned empty string.
rt_string rt_node_animator3d_get_current_clip(void *animator);

/// @brief Play the named node-animation clip immediately and reset its playback time.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param name Borrowed exact clip name.
/// @return Nonzero when a matching clip was selected and started.
int8_t rt_node_animator3d_play(void *animator, rt_string name);

/// @brief Stop a NodeAnimator3D without clearing its clip selection or current time.
/// @param animator Borrowed NodeAnimator3D handle.
void rt_node_animator3d_stop(void *animator);

/// @brief Set the global playback speed multiplier for a NodeAnimator3D.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param speed Finite playback multiplier.
void rt_node_animator3d_set_speed(void *animator, double speed);

/// @brief Get the global playback speed multiplier for a NodeAnimator3D.
/// @param animator Borrowed NodeAnimator3D handle.
/// @return Finite playback multiplier, or a safe default for an invalid handle.
double rt_node_animator3d_get_speed(void *animator);

/// @brief Set the current clip time in seconds; the next update clamps or wraps it to the clip.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param time Finite requested playback time in seconds.
void rt_node_animator3d_set_time(void *animator, double time);

/// @brief Get the current clip time in seconds.
/// @param animator Borrowed NodeAnimator3D handle.
/// @return Finite current playback time, or zero for an invalid handle.
double rt_node_animator3d_get_time(void *animator);

/// @brief Return whether the NodeAnimator3D is currently advancing playback.
/// @param animator Borrowed NodeAnimator3D handle.
/// @return Nonzero while playback is active.
int8_t rt_node_animator3d_get_playing(void *animator);

/// @brief Advance and apply the current node-animation clip by @p dt seconds.
/// @param animator Borrowed NodeAnimator3D handle.
/// @param dt Finite frame delta in seconds.
void rt_node_animator3d_update(void *animator, double dt);

/* LOD — Level of Detail */
/// @brief Add a level-of-detail mesh swap: when camera distance exceeds @p distance, use @p mesh.
/// @param node Borrowed SceneNode3D handle.
/// @param distance Non-negative camera-distance threshold in world units.
/// @param mesh Borrowed Mesh3D retained for this LOD entry.
void rt_scene_node3d_add_lod(void *node, double distance, void *mesh);

/// @brief Enable screen-size-driven selection across authored LOD meshes.
/// @param node Borrowed SceneNode3D handle.
/// @param enabled Nonzero to select LOD by projected screen error.
/// @param screen_error_px Positive target error in pixels.
void rt_scene_node3d_set_auto_lod(void *node, int8_t enabled, double screen_error_px);

/// @brief Mark a node as static bake input (lightmaps/probes/reflection captures).
/// @param node Borrowed SceneNode3D handle.
/// @param is_static Nonzero to include the node in static-bake inputs.
void rt_scene_node3d_set_static(void *node, int8_t is_static);

/// @brief True when the node is flagged static for baking.
/// @param node Borrowed SceneNode3D handle.
/// @return Nonzero when static-bake participation is enabled.
int8_t rt_scene_node3d_get_static(void *node);

/// @brief Bind a generated textured impostor proxy used at or beyond @p distance.
/// @param node Borrowed SceneNode3D handle.
/// @param distance Non-negative camera-distance threshold in world units.
/// @param pixels Borrowed Pixels image retained by the node, or `NULL` to clear.
void rt_scene_node3d_set_impostor(void *node, double distance, void *pixels);

/// @brief Bind a yaw-selected multi-frame impostor from an N-frame horizontal strip.
/// @param node Borrowed SceneNode3D handle.
/// @param distance Non-negative camera-distance threshold in world units.
/// @param pixels Borrowed horizontal-strip Pixels image retained by the node.
/// @param frames Positive number of equal-width yaw frames in the strip.
void rt_scene_node3d_set_impostor_frames(void *node, double distance, void *pixels, int64_t frames);

/// @brief Last impostor frame index selected by the draw path (0 for single-frame).
/// @param node Borrowed SceneNode3D handle.
/// @return Non-negative last selected frame index.
int64_t rt_scene_node3d_get_impostor_frame_index(void *node);

/// @brief Remove all LOD entries (revert to the base mesh at all distances).
/// @param node Borrowed SceneNode3D handle.
void rt_scene_node3d_clear_lod(void *node);

/// @brief Number of LOD entries registered on this node.
/// @param node Borrowed SceneNode3D handle.
/// @return Non-negative authored LOD entry count.
int64_t rt_scene_node3d_get_lod_count(void *node);

/// @brief Get the camera distance threshold for the @p index-th LOD entry.
/// @param node Borrowed SceneNode3D handle.
/// @param index Zero-based LOD entry index.
/// @return Stored distance threshold, or zero when out of range.
double rt_scene_node3d_get_lod_distance(void *node, int64_t index);

/// @brief Get the mesh used for the @p index-th LOD entry.
/// @param node Borrowed SceneNode3D handle.
/// @param index Zero-based LOD entry index.
/// @return Borrowed Mesh3D handle, or `NULL` when out of range or nonresident.
void *rt_scene_node3d_get_lod_mesh(void *node, int64_t index);

/// @brief Mark the @p index-th LOD mesh payload resident/nonresident.
/// @param node Borrowed SceneNode3D handle.
/// @param index Zero-based LOD entry index.
/// @param resident Nonzero to mark the payload available for selection.
void rt_scene_node3d_set_lod_resident(void *node, int64_t index, int8_t resident);

/// @brief Return whether the @p index-th LOD mesh payload is resident.
/// @param node Borrowed SceneNode3D handle.
/// @param index Zero-based LOD entry index.
/// @return Nonzero when the indexed mesh payload is resident.
int8_t rt_scene_node3d_get_lod_resident(void *node, int64_t index);

/// @brief Return estimated resident bytes for the @p index-th LOD mesh.
/// @param node Borrowed SceneNode3D handle.
/// @param index Zero-based LOD entry index.
/// @return Non-negative estimated resident bytes, or zero when unavailable.
int64_t rt_scene_node3d_get_lod_resident_bytes(void *node, int64_t index);

/* Scene3D — frustum culling stats */
/// @brief Number of nodes culled by the frustum during the most recent Draw (perf metric).
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative latest frustum-cull count.
int64_t rt_scene3d_get_culled_count(void *scene);

/// @brief Number of drawable nodes submitted by the most recent Draw.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative latest submitted-node count.
int64_t rt_scene3d_get_visible_node_count(void *scene);

/// @brief Number of drawable nodes skipped by the portal/PVS visibility mask in the latest Draw.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative latest PVS-cull count.
int64_t rt_scene3d_get_pvs_culled_count(void *scene);

/// @brief Number of authored visibility zones.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative authored zone count.
int64_t rt_scene3d_get_visibility_zone_count(void *scene);

/// @brief Number of directed visibility portal links.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative directed portal-link count.
int64_t rt_scene3d_get_visibility_portal_count(void *scene);

/// @brief Toggle portal-frustum PVS clipping (default on; off = reachability flood-fill).
/// @param scene Borrowed Scene3D handle.
/// @param enabled Nonzero for clipped portal traversal; zero for reachability only.
void rt_scene3d_set_portal_clipping(void *scene, int8_t enabled);

/// @brief Current portal-clipping mode.
/// @param scene Borrowed Scene3D handle.
/// @return Nonzero when portal frusta clip the PVS traversal.
int8_t rt_scene3d_get_portal_clipping(void *scene);

/// @brief Portal expansions evaluated during the most recent PVS build.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative latest portal-expansion count.
int64_t rt_scene3d_get_portal_traversal_count(void *scene);

/// @brief Total number of nodes in the scene graph.
/// @param scene Borrowed Scene3D handle.
/// @return Non-negative total node count, including the implicit root.
int64_t rt_scene3d_get_node_count(void *scene);

#ifdef __cplusplus
}
#endif
