//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d_internal.h
// Purpose: Internal shared structs for Scene3D / SceneNode3D implementation,
//   including node-animation (glTF-style channel) types. Private to the scene
//   runtime — not part of the public Graphics3D ABI.
// Key invariants:
//   - Public handles are validated before their private payload is accessed.
//   - Hierarchy owner changes are collected completely before publication.
//   - Node metadata tables remain sorted and within public ABI limits.
// Ownership/Lifetime:
//   - Scene nodes and scenes are runtime-managed objects referenced by opaque handles.
//   - Internal transaction buffers are native allocations owned by their transaction.
// Links: rt_scene3d.c, rt_scene3d_node.c, rt_scene3d_metadata.c,
//   rt_scene3d_helpers.inc,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md,
//   docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_scene3d.h"
#include <stddef.h>
#include <stdint.h>

#ifdef ZANNA_ENABLE_GRAPHICS

#define RT_NODE_ANIM_PATH_TRANSLATION 0
#define RT_NODE_ANIM_PATH_ROTATION 1
#define RT_NODE_ANIM_PATH_SCALE 2
#define RT_NODE_ANIM_PATH_WEIGHTS 3
#define RT_NODE_ANIM_PATH_CAMERA_FOV 4
#define RT_NODE_ANIM_PATH_CAMERA_ASPECT 5
#define RT_NODE_ANIM_PATH_CAMERA_NEAR 6
#define RT_NODE_ANIM_PATH_CAMERA_FAR 7
#define RT_NODE_ANIM_PATH_CAMERA_ORTHO_SIZE 8
#define RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE 9
#define RT_NODE_ANIM_PATH_LAST RT_NODE_ANIM_PATH_CAMERA_PROJECTION_MODE

#define RT_NODE_ANIM_INTERP_LINEAR 0
#define RT_NODE_ANIM_INTERP_STEP 1
#define RT_NODE_ANIM_INTERP_CUBICSPLINE 2

#define NODE_INIT_CHILDREN 4
#define SCENE3D_ABS_MAX 1000000000000.0
#define SCENE3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define SCENE3D_PI 3.14159265358979323846
#define RT_SCENE3D_MAX_OWNER_TRANSACTION_NODES 1000000u

/// @brief One node-animation channel (glTF-style): the target node name, the animated
///   path (TRS/weights), interpolation mode, and the keyframe times/values plus optional
///   cubic-spline in/out tangents.
typedef struct {
    rt_string target_name;
    int32_t path;
    int32_t interpolation;
    int32_t key_count;
    int32_t value_width;
    double *times;
    float *values;
    float *in_tangents;
    float *out_tangents;
    int32_t target_node_index;
    struct rt_scene_node3d *cached_root;
    struct rt_scene_node3d *cached_target;
} rt_node_anim_channel3d;

/// @brief A node-animation clip: a named, fixed-duration set of channels plus a loop flag.
typedef struct rt_node_animation3d {
    void *vptr;
    rt_string name;
    double duration;
    rt_node_anim_channel3d *channels;
    int32_t channel_count;
    int32_t channel_capacity;
    int8_t looping;
} rt_node_animation3d;

/// @brief A node animator: a set of clips plus playback cursor (current clip, time, speed,
///   playing flag) rooted at the scene node whose subtree it drives.
typedef struct rt_node_animator3d {
    void *vptr;
    rt_node_animation3d **animations;
    int32_t animation_count;
    int32_t animation_capacity;
    int32_t current_animation;
    double time;
    double speed;
    int8_t playing;
    struct rt_scene_node3d *root;
    struct rt_scene_node3d **cached_targets;
    int32_t cached_target_capacity;
    int32_t cached_clip_index;
    struct rt_scene_node3d *cached_root;
    float *sample_scratch;
    int32_t sample_scratch_capacity;
    struct rt_scene_node3d **traversal_stack;
    size_t traversal_stack_capacity;
} rt_node_animator3d;

/// @brief One immutable VSCN v4+ scene carried privately from SceneGraph.Load to SceneAsset.Load.
typedef struct rt_vscn_loaded_scene3d {
    struct rt_scene_node3d *root;
    char *name;
    int32_t *camera_indices;
    int32_t camera_count;
} rt_vscn_loaded_scene3d;

