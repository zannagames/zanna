//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_3d_scene_stubs.c
/// @brief Graphics-disabled scene graph, transform, particles, decals, water,
/// and post-processing stubs.
///
/// @details This split source contains the scene-family unavailable-backend
/// API surface and keeps those symbols physically separated from assets,
/// physics, media, and Canvas3D stubs.
///
// File: src/runtime/graphics/common/rt_3d_scene_stubs.c
// Purpose: Graphics-disabled 3D scene graph and Game3D scene entry points.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//   docs/adr/0161-stable-scenenode-sibling-reordering.md
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* Scene3D / SceneNode3D stubs */

/// @brief Stub for `Scene3D.New` — would normally create an empty scene
///        graph with a single root node ready to receive child nodes.
///
/// Trapping stub: scene graphs are accessed via the returned root, so a
/// NULL return would crash on the first `GetRoot` / `Add` call.
///
/// @return Never returns normally.
void *rt_scene3d_new(void) {
    rt_graphics_unavailable_("Scene3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Scene3D.Root` — get the scene's root SceneNode3D.
///
/// Silent stub returning NULL because there is no Scene3D to query.
///
/// @param s Scene3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene3d_get_root(void *s) {
    (void)s;
    return NULL;
}

/// @brief Stub for `Scene3D.Add` — would normally attach a SceneNode3D
///        as a top-level child of the scene's root.
///
/// Silent no-op stub.
///
/// @param s Scene3D handle (ignored).
/// @param n SceneNode3D handle (ignored).
void rt_scene3d_add(void *s, void *n) {
    (void)s;
    (void)n;
}

/// @brief Report that a node cannot be added to a graphics-disabled scene.
///
/// @param s Scene3D handle (ignored).
/// @param n SceneNode3D handle to add (ignored).
///
/// @return `0`, indicating that the node was not added.
int8_t rt_scene3d_try_add(void *s, void *n) {
    (void)s;
    (void)n;
    return 0;
}

/// @brief Stub for `Scene3D.Remove` — would normally detach a SceneNode3D
///        from its parent, leaving subtree intact for re-attachment.
///
/// Silent no-op stub.
///
/// @param s Scene3D handle (ignored).
/// @param n SceneNode3D handle (ignored).
void rt_scene3d_remove(void *s, void *n) {
    (void)s;
    (void)n;
}

/// @brief Stub for `Scene3D.Find` — would normally search the scene tree
///        for a node by name and return the first match.
///
/// Silent stub returning NULL (not found).
///
/// @param s Scene3D handle (ignored).
/// @param n Node name to search for (ignored).
///
/// @return `NULL`.
void *rt_scene3d_find(void *s, rt_string n) {
    (void)s;
    (void)n;
    return NULL;
}

/// @brief Stub for `Scene3D.FindOption` — recursive name lookup as Option.
/// @details Graphics-disabled builds have no scene graph to search, so the
///          modern absence-aware API returns `None`.
/// @param s Scene3D handle (ignored).
/// @param n Node name to search for (ignored).
/// @return `None`.
void *rt_scene3d_find_option(void *s, rt_string n) {
    (void)s;
    (void)n;
    return rt_option_none();
}

/// @brief Return no scene nodes for an axis-aligned bounding-box query.
///
/// @param s   Scene3D handle (ignored).
/// @param min Vec3 query-box minimum corner (ignored).
/// @param max Vec3 query-box maximum corner (ignored).
///
/// @return `NULL`, because a graphics-disabled scene contains no nodes.
void *rt_scene3d_query_aabb(void *s, void *min, void *max) {
    (void)s;
    (void)min;
    (void)max;
    return NULL;
}

/// @brief Return no scene nodes for a bounding-sphere query.
///
/// @param s      Scene3D handle (ignored).
/// @param center Vec3 query-sphere center (ignored).
/// @param radius Query-sphere radius (ignored).
///
/// @return `NULL`, because a graphics-disabled scene contains no nodes.
void *rt_scene3d_query_sphere(void *s, void *center, double radius) {
    (void)s;
    (void)center;
    (void)radius;
    return NULL;
}

/// @brief Return no scene-node hit for a graphics-disabled raycast.
///
/// @param s            Scene3D handle (ignored).
/// @param origin       Vec3 ray origin (ignored).
/// @param direction    Vec3 ray direction (ignored).
/// @param max_distance Maximum query distance (ignored).
///
/// @return `NULL`.
void *rt_scene3d_raycast_nodes(void *s, void *origin, void *direction, double max_distance) {
    (void)s;
    (void)origin;
    (void)direction;
    (void)max_distance;
    return NULL;
}

/// @brief Report that a visibility zone cannot be added without graphics support.
///
/// @param s    Scene3D handle (ignored).
/// @param name Visibility-zone name (ignored).
/// @param min  Vec3 zone minimum corner (ignored).
/// @param max  Vec3 zone maximum corner (ignored).
///
/// @return `-1`, the invalid zone index.
int64_t rt_scene3d_add_visibility_zone(void *s, rt_string name, void *min, void *max) {
    (void)s;
    (void)name;
    (void)min;
    (void)max;
    return -1;
}

/// @brief Report that a visibility portal cannot be added without graphics support.
///
/// @param s             Scene3D handle (ignored).
/// @param from_zone     Source visibility-zone index (ignored).
/// @param to_zone       Destination visibility-zone index (ignored).
/// @param bidirectional Non-zero to traverse the portal in both directions (ignored).
///
/// @return `-1`, the invalid portal index.
int64_t rt_scene3d_add_visibility_portal(void *s,
                                         int64_t from_zone,
                                         int64_t to_zone,
                                         int8_t bidirectional) {
    (void)s;
    (void)from_zone;
    (void)to_zone;
    (void)bidirectional;
    return -1;
}

/// @brief Stub for `Scene3D.Draw` — would normally walk the scene graph
///        and issue draw calls for every visible mesh node, in front-to-back
///        order with frustum culling.
///
/// Silent no-op stub.
///
/// @param s   Scene3D handle (ignored).
/// @param c   Canvas3D handle (ignored).
/// @param cam Camera3D handle (ignored).
void rt_scene3d_draw(void *s, void *c, void *cam) {
    (void)s;
    (void)c;
    (void)cam;
}

/// @brief Stub for `Scene3D.Clear` — would normally remove all top-level
///        children from the scene's root, releasing references.
///
/// Silent no-op stub.
///
/// @param s Scene3D handle (ignored).
void rt_scene3d_clear(void *s) {
    (void)s;
}

/// @brief Stub for `Scene3D.NodeCount` — would normally return the total
///        number of nodes in the scene graph (recursive count from root).
///
/// Silent stub returning `0`.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_node_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `Scene3D.Save` — would normally serialize the scene
///        graph to a `.vscn` file (Zanna's native scene format) at
///        `path`. The format records node hierarchy, transforms, and
///        bound mesh/material references; it does not embed mesh data.
///
/// Trapping stub.
///
/// @param s    Scene3D handle (ignored).
/// @param path Output filesystem path (ignored).
///
/// @return Never returns normally.
int64_t rt_scene3d_save(void *s, rt_string path) {
    (void)s;
    (void)path;
    RT_GRAPHICS_TRAP_RET("Scene3D.Save: graphics support not compiled in", 0);
}

/// @brief Stub for `Scene3D.Load` — would normally parse a `.vscn`
///        file (Zanna's native scene format) and reconstruct the node
///        hierarchy with shared resources.
///
/// Trapping stub.
///
/// @param path Filesystem path to the .vscn file (ignored).
///
/// @return Never returns normally.
void *rt_scene3d_load(rt_string path) {
    (void)path;
    rt_graphics_unavailable_("Scene3D.Load: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `Scene3D.SyncBindings` — batch-update transforms for
///        every node bound to a Physics3DBody, and advance every bound
///        AnimController3D by `dt` seconds. Should be called once per
///        frame after physics step but before draw.
///
/// Silent no-op stub.
///
/// @param s  Scene3D handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_scene3d_sync_bindings(void *s, double dt) {
    (void)s;
    (void)dt;
}

/// @brief Stub for `SceneNode3D.New` — would normally create a new
///        scene-graph node with identity transform and no children.
///        Attach to a scene via `Scene3D.Add` or `SceneNode3D.AddChild`.
///
/// Trapping stub: nodes are wired into the scene immediately after
/// creation; a NULL return would crash the caller.
///
/// @return Never returns normally.
void *rt_scene_node3d_new(void) {
    rt_graphics_unavailable_("SceneNode3D.New: graphics support not compiled in");
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetPosition` — set the node's local-
///        space position. Combined with the parent's world transform on
///        `Draw` to derive the node's world position.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param x Local x (ignored).
/// @param y Local y (ignored).
/// @param z Local z (ignored).
void rt_scene_node3d_set_position(void *n, double x, double y, double z) {
    (void)n;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `SceneNode3D.Position` — get the node's local-space
///        position as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_position(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetRotation` — set the node's local
///        rotation from a Quaternion handle.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param q Quaternion handle (ignored).
void rt_scene_node3d_set_rotation(void *n, void *q) {
    (void)n;
    (void)q;
}

/// @brief Stub for `SceneNode3D.Rotation` — get the node's local
///        rotation as a Quaternion handle.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_rotation(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetScale` — set the node's per-axis
///        local scale. `(1, 1, 1)` is identity.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param x Scale x (ignored).
/// @param y Scale y (ignored).
/// @param z Scale z (ignored).
void rt_scene_node3d_set_scale(void *n, double x, double y, double z) {
    (void)n;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `SceneNode3D.Scale` — get the node's local scale as
///        a Vec3.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_scale(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.WorldMatrix` — get the node's composed
///        world-space TRS transform as a 4x4 matrix (concatenation of
///        every ancestor's local transform). Computed lazily on Draw.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_world_matrix(void *n) {
    (void)n;
    return NULL;
}

/// @brief Return the unavailable world-space position of a graphics-disabled scene node.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_world_position(void *n) {
    (void)n;
    return NULL;
}

/// @brief Return the unavailable world-space rotation of a graphics-disabled scene node.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_world_rotation(void *n) {
    (void)n;
    return NULL;
}

/// @brief Return the unavailable world-space scale of a graphics-disabled scene node.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_world_scale(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.BindBody` — would normally attach a
///        Physics3DBody to this node so the scene-graph transform follows
///        the simulated body each frame (governed by the node's sync mode).
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param b Body3D handle, or NULL to detach (ignored).
void rt_scene_node3d_bind_body(void *n, void *b) {
    (void)n;
    (void)b;
}

/// @brief Stub for `SceneNode3D.ClearBodyBinding` — detach any bound
///        Physics3DBody from this node.
///
/// Silent no-op stub. Equivalent to `BindBody(node, NULL)`.
///
/// @param n SceneNode3D handle (ignored).
void rt_scene_node3d_clear_body_binding(void *n) {
    (void)n;
}

/// @brief Stub for `SceneNode3D.Body` — get the bound Physics3DBody, or
///        NULL if none is bound.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_body(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetSyncMode` — controls how the node and
///        its bound Physics3DBody exchange transforms each frame.
///        0 = none, 1 = body→node (kinematic visual), 2 = node→body
///        (kinematic physics).
///
/// Silent no-op stub.
///
/// @param n    SceneNode3D handle (ignored).
/// @param mode Sync mode, 0..2 (ignored).
void rt_scene_node3d_set_sync_mode(void *n, int64_t mode) {
    (void)n;
    (void)mode;
}

/// @brief Stub for `SceneNode3D.SyncMode` — get the current body/node
///        sync mode (see `SetSyncMode` for the value space).
///
/// Silent stub returning `0` (no sync).
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene_node3d_get_sync_mode(void *n) {
    (void)n;
    return 0;
}

/// @brief Stub for `SceneNode3D.BindAnimator` — attach an
///        AnimController3D so `Scene3D.SyncBindings(dt)` advances the
///        controller and applies the resulting pose to this node's skeleton.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param c AnimController3D handle, or NULL to detach (ignored).
void rt_scene_node3d_bind_animator(void *n, void *c) {
    (void)n;
    (void)c;
}

/// @brief Stub for `SceneNode3D.ClearAnimatorBinding` — detach any bound
///        AnimController3D from this node.
///
/// Silent no-op stub. Equivalent to `BindAnimator(node, NULL)`.
///
/// @param n SceneNode3D handle (ignored).
void rt_scene_node3d_clear_animator_binding(void *n) {
    (void)n;
}

/// @brief Stub for `SceneNode3D.Animator` — get the bound
///        AnimController3D, or NULL if none.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_animator(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.BindNodeAnimator` — attach a NodeAnimator3D to drive
///        scene-node TRS or morph weights.
///
/// @param n SceneNode3D handle (ignored).
/// @param a NodeAnimator3D handle, or `NULL` to detach (ignored).
void rt_scene_node3d_bind_node_animator(void *n, void *a) {
    (void)n;
    (void)a;
}

/// @brief Stub for `SceneNode3D.ClearNodeAnimatorBinding` — detach any bound node animator.
///
/// @param n SceneNode3D handle (ignored).
void rt_scene_node3d_clear_node_animator_binding(void *n) {
    (void)n;
}

/// @brief Stub for `SceneNode3D.NodeAnimator` — get the bound NodeAnimator3D, or NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_node_animator(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.AddChild` — would normally append `c` as
///        the last child of `n`, taking ownership of the reference.
///
/// Silent no-op stub.
///
/// @param n Parent SceneNode3D handle (ignored).
/// @param c Child SceneNode3D handle (ignored).
void rt_scene_node3d_add_child(void *n, void *c) {
    (void)n;
    (void)c;
}

/// @brief Stub for `SceneNode3D.RemoveChild` — detach `c` from `n` (if it
///        is a child); the subtree is preserved for re-attachment.
///
/// Silent no-op stub.
///
/// @param n Parent SceneNode3D handle (ignored).
/// @param c Child SceneNode3D handle (ignored).
void rt_scene_node3d_remove_child(void *n, void *c) {
    (void)n;
    (void)c;
}

/// @brief Stub for `SceneNode3D.TryMoveChild` in graphics-disabled builds.
///
/// @param n     Parent SceneNode3D handle (ignored).
/// @param c     Child SceneNode3D handle to reorder (ignored).
/// @param index Requested sibling index (ignored).
///
/// @return Always `0`; no hierarchy exists without graphics support.
int8_t rt_scene_node3d_try_move_child(void *n, void *c, int64_t index) {
    (void)n;
    (void)c;
    (void)index;
    return 0;
}

/// @brief Stub for `SceneNode3D.ChildCount` — number of direct children
///        (not recursive).
///
/// Silent stub returning `0`.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene_node3d_child_count(void *n) {
    (void)n;
    return 0;
}