/// @brief Complete retained VSCN v4+ asset inventory staged by the scene parser.
/// @details Arrays own one runtime reference per slot. Scene roots are additionally retained even
/// when scene zero aliases the live Scene3D root. Names and camera-index arrays are native-owned.
typedef struct rt_vscn_loaded_asset3d {
    void **meshes;
    int32_t mesh_count;
    void **materials;
    int32_t material_count;
    void **skeletons;
    int32_t skeleton_count;
    void **animations;
    int32_t animation_count;
    void **node_animations;
    int32_t node_animation_count;
    void **cameras;
    int32_t camera_count;
    rt_vscn_loaded_scene3d *scenes;
    int32_t scene_count;
    char **variant_names;
    int32_t variant_count;
} rt_vscn_loaded_asset3d;

struct rt_scene3d;

typedef struct {
    struct rt_scene_node3d *node;
    double world_min[3];
    double world_max[3];
    int32_t traversal_order;
    int8_t cullable;
    /* Effective (self AND ancestors) visibility. Hidden entries stay in the BVH
     * so visibility toggles are cheap refits instead of full rebuilds; queries
     * filter on this flag. */
    int8_t visible;
    /* Index of the BVH leaf containing this entry (-1 before the tree exists);
     * lets a refit re-union only the changed leaf-to-root paths. */
    int32_t leaf_node;
    uint32_t world_revision;
    uint32_t geometry_revision;
} rt_scene3d_spatial_entry;

typedef struct {
    double world_min[3];
    double world_max[3];
    int32_t left;
    int32_t right;
    /* Parent node index (-1 for the root); enables bottom-up path refits. */
    int32_t parent;
    int32_t start;
    int32_t count;
    int32_t cullable_count;
    int8_t leaf;
} rt_scene3d_spatial_bvh_node;

typedef struct {
    rt_scene3d_spatial_entry *entries;
    int32_t count;
    int32_t capacity;
    int32_t *entry_indices;
    int32_t entry_index_capacity;
    rt_scene3d_spatial_bvh_node *nodes;
    int32_t node_count;
    int32_t node_capacity;
    int32_t root_node;
    int8_t dirty;
    int8_t topology_dirty;
    int8_t valid;
    uint32_t build_count;
    uint32_t refit_count;
    uint64_t mesh_geometry_epoch;
    int32_t last_candidate_count;
    int32_t last_prefiltered_count;
} rt_scene3d_spatial_index;

typedef struct {
    rt_string name;
    double world_min[3];
    double world_max[3];
} rt_scene3d_visibility_zone;

typedef struct {
    int32_t from_zone;
    int32_t to_zone;
} rt_scene3d_visibility_portal;

/// @brief Per-camera/view LOD hysteresis state cached on a SceneNode3D.
/// @details Nodes keep a small fixed cache instead of one global LOD/impostor selection so
///   alternating cameras do not contaminate each other's hysteresis and cause mesh popping.
typedef struct rt_scene3d_lod_view_state {
    uintptr_t view_key;
    uint32_t last_used;
    int32_t lod_selected_index;
    int8_t lod_selection_valid;
    int8_t impostor_selected;
} rt_scene3d_lod_view_state;

#define RT_SCENE3D_LOD_VIEW_STATE_COUNT 4

/// @brief Scalar kinds accepted by durable gameplay-facing node metadata.
typedef enum rt_scene3d_metadata_kind {
    RT_SCENE3D_METADATA_NULL = 0,
    RT_SCENE3D_METADATA_BOOL = 1,
    RT_SCENE3D_METADATA_INT = 2,
    RT_SCENE3D_METADATA_FLOAT = 3,
    RT_SCENE3D_METADATA_STRING = 4,
} rt_scene3d_metadata_kind;

/// @brief One native-owned, key-sorted metadata scalar.
typedef struct rt_scene3d_metadata_entry {
    char *key;
    int32_t key_length;
    rt_scene3d_metadata_kind kind;

    union {
        int8_t bool_value;
        int64_t int_value;
        double float_value;

        struct {
            char *data;
            int32_t length;
        } string_value;
    } value;
} rt_scene3d_metadata_entry;

/// @brief SceneNode3D payload: local TRS, lazily-recomputed world matrix with dirty flag,
///   parent/children links, bound mesh/material/light/body/animator(s) and sync mode,
///   visibility, name, cached subtree AABB/bounding-sphere, LOD mesh levels,
///   optional screen-error LOD selection, and generated impostor proxy assets.
typedef struct rt_scene_node3d {
    void *vptr;

    double position[3];
    double rotation[4];
    double scale_xyz[3];

    double world_matrix[16];
    int8_t world_dirty;
    uint32_t world_revision;
    uint32_t parent_world_revision_seen;

    struct rt_scene_node3d *parent;
    struct rt_scene3d *owner_scene;
    struct rt_scene_node3d **children;
    int32_t child_count;
    int32_t child_capacity;

    void *mesh;
    void *material;
    void *light;
    void *camera;
    void *bound_body;
    void *bound_animator;
    void *bound_node_animator;
    int8_t bound_animator_scene_update;
    int32_t sync_mode;
    int32_t import_index;

    /* Bone socket: while socket_animator is set, SyncBindings drives this
     * node's world transform from parentWorld x bonePose x offset. The node
     * must be parented under the node that renders the skinned model (the
     * bone pose is model-space). Socket sync supersedes body sync. */
    void *socket_animator; /* retained AnimController3D or NULL */
    int32_t socket_bone;
    double socket_offset_pos[3];
    double socket_offset_quat[4];

    int8_t visible;
    rt_string name;
    rt_scene3d_metadata_entry *metadata;
    int32_t metadata_count;
    int32_t metadata_capacity;

    /* VSCN v7 prefab reference (ADR 0187): the portable source path this
     * node instantiates, or NULL for ordinary nodes. Grafted descendants
     * carry the transient instance-content flag; the flag itself is never
     * serialized — reloading re-grafts from the reference. */
    rt_string prefab_path;
    int8_t is_instance_content;

    float aabb_min[3];
    float aabb_max[3];
    float bsphere_radius;
    uint32_t mesh_bounds_revision;

    struct {
        double distance;
        void *mesh;
    } *lod_levels;

    int32_t lod_count;
    int32_t lod_capacity;

    int8_t auto_lod_enabled;
    double auto_lod_screen_error_px;
    int32_t lod_selected_index;
    int8_t lod_selection_valid;
    uint32_t lod_view_state_clock;
    rt_scene3d_lod_view_state lod_view_states[RT_SCENE3D_LOD_VIEW_STATE_COUNT];

    /* KHR_materials_variants: variant-indexed retained Material3D pointers imported
     * with this node's glTF primitive. Slot v holds the material the node uses when
     * variant v is applied (unmapped variants resolve to the primitive's default
     * material at import). NULL table = node is unaffected by variant switching.
     * Cloned with the node; not persisted by .vscn. */
    void **variant_materials;
    int32_t variant_material_count;

    int8_t is_static; /* bake hint: node participates in lightmap/probe bakes */
    int8_t has_impostor;
    int8_t impostor_selected;
    double impostor_distance;
    void *impostor_pixels;
    void *impostor_mesh;
    void *impostor_material;
    int32_t impostor_frame_count; /* >1 = yaw-selected multi-frame impostor strip */
    int32_t impostor_frame_index; /* last frame selected by the draw path */
    void **impostor_frame_meshes; /* per-frame UV-windowed quads (retained), or NULL */
    /* Allocation generation: draw submission salts the node's motion-history key with
     * this so a destroyed node's same-address successor never inherits its previous
     * transform (which would flash a bogus motion vector on its first frame). */
    uint32_t identity_serial;
} rt_scene_node3d;

/// @brief Release every native-owned metadata allocation on one node.
void rt_scene_node3d_metadata_clear_internal(rt_scene_node3d *node);

/// @brief Scene3D payload: the implicit root node, total node count, and the
///   frustum-culled count from the most recent draw (a perf metric).
typedef struct rt_scene3d {
    void *vptr;
    rt_scene_node3d *root;
    int32_t node_count;
    int32_t last_culled_count;
    int32_t last_visible_node_count;
    int32_t last_pvs_culled_count;
    int8_t use_spatial_index;
    rt_scene3d_spatial_index spatial_index;
    rt_scene3d_visibility_zone *visibility_zones;
    int32_t visibility_zone_count;
    int32_t visibility_zone_capacity;
    rt_scene3d_visibility_portal *visibility_portals;
    int32_t visibility_portal_count;
    int32_t visibility_portal_capacity;
    int8_t portal_clipping_disabled;     /* 1 = legacy reachability flood-fill */
    int32_t last_portal_traversal_count; /* portal expansions in the last PVS build */
    rt_scene3d_spatial_entry **query_candidates;
    rt_scene_node3d **query_traversal_stack;
    rt_scene_node3d **world_matrix_stack;
    /* Persistent per-scene traversal stack for SyncBindings so the once-per-tick
     * whole-tree walk stops paying a malloc/free per frame. */
    rt_scene_node3d **sync_stack;
    int32_t query_candidate_capacity;
    size_t query_traversal_stack_capacity;
    size_t world_matrix_stack_capacity;
    size_t sync_stack_capacity;
    /* Baked animation clips carried by v3 .vscn files: the loader fills these,
     * the SceneAsset wrapper drains them into the model's clip list, and any
     * leftovers release with the scene. Retained rt_animation3d handles. */
    void **baked_animations;
    int32_t baked_animation_count;
    /* Complete v4 SceneAsset carrier. SceneGraph callers never inspect it; the Model3D
     * loader copies its retained inventories and immutable scene definitions. */
    rt_vscn_loaded_asset3d *baked_asset;
} rt_scene3d;