/// @brief Stub for `SceneNode3D.Child(i)` — get the `i`th direct child by
///        insertion order.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
/// @param i Child index, 0..ChildCount-1 (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_child(void *n, int64_t i) {
    (void)n;
    (void)i;
    return NULL;
}

/// @brief Stub for `SceneNode3D.Parent` — get the parent node, or NULL
///        for the scene root.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_parent(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.Find` — recursive name lookup within the
///        subtree rooted at `n`. Returns the first match or NULL.
///
/// Silent stub returning NULL.
///
/// @param n    SceneNode3D handle (ignored).
/// @param name Name to search for (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_find(void *n, rt_string name) {
    (void)n;
    (void)name;
    return NULL;
}

/// @brief Stub for `SceneNode3D.FindOption` — recursive subtree lookup as Option.
/// @details Graphics-disabled builds have no node hierarchy to search, so the
///          modern absence-aware API returns `None`.
/// @param n SceneNode3D handle (ignored).
/// @param name Node name to search for (ignored).
/// @return `None`.
void *rt_scene_node3d_find_option(void *n, rt_string name) {
    (void)n;
    (void)name;
    return rt_option_none();
}

/// @brief Stub for `SceneNode3D.SetMesh` — bind a Mesh3D to this node so
///        it will be drawn at the node's world transform during
///        `Scene3D.Draw`.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param m Mesh3D handle, or NULL to make this node a transform-only
///          parent (ignored).
void rt_scene_node3d_set_mesh(void *n, void *m) {
    (void)n;
    (void)m;
}

/// @brief Stub for `SceneNode3D.Mesh` — get the bound Mesh3D, or NULL if
///        none.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_mesh(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetMaterial` — bind a Material3D used
///        when drawing this node's mesh. If unset, the renderer falls back
///        to a default white Blinn-Phong material.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param m Material3D handle, or NULL (ignored).
void rt_scene_node3d_set_material(void *n, void *m) {
    (void)n;
    (void)m;
}

/// @brief Stub for `SceneNode3D.Material` — get the bound Material3D, or
///        NULL if none.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_material(void *n) {
    (void)n;
    return NULL;
}

/// @brief Graphics-disabled SceneNode.Camera setter stub.
///
/// @param n      SceneNode3D handle (ignored).
/// @param camera Camera3D handle, or `NULL` to detach (ignored).
void rt_scene_node3d_set_camera(void *n, void *camera) {
    (void)n;
    (void)camera;
}

/// @brief Graphics-disabled SceneNode.Camera getter stub.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_camera(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetVisible`.
///
/// Silent no-op by default, or an unavailable-graphics trap when
/// `ZANNA_GRAPHICS_STUBS_STRICT` is enabled.
///
/// @param n SceneNode3D handle (ignored).
/// @param v Non-zero to make visible (ignored).
void rt_scene_node3d_set_visible(void *n, int8_t v) {
    (void)n;
    (void)v;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("SceneNode3D.SetVisible: graphics support not compiled in");
}

/// @brief Stub for `SceneNode3D.Visible` — get the visibility flag.
///
/// Silent stub returning `1`, matching the real implementation default, or an
/// unavailable-graphics trap when `ZANNA_GRAPHICS_STUBS_STRICT` is enabled.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `1`.
int8_t rt_scene_node3d_get_visible(void *n) {
    (void)n;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneNode3D.Visible: graphics support not compiled in", 1);
}

/// @brief Stub for `SceneNode3D.SetName` — assign a name to the node so
///        it can be located via `Scene3D.Find` / `SceneNode3D.Find`.
///
/// Silent no-op stub. Names are not required to be unique.
///
/// @param n SceneNode3D handle (ignored).
/// @param s Name string (ignored).
void rt_scene_node3d_set_name(void *n, rt_string s) {
    (void)n;
    (void)s;
}

/// @brief Stub for `SceneNode3D.Name` — get the assigned name.
///
/// Silent stub returning an owned empty runtime string.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return An owned empty runtime string.
rt_string rt_scene_node3d_get_name(void *n) {
    (void)n;
    return rt_const_cstr("");
}