/// @brief Bound a private dynamic-array count by the pointer and recorded capacity.
static inline int32_t scene3d_clamped_array_count(const void *array,
                                                  int32_t count,
                                                  int32_t capacity) {
    if (!array || count <= 0 || capacity <= 0)
        return 0;
    return count < capacity ? count : capacity;
}

/// @brief Safe number of children that may be read directly from a SceneNode3D.
static inline int32_t scene3d_node_child_count(const rt_scene_node3d *node) {
    return node ? scene3d_clamped_array_count(
                      node->children, node->child_count, node->child_capacity)
                : 0;
}

/// @brief Safe number of LOD entries that may be read directly from a SceneNode3D.
static inline int32_t scene3d_node_lod_count(const rt_scene_node3d *node) {
    return node ? scene3d_clamped_array_count(node->lod_levels, node->lod_count, node->lod_capacity)
                : 0;
}

/// @brief Safe number of animation channels that may be read directly from a node clip.
static inline int32_t scene3d_node_animation_channel_count(const rt_node_animation3d *animation) {
    return animation ? scene3d_clamped_array_count(animation->channels,
                                                   animation->channel_count,
                                                   animation->channel_capacity)
                     : 0;
}

/// @brief Safe number of clips that may be read directly from a node animator.
static inline int32_t scene3d_node_animator_clip_count(const rt_node_animator3d *animator) {
    return animator ? scene3d_clamped_array_count(animator->animations,
                                                  animator->animation_count,
                                                  animator->animation_capacity)
                    : 0;
}

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Add a keyframe channel targeting @p target_name's @p path property.
/// @details @p interpolation selects step/linear; keys are @p key_count tuples
///          of @p value_width floats sampled at @p times. @return channel index.
int64_t rt_node_animation3d_add_channel(void *obj,
                                        rt_string target_name,
                                        int64_t path,
                                        int64_t interpolation,
                                        int64_t key_count,
                                        int64_t value_width,
                                        const double *times,
                                        const float *values);
/// @brief Add a cubic-spline keyframe channel (with in/out tangents) to a clip.
/// @return The new channel's index within the animation.
int64_t rt_node_animation3d_add_cubic_channel(void *obj,
                                              rt_string target_name,
                                              int64_t path,
                                              int64_t key_count,
                                              int64_t value_width,
                                              const double *times,
                                              const float *values,
                                              const float *in_tangents,
                                              const float *out_tangents);
/// @brief Internal importer hook: bind a channel to a stable source node index.
void rt_node_animation3d_set_channel_target_node_index(void *obj,
                                                       int64_t channel_index,
                                                       int64_t node_index);
/// @brief Build a node animator that blends/sequences the given clips.
void *rt_node_animator3d_new_from_clips(void **clips, int64_t clip_count);
/// @brief Enable/disable Scene3D.SyncBindings auto-update for the skeletal controller binding.
void rt_scene_node3d_set_animator_scene_update(void *obj, int8_t enabled);
/// @brief Importer hook: install a variant-indexed material table on a node.
/// @details Replaces any existing table. Retains each non-NULL entry; @p materials may
///          contain NULL slots (variant leaves the node's material untouched). Passing
///          NULL/0 clears the table. Returns 1 on success, 0 on allocation failure.
int rt_scene_node3d_assign_variant_materials(rt_scene_node3d *node,
                                             void *const *materials,
                                             int32_t count);