/// @brief Return the unavailable metadata-key collection for a graphics-disabled node.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_metadata_keys(void *n) {
    (void)n;
    return NULL;
}

/// @brief Return the fallback kind name for an absent metadata entry.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
///
/// @return An empty constant runtime string.
rt_string rt_scene_node3d_metadata_kind(void *n, rt_string key) {
    (void)n;
    (void)key;
    return rt_const_cstr("");
}

/// @brief Report whether a graphics-disabled scene node contains a metadata key.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
///
/// @return `0`, because the stub stores no metadata.
int8_t rt_scene_node3d_metadata_has(void *n, rt_string key) {
    (void)n;
    (void)key;
    return 0;
}

/// @brief Return an integer default when node metadata is unavailable.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
/// @param def Caller-provided fallback value.
///
/// @return `def`.
int64_t rt_scene_node3d_metadata_get_int(void *n, rt_string key, int64_t def) {
    (void)n;
    (void)key;
    return def;
}

/// @brief Return a floating-point default when node metadata is unavailable.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
/// @param def Caller-provided fallback value.
///
/// @return `def`.
double rt_scene_node3d_metadata_get_float(void *n, rt_string key, double def) {
    (void)n;
    (void)key;
    return def;
}

/// @brief Return a normalized Boolean default when node metadata is unavailable.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
/// @param def Caller-provided fallback truth value.
///
/// @return `1` when `def` is non-zero; otherwise `0`.
int8_t rt_scene_node3d_metadata_get_bool(void *n, rt_string key, int8_t def) {
    (void)n;
    (void)key;
    return def ? 1 : 0;
}

/// @brief Return a retained string default when node metadata is unavailable.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
/// @param def Caller-provided runtime string default.
///
/// @return A retained reference to `def` when it is a valid string handle;
///         otherwise an empty constant runtime string.
rt_string rt_scene_node3d_metadata_get_string(void *n, rt_string key, rt_string def) {
    (void)n;
    (void)key;
    return def && rt_string_is_handle(def) ? rt_string_ref(def) : rt_const_cstr("");
}

/// @brief Report that a null metadata value was not stored on the stub node.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_set_null(void *n, rt_string key) {
    (void)n;
    (void)key;
    return 0;
}

/// @brief Report that an integer metadata value was not stored on the stub node.
///
/// @param n     SceneNode3D handle (ignored).
/// @param key   Metadata key (ignored).
/// @param value Integer value (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_set_int(void *n, rt_string key, int64_t value) {
    (void)n;
    (void)key;
    (void)value;
    return 0;
}

/// @brief Report that a floating-point metadata value was not stored on the stub node.
///
/// @param n     SceneNode3D handle (ignored).
/// @param key   Metadata key (ignored).
/// @param value Floating-point value (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_set_float(void *n, rt_string key, double value) {
    (void)n;
    (void)key;
    (void)value;
    return 0;
}

/// @brief Report that a Boolean metadata value was not stored on the stub node.
///
/// @param n     SceneNode3D handle (ignored).
/// @param key   Metadata key (ignored).
/// @param value Boolean value (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_set_bool(void *n, rt_string key, int8_t value) {
    (void)n;
    (void)key;
    (void)value;
    return 0;
}

/// @brief Report that a string metadata value was not stored on the stub node.
///
/// @param n     SceneNode3D handle (ignored).
/// @param key   Metadata key (ignored).
/// @param value Runtime string value (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_set_string(void *n, rt_string key, rt_string value) {
    (void)n;
    (void)key;
    (void)value;
    return 0;
}

/// @brief Report that no metadata entry was removed from the stub node.
///
/// @param n   SceneNode3D handle (ignored).
/// @param key Metadata key to remove (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_metadata_remove(void *n, rt_string key) {
    (void)n;
    (void)key;
    return 0;
}

/// @brief Stub for `SceneNode3D.BoundsMin` — get the min corner of the
///        node's world-space axis-aligned bounding box.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_aabb_min(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `SceneNode3D.BoundsMax` — get the max corner of the
///        node's world-space axis-aligned bounding box.
///
/// Silent stub returning NULL.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_aabb_max(void *n) {
    (void)n;
    return NULL;
}

/// @brief Stub for `Scene3D.CulledCount` — number of nodes that were
///        skipped during the most recent `Draw` due to frustum culling
///        (debug / profiling).
///
/// Silent stub returning `0`.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_culled_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Return the fallback number of visible nodes from the most recent draw.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_visible_node_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Return the fallback number of nodes rejected by potential-visibility culling.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_pvs_culled_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Return the number of visibility zones in a graphics-disabled scene.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_visibility_zone_count(void *s) {
    (void)s;
    return 0;
}

/// @brief Return the number of visibility portals in a graphics-disabled scene.
///
/// @param s Scene3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene3d_get_visibility_portal_count(void *s) {
    (void)s;
    return 0;
}

/* LOD stubs */

/// @brief Stub for `SceneNode3D.AddLOD` — add a level-of-detail entry:
///        when the camera is `>= d` world units away, render `m` instead
///        of the node's primary mesh. Multiple LODs can be stacked at
///        increasing distances.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param d Distance threshold for this LOD level (ignored).
/// @param m Mesh3D handle to render at or beyond `d` (ignored).
void rt_scene_node3d_add_lod(void *n, double d, void *m) {
    (void)n;
    (void)d;
    (void)m;
}

/// @brief Stub for `SceneNode3D.SetAutoLOD` — enable screen-error selection
///        over authored LOD entries.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param e Non-zero to enable (ignored).
/// @param p Screen-error threshold in pixels (ignored).
void rt_scene_node3d_set_auto_lod(void *n, int8_t e, double p) {
    (void)n;
    (void)e;
    (void)p;
}

/// @brief Stub for `SceneNode3D.SetImpostor` — bind a distant textured proxy.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
/// @param d Distance threshold (ignored).
/// @param p Pixels handle (ignored).
void rt_scene_node3d_set_impostor(void *n, double d, void *p) {
    (void)n;
    (void)d;
    (void)p;
}

/// @brief Silent stub for `SceneNode.SetImpostorFrames` — no-op.
///
/// @param node     SceneNode3D handle (ignored).
/// @param distance Camera-distance threshold for the impostor (ignored).
/// @param pixels   Pixels handle containing the impostor frame strip (ignored).
/// @param frames   Number of frames in the strip (ignored).
void rt_scene_node3d_set_impostor_frames(void *node,
                                         double distance,
                                         void *pixels,
                                         int64_t frames) {
    (void)node;
    (void)distance;
    (void)pixels;
    (void)frames;
}

/// @brief Silent stub for `SceneNode.GetImpostorFrameIndex` — no-op; returns 0.
///
/// @param node SceneNode3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene_node3d_get_impostor_frame_index(void *node) {
    (void)node;
    return 0;
}

/// @brief Silent stub for `SceneNode.SetStatic` — no-op.
///
/// @param node      SceneNode3D handle (ignored).
/// @param is_static Non-zero to classify the node as static geometry (ignored).
void rt_scene_node3d_set_static(void *node, int8_t is_static) {
    (void)node;
    (void)is_static;
}

/// @brief Silent stub for `SceneNode.GetStatic` — no-op; returns 0.
///
/// @param node SceneNode3D handle (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_get_static(void *node) {
    (void)node;
    return 0;
}

/// @brief Silent stub for `LightBaker3D.New` — no-op; returns NULL.
///
/// @param scene Scene3D whose static lighting would be baked (ignored).
///
/// @return `NULL`.
void *rt_lightbaker3d_new(void *scene) {
    (void)scene;
    return 0;
}

/// @brief Silent stub for `LightBaker3D.set_TexelsPerUnit` — no-op.
///
/// @param baker  LightBaker3D handle (ignored).
/// @param texels Lightmap texel density per world unit (ignored).
void rt_lightbaker3d_set_texels_per_unit(void *baker, double texels) {
    (void)baker;
    (void)texels;
}

/// @brief Silent stub for `LightBaker3D.get_TexelsPerUnit` — no-op; returns 0.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `0.0`.
double rt_lightbaker3d_get_texels_per_unit(void *baker) {
    (void)baker;
    return 0.0;
}

/// @brief Silent stub for `LightBaker3D.set_Samples` — no-op.
///
/// @param baker   LightBaker3D handle (ignored).
/// @param samples Samples per texel or probe (ignored).
void rt_lightbaker3d_set_samples(void *baker, int64_t samples) {
    (void)baker;
    (void)samples;
}

/// @brief Silent stub for `LightBaker3D.get_Samples` — no-op; returns 0.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `0`.
int64_t rt_lightbaker3d_get_samples(void *baker) {
    (void)baker;
    return 0;
}

/// @brief Silent stub for `LightBaker3D.set_Bounces` — no-op.
///
/// @param baker   LightBaker3D handle (ignored).
/// @param bounces Indirect-light bounce count (ignored).
void rt_lightbaker3d_set_bounces(void *baker, int64_t bounces) {
    (void)baker;
    (void)bounces;
}

/// @brief Silent stub for `LightBaker3D.get_Bounces` — no-op; returns 0.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `0`.
int64_t rt_lightbaker3d_get_bounces(void *baker) {
    (void)baker;
    return 0;
}

/// @brief Silent stub for `LightBaker3D.SetSkyColor` — no-op.
///
/// @param baker LightBaker3D handle (ignored).
/// @param r     Red component of the ambient sky color (ignored).
/// @param g     Green component of the ambient sky color (ignored).
/// @param b     Blue component of the ambient sky color (ignored).
void rt_lightbaker3d_set_sky_color(void *baker, double r, double g, double b) {
    (void)baker;
    (void)r;
    (void)g;
    (void)b;
}

/// @brief Silent stub for `LightBaker3D.get_Progress` — no-op; returns 0.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `0.0`, the fallback completion fraction.
double rt_lightbaker3d_get_progress(void *baker) {
    (void)baker;
    return 0.0;
}

/// @brief Silent stub for `LightBaker3D.AddLight` — no-op.
///
/// @param baker LightBaker3D handle (ignored).
/// @param light Light3D handle to include in the bake (ignored).
void rt_lightbaker3d_add_light(void *baker, void *light) {
    (void)baker;
    (void)light;
}

/// @brief Silent stub for `LightBaker3D.BakeStep` — no-op; reports done.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `1`, indicating that no further bake work is required.
int8_t rt_lightbaker3d_bake_step(void *baker) {
    (void)baker;
    return 1;
}

/// @brief Silent stub for `LightBaker3D.Apply` — no-op.
///
/// @param baker LightBaker3D handle (ignored).
void rt_lightbaker3d_apply(void *baker) {
    (void)baker;
}

/// @brief Silent stub for `LightBaker3D.get_Atlas` — no-op; returns NULL.
///
/// @param baker LightBaker3D handle (ignored).
///
/// @return `NULL`.
void *rt_lightbaker3d_get_atlas(void *baker) {
    (void)baker;
    return 0;
}

/// @brief Silent stub for `LightProbeGrid3D.New` — no-op; returns NULL.
///
/// @param min_v   Vec3 grid minimum corner (ignored).
/// @param max_v   Vec3 grid maximum corner (ignored).
/// @param spacing Distance between adjacent probes (ignored).
///
/// @return `NULL`.
void *rt_lightprobegrid3d_new(void *min_v, void *max_v, double spacing) {
    (void)min_v;
    (void)max_v;
    (void)spacing;
    return 0;
}

/// @brief Silent stub for `LightProbeGrid3D.get_ProbeCount` — no-op; returns 0.
///
/// @param grid LightProbeGrid3D handle (ignored).
///
/// @return `0`.
int64_t rt_lightprobegrid3d_get_probe_count(void *grid) {
    (void)grid;
    return 0;
}

/// @brief Silent stub for `LightProbeGrid3D.Bake` — no-op.
///
/// @param grid  LightProbeGrid3D handle (ignored).
/// @param baker LightBaker3D providing the baked lighting inputs (ignored).
void rt_lightprobegrid3d_bake(void *grid, void *baker) {
    (void)grid;
    (void)baker;
}

/// @brief Silent stub for `LightProbeGrid3D.Sample` — no-op; returns NULL.
///
/// @param grid     LightProbeGrid3D handle (ignored).
/// @param position Vec3 world-space sample position (ignored).
/// @param normal   Vec3 surface normal used for directional sampling (ignored).
///
/// @return `NULL`.
void *rt_lightprobegrid3d_sample(void *grid, void *position, void *normal) {
    (void)grid;
    (void)position;
    (void)normal;
    return 0;
}