/// @brief Copy the variant-material table from @p src onto @p dst (clone helper).
int rt_scene_node3d_copy_variant_materials(rt_scene_node3d *dst, const rt_scene_node3d *src);

// Shared internal helpers exposed for the split rt_scene3d_*.c sibling TUs.
rt_scene_node3d *find_by_name(rt_scene_node3d *node, const char *target);
void mark_dirty(rt_scene_node3d *node);
void mat4d_identity(double *out);
void node_animator_update(rt_node_animator3d *animator, double dt);
int node_contains(const rt_scene_node3d *root, const rt_scene_node3d *target);
void scene3d_quat_normalize_local(double *q);
void recompute_world_matrix(rt_scene_node3d *node);
void scene_bounds_reset(float out_min[3], float out_max[3]);
void scene_mesh_bounds(rt_mesh3d *mesh, float out_min[3], float out_max[3], float *out_radius);
/// @brief Combine the geometry revisions of a node's drawable meshes into one dirty signature.
uint32_t scene_node_geometry_revision_signature(rt_scene_node3d *node);
/// @brief Refresh a node's cached local mesh bounds if its primary mesh geometry changed.
void scene_node_refresh_mesh_bounds(rt_scene_node3d *node);

/// @brief Preflight state for one transactional subtree owner change.
/// @ownership `nodes` is native storage owned by this value until discarded.
typedef struct scene_owner_transaction {
    rt_scene_node3d **nodes;
    size_t count;
    size_t capacity;
} scene_owner_transaction;

/// @brief Collect a complete subtree before any owner field is mutated.
/// @param node Borrowed root of the subtree to collect.
/// @param transaction Caller-owned, zero-initialized transaction destination.
/// @return `1` with every valid node collected, or `0` after releasing partial
///   native storage on overflow/allocation failure.
int scene_node_owner_transaction_prepare(rt_scene_node3d *node,
                                         scene_owner_transaction *transaction);
/// @brief Release a prepared owner transaction without changing any scene node.
/// @param transaction Caller-owned transaction to discard; zeroed on return.
void scene_node_owner_transaction_discard(scene_owner_transaction *transaction);
/// @brief Assign @p owner to every node in a completely prepared transaction.
/// @details This commit phase performs no allocation and cannot partially fail.
void scene_node_owner_transaction_assign(scene_owner_transaction *transaction, rt_scene3d *owner);
/// @brief Clear @p owner from matching nodes in a completely prepared transaction.
/// @details Nodes with a different owner are preserved to tolerate corrupt or
///   deliberately detached subtrees without stealing another scene's ownership.
void scene_node_owner_transaction_clear(scene_owner_transaction *transaction, rt_scene3d *owner);
/// @brief Clear matching owners below the transaction root while preserving the root itself.
void scene_node_owner_transaction_clear_descendants(scene_owner_transaction *transaction,
                                                    rt_scene3d *owner);
/// @brief Transactionally assign an owner across one subtree; traps and returns 0 on preflight OOM.
int scene_node_assign_owner_recursive(rt_scene_node3d *node, rt_scene3d *owner);
/// @brief Transactionally clear a matching owner across one subtree; traps and returns 0 on OOM.
int scene_node_clear_owner_recursive(rt_scene_node3d *node, rt_scene3d *owner);
/// @brief Test-only fault injection for owner-transaction native growth.
/// @param successful_growths Number of required buffer growths to allow before
///   failing one; `-1` disables injection and `0` fails the next growth.
void rt_scene3d_test_set_owner_growth_failure(int32_t successful_growths);
int scene_node_collect_subtree_bounds(rt_scene_node3d *node,
                                      const double *node_to_root,
                                      float out_min[3],
                                      float out_max[3]);
void scene_node_get_world_position(rt_scene_node3d *node, double *x, double *y, double *z);
void scene_node_get_world_rotation(rt_scene_node3d *node, double *out_quat);
/// @brief Strict finite matrix equality used by exact SceneNode transform transactions.
int scene3d_preserve_matrix_equal(const double a[16], const double b[16]);
/// @brief Derive an exact parent-relative TRS for one requested complete world matrix.
/// @param parent The prospective/current parent, or NULL for a parentless node.
/// @return 1 when exactly representable; 0 for singular, sheared, projective, or invalid input.
int scene_node_compute_local_trs_for_world_matrix(rt_scene_node3d *parent,
                                                  const double desired_world[16],
                                                  double out_position[3],
                                                  double out_rotation[4],
                                                  double out_scale[3]);