/// @brief Silent stub for `LightProbeGrid3D.Save` — no-op; returns 0.
///
/// @param grid LightProbeGrid3D handle (ignored).
/// @param path Destination file path (ignored).
///
/// @return `0`, indicating that no data was saved.
int8_t rt_lightprobegrid3d_save(void *grid, rt_string path) {
    (void)grid;
    (void)path;
    return 0;
}

/// @brief Silent stub for `LightProbeGrid3D.Load` — no-op; returns 0.
///
/// @param grid LightProbeGrid3D handle (ignored).
/// @param path Source file path (ignored).
///
/// @return `0`, indicating that no data was loaded.
int8_t rt_lightprobegrid3d_load(void *grid, rt_string path) {
    (void)grid;
    (void)path;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.New` — no-op; returns NULL.
///
/// @param position Vec3 probe position (ignored).
/// @param box_min  Vec3 influence-box minimum relative to the probe (ignored).
/// @param box_max  Vec3 influence-box maximum relative to the probe (ignored).
///
/// @return `NULL`.
void *rt_reflectionprobe3d_new(void *position, void *box_min, void *box_max) {
    (void)position;
    (void)box_min;
    (void)box_max;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.get_Position` — no-op; returns NULL.
///
/// @param probe ReflectionProbe3D handle (ignored).
///
/// @return `NULL`.
void *rt_reflectionprobe3d_get_position(void *probe) {
    (void)probe;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.set_InfluenceScale` — no-op.
///
/// @param probe ReflectionProbe3D handle (ignored).
/// @param scale Influence-volume multiplier (ignored).
void rt_reflectionprobe3d_set_influence_scale(void *probe, double scale) {
    (void)probe;
    (void)scale;
}

/// @brief Silent stub for `ReflectionProbe3D.get_InfluenceScale` — no-op; returns 0.
///
/// @param probe ReflectionProbe3D handle (ignored).
///
/// @return `0.0`.
double rt_reflectionprobe3d_get_influence_scale(void *probe) {
    (void)probe;
    return 0.0;
}

/// @brief Silent stub for `ReflectionProbe3D.set_Resolution` — no-op.
///
/// @param probe      ReflectionProbe3D handle (ignored).
/// @param resolution Cubemap face resolution in pixels (ignored).
void rt_reflectionprobe3d_set_resolution(void *probe, int64_t resolution) {
    (void)probe;
    (void)resolution;
}

/// @brief Silent stub for `ReflectionProbe3D.get_Resolution` — no-op; returns 0.
///
/// @param probe ReflectionProbe3D handle (ignored).
///
/// @return `0`.
int64_t rt_reflectionprobe3d_get_resolution(void *probe) {
    (void)probe;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.set_CaptureDirty` — no-op.
///
/// @param probe ReflectionProbe3D handle (ignored).
/// @param dirty Non-zero to request a fresh cubemap capture (ignored).
void rt_reflectionprobe3d_set_capture_dirty(void *probe, int8_t dirty) {
    (void)probe;
    (void)dirty;
}

/// @brief Silent stub for `ReflectionProbe3D.get_CaptureDirty` — no-op; returns 0.
///
/// @param probe ReflectionProbe3D handle (ignored).
///
/// @return `0`.
int8_t rt_reflectionprobe3d_get_capture_dirty(void *probe) {
    (void)probe;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.Contains` — no-op; returns 0.
///
/// @param probe    ReflectionProbe3D handle (ignored).
/// @param position Vec3 world-space query position (ignored).
///
/// @return `0`, indicating that the point is outside the absent influence volume.
int8_t rt_reflectionprobe3d_contains(void *probe, void *position) {
    (void)probe;
    (void)position;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.get_Cubemap` — no-op; returns NULL.
///
/// @param probe ReflectionProbe3D handle (ignored).
///
/// @return `NULL`.
void *rt_reflectionprobe3d_get_cubemap(void *probe) {
    (void)probe;
    return 0;
}

/// @brief Silent stub for `ReflectionProbe3D.Capture` — no-op; returns 0.
///
/// @param probe  ReflectionProbe3D handle (ignored).
/// @param canvas Canvas3D used for the cubemap render (ignored).
/// @param scene  Scene3D to capture (ignored).
///
/// @return `0`, indicating that no cubemap was captured.
int8_t rt_reflectionprobe3d_capture(void *probe, void *canvas, void *scene) {
    (void)probe;
    (void)canvas;
    (void)scene;
    return 0;
}

/// @brief Silent stub for `Sky3D.New` — no-op; returns NULL.
///
/// @return `NULL`.
void *rt_sky3d_new(void) {
    return 0;
}

/// @brief Silent stub for `Sky3D.SetSunDirection` — no-op.
///
/// @param sky       Sky3D handle (ignored).
/// @param direction Vec3 direction toward the sun (ignored).
void rt_sky3d_set_sun_direction(void *sky, void *direction) {
    (void)sky;
    (void)direction;
}

/// @brief Silent stub for `Sky3D.set_Turbidity` — no-op.
///
/// @param sky       Sky3D handle (ignored).
/// @param turbidity Atmospheric turbidity value (ignored).
void rt_sky3d_set_turbidity(void *sky, double turbidity) {
    (void)sky;
    (void)turbidity;
}

/// @brief Silent stub for `Sky3D.get_Turbidity` — no-op; returns 0.
///
/// @param sky Sky3D handle (ignored).
///
/// @return `0.0`.
double rt_sky3d_get_turbidity(void *sky) {
    (void)sky;
    return 0.0;
}

/// @brief Silent stub for `Sky3D.SetGroundAlbedo` — no-op.
///
/// @param sky Sky3D handle (ignored).
/// @param r   Red component of ground reflectance (ignored).
/// @param g   Green component of ground reflectance (ignored).
/// @param b   Blue component of ground reflectance (ignored).
void rt_sky3d_set_ground_albedo(void *sky, double r, double g, double b) {
    (void)sky;
    (void)r;
    (void)g;
    (void)b;
}

/// @brief Silent stub for `Sky3D.set_Resolution` — no-op.
///
/// @param sky        Sky3D handle (ignored).
/// @param resolution Generated cubemap face resolution (ignored).
void rt_sky3d_set_resolution(void *sky, int64_t resolution) {
    (void)sky;
    (void)resolution;
}

/// @brief Silent stub for `Sky3D.get_Resolution` — no-op; returns 0.
///
/// @param sky Sky3D handle (ignored).
///
/// @return `0`.
int64_t rt_sky3d_get_resolution(void *sky) {
    (void)sky;
    return 0;
}

/// @brief Silent stub for `Sky3D.get_Dirty` — no-op; returns 0.
///
/// @param sky Sky3D handle (ignored).
///
/// @return `0`.
int8_t rt_sky3d_get_dirty(void *sky) {
    (void)sky;
    return 0;
}

/// @brief Silent stub for `Sky3D.Update` — no-op; returns 0.
///
/// @param sky    Sky3D handle (ignored).
/// @param canvas Canvas3D used to generate the sky cubemap (ignored).
///
/// @return `0`, indicating that no cubemap update occurred.
int8_t rt_sky3d_update(void *sky, void *canvas) {
    (void)sky;
    (void)canvas;
    return 0;
}

/// @brief Silent stub for `Sky3D.get_Cubemap` — no-op; returns NULL.
///
/// @param sky Sky3D handle (ignored).
///
/// @return `NULL`.
void *rt_sky3d_get_cubemap(void *sky) {
    (void)sky;
    return 0;
}

/// @brief Silent stub for `TimeOfDay3D.New` — no-op; returns NULL.
///
/// @return `NULL`.
void *rt_timeofday3d_new(void) {
    return 0;
}

/// @brief Silent stub for `TimeOfDay3D.set_Hours` — no-op.
///
/// @param tod   TimeOfDay3D handle (ignored).
/// @param hours Time of day in hours (ignored).
void rt_timeofday3d_set_hours(void *tod, double hours) {
    (void)tod;
    (void)hours;
}

/// @brief Silent stub for `TimeOfDay3D.get_Hours` — no-op; returns 0.
///
/// @param tod TimeOfDay3D handle (ignored).
///
/// @return `0.0`.
double rt_timeofday3d_get_hours(void *tod) {
    (void)tod;
    return 0.0;
}

/// @brief Silent stub for `TimeOfDay3D.set_DayLengthSeconds` — no-op.
///
/// @param tod     TimeOfDay3D handle (ignored).
/// @param seconds Real-time duration of one simulated day (ignored).
void rt_timeofday3d_set_day_length_seconds(void *tod, double seconds) {
    (void)tod;
    (void)seconds;
}

/// @brief Silent stub for `TimeOfDay3D.get_DayLengthSeconds` — no-op; returns 0.
///
/// @param tod TimeOfDay3D handle (ignored).
///
/// @return `0.0`.
double rt_timeofday3d_get_day_length_seconds(void *tod) {
    (void)tod;
    return 0.0;
}

/// @brief Silent stub for `TimeOfDay3D.set_LatitudeDegrees` — no-op.
///
/// @param tod     TimeOfDay3D handle (ignored).
/// @param degrees Observer latitude in degrees (ignored).
void rt_timeofday3d_set_latitude_degrees(void *tod, double degrees) {
    (void)tod;
    (void)degrees;
}

/// @brief Silent stub for `TimeOfDay3D.get_LatitudeDegrees` — no-op; returns 0.
///
/// @param tod TimeOfDay3D handle (ignored).
///
/// @return `0.0`.
double rt_timeofday3d_get_latitude_degrees(void *tod) {
    (void)tod;
    return 0.0;
}

/// @brief Silent stub for `TimeOfDay3D.set_RefreshDegrees` — no-op.
///
/// @param tod     TimeOfDay3D handle (ignored).
/// @param degrees Minimum sun-angle change before dependent captures refresh (ignored).
void rt_timeofday3d_set_refresh_degrees(void *tod, double degrees) {
    (void)tod;
    (void)degrees;
}

/// @brief Silent stub for `TimeOfDay3D.get_RefreshDegrees` — no-op; returns 0.
///
/// @param tod TimeOfDay3D handle (ignored).
///
/// @return `0.0`.
double rt_timeofday3d_get_refresh_degrees(void *tod) {
    (void)tod;
    return 0.0;
}

/// @brief Silent stub for `TimeOfDay3D.SetSunLight` — no-op.
///
/// @param tod   TimeOfDay3D handle (ignored).
/// @param light Directional Light3D representing the sun (ignored).
void rt_timeofday3d_set_sun_light(void *tod, void *light) {
    (void)tod;
    (void)light;
}

/// @brief Silent stub for `TimeOfDay3D.SetSky` — no-op.
///
/// @param tod TimeOfDay3D handle (ignored).
/// @param sky Sky3D handle to update (ignored).
void rt_timeofday3d_set_sky(void *tod, void *sky) {
    (void)tod;
    (void)sky;
}

/// @brief Silent stub for `TimeOfDay3D.SetReflectionProbe` — no-op.
///
/// @param tod   TimeOfDay3D handle (ignored).
/// @param probe ReflectionProbe3D handle to refresh as lighting changes (ignored).
void rt_timeofday3d_set_reflection_probe(void *tod, void *probe) {
    (void)tod;
    (void)probe;
}

/// @brief Silent stub for the raw sun-direction query — no-op; writes up.
///
/// @param tod     TimeOfDay3D handle (ignored).
/// @param out_dir Optional `double[3]` output receiving `(0, 1, 0)`.
void rt_timeofday3d_get_sun_direction_raw(void *tod, double out_dir[3]) {
    (void)tod;
    if (out_dir) {
        out_dir[0] = 0.0;
        out_dir[1] = 1.0;
        out_dir[2] = 0.0;
    }
}

/// @brief Silent stub for `TimeOfDay3D.get_SunDirection` — no-op; returns NULL.
///
/// @param tod TimeOfDay3D handle (ignored).
///
/// @return `NULL`.
void *rt_timeofday3d_get_sun_direction(void *tod) {
    (void)tod;
    return 0;
}

/// @brief Silent stub for `TimeOfDay3D.Advance` — no-op.
///
/// @param tod    TimeOfDay3D handle (ignored).
/// @param dt     Elapsed real time in seconds (ignored).
/// @param canvas Canvas3D used for dependent environment updates (ignored).
void rt_timeofday3d_advance(void *tod, double dt, void *canvas) {
    (void)tod;
    (void)dt;
    (void)canvas;
}

/// @brief Stub for `SceneNode3D.ClearLOD` — remove all LOD entries.
///        After this the node always renders its primary mesh regardless
///        of camera distance.
///
/// Silent no-op stub.
///
/// @param n SceneNode3D handle (ignored).
void rt_scene_node3d_clear_lod(void *n) {
    (void)n;
}

/// @brief Stub for `SceneNode3D.LODCount` — number of LOD entries
///        attached to the node.
///
/// Silent stub returning `0`.
///
/// @param n SceneNode3D handle (ignored).
///
/// @return `0`.
int64_t rt_scene_node3d_get_lod_count(void *n) {
    (void)n;
    return 0;
}

/// @brief Stub for `SceneNode3D.LODDistance(i)` — get the distance
///        threshold of the `i`th LOD entry.
///
/// Silent stub returning `0.0`.
///
/// @param n     SceneNode3D handle (ignored).
/// @param index LOD index, 0..LODCount-1 (ignored).
///
/// @return `0.0`.
double rt_scene_node3d_get_lod_distance(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0.0;
}

/// @brief Stub for `SceneNode3D.LODMesh(i)` — get the Mesh3D associated
///        with the `i`th LOD entry.
///
/// Silent stub returning NULL.
///
/// @param n     SceneNode3D handle (ignored).
/// @param index LOD index (ignored).
///
/// @return `NULL`.
void *rt_scene_node3d_get_lod_mesh(void *n, int64_t index) {
    (void)n;
    (void)index;
    return NULL;
}

/// @brief Stub for `SceneNode3D.SetLodResident`.
///
/// Silent no-op stub.
///
/// @param n        SceneNode3D handle (ignored).
/// @param index    LOD entry index (ignored).
/// @param resident Non-zero to request resident mesh data (ignored).
void rt_scene_node3d_set_lod_resident(void *n, int64_t index, int8_t resident) {
    (void)n;
    (void)index;
    (void)resident;
}

/// @brief Stub for `SceneNode3D.GetLodResident`.
///
/// Silent stub returning `false`.
///
/// @param n     SceneNode3D handle (ignored).
/// @param index LOD entry index (ignored).
///
/// @return `0`.
int8_t rt_scene_node3d_get_lod_resident(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0;
}

/// @brief Stub for `SceneNode3D.GetLodResidentBytes`.
///
/// Silent stub returning `0`.
///
/// @param n     SceneNode3D handle (ignored).
/// @param index LOD entry index (ignored).
///
/// @return `0`.
int64_t rt_scene_node3d_get_lod_resident_bytes(void *n, int64_t index) {
    (void)n;
    (void)index;
    return 0;
}

/* Stub-parity audit additions: entry points registered in the 3D runtime
 * defs whose implementations compile only in graphics-enabled builds. */

/// @brief Trapping stub for `SceneNode.GenerateLODs` (graphics-disabled build).
///
/// @param o  SceneNode3D handle (ignored before trapping).
/// @param a1 Number of LOD levels to generate (ignored before trapping).
/// @param a2 Simplification ratio between levels (ignored before trapping).
void rt_scene_node3d_generate_lods(void *o, int64_t a1, double a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_TRAP_VOID("SceneNode.GenerateLODs: graphics support not compiled in");
}

/// @brief Trapping stub for `SceneNode.SetTransform` (graphics-disabled build).
///
/// The ten scalar components encode translation, quaternion rotation, and
/// scale in the runtime's flattened transform convention.
///
/// @param o   SceneNode3D handle (ignored before trapping).
/// @param a1  Translation X (ignored before trapping).
/// @param a2  Translation Y (ignored before trapping).
/// @param a3  Translation Z (ignored before trapping).
/// @param a4  Quaternion X (ignored before trapping).
/// @param a5  Quaternion Y (ignored before trapping).
/// @param a6  Quaternion Z (ignored before trapping).
/// @param a7  Quaternion W (ignored before trapping).
/// @param a8  Scale X (ignored before trapping).
/// @param a9  Scale Y (ignored before trapping).
/// @param a10 Scale Z (ignored before trapping).
void rt_scene_node3d_set_transform(void *o,
                                   double a1,
                                   double a2,
                                   double a3,
                                   double a4,
                                   double a5,
                                   double a6,
                                   double a7,
                                   double a8,
                                   double a9,
                                   double a10) {
    (void)o;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    (void)a7;
    (void)a8;
    (void)a9;
    (void)a10;
    RT_GRAPHICS_TRAP_VOID("SceneNode.SetTransform: graphics support not compiled in");
}

/// @brief Silent fallback stub for `SceneGraph.get_PortalClipping` (graphics-disabled build).
///
/// @param o Scene3D handle (ignored).
///
/// @return `0`, indicating that portal clipping is disabled.
int8_t rt_scene3d_get_portal_clipping(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SceneGraph.get_PortalClipping: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `SceneGraph.get_PortalTraversalCount` (graphics-disabled build).
///
/// @param o Scene3D handle (ignored).
///
/// @return `0`, because no visibility portals were traversed.
int64_t rt_scene3d_get_portal_traversal_count(void *o) {
    (void)o;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "SceneGraph.get_PortalTraversalCount: graphics support not compiled in", 0);
}

/// @brief Trapping stub for `SceneGraph.SetNodeTransforms` (graphics-disabled build).
///
/// @param o  Scene3D handle (ignored before trapping).
/// @param a1 Node collection or transform-target data (ignored before trapping).
/// @param a2 Packed transform data (ignored before trapping).
void rt_scene3d_set_node_transforms(void *o, void *a1, void *a2) {
    (void)o;
    (void)a1;
    (void)a2;
    RT_GRAPHICS_TRAP_VOID("SceneGraph.SetNodeTransforms: graphics support not compiled in");
}

/// @brief Trapping stub for `SceneGraph.set_PortalClipping` (graphics-disabled build).
///
/// @param o  Scene3D handle (ignored before trapping).
/// @param a1 Non-zero to enable portal clipping (ignored before trapping).
void rt_scene3d_set_portal_clipping(void *o, int8_t a1) {
    (void)o;
    (void)a1;
    RT_GRAPHICS_TRAP_VOID("SceneGraph.set_PortalClipping: graphics support not compiled in");
}