/// @brief Derive an exact local TRS that keeps @p child at its current world matrix.
/// @return 1 when representable beneath @p new_parent; 0 for singular, sheared, or invalid input.
int scene_node_compute_preserved_local_trs(rt_scene_node3d *new_parent,
                                           rt_scene_node3d *child,
                                           double out_position[3],
                                           double out_rotation[4],
                                           double out_scale[3]);
rt_scene_node3d *scene_node3d_checked(void *obj);
double scene3d_clamp_abs_or(double value, double fallback);
void scene3d_canonicalize_aabb_d(double mn[3], double mx[3]);
double scene3d_distance_or_zero(double value);
void scene3d_mark_spatial_dirty(rt_scene3d *scene);
void scene3d_mark_spatial_visibility_dirty(rt_scene3d *scene);
int scene3d_normalize_vec3d(double v[3]);
void scene3d_release_ref(void **slot);
double scene3d_scale_or_unit(double value);
int scene3d_grow_stack_storage(void **buffer, size_t *capacity, size_t elem_size);
int scene3d_grow_array_i32(void **buffer,
                           int32_t *capacity,
                           int32_t needed,
                           int32_t min_capacity,
                           size_t elem_size,
                           int zero_new);
int scene_node_stack_push(rt_scene_node3d ***stack,
                          size_t *count,
                          size_t *capacity,
                          rt_scene_node3d *node);

typedef struct {
    rt_scene_node3d *node;
    void *inherited_animator;
} scene_index_build_stack_item_t;

typedef struct {
    rt_scene3d_spatial_entry **items;
    int32_t count;
    int32_t capacity;
} scene3d_spatial_candidate_list_t;

void scene_bounds_include_point_d(double bounds_min[3],
                                  double bounds_max[3],
                                  const double point[3]);
void scene_bounds_reset_d(double out_min[3], double out_max[3]);
int scene_index_build_stack_push(scene_index_build_stack_item_t **stack,
                                 size_t *count,
                                 size_t *capacity,
                                 rt_scene_node3d *node,
                                 void *inherited_animator);
rt_scene3d *scene3d_checked(void *obj);
int scene3d_read_vec3d(void *obj, double out[3], const char *trap_message);
int scene3d_node_world_mesh_aabb(rt_scene_node3d *node, double world_min[3], double world_max[3]);
int scene3d_aabb_intersects_aabb(const double a_min[3],
                                 const double a_max[3],
                                 const double b_min[3],
                                 const double b_max[3]);
int scene3d_aabb_intersects_sphere(const double aabb_min[3],
                                   const double aabb_max[3],
                                   const double center[3],
                                   double radius);
int scene3d_ray_intersects_aabb(const double origin[3],
                                const double direction[3],
                                const double aabb_min[3],
                                const double aabb_max[3],
                                double max_distance,
                                double *out_t);
int scene3d_ray_sweep_bounds(const double origin[3],
                             const double direction[3],
                             double max_distance,
                             double out_min[3],
                             double out_max[3]);
float scene3d_float_or_zero(double value);
int scene3d_transform_aabb_d(const float obj_min[3],
                             const float obj_max[3],
                             const double world_matrix[16],
                             double out_min[3],
                             double out_max[3]);
int scene3d_spatial_collect_aabb(rt_scene3d *scene,
                                 const double query_min[3],
                                 const double query_max[3],
                                 scene3d_spatial_candidate_list_t *out,
                                 int count_cullable_prefilter);
int scene3d_spatial_collect_all(rt_scene3d *scene, scene3d_spatial_candidate_list_t *out);
int scene3d_node_world_draw_union_aabb(rt_scene_node3d *node,
                                       void *effective_animator,
                                       double world_min[3],
                                       double world_max[3],
                                       double *out_radius);
int scene3d_mesh_has_dynamic_deformation(rt_mesh3d *mesh, void *effective_animator);
double scene3d_mesh_dynamic_bound_pad(rt_mesh3d *mesh,
                                      void *effective_animator,
                                      double base_radius);

typedef struct {
    int32_t *items;
    int32_t count;
    int32_t capacity;
} scene3d_spatial_node_stack_t;

void *scene3d_effective_animator(rt_scene_node3d *node);

#ifdef __cplusplus
}
#endif

#endif /* ZANNA_ENABLE_GRAPHICS */
