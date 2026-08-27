//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf.c
// Purpose: glTF 2.0 (.gltf/.glb) loader implementation.
// Key invariants:
//   - Uses existing rt_json parser for JSON content
//   - Supports .glb binary container (magic 0x46546C67)
//   - Preserves glTF metallic-roughness materials on Material3D's native PBR surface
//   - Mesh primitives support POSITION/NORMAL/TEXCOORD_0 plus COLOR_0, TANGENT,
//     and JOINTS_0/WEIGHTS_0 + JOINTS_1/WEIGHTS_1 vertex attributes
//   - Triangle-list, triangle-strip, and triangle-fan primitives are triangulated
// Ownership/Lifetime:
//   - All extracted objects are GC-managed
// Links: rt_gltf.h, rt_json.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf.c
 * @brief Implements glTF 2.0/GLB loading, resource ownership, JSON orchestration,
 *        and the public asset query API.
 *
 * The loader validates the root document and required extensions, resolves binary,
 * data-URI, external, asset-manager, or preloaded dependencies, then invokes the
 * specialized accessor, codec, material, mesh, skin, animation, and scene import
 * fragments in transactional order. Successful assets own all published resources;
 * decoded buffers, texture tables, primitive mappings, and parsed JSON remain scratch
 * state and are released before return.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_gltf.h"
#include "rt_asset.h"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collection_ids.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_gif.h"
#include "rt_gltf_json.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_quat.h"
#include "rt_scene3d_internal.h"
#include "rt_skeleton3d.h"
#include "rt_skeleton3d_internal.h"
#include "rt_string.h"
#include "rt_textureasset3d.h"
#include "rt_untrusted_count.h"
#include "rt_vec3.h"

#include "rt_gltf_internal.h"
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLTF_MAX_URI_PATH_BYTES (1024u * 1024u)

// Forward declarations for runtime JSON and collection APIs
extern void *rt_json_parse_object(rt_string text);
extern void *rt_map_get(void *map, rt_string key);
extern void *rt_map_keys(void *map);
extern int64_t rt_map_len(void *map);
extern int8_t rt_obj_is_instance(void *p, int64_t class_id, size_t min_payload_bytes);
extern int32_t rt_obj_release_check0(void *p);
extern int64_t rt_seq_len(void *seq);
extern void *rt_seq_get(void *seq, int64_t index);
extern int64_t rt_box_type(void *box);
extern int64_t rt_unbox_i64(void *boxed);
extern double rt_unbox_f64(void *boxed);
extern int64_t rt_unbox_i1(void *boxed);
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
#include "rt_trap.h"
extern void rt_trap_set_recovery(jmp_buf *buf);
extern void rt_trap_clear_recovery(void);
extern const char *rt_trap_get_error(void);
extern void rt_obj_free(void *obj);
extern void *rt_asset_decode_typed(const char *name, const uint8_t *data, size_t size);
extern void *rt_pixels_load(void *path);
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
extern void *rt_material3d_new_pbr(double r, double g, double b);
extern void rt_material3d_set_texture(void *obj, void *pixels);
extern void rt_material3d_set_normal_map(void *obj, void *pixels);
extern void rt_material3d_set_metallic(void *obj, double value);
extern void rt_material3d_set_roughness(void *obj, double value);
extern void rt_material3d_set_ao(void *obj, double value);
extern void rt_material3d_set_emissive_intensity(void *obj, double value);
extern void rt_material3d_set_metallic_roughness_map(void *obj, void *pixels);
extern void rt_material3d_set_ao_map(void *obj, void *pixels);
extern void rt_material3d_set_specular_map(void *obj, void *pixels);
extern void rt_material3d_set_emissive_map(void *obj, void *pixels);
extern void rt_material3d_set_alpha(void *obj, double alpha);
extern void rt_material3d_set_normal_scale(void *obj, double value);
extern void rt_material3d_set_alpha_mode(void *obj, int64_t mode);
extern void rt_material3d_set_double_sided(void *obj, int8_t enabled);
extern void rt_material3d_set_unlit(void *obj, int8_t unlit);
extern void rt_material3d_set_shading_model(void *obj, int64_t model);
extern void rt_material3d_set_reflectivity(void *obj, double value);
extern void rt_material3d_set_custom_param(void *obj, int64_t index, double value);

/// @brief Cast a parsed JSON number to int64 only when it is finite, exact, and in range.
/// @param value Parsed runtime JSON number.
/// @param out Receives the exact integral value on success.
/// @return 1 for a mathematically integral in-range value, otherwise 0.
static int gltf_double_to_i64_checked(double value, int64_t *out) {
    if (!out || !isfinite(value) || trunc(value) != value)
        return 0;
    if (value < (-9223372036854775807.0 - 1.0) || value >= 9223372036854775808.0)
        return 0;
    *out = (int64_t)value;
    return 1;
}

/// @brief Narrow a signed 64-bit JSON value to 32 bits with a range fallback.
/// @param value Value to narrow.
/// @param fallback Value returned when @p value is outside the int32 range.
/// @return Safely narrowed value or @p fallback.
static int32_t gltf_i32_from_i64_or(int64_t value, int32_t fallback) {
    if (value < (int64_t)INT32_MIN || value > (int64_t)INT32_MAX)
        return fallback;
    return (int32_t)value;
}

extern void rt_material3d_set_import_texture_slot(void *obj,
                                                  int64_t slot,
                                                  int64_t uv_set,
                                                  double offset_u,
                                                  double offset_v,
                                                  double scale_u,
                                                  double scale_v,
                                                  double rotation,
                                                  int64_t wrap_s,
                                                  int64_t wrap_t,
                                                  int64_t filter);

//===----------------------------------------------------------------------===//
// Asset container
//===----------------------------------------------------------------------===//

/// @brief Asset-owned metadata and resources for one immutable imported scene.
typedef struct {
    /// Owned root SceneNode3D reference.
    void *root;
    /// Owned NUL-terminated scene display name.
    char *name;
    /// Count of nodes reachable from @ref root.
    int32_t node_count;
    /// Owned reference array of cameras reachable from the scene.
    void **cameras;
    /// Number of initialized camera references.
    int32_t camera_count;
    /// Allocated camera-reference capacity.
    int32_t camera_capacity;
} gltf_scene_info_t;

/// @brief GC-managed aggregate of all resources imported from one glTF document.
typedef struct {
    /// Runtime object header/vtable slot.
    void *vptr;
    /// Owned Mesh3D reference array.
    void **meshes;
    /// Number of initialized meshes.
    int32_t mesh_count;
    /// Allocated mesh capacity.
    int32_t mesh_capacity;
    /// Owned Material3D reference array.
    void **materials;
    /// Number of initialized materials.
    int32_t material_count;
    /// Allocated material capacity.
    int32_t material_capacity;
    /// Owned Skeleton3D reference array.
    void **skeletons;
    /// Number of initialized skeletons.
    int32_t skeleton_count;
    /// Allocated skeleton capacity.
    int32_t skeleton_capacity;
    /// Owned skeletal Animation3D reference array.
    void **animations;
    /// Number of initialized skeletal clips.
    int32_t animation_count;
    /// Allocated skeletal-clip capacity.
    int32_t animation_capacity;
    /// Owned NodeAnimation3D reference array.
    void **node_animations;
    /// Number of initialized node clips.
    int32_t node_animation_count;
    /// Allocated node-clip capacity.
    int32_t node_animation_capacity;
    /// Owned active-scene Camera3D reference array.
    void **cameras;
    /// Number of initialized active-scene cameras.
    int32_t camera_count;
    /// Allocated active-scene camera capacity.
    int32_t camera_capacity;
    /// Owned immutable scene metadata array.
    gltf_scene_info_t *scenes;
    /// Number of initialized scenes.
    int32_t scene_count;
    /// Allocated scene capacity.
    int32_t scene_capacity;
    /// Owned active-scene root reference.
    void *scene_root;
    /// Count of nodes reachable from @ref scene_root.
    int32_t node_count;
    /* KHR_materials_variants: variant display names, index-aligned with the
     * per-node variant material tables built during scene import. */
    char **variant_names;
    /// Number of owned entries in @ref variant_names.
    int32_t variant_name_count;
} rt_gltf_asset;

/// @brief Validate a runtime handle as a glTF asset.
/// @param obj Candidate runtime object.
/// @return Typed borrowed asset pointer, or NULL on NULL/class mismatch.
static rt_gltf_asset *gltf_asset_checked(void *obj) {
    return (rt_gltf_asset *)rt_g3d_checked_or_null(obj, RT_G3D_GLTF_ASSET_CLASS_ID);
}

/// @brief Load-local mapping from glTF skin joints to a runtime skeleton.
typedef struct {
    /// Owned or staged Skeleton3D reference.
    void *skeleton;
    /// Owned array mapping skin-joint ordinal to glTF node index.
    int32_t *joint_nodes;
    /// Owned array mapping skin-joint ordinal to runtime bone index.
    int32_t *joint_to_bone;
    /// Number of initialized joint mappings.
    int32_t joint_count;
} gltf_skin_t;

/// @brief Normalized sampler state associated with one glTF texture.
typedef struct {
    /// Horizontal texture wrap mode.
    int32_t wrap_s;
    /// Vertical texture wrap mode.
    int32_t wrap_t;
    /// Minification texel filter.
    int32_t min_filter;
    /// Magnification texel filter.
    int32_t mag_filter;
    /// Mipmap selection filter.
    int32_t mip_filter;
} gltf_sampler_info_t;

/// @brief Parsed textureInfo UV-set and KHR_texture_transform metadata.
typedef struct {
    /// Selected `TEXCOORD_n` index.
    int32_t texcoord;
    /// Non-zero when the textureInfo object exists.
    int8_t present;
    /// Non-zero when a KHR_texture_transform block exists.
    int8_t has_transform;
    /// UV translation.
    double offset[2];
    /// UV scale.
    double scale[2];
    /// UV rotation in radians.
    double rotation;
} gltf_texture_info_t;

/// @brief Per-material textureInfo metadata indexed by runtime material slot.
typedef struct {
    /// Texture slot metadata parallel to the Material3D slot enumeration.
    gltf_texture_info_t slots[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
} gltf_material_info_t;

static void *jget(void *obj, const char *key);
static const char *jstr(void *obj, const char *key);
static int64_t jvalue_int(void *value, int64_t def);
static int gltf_ascii_ieq_n(const char *a, const char *b, size_t len);

/// @brief Number of skin joints safe to read (clamped to VGFX3D_MAX_SKELETON_BONES);
///   0 when any backing array is absent.
/// @param skin Skin mapping to validate.
/// @return Safe readable joint count, clamped to the runtime skeleton limit.
static int32_t gltf_skin_safe_joint_count(const gltf_skin_t *skin) {
    if (!skin || skin->joint_count <= 0 || !skin->joint_nodes || !skin->joint_to_bone)
        return 0;
    return skin->joint_count < VGFX3D_MAX_SKELETON_BONES ? skin->joint_count
                                                         : VGFX3D_MAX_SKELETON_BONES;
}

/// @brief Clamp a reference-array count/capacity pair to a safe readable element count.
/// @param items Reference-array storage.
/// @param count Reported initialized count.
/// @param capacity Allocated capacity.
/// @return Zero for invalid storage/metadata, otherwise `min(count, capacity)`.
static int32_t gltf_asset_safe_count(void **items, int32_t count, int32_t capacity) {
    if (!items || count <= 0 || capacity <= 0)
        return 0;
    if (count > capacity)
        return capacity;
    return count;
}

/// @brief Return the number of imported scenes safe to read.
/// @param asset Candidate typed asset.
/// @return Zero for invalid scene storage, otherwise the live count clamped to capacity.
static int32_t gltf_asset_safe_scene_count(const rt_gltf_asset *asset) {
    if (!asset || !asset->scenes || asset->scene_count <= 0 || asset->scene_capacity <= 0)
        return 0;
    if (asset->scene_count > asset->scene_capacity)
        return asset->scene_capacity;
    return asset->scene_count;
}

/// @brief Release every reference in a glTF ref array (over its safe count) and free the
///   backing storage, resetting the count/capacity to zero.
/// @param[in,out] items Address of the owned reference-array pointer.
/// @param[in,out] count Address of its initialized count.
/// @param[in,out] capacity Address of its allocated capacity.
static void gltf_asset_release_ref_array(void ***items, int32_t *count, int32_t *capacity) {
    void **array = items ? *items : NULL;
    int32_t safe_count = gltf_asset_safe_count(array, count ? *count : 0, capacity ? *capacity : 0);
    if (array) {
        for (int32_t i = 0; i < safe_count; i++) {
            if (array[i] && rt_obj_release_check0(array[i]))
                rt_obj_free(array[i]);
            array[i] = NULL;
        }
        free(array);
    }
    if (items)
        *items = NULL;
    if (count)
        *count = 0;
    if (capacity)
        *capacity = 0;
}

/// @brief Release every resource and allocation owned by a glTF asset.
/// @param[in,out] obj Asset being finalized; NULL is accepted as a no-op.
static void gltf_asset_finalize(void *obj) {
    rt_gltf_asset *a = (rt_gltf_asset *)obj;
    if (!a)
        return;
    gltf_asset_release_ref_array(&a->meshes, &a->mesh_count, &a->mesh_capacity);
    gltf_asset_release_ref_array(&a->materials, &a->material_count, &a->material_capacity);
    gltf_asset_release_ref_array(&a->skeletons, &a->skeleton_count, &a->skeleton_capacity);
    gltf_asset_release_ref_array(&a->animations, &a->animation_count, &a->animation_capacity);
    gltf_asset_release_ref_array(
        &a->node_animations, &a->node_animation_count, &a->node_animation_capacity);
    gltf_asset_release_ref_array(&a->cameras, &a->camera_count, &a->camera_capacity);
    if (a->scenes) {
        int32_t scene_count = gltf_asset_safe_scene_count(a);
        for (int32_t i = 0; i < scene_count; i++) {
            gltf_scene_info_t *scene = &a->scenes[i];
            if (scene->root && rt_obj_release_check0(scene->root))
                rt_obj_free(scene->root);
            scene->root = NULL;
            free(scene->name);
            scene->name = NULL;
            gltf_asset_release_ref_array(
                &scene->cameras, &scene->camera_count, &scene->camera_capacity);
        }
    }
    free(a->scenes);
    a->scenes = NULL;
    a->scene_count = 0;
    a->scene_capacity = 0;
    if (a->scene_root && rt_obj_release_check0(a->scene_root))
        rt_obj_free(a->scene_root);
    a->scene_root = NULL;
    a->node_count = 0;
    if (a->variant_names) {
        for (int32_t i = 0; i < a->variant_name_count; i++)
            free(a->variant_names[i]);
        free(a->variant_names);
    }
    a->variant_names = NULL;
    a->variant_name_count = 0;
}

//===----------------------------------------------------------------------===//
// JSON helpers
//===----------------------------------------------------------------------===//

/// @brief Set a scene-node name from a non-empty C string.
/// @param[in,out] node Runtime scene node to rename.
/// @param name NUL-terminated name; NULL or empty is a no-op.
static void gltf_set_node_name(rt_scene_node3d *node, const char *name) {
    if (!node || !name || name[0] == '\0')
        return;
    rt_string runtime_name = rt_const_cstr(name);
    rt_scene_node3d_set_name(node, runtime_name);
    rt_string_unref(runtime_name);
}

/// @brief Return the best available name for a glTF node, falling back to a synthetic index string.
/// @details First tries the JSON `"name"` field for the node at @p node_index. If that field
///   is absent or empty, a fallback string `"node_N"` is written into @p buffer and returned.
///   The buffer fallback avoids returning NULL to callers that always want a printable label,
///   at the cost of a stack-allocated temporary that must remain live as long as the returned
///   pointer is used. Returns NULL only when both the JSON name is absent and no buffer is
///   provided.
/// @param nodes_arr   JSON array of glTF node objects.
/// @param node_index  Zero-based index into @p nodes_arr.
/// @param buffer      Caller-supplied scratch buffer for the synthetic name; may be NULL.
/// @param buffer_size Capacity of @p buffer including NUL terminator.
/// @return Borrowed pointer to the name string (JSON storage or @p buffer), or NULL.
static const char *gltf_effective_node_name(void *nodes_arr,
                                            int32_t node_index,
                                            char *buffer,
                                            size_t buffer_size) {
    void *node_json;
    const char *name = NULL;
    if (nodes_arr && node_index >= 0 && node_index < jarr_len(nodes_arr)) {
        node_json = rt_seq_get(nodes_arr, node_index);
        name = jstr(node_json, "name");
    }
    if (name && name[0] != '\0')
        return name;
    if (buffer && buffer_size > 0) {
        snprintf(buffer, buffer_size, "node_%d", (int)node_index);
        return buffer;
    }
    return NULL;
}

/// @brief Write an identity transform into the given outputs: zero position, identity
///   quaternion (0,0,0,1), unit scale. NULL outputs are skipped.

// ---------------------------------------------------------------------------
// JSON accessor helpers — adapt the runtime JSON map/array API to
// the glTF parser's conventions (default values for missing keys,
// borrowed cstr returns, etc.).
// ---------------------------------------------------------------------------

/// @brief Look up a JSON object field by key.
/// @param obj Borrowed runtime JSON map.
/// @param key NUL-terminated property name.
/// @return Borrowed boxed value, or NULL when absent or @p obj is NULL.
static void *jget(void *obj, const char *key) {
    void *value;
    if (!obj)
        return NULL;
    rt_string k = rt_const_cstr(key);
    value = rt_map_get(obj, k);
    rt_string_unref(k);
    return value;
}

/// @brief Read a JSON object field as a finite double.
/// @param obj Borrowed runtime JSON object.
/// @param key Property name.
/// @param def Fallback for absence, unsupported type, or non-finite floating value.
/// @return Converted integer/float/boolean value, or @p def.
static double jnum(void *obj, const char *key, double def) {
    void *v = jget(obj, key);
    if (!v)
        return def;
    switch (rt_box_type(v)) {
        case 0:
            return (double)rt_unbox_i64(v);
        case 1: {
            double value = rt_unbox_f64(v);
            return isfinite(value) ? value : def;
        }
        case 2:
            return (double)rt_unbox_i1(v);
        default:
            return def;
    }
}

/// @brief Read a JSON object field as an exact mathematical integer.
/// @param obj Borrowed runtime JSON object.
/// @param key Property name.
/// @param def Fallback for absence, unsupported type, fractional, non-finite, or range error.
/// @return Exact converted integer/boolean value, or @p def.
static int64_t jint(void *obj, const char *key, int64_t def) {
    void *v = jget(obj, key);
    if (!v)
        return def;
    switch (rt_box_type(v)) {
        case 0:
            return rt_unbox_i64(v);
        case 1: {
            int64_t coerced;
            return gltf_double_to_i64_checked(rt_unbox_f64(v), &coerced) ? coerced : def;
        }
        case 2:
            return rt_unbox_i1(v);
        default:
            return def;
    }
}

/// @brief Read `obj[key]` as a strict boolean or exact compatibility integer 0/1.
/// @param obj Borrowed JSON object.
/// @param key Object property name.
/// @param def Fallback returned for absence, wrong type, or an integer outside 0/1.
/// @return Normalized 0/1 value or @p def.
static int64_t jbool(void *obj, const char *key, int64_t def) {
    void *v = jget(obj, key);
    int64_t value;
    if (!v)
        return def;
    if (rt_box_type(v) == 2)
        return rt_unbox_i1(v) ? 1 : 0;
    if (rt_box_type(v) == 0)
        value = rt_unbox_i64(v);
    else if (rt_box_type(v) == 1) {
        if (!gltf_double_to_i64_checked(rt_unbox_f64(v), &value))
            return def;
    } else
        return def;
    return value == 0 || value == 1 ? value : def;
}

/// @brief Read a JSON object field as a borrowed C string.
/// @param obj Borrowed runtime JSON object.
/// @param key Property name.
/// @return Borrowed NUL-terminated runtime-string data, or NULL when absent/non-string.
static const char *jstr(void *obj, const char *key) {
    void *v = jget(obj, key);
    if (!rt_string_is_handle(v))
        return NULL;
    return rt_string_cstr((rt_string)v);
}

/// @brief Read a JSON object field as an array-shaped borrowed value.
/// @param obj Borrowed runtime JSON object.
/// @param key Property name.
/// @return Borrowed field value, or NULL; callers validate array behavior through @ref jarr_len.
void *jarr(void *obj, const char *key) {
    return jget(obj, key);
}

/// @brief Return the runtime length of a JSON array.
/// @param arr Borrowed runtime sequence, or NULL.
/// @return Sequence length, or zero for NULL.
int64_t jarr_len(void *arr) {
    return arr ? rt_seq_len(arr) : 0;
}

/// @brief Convert a JSON array length to a safely indexable 32-bit count.
/// @param arr Borrowed runtime sequence, or NULL.
/// @param[out] out_len Receives the non-negative 32-bit length.
/// @return Non-zero on success, or zero when the length is negative/out of range.
static int gltf_jarr_len_i32(void *arr, int *out_len) {
    int64_t len;
    if (!out_len)
        return 0;
    len = jarr_len(arr);
    if (len < 0 || len > INT32_MAX)
        return 0;
    *out_len = (int)len;
    return 1;
}

/// @brief Coerce a boxed JSON numeric value to a finite double.
/// @param value Borrowed boxed JSON value.
/// @param def Fallback for NULL, unsupported type, or non-finite float.
/// @return Converted integer/float value, or @p def.
double jvalue_num(void *value, double def) {
    if (!value)
        return def;
    switch (rt_box_type(value)) {
        case 0:
            return (double)rt_unbox_i64(value);
        case 1: {
            double number = rt_unbox_f64(value);
            return isfinite(number) ? number : def;
        }
        default:
            return def;
    }
}

/// @brief Read a boxed JSON value as an exact mathematical integer.
/// @param value Borrowed boxed JSON value.
/// @param def Fallback for NULL, unsupported type, fractional, non-finite, or range error.
/// @return Exact converted integer, or @p def.
static int64_t jvalue_int(void *value, int64_t def) {
    if (!value)
        return def;
    switch (rt_box_type(value)) {
        case 0:
            return rt_unbox_i64(value);
        case 1: {
            int64_t coerced;
            return gltf_double_to_i64_checked(rt_unbox_f64(value), &coerced) ? coerced : def;
        }
        default:
            return def;
    }
}

/// @brief Count a scene subtree iteratively.
/// @param node Root node to count.
/// @return One plus all reachable descendants, zero for NULL, or the partial saturated
///         count if scratch allocation/capacity growth fails.
static int32_t gltf_count_subtree(const rt_scene_node3d *node) {
    const rt_scene_node3d **stack = NULL;
    int32_t count = 0;
    int32_t capacity = 0;
    int32_t total = 0;
    if (!node)
        return 0;
    while (count >= capacity) {
        const rt_scene_node3d **grown;
        int64_t next_capacity64 = capacity == 0 ? 32 : (int64_t)capacity * 2;
        if (next_capacity64 > INT32_MAX) {
            free(stack);
            return total;
        }
        int32_t next_capacity = (int32_t)next_capacity64;
        grown = (const rt_scene_node3d **)realloc(stack, (size_t)next_capacity * sizeof(*stack));
        if (!grown) {
            free(stack);
            return total;
        }
        stack = grown;
        capacity = next_capacity;
    }
    stack[count++] = node;
    while (count > 0) {
        const rt_scene_node3d *current = stack[--count];
        if (!current)
            continue;
        if (total < INT32_MAX)
            total++;
        for (int32_t i = 0; i < current->child_count; ++i) {
            if (count >= capacity) {
                int32_t next_capacity = capacity == 0 ? 32 : capacity * 2;
                const rt_scene_node3d **grown;
                if (capacity > INT32_MAX / 2) {
                    free(stack);
                    return total;
                }
                grown = (const rt_scene_node3d **)realloc(stack,
                                                          (size_t)next_capacity * sizeof(*stack));
                if (!grown) {
                    free(stack);
                    return total;
                }
                stack = grown;
                capacity = next_capacity;
            }
            stack[count++] = current->children[i];
        }
    }
    free(stack);
    return total;
}

/// @brief Membership test for the set of glTF extensions this loader handles when
///        they appear in `extensionsRequired`.
/// @details The supported list is deliberately small and hardcoded: adding an entry
///          here is a commitment to actually interpret that extension's JSON in
///          material / node / mesh parsing. Listing an extension here without
///          implementing it is worse than not listing it, because assets that require
///          it will load and then silently render wrong.
/// @param name Extension name.
/// @return 1 if the extension is supported, 0 otherwise (including NULL input).
static int gltf_required_extension_supported(const char *name) {
    if (!name)
        return 0;
    return strcmp(name, "KHR_texture_transform") == 0 ||
           strcmp(name, "KHR_materials_emissive_strength") == 0 ||
           strcmp(name, "KHR_materials_unlit") == 0 ||
           strcmp(name, "KHR_materials_specular") == 0 ||
           strcmp(name, "KHR_materials_pbrSpecularGlossiness") == 0 ||
           strcmp(name, "KHR_materials_variants") == 0 ||
           strcmp(name, "EXT_meshopt_compression") == 0 ||
           strcmp(name, "KHR_mesh_quantization") == 0 || strcmp(name, "KHR_texture_basisu") == 0 ||
           strcmp(name, "KHR_draco_mesh_compression") == 0 ||
           strcmp(name, "KHR_lights_punctual") == 0;
}

/// @brief Membership test for extensions this loader handles in optional `extensionsUsed` form.
/// @details Some extensions have partial optional support but are intentionally not accepted in
///          `extensionsRequired`, where the glTF contract demands complete rendering semantics.
/// @param name Extension name.
/// @return Non-zero when the extension is fully required-compatible or intentionally
///         supported in optional-used form.
static int gltf_used_extension_supported(const char *name) {
    if (gltf_required_extension_supported(name))
        return 1;
    if (!name)
        return 0;
    return strcmp(name, "KHR_texture_basisu") == 0 ||
           strcmp(name, "KHR_materials_clearcoat") == 0 ||
           strcmp(name, "KHR_materials_transmission") == 0 ||
           strcmp(name, "KHR_materials_ior") == 0 || strcmp(name, "KHR_materials_volume") == 0 ||
           strcmp(name, "KHR_materials_sheen") == 0 ||
           strcmp(name, "KHR_materials_anisotropy") == 0;
}

/// @brief Append one unsupported extension name to a growable comma-separated diagnostic list.
/// @details The returned pointer may differ from @p list and remains malloc-owned by the caller.
///          The helper leaves @p list untouched on allocation failure so callers can still free it.
/// @param list Existing malloc-owned list, or NULL for the first name.
/// @param name Extension name to append; invalid names are rendered explicitly.
/// @return Updated malloc-owned list, or NULL on allocation failure.
static char *gltf_append_extension_name_owned(char *list, const char *name) {
    const char *safe_name = (name && *name) ? name : "<invalid extension name>";
    size_t old_len = list ? strlen(list) : 0u;
    size_t name_len = strlen(safe_name);
    size_t sep_len = old_len > 0u ? 2u : 0u;
    if (old_len > SIZE_MAX - sep_len || old_len + sep_len > SIZE_MAX - name_len ||
        old_len + sep_len + name_len > SIZE_MAX - 1u)
        return NULL;
    size_t new_len = old_len + sep_len + name_len;
    char *grown = (char *)realloc(list, new_len + 1u);
    if (!grown)
        return NULL;
    if (sep_len > 0u) {
        grown[old_len] = ',';
        grown[old_len + 1u] = ' ';
    }
    memcpy(grown + old_len + sep_len, safe_name, name_len + 1u);
    return grown;
}

/// @brief Enforce the glTF `extensionsRequired` contract.
/// @details If the document declares extensions as REQUIRED (not merely USED), the
///          spec says a loader that can't handle any of them must refuse to load the
///          asset rather than produce a degraded rendering. This function walks the
///          required list, records every unsupported extension in the load error, and
///          returns 0 so the top-level loader can bail cleanly. Missing or empty
///          `extensionsRequired` is treated as "nothing required" (returns 1).
/// @param root Parsed glTF root JSON object.
/// @return 1 when every required extension is supported (or the array is absent);
///         0 when any required extension is unsupported and the load should fail.
static int gltf_validate_required_extensions(void *root) {
    void *required = jarr(root, "extensionsRequired");
    char *unsupported = NULL;
    int has_unsupported = 0;
    int oom = 0;
    if (!required)
        return 1;
    for (int64_t i = 0; i < jarr_len(required); i++) {
        rt_string name = (rt_string)rt_seq_get(required, i);
        const char *ext = rt_string_is_handle(name) ? rt_string_cstr(name) : NULL;
        if (!gltf_required_extension_supported(ext)) {
            char *next = gltf_append_extension_name_owned(unsupported, ext);
            if (next)
                unsupported = next;
            else
                oom = 1;
            has_unsupported = 1;
        }
    }
    if (!has_unsupported)
        return 1;
    if (oom || !unsupported)
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                            "GLTF.Load: requires unsupported extensions");
    else
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNSUPPORTED, "GLTF.Load: requires %s (unsupported)", unsupported);
    free(unsupported);
    return 0;
}

/// @brief Record warnings for optional `extensionsUsed` entries this loader does not implement.
/// @details Optional extensions are legal to ignore, but silently dropping them can make assets
///          appear wrong. Warnings name the extension and explain the visual degradation.
/// @param root Parsed glTF root JSON object.
static void gltf_warn_unsupported_used_extensions(void *root) {
    void *used = jarr(root, "extensionsUsed");
    if (!used)
        return;
    for (int64_t i = 0; i < jarr_len(used); i++) {
        rt_string name = (rt_string)rt_seq_get(used, i);
        const char *ext = rt_string_is_handle(name) ? rt_string_cstr(name) : NULL;
        if (gltf_used_extension_supported(ext))
            continue;
        rt_asset_error_add_import_stat(RT_ASSET_IMPORT_STAT_IGNORED_EXTENSIONS, 1);
        rt_asset_error_add_warningf("glTF extension '%s' ignored: visual result may miss "
                                    "extension-specific material, geometry, animation, lighting, "
                                    "or texture behavior",
                                    (ext && *ext) ? ext : "<invalid extension name>");
    }
}

/// @brief Zero-initialise a `gltf_texture_info_t` to identity-transform defaults.
/// @details Sets texcoord=0, has_transform=0, offset=(0,0), scale=(1,1), rotation=0.0 so callers
///   can always call `gltf_read_texture_info` after this without guarding on partial
///   initialisation.
/// @param[out] info Texture metadata to reset; NULL is a no-op.
static void gltf_texture_info_init(gltf_texture_info_t *info) {
    if (!info)
        return;
    info->texcoord = 0;
    info->present = 0;
    info->has_transform = 0;
    info->offset[0] = 0.0;
    info->offset[1] = 0.0;
    info->scale[0] = 1.0;
    info->scale[1] = 1.0;
    info->rotation = 0.0;
}

/// @brief Parse a glTF `textureInfo` object (including `KHR_texture_transform` if present).
/// @details Reads the `texCoord` index and, if the `KHR_texture_transform` extension block is
///   present in `extensions`, reads the UV transform (offset, scale, rotation) and sets
///   `has_transform = 1`. The extension's `texCoord` can override the base texcoord index,
///   matching the KHR_texture_transform spec. Authored indices remain unclamped so primitive
///   validation can distinguish unsupported UV sets from TEXCOORD_0. When no transform block is
///   present the struct
///   retains identity defaults from `gltf_texture_info_init`. Null @p texture_info leaves @p out
///   at defaults so callers need not guard against missing fields.
/// @param texture_info  Parsed glTF `textureInfo` JSON object (may be NULL for default texture
/// slot).
/// @param out           Output struct; always initialised to defaults before filling.
static void gltf_read_texture_info(void *texture_info, gltf_texture_info_t *out) {
    void *extensions;
    void *transform;
    void *offset;
    void *scale;
    if (!out)
        return;
    gltf_texture_info_init(out);
    if (!texture_info)
        return;
    out->present = 1;
    out->texcoord = gltf_i32_from_i64_or(jint(texture_info, "texCoord", 0), INT32_MAX);
    extensions = jget(texture_info, "extensions");
    transform = extensions ? jget(extensions, "KHR_texture_transform") : NULL;
    if (!transform)
        return;
    out->has_transform = 1;
    out->texcoord = gltf_i32_from_i64_or(jint(transform, "texCoord", out->texcoord), INT32_MAX);
    offset = jarr(transform, "offset");
    scale = jarr(transform, "scale");
    if (offset && jarr_len(offset) >= 2) {
        out->offset[0] = jvalue_num(rt_seq_get(offset, 0), 0.0);
        out->offset[1] = jvalue_num(rt_seq_get(offset, 1), 0.0);
    }
    if (scale && jarr_len(scale) >= 2) {
        out->scale[0] = jvalue_num(rt_seq_get(scale, 0), 1.0);
        out->scale[1] = jvalue_num(rt_seq_get(scale, 1), 1.0);
    }
    out->rotation = jnum(transform, "rotation", 0.0);
}

/// @brief Map a glTF sampler `wrapS`/`wrapT` integer to a Zanna wrap-mode constant.
/// @details glTF uses the original GL enum integers: 33071 = GL_CLAMP_TO_EDGE,
///   33648 = GL_MIRRORED_REPEAT. Anything else (including the default 10497 =
///   GL_REPEAT) maps to `RT_MATERIAL3D_TEXTURE_WRAP_REPEAT`.
/// @param wrap  Raw integer from the glTF sampler JSON.
/// @return One of the `RT_MATERIAL3D_TEXTURE_WRAP_*` constants.
static int32_t gltf_map_sampler_wrap(int64_t wrap) {
    if (wrap == 33071)
        return RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE;
    if (wrap == 33648)
        return RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT;
    return RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Map the texel-selection component of a glTF filter enum to nearest or linear.
/// @param filter Raw glTF minification/magnification enum, or -1 when absent.
/// @return The independent Zanna min/mag filter constant.
static int32_t gltf_map_sampler_texel_filter(int64_t filter) {
    if (filter == 9728 || filter == 9984 || filter == 9986)
        return RT_MATERIAL3D_TEXTURE_FILTER_NEAREST;
    return RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

/// @brief Map a glTF minification enum to its independent mip-selection axis.
/// @details 9728/9729 select no mipmapping, 9984/9985 select the nearest mip, and 9986/9987
/// linearly blend adjacent mips. An absent or unknown value uses the loader's linear-mip default.
/// @param min_filter Raw glTF `minFilter` enum, or -1 when absent.
/// @return One `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` constant.
static int32_t gltf_map_sampler_mip_filter(int64_t min_filter) {
    if (min_filter == 9728 || min_filter == 9729)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
    if (min_filter == 9984 || min_filter == 9985)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST;
    return RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
}

/// @brief Initialise a `gltf_sampler_info_t` to the glTF default sampler state.
/// @details glTF defaults are repeat wrapping with linear min/mag texel filtering and linear mip
///          interpolation.
/// @param[out] info Sampler metadata to reset; NULL is a no-op.
static void gltf_sampler_info_init(gltf_sampler_info_t *info) {
    if (!info)
        return;
    info->wrap_s = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
    info->wrap_t = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
    info->min_filter = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    info->mag_filter = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    info->mip_filter = RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
}

/// @brief Parse a glTF sampler JSON object into a `gltf_sampler_info_t`.
/// @details Reads `wrapS`, `wrapT`, `minFilter`, and `magFilter` using their glTF
///   default values (10497 = REPEAT for wrap; -1 = absent for filter) and maps them
///   to Zanna constants via the independent wrap, texel-filter, and mip-filter mappers.
///   A NULL @p sampler_json leaves @p out at the glTF defaults.
/// @param sampler_json  Parsed glTF sampler object JSON; may be NULL for default state.
/// @param out           Output struct; always initialised before filling.
static void gltf_read_sampler_info(void *sampler_json, gltf_sampler_info_t *out) {
    if (!out)
        return;
    gltf_sampler_info_init(out);
    if (!sampler_json)
        return;
    out->wrap_s = gltf_map_sampler_wrap(jint(sampler_json, "wrapS", 10497));
    out->wrap_t = gltf_map_sampler_wrap(jint(sampler_json, "wrapT", 10497));
    {
        int64_t min_filter = jint(sampler_json, "minFilter", -1);
        int64_t mag_filter = jint(sampler_json, "magFilter", -1);
        out->min_filter = gltf_map_sampler_texel_filter(min_filter);
        out->mag_filter = gltf_map_sampler_texel_filter(mag_filter);
        out->mip_filter = gltf_map_sampler_mip_filter(min_filter);
    }
}

/// @brief DFS helper for cycle detection in the glTF node graph.
/// @details Uses a three-colour state array: 0 = unvisited, 1 = in-progress (grey), 2 = done
/// (black).
///   Returning 0 from a grey node means a back-edge (cycle) was found. Children that are out of
///   range or already grey are also treated as invalid. Called by `gltf_validate_node_graph`
///   once per node. The @p state array must be zero-initialised by the caller before the first
///   call.
/// @param nodes_arr   JSON array of glTF node objects.
/// @param node_count  Total number of nodes (bounds the valid index range).
/// @param node_idx    Node to visit.
/// @param state       Per-node colour byte array of length @p node_count.
/// @return 1 if the subtree is DAG-valid, 0 if a cycle or out-of-bounds child is found.
static int gltf_validate_node_visit(void *nodes_arr,
                                    int32_t node_count,
                                    int32_t node_idx,
                                    uint8_t *state) {
    typedef struct gltf_node_visit_item {
        int32_t node;
        int8_t exit;
    } gltf_node_visit_item;

    gltf_node_visit_item *stack = NULL;
    int32_t stack_count = 0;
    int32_t stack_capacity = 0;
    if (!nodes_arr || !state || node_idx < 0 || node_idx >= node_count)
        return 0;
    if (state[node_idx] == 1)
        return 0;
    if (state[node_idx] == 2)
        return 1;
    stack_capacity = 32;
    stack = (gltf_node_visit_item *)malloc((size_t)stack_capacity * sizeof(*stack));
    if (!stack)
        return 0;
    stack[stack_count++] = (gltf_node_visit_item){node_idx, 0};
    while (stack_count > 0) {
        gltf_node_visit_item item = stack[--stack_count];
        if (item.node < 0 || item.node >= node_count) {
            free(stack);
            return 0;
        }
        if (item.exit) {
            state[item.node] = 2;
            continue;
        }
        if (state[item.node] == 2)
            continue;
        if (state[item.node] == 1) {
            free(stack);
            return 0;
        }
        state[item.node] = 1;
        if (stack_count >= stack_capacity) {
            int32_t next_capacity;
            gltf_node_visit_item *grown;
            if (stack_capacity <= 0 || stack_capacity > INT32_MAX / 2 ||
                (size_t)stack_capacity * 2u > SIZE_MAX / sizeof(*stack)) {
                free(stack);
                return 0;
            }
            next_capacity = stack_capacity * 2;
            grown = (gltf_node_visit_item *)realloc(stack, (size_t)next_capacity * sizeof(*stack));
            if (!grown) {
                free(stack);
                return 0;
            }
            stack = grown;
            stack_capacity = next_capacity;
        }
        stack[stack_count++] = (gltf_node_visit_item){item.node, 1};
        {
            void *node_json = rt_seq_get(nodes_arr, item.node);
            void *children = jarr(node_json, "children");
            int64_t child_len = jarr_len(children);
            for (int64_t ci = child_len - 1; ci >= 0; --ci) {
                int64_t child = jvalue_int(rt_seq_get(children, ci), -1);
                if (child < 0 || child >= node_count || state[child] == 1) {
                    free(stack);
                    return 0;
                }
                if (state[child] == 2)
                    continue;
                if (stack_count >= stack_capacity) {
                    int32_t next_capacity;
                    gltf_node_visit_item *grown;
                    if (stack_capacity <= 0 || stack_capacity > INT32_MAX / 2 ||
                        (size_t)stack_capacity * 2u > SIZE_MAX / sizeof(*stack)) {
                        free(stack);
                        return 0;
                    }
                    next_capacity = stack_capacity * 2;
                    grown = (gltf_node_visit_item *)realloc(stack,
                                                            (size_t)next_capacity * sizeof(*stack));
                    if (!grown) {
                        free(stack);
                        return 0;
                    }
                    stack = grown;
                    stack_capacity = next_capacity;
                }
                stack[stack_count++] = (gltf_node_visit_item){(int32_t)child, 0};
            }
        }
    }
    free(stack);
    return 1;
}

/// @brief Validate the glTF node graph for a well-formed forest (no cycles, unique parents).
/// @details A valid glTF scene node graph must be a directed acyclic forest: each node has at most
///   one parent, there are no back-edges (cycles), and all child indices are in `[0, node_count)`.
///   This function builds a parent array (each entry is -1 for roots or the parent's index), then
///   runs a DFS over every node to detect cycles via `gltf_validate_node_graph`. Returns 0 and
///   frees intermediates if any of these invariants are violated. When @p out_parent is non-NULL
///   and validation succeeds the parent array is returned (caller must `free` it); otherwise it is
///   freed internally. A null or empty nodes array is treated as a trivially valid empty forest.
/// @param nodes_arr   JSON array of glTF node objects.
/// @param node_count  Number of nodes; must match `jarr_len(nodes_arr)`.
/// @param out_parent  If non-NULL, receives the allocated parent-index array on success.
/// @return 1 if the graph is valid, 0 if a cycle, duplicate parent, or OOM is detected.
static int gltf_validate_node_graph(void *nodes_arr, int32_t node_count, int **out_parent) {
    int *parent = NULL;
    uint8_t *state = NULL;
    if (out_parent)
        *out_parent = NULL;
    if (!nodes_arr || node_count <= 0)
        return 1;
    parent = (int *)malloc((size_t)node_count * sizeof(*parent));
    state = (uint8_t *)calloc((size_t)node_count, sizeof(*state));
    if (!parent || !state) {
        free(parent);
        free(state);
        return 0;
    }
    for (int32_t i = 0; i < node_count; i++)
        parent[i] = -1;
    for (int32_t ni = 0; ni < node_count; ni++) {
        void *node_json = rt_seq_get(nodes_arr, ni);
        void *children = jarr(node_json, "children");
        for (int64_t ci = 0; ci < jarr_len(children); ci++) {
            int64_t child = jvalue_int(rt_seq_get(children, ci), -1);
            if (child < 0 || child >= node_count || parent[child] >= 0) {
                free(parent);
                free(state);
                return 0;
            }
            parent[child] = ni;
        }
    }
    for (int32_t ni = 0; ni < node_count; ni++) {
        if (!gltf_validate_node_visit(nodes_arr, node_count, ni, state)) {
            free(parent);
            free(state);
            return 0;
        }
    }
    free(state);
    if (out_parent)
        *out_parent = parent;
    else
        free(parent);
    return 1;
}

/// @brief Bind a texture index + sampler state + UV-transform to one material texture slot.
/// @details Looks up independent wrap/min/mag/mip state from the pre-built @p texture_samplers
///   array at @p texture_index (using glTF defaults when the index is out of range), then calls
///   `rt_material3d_set_import_texture_slot_sampler_axes`. If @p info is NULL
///   an identity `gltf_texture_info_t` (texcoord=0, no transform) is used so callers can pass NULL
///   for textures that have no `KHR_texture_transform` extension block. Slots outside the valid
///   range `[0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT)` are silently ignored.
/// @param texture_samplers  Per-texture sampler state array resolved from the `"samplers"` array.
/// @param texture_count     Length of @p texture_samplers.
/// @param texture_index     glTF texture index into @p texture_samplers (and the image table).
/// @param material          Zanna material object to write the slot into.
/// @param slot              Destination slot index in `[0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT)`.
/// @param info              Optional UV-transform + texcoord override; NULL uses identity.
static void gltf_apply_texture_slot(const gltf_sampler_info_t *texture_samplers,
                                    int32_t texture_count,
                                    int64_t texture_index,
                                    void *material,
                                    int64_t slot,
                                    const gltf_texture_info_t *info) {
    gltf_texture_info_t identity;
    const gltf_texture_info_t *texture_info = info;
    int32_t wrap_s = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
    int32_t wrap_t = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
    int32_t min_filter = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    int32_t mag_filter = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
    int32_t mip_filter = RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    if (!material || slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return;
    if (!texture_info) {
        gltf_texture_info_init(&identity);
        texture_info = &identity;
    }
    if (texture_samplers && texture_index >= 0 && texture_index < texture_count) {
        wrap_s = texture_samplers[texture_index].wrap_s;
        wrap_t = texture_samplers[texture_index].wrap_t;
        min_filter = texture_samplers[texture_index].min_filter;
        mag_filter = texture_samplers[texture_index].mag_filter;
        mip_filter = texture_samplers[texture_index].mip_filter;
    }
    rt_material3d_set_import_texture_slot_sampler_axes(material,
                                                       slot,
                                                       texture_info->texcoord,
                                                       texture_info->offset[0],
                                                       texture_info->offset[1],
                                                       texture_info->scale[0],
                                                       texture_info->scale[1],
                                                       texture_info->rotation,
                                                       wrap_s,
                                                       wrap_t,
                                                       min_filter,
                                                       mag_filter,
                                                       mip_filter);
}

/// @brief True if a texture is a supported format but its decoded image is still missing.
/// @details Flags an in-range, supported-format texture whose image slot is NULL, i.e. one the
///          caller still needs to decode/stage before the material can reference it.
/// @param texture_index Candidate glTF texture index.
/// @param texture_count Number of entries in the parallel texture arrays.
/// @param texture_images Decoded image-reference array, or NULL.
/// @param texture_supported Per-texture supported-format flags.
/// @return Non-zero when the index is valid and supported but has no decoded image.
static int gltf_texture_index_missing_supported_payload(int64_t texture_index,
                                                        int32_t texture_count,
                                                        void **texture_images,
                                                        const uint8_t *texture_supported) {
    return texture_index >= 0 && texture_index < texture_count && texture_supported &&
           texture_supported[texture_index] && (!texture_images || !texture_images[texture_index]);
}

/// @brief Resolve the image source for a parsed glTF texture, including KHR_texture_basisu.
/// @param texture_json Parsed texture object.
/// @return Basis Universal extension source when valid, otherwise the core `source`,
///         or -1 when neither is present.
static int64_t gltf_texture_source_index(void *texture_json) {
    void *extensions;
    void *basisu;
    int64_t source_idx = jint(texture_json, "source", -1);
    extensions = jget(texture_json, "extensions");
    basisu = extensions ? jget(extensions, "KHR_texture_basisu") : NULL;
    int64_t basisu_source_idx = jint(basisu, "source", -1);
    if (basisu_source_idx >= 0)
        return basisu_source_idx;
    return source_idx;
}

//===----------------------------------------------------------------------===//
// Implementation split across cohesive .inc units compiled as one translation unit.
//===----------------------------------------------------------------------===//
// clang-format off
#include "rt_gltf_codec.inc"
#include "rt_gltf_meshopt.inc"
#include "rt_gltf_draco.inc"
#include "rt_gltf_accessor.inc"
#include "rt_gltf_import.inc"
#include "rt_gltf_preload.inc"
#include "rt_gltf_anim.inc"
#include "rt_gltf_material.inc"
#include "rt_gltf_mesh.inc"
#include "rt_gltf_scene.inc"
// clang-format on

//===----------------------------------------------------------------------===//
// Main loader
//===----------------------------------------------------------------------===//

/// @brief Release a decoded glTF buffer table while preserving the borrowed GLB BIN chunk.
/// @param[in,out] buffers Heap buffer table and owned dependency payloads.
/// @param buf_count Number of initialized buffer entries.
/// @param bin_chunk Borrowed pointer into the root GLB allocation, never freed here.
static void gltf_free_buffers(gltf_buffer_t *buffers, int buf_count, const uint8_t *bin_chunk) {
    if (!buffers)
        return;
    for (int i = 0; i < buf_count; i++) {
        if (buffers[i].data != bin_chunk)
            free(buffers[i].data);
    }
    free(buffers);
}

/// @brief Resolve one glTF buffer URI from staged, data-URI, filesystem, or asset data.
/// @param filepath Root glTF path used to resolve relative dependencies.
/// @param load_assets Non-zero to resolve external bytes through the asset manager.
/// @param[in,out] preload_bundle Optional staged dependency bundle consumed by key/path.
/// @param buffer_index Zero-based buffer index used for staged dependency keys.
/// @param uri Non-NULL glTF buffer URI.
/// @param byte_length Declared minimum buffer byte length.
/// @param[out] buffer Destination that takes ownership of loaded bytes.
/// @return Non-zero on success, including a legal empty data buffer; zero for malformed,
///         missing, undersized, unsupported, or allocation-failed dependencies.
static int gltf_load_buffer_uri(const char *filepath,
                                int load_assets,
                                rt_gltf_preload_bundle *preload_bundle,
                                int buffer_index,
                                const char *uri,
                                size_t byte_length,
                                gltf_buffer_t *buffer) {
    if (strncmp(uri, "data:", 5) == 0) {
        char mime_type[64];
        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        char preload_key[64];
        gltf_preload_buffer_key(buffer_index, preload_key, sizeof(preload_key));
        decoded = gltf_preload_bundle_take_dependency(
            preload_bundle, preload_key, GLTF_PRELOAD_DEP_BUFFER, &decoded_len);
        if (decoded ||
            gltf_parse_data_uri(uri, mime_type, sizeof(mime_type), &decoded, &decoded_len)) {
            if (decoded_len < byte_length) {
                free(decoded);
                return 0;
            }
            buffer->data = decoded;
            buffer->len = byte_length;
            return 1;
        }
        return byte_length == 0;
    }

    char *buf_path = gltf_resolve_relative_path_alloc(filepath, uri);
    if (!buf_path) {
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                            "GLTF.Load: unsupported buffer dependency '%s' for '%s'",
                            uri ? uri : "",
                            filepath ? filepath : "");
        return 0;
    }
    buffer->data =
        gltf_load_dependency_bytes(buf_path, load_assets, byte_length, preload_bundle, NULL);
    if (buffer->data)
        buffer->len = byte_length;
    if (byte_length > 0 && (!buffer->data || buffer->len < byte_length)) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND,
                            "GLTF.Load: failed to load buffer dependency '%s' for '%s'",
                            buf_path,
                            filepath ? filepath : "");
        free(buf_path);
        return 0;
    }
    free(buf_path);
    return 1;
}

/// @brief Build the load-local glTF buffer table from JSON and an optional GLB BIN chunk.
/// @details The first URI-less buffer may borrow @p bin_chunk. Other buffers own bytes
///          loaded by @ref gltf_load_buffer_uri. URI-less placeholders are permitted only
///          when compressed buffer views make their uncompressed fallback optional.
/// @param root Parsed glTF root JSON object.
/// @param filepath Root document path for relative dependency resolution.
/// @param load_assets Non-zero to use asset-manager dependency resolution.
/// @param[in,out] preload_bundle Optional staged dependency bundle.
/// @param bin_chunk Borrowed GLB BIN payload, or NULL.
/// @param bin_chunk_len Readable BIN payload length, including permitted padding.
/// @param root_size Root source size used to bound untrusted table counts.
/// @param allow_placeholder_buffers Non-zero to allow data-less meshopt fallback buffers.
/// @param[out] out_buffers Receives the newly allocated buffer table.
/// @param[out] out_buf_count Receives the number of declared buffer entries.
/// @return Non-zero on success, or zero for invalid counts, lengths, dependencies,
///         placeholder policy, or allocation failure.
static int gltf_load_buffers(void *root,
                             const char *filepath,
                             int load_assets,
                             rt_gltf_preload_bundle *preload_bundle,
                             uint8_t *bin_chunk,
                             size_t bin_chunk_len,
                             size_t root_size,
                             int allow_placeholder_buffers,
                             gltf_buffer_t **out_buffers,
                             int *out_buf_count) {
    void *buffers_arr = jarr(root, "buffers");
    int64_t buf_count64 = jarr_len(buffers_arr);
    if (buf_count64 < 0 || buf_count64 > INT32_MAX ||
        !rt_untrusted_count_ok(buf_count64, 1u, root_size))
        return 0;
    size_t buffer_slots = (size_t)buf_count64 + 1u;
    if (buffer_slots > SIZE_MAX / sizeof(gltf_buffer_t))
        return 0;
    int buf_count = (int)buf_count64;
    gltf_buffer_t *buffers = (gltf_buffer_t *)calloc(buffer_slots, sizeof(gltf_buffer_t));
    if (!buffers)
        return 0;
    for (int i = 0; i < buf_count; i++) {
        void *buf_obj = rt_seq_get(buffers_arr, (int64_t)i);
        int64_t byte_length_raw = jint(buf_obj, "byteLength", -1);
        const char *uri = jstr(buf_obj, "uri");
        size_t byte_length;
        if (byte_length_raw < 0) {
            gltf_free_buffers(buffers, buf_count, bin_chunk);
            return 0;
        }
        byte_length = (size_t)byte_length_raw;
        if (i == 0 && bin_chunk && !uri) {
            if (byte_length > bin_chunk_len || byte_length > SIZE_MAX - 3u ||
                bin_chunk_len > byte_length + 3u) {
                gltf_free_buffers(buffers, buf_count, bin_chunk);
                return 0;
            }
            buffers[i].data = bin_chunk;
            buffers[i].len = byte_length;
        } else if (uri) {
            if (!gltf_load_buffer_uri(
                    filepath, load_assets, preload_bundle, i, uri, byte_length, &buffers[i])) {
                gltf_free_buffers(buffers, buf_count, bin_chunk);
                return 0;
            }
        } else if (byte_length > 0 && !allow_placeholder_buffers) {
            /* URI-less buffers are only legal as EXT_meshopt_compression fallback
             * placeholders; compressed views bypass them via the shadow table and
             * plain views referencing a data-less buffer keep failing bounds checks. */
            gltf_free_buffers(buffers, buf_count, bin_chunk);
            return 0;
        }
    }
    *out_buffers = buffers;
    *out_buf_count = buf_count;
    return 1;
}

/// @brief Allocate an empty, finalized glTF asset container.
/// @return Newly allocated runtime asset with all arrays/counts initialized, or NULL.
static rt_gltf_asset *gltf_asset_new_empty(void) {
    rt_gltf_asset *asset =
        (rt_gltf_asset *)rt_obj_new_i64(RT_G3D_GLTF_ASSET_CLASS_ID, (int64_t)sizeof(rt_gltf_asset));
    if (!asset)
        return NULL;
    asset->vptr = NULL;
    asset->meshes = NULL;
    asset->mesh_count = 0;
    asset->mesh_capacity = 0;
    asset->materials = NULL;
    asset->material_count = 0;
    asset->material_capacity = 0;
    asset->skeletons = NULL;
    asset->skeleton_count = 0;
    asset->skeleton_capacity = 0;
    asset->animations = NULL;
    asset->animation_count = 0;
    asset->animation_capacity = 0;
    asset->node_animations = NULL;
    asset->node_animation_count = 0;
    asset->node_animation_capacity = 0;
    asset->cameras = NULL;
    asset->camera_count = 0;
    asset->camera_capacity = 0;
    asset->scenes = NULL;
    asset->scene_count = 0;
    asset->scene_capacity = 0;
    asset->scene_root = NULL;
    asset->node_count = 0;
    rt_obj_set_finalizer(asset, gltf_asset_finalize);
    return asset;
}

/// @brief Extract an owned NUL-terminated JSON copy and borrowed BIN view from glTF/GLB bytes.
/// @param file_data Root source allocation.
/// @param file_size Number of readable source bytes.
/// @param[out] out_json_str Receives a newly allocated JSON copy.
/// @param[out] out_bin_chunk Receives a borrowed pointer into @p file_data, or NULL.
/// @param[out] out_bin_chunk_len Receives the BIN payload length.
/// @return Non-zero on success, or zero for malformed root framing, size overflow,
///         or allocation failure.
static int gltf_extract_json_document(uint8_t *file_data,
                                      size_t file_size,
                                      char **out_json_str,
                                      uint8_t **out_bin_chunk,
                                      size_t *out_bin_chunk_len) {
    gltf_root_document_view document;
    char *json_str;
    *out_json_str = NULL;
    *out_bin_chunk = NULL;
    *out_bin_chunk_len = 0;
    if (!gltf_parse_root_document_view(file_data, file_size, &document) ||
        document.json_len > SIZE_MAX - 1u)
        return 0;
    json_str = (char *)malloc(document.json_len + 1u);
    if (!json_str)
        return 0;
    memcpy(json_str, document.json, document.json_len);
    json_str[document.json_len] = '\0';
    *out_json_str = json_str;
    *out_bin_chunk = document.bin;
    *out_bin_chunk_len = document.bin_len;
    return 1;
}

/// @brief Validate and parse an owned glTF JSON string into a runtime root object.
/// @details Enforces exact integer-token syntax, required data-URI validity, glTF 2.x
///          versioning, and required-extension support. JSON-parser traps are converted
///          into recoverable asset errors.
/// @param[in,out] json_str Owned NUL-terminated JSON allocation, consumed on every path.
/// @param json_len Exact JSON byte length excluding the terminator.
/// @return Owned parsed root JSON object on success, or NULL with an asset error set.
static void *gltf_parse_validated_root_json(char *json_str, size_t json_len) {
    rt_string json_rts;
    void *root = NULL;
    jmp_buf json_recovery;
    const char *trap_message = NULL;
    if (!json_str)
        return NULL;
    if (json_len == 0 || memchr(json_str, '\0', json_len) != NULL) {
        free(json_str);
        return NULL;
    }
    if (!gltf_json_validate_gltf_integral_tokens(json_str, json_len)) {
        rt_asset_error_set_if_empty(
            RT_ASSET_ERROR_CORRUPT,
            "GLTF.Load: integral fields must use exact non-exponent JSON integer tokens");
        free(json_str);
        return NULL;
    }
    if (!gltf_validate_required_data_uri_images(json_str, json_len)) {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_UNSUPPORTED,
                                    "GLTF.Load: required data URI image payload is invalid");
        free(json_str);
        return NULL;
    }
    json_rts = rt_string_from_bytes(json_str, json_len);
    rt_trap_set_recovery(&json_recovery);
    if (setjmp(json_recovery) == 0)
        root = rt_json_parse_object(json_rts);
    else
        trap_message = rt_trap_get_error();
    rt_trap_clear_recovery();
    rt_string_unref(json_rts);
    free(json_str);
    if (!root) {
        rt_asset_error_setf_if_empty(RT_ASSET_ERROR_CORRUPT,
                                     "GLTF.Load: invalid JSON%s%s",
                                     (trap_message && trap_message[0]) ? ": " : "",
                                     (trap_message && trap_message[0]) ? trap_message : "");
        return NULL;
    }
    {
        void *asset_json = jget(root, "asset");
        const char *version = jstr(asset_json, "version");
        if (!version || strncmp(version, "2.", 2) != 0) {
            rt_asset_error_set_if_empty(RT_ASSET_ERROR_UNSUPPORTED,
                                        "GLTF.Load: asset.version must be 2.x");
            gltf_release_local(root);
            return NULL;
        }
    }
    if (!gltf_validate_required_extensions(root)) {
        gltf_release_local(root);
        return NULL;
    }
    gltf_warn_unsupported_used_extensions(root);
    return root;
}

/// @brief Transaction-local resource tables shared across glTF payload import phases.
typedef struct {
    /// Owned decoded image references.
    void **images;
    /// Number of entries in @ref images.
    int image_count;
    /// Per-image flags indicating a required material dependency.
    uint8_t *image_required;
    /// Borrowed/parallel decoded images indexed by texture.
    void **texture_images;
    /// Per-texture supported-format flags.
    uint8_t *texture_supported;
    /// Normalized sampler state indexed by texture.
    gltf_sampler_info_t *texture_samplers;
    /// Number of entries in texture-parallel arrays.
    int texture_count;
    /// First flattened primitive index for each source mesh.
    int *mesh_prim_start;
    /// Flattened primitive count for each source mesh.
    int *mesh_prim_count;
    /// Material references parallel to flattened primitives.
    void **primitive_materials;
    /// Per-primitive KHR_materials_variants material-index tables.
    int32_t **primitive_variant_mappings;
    /// Number of initialized mapping-table pointers.
    int32_t primitive_variant_mapping_count;
    /// Parsed texture-slot metadata parallel to imported materials.
    gltf_material_info_t *material_infos;
    /// Owned load-local skin mappings.
    gltf_skin_t *skins;
    /// Number of initialized skins.
    int32_t skin_count;
    /// Owned references to parsed KHR_lights_punctual templates.
    void **imported_lights;
    /// Number of initialized imported light references.
    int32_t imported_light_count;
} gltf_load_scratch_t;

/// @brief Release every allocation and temporary reference in a load scratch record.
/// @param[in,out] scratch Scratch tables to clear; NULL is accepted as a no-op.
static void gltf_load_scratch_cleanup(gltf_load_scratch_t *scratch) {
    if (!scratch)
        return;
    if (scratch->images) {
        for (int i = 0; i < scratch->image_count; i++)
            gltf_release_ref(&scratch->images[i]);
    }
    free(scratch->images);
    free(scratch->image_required);
    free(scratch->texture_images);
    free(scratch->texture_supported);
    free(scratch->texture_samplers);
    free(scratch->mesh_prim_start);
    free(scratch->mesh_prim_count);
    free(scratch->primitive_materials);
    if (scratch->primitive_variant_mappings) {
        for (int32_t i = 0; i < scratch->primitive_variant_mapping_count; i++)
            free(scratch->primitive_variant_mappings[i]);
    }
    free(scratch->primitive_variant_mappings);
    free(scratch->material_infos);
    if (scratch->imported_lights) {
        for (int32_t i = 0; i < scratch->imported_light_count; i++)
            gltf_release_ref(&scratch->imported_lights[i]);
    }
    free(scratch->imported_lights);
    gltf_free_skins(scratch->skins, scratch->skin_count);
}

/// @brief Parse root `extensions.KHR_materials_variants.variants[].name` into the asset.
/// @details Missing or unnamed entries synthesize "variant_N" so every variant stays
///          addressable by index and by a stable display name. Absent extension → no-op.
/// @param[in,out] asset Asset receiving owned variant-name copies.
/// @param root Parsed glTF root JSON object.
static void gltf_load_variant_names(rt_gltf_asset *asset, void *root) {
    void *extensions = jget(root, "extensions");
    void *variants_ext = extensions ? jget(extensions, "KHR_materials_variants") : NULL;
    void *variants_arr = variants_ext ? jarr(variants_ext, "variants") : NULL;
    int64_t count = jarr_len(variants_arr);
    if (!asset || count <= 0)
        return;
    if (count > INT32_MAX || (size_t)count > SIZE_MAX / sizeof(char *))
        return;
    asset->variant_names = (char **)calloc((size_t)count, sizeof(char *));
    if (!asset->variant_names)
        return;
    for (int64_t i = 0; i < count; i++) {
        const char *name = jstr(rt_seq_get(variants_arr, i), "name");
        char fallback[64];
        if (!name || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "variant_%d", (int)i);
            name = fallback;
        }
        asset->variant_names[i] = gltf_strdup_cstr(name);
        if (!asset->variant_names[i]) {
            for (int64_t j = 0; j < i; j++)
                free(asset->variant_names[j]);
            free(asset->variant_names);
            asset->variant_names = NULL;
            return;
        }
    }
    asset->variant_name_count = (int32_t)count;
}

/// @brief Run the ordered image, material, mesh, skin, animation, and scene import phases.
/// @param[in,out] asset Empty staged asset receiving published runtime resources.
/// @param root Parsed and validated glTF root JSON object.
/// @param filepath Root source path for dependency resolution.
/// @param load_assets Non-zero to use asset-manager dependency resolution.
/// @param[in,out] preload_bundle Optional staged dependency bundle.
/// @param buffers Validated load-local buffer table.
/// @param buf_count Number of entries in @p buffers.
/// @param[in,out] scratch Transaction-local cross-phase tables.
/// @return Zero when all phases succeed, or non-zero when any hard import phase fails.
static int gltf_load_asset_payload(rt_gltf_asset *asset,
                                   void *root,
                                   const char *filepath,
                                   int load_assets,
                                   rt_gltf_preload_bundle *preload_bundle,
                                   gltf_buffer_t *buffers,
                                   int buf_count,
                                   gltf_load_scratch_t *scratch) {
    int load_failed = 0;
    gltf_load_images_and_textures(root,
                                  filepath,
                                  load_assets,
                                  preload_bundle,
                                  buffers,
                                  buf_count,
                                  &scratch->images,
                                  &scratch->image_count,
                                  &scratch->image_required,
                                  &scratch->texture_images,
                                  &scratch->texture_supported,
                                  &scratch->texture_samplers,
                                  &scratch->texture_count,
                                  &load_failed);
    gltf_load_materials(asset,
                        root,
                        scratch->texture_images,
                        scratch->texture_supported,
                        scratch->texture_samplers,
                        scratch->texture_count,
                        &scratch->material_infos,
                        &load_failed);
    gltf_parse_punctual_lights(root, &scratch->imported_lights, &scratch->imported_light_count);
    gltf_load_variant_names(asset, root);
    gltf_load_meshes(asset,
                     root,
                     buffers,
                     buf_count,
                     preload_bundle,
                     scratch->material_infos,
                     &scratch->mesh_prim_start,
                     &scratch->mesh_prim_count,
                     &scratch->primitive_materials,
                     &scratch->primitive_variant_mappings,
                     &scratch->primitive_variant_mapping_count,
                     &load_failed);
    gltf_parse_skins(
        asset, root, buffers, buf_count, &scratch->skins, &scratch->skin_count, &load_failed);
    if (!load_failed) {
        gltf_parse_animations(asset, root, buffers, buf_count, scratch->skins, scratch->skin_count);
        gltf_parse_node_animations(
            asset, root, buffers, buf_count, scratch->skins, scratch->skin_count);
        load_failed = gltf_build_node_hierarchy(asset,
                                                root,
                                                scratch->mesh_prim_start,
                                                scratch->mesh_prim_count,
                                                scratch->primitive_materials,
                                                scratch->primitive_variant_mappings,
                                                scratch->skins,
                                                scratch->skin_count,
                                                scratch->imported_lights,
                                                scratch->imported_light_count);
    }
    return load_failed;
}

/// @brief Load a glTF 2.0 asset from disk and build the engine representation.
/// @details Top-level entry point that orchestrates the entire load pipeline. The
///          high-level stages are:
///            1. Read the whole file (hard cap 256 MiB to avoid denial-of-service
///              on pathological inputs).
///            2. Detect format: the 4-byte magic `glTF` signals a binary GLB
///              container, otherwise treat the bytes as a JSON `.gltf`.
///            3. For GLB: walk the chunk list, extracting the JSON chunk and the
///              embedded BIN chunk (which substitutes for buffer index 0).
///            4. Parse buffers (data-URIs and external files), bufferViews,
///              accessors, and textures into a `gltf_buffer_t[]` scratch table.
///            5. Walk the first scene: for every mesh primitive, decode
///              POSITION / NORMAL / TEXCOORD_0 / TANGENT / JOINTS_0 / WEIGHTS_0
///              attributes through their accessor views, plus the index buffer,
///              and build a Mesh3D.
///            6. Materials: baseColorFactor + baseColorTexture resolved to
///              Material3D. Missing materials fall back to white.
///            7. If skins are present, `gltf_parse_skins` builds Skeleton3D objects
///              and `gltf_apply_skin_to_mesh` retargets the vertex bone indices
///              onto engine bones.
///            8. `gltf_parse_animations` converts glTF sampler/channel data into
///              bone-oriented Animation3D.
///            9. Release scratch buffers (decoded binary data freed; accessor
///               views point into buffers freed at end).
///
///          Failure paths: any I/O error, JSON parse error, or allocation failure
///          returns NULL. Partial state is rolled back via `gltf_release_ref` so
///          the caller never sees a half-built asset.
/// @param path File path to `.gltf` or `.glb`.
/// @param load_assets Non-zero to resolve root/external dependencies through the asset manager.
/// @param[in,out] preloaded_data Optional owned root-source buffer, consumed by this load.
/// @param preloaded_size Number of readable bytes in @p preloaded_data.
/// @param[in,out] preload_bundle Optional owned staged-dependency bundle; dependencies
///                               are consumed but the caller retains/frees the bundle object.
/// @return Opaque rt_gltf_asset*, or NULL on failure.
static void *rt_gltf_load_impl(rt_string path,
                               int load_assets,
                               uint8_t *preloaded_data,
                               size_t preloaded_size,
                               rt_gltf_preload_bundle *preload_bundle) {
    if (!path) {
        free(preloaded_data);
        return NULL;
    }
    const char *filepath = rt_string_cstr(path);
    if (!filepath) {
        free(preloaded_data);
        return NULL;
    }

    size_t file_size = 0;
    uint8_t *file_data = preloaded_data;
    if (file_data) {
        file_size = preloaded_size;
    } else {
        file_data = gltf_load_root_bytes(path, filepath, load_assets, &file_size);
    }
    if (!file_data) {
        rt_asset_error_setf_if_empty(
            RT_ASSET_ERROR_UNREADABLE, "GLTF.Load: failed to read '%s'", filepath);
        return NULL;
    }
    /* Hard cap (256 MiB) bounding worst-case parse/allocation cost, matching the documented
     * limit in this function's header and the sibling TEXTUREASSET3D/VSCN loaders. The
     * previous LONG_MAX gate left the effective ceiling at ~8 EiB on 64-bit, contradicting
     * the doc comment and admitting multi-GiB files. */
    if (file_size > (size_t)256u * 1024u * 1024u) {
        if (!preloaded_data)
            free(file_data);
        rt_asset_error_setf(RT_ASSET_ERROR_TOO_LARGE, "GLTF.Load: '%s' is too large", filepath);
        return NULL;
    }

    char *json_str = NULL;
    uint8_t *bin_chunk = NULL;
    size_t bin_chunk_len = 0;
    if (!gltf_extract_json_document(file_data, file_size, &json_str, &bin_chunk, &bin_chunk_len)) {
        free(file_data);
        rt_asset_error_setf(RT_ASSET_ERROR_CORRUPT,
                            "GLTF.Load: '%s' has an invalid glTF/GLB root document",
                            filepath);
        return NULL;
    }

    gltf_root_document_view root_document;
    size_t json_len = 0;
    if (gltf_parse_root_document_view(file_data, file_size, &root_document))
        json_len = root_document.json_len;
    void *root = gltf_parse_validated_root_json(json_str, json_len);
    if (!root) {
        free(file_data);
        rt_asset_error_setf_if_empty(
            RT_ASSET_ERROR_CORRUPT, "GLTF.Load: '%s' has invalid JSON", filepath);
        return NULL;
    }

    int buf_count = 0;
    gltf_buffer_t *buffers = NULL;
    gltf_load_scratch_t scratch;
    gltf_meshopt_table_t meshopt_table;
    const gltf_meshopt_table_t *prev_meshopt_views;
    int load_failed = 0;
    memset(&scratch, 0, sizeof(scratch));
    memset(&meshopt_table, 0, sizeof(meshopt_table));
    if (!gltf_load_buffers(root,
                           filepath,
                           load_assets,
                           preload_bundle,
                           bin_chunk,
                           bin_chunk_len,
                           file_size,
                           gltf_document_lists_meshopt(root),
                           &buffers,
                           &buf_count)) {
        gltf_release_local(root);
        free(file_data);
        rt_asset_error_setf_if_empty(
            RT_ASSET_ERROR_CORRUPT, "GLTF.Load: '%s' has invalid buffers", filepath);
        return NULL;
    }

    /* EXT_meshopt_compression: materialize compressed bufferViews before anything
     * (sparse validation included) resolves view data. The decoded table is
     * thread-scoped so gltf_get_buffer_view_data can consult it transparently. */
    if (!gltf_meshopt_decode_views(root, buffers, buf_count, &meshopt_table)) {
        gltf_free_buffers(buffers, buf_count, bin_chunk);
        gltf_release_local(root);
        free(file_data);
        rt_asset_error_setf_if_empty(RT_ASSET_ERROR_CORRUPT,
                                     "GLTF.Load: '%s' has invalid compressed buffer views",
                                     filepath);
        return NULL;
    }
    prev_meshopt_views = g_gltf_meshopt_views;
    g_gltf_meshopt_views = &meshopt_table;

    if (!gltf_validate_sparse_accessors(root, buffers, buf_count)) {
        g_gltf_meshopt_views = prev_meshopt_views;
        gltf_meshopt_table_free(&meshopt_table);
        gltf_free_buffers(buffers, buf_count, bin_chunk);
        gltf_release_local(root);
        free(file_data);
        rt_asset_error_setf(
            RT_ASSET_ERROR_CORRUPT, "GLTF.Load: '%s' has invalid sparse accessors", filepath);
        return NULL;
    }

    rt_gltf_asset *asset = gltf_asset_new_empty();
    if (!asset) {
        g_gltf_meshopt_views = prev_meshopt_views;
        gltf_meshopt_table_free(&meshopt_table);
        gltf_free_buffers(buffers, buf_count, bin_chunk);
        gltf_release_local(root);
        free(file_data);
        return NULL;
    }

    load_failed = gltf_load_asset_payload(
        asset, root, filepath, load_assets, preload_bundle, buffers, buf_count, &scratch);
    gltf_load_scratch_cleanup(&scratch);

    g_gltf_meshopt_views = prev_meshopt_views;
    gltf_meshopt_table_free(&meshopt_table);
    gltf_free_buffers(buffers, buf_count, bin_chunk);
    gltf_release_local(root);
    free(file_data);

    if (load_failed) {
        gltf_release_ref((void **)&asset);
        rt_asset_error_setf_if_empty(RT_ASSET_ERROR_CORRUPT,
                                     "GLTF.Load: '%s' contains unsupported or corrupt data",
                                     filepath);
        return NULL;
    }
    return asset;
}

/* Plan 09: thread-scoped import options — reaches the mesh decode gates without
 * threading a parameter through every loader layer. Zero value = defaults. */
static RT_THREAD_LOCAL rt_gltf_load_options g_gltf_thread_load_options;

/// @brief Return the default glTF import options with every optional behavior disabled.
/// @return Zero-initialized option value suitable for selective field overrides.
rt_gltf_load_options rt_gltf_load_options_default(void) {
    rt_gltf_load_options opts;
    memset(&opts, 0, sizeof(opts));
    return opts;
}

/// @brief Borrow the calling thread's active glTF import options.
/// @return Pointer to thread-local option storage, valid on the calling thread until
///         the next option update or thread teardown.
const rt_gltf_load_options *rt_gltf_active_load_options(void) {
    return &g_gltf_thread_load_options;
}

/// @brief Install thread-scoped import options.
/// @param opts Options to copy, or NULL to restore defaults.
/// @return Previous thread-local option value for scoped restoration.
rt_gltf_load_options rt_gltf_set_thread_load_options(const rt_gltf_load_options *opts) {
    rt_gltf_load_options previous = g_gltf_thread_load_options;
    g_gltf_thread_load_options = opts ? *opts : rt_gltf_load_options_default();
    return previous;
}

/// @brief Load a glTF/GLB directly from the filesystem.
/// @param path Runtime string containing the native root-document path.
/// @return Newly allocated glTF asset on success, or NULL after recording load failure.
void *rt_gltf_load(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("GLTF.Load: path must not be null");
        return NULL;
    }
    if (!rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_trap("GLTF.Load: invalid path");
        return NULL;
    }
    void *asset = rt_gltf_load_impl(path, 0, NULL, 0, NULL);
    if (asset) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "GLTF.Load: failed to load glTF");
        rt_asset_error_end_load_failure();
    }
    return asset;
}

/// @brief Load a glTF/GLB through mounted/embedded asset resolution with development fallback.
/// @param path Runtime string containing the logical asset path.
/// @return Newly allocated glTF asset on success, or NULL after recording load failure.
void *rt_gltf_load_asset(rt_string path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("GLTF.LoadAsset: path must not be null");
        return NULL;
    }
    if (!rt_string_cstr(path)) {
        rt_asset_error_end_load_failure();
        rt_trap("GLTF.LoadAsset: invalid path");
        return NULL;
    }
    void *asset = rt_gltf_load_impl(path, 1, NULL, 0, NULL);
    if (asset) {
        rt_asset_error_end_load_success();
    } else {
        rt_asset_error_set_if_empty(RT_ASSET_ERROR_CORRUPT, "GLTF.LoadAsset: failed to load glTF");
        rt_asset_error_end_load_failure();
    }
    return asset;
}

/// @brief Internal async path: build a glTF/GLB asset from worker-staged root bytes.
/// @details Transfers ownership of @p preloaded_data to the synchronous loader and
///          performs no additional root-file I/O.
/// @param path Runtime source path retained for diagnostics and relative dependency resolution.
/// @param[in,out] preloaded_data Owned staged root-document bytes.
/// @param preloaded_size Number of readable staged bytes.
/// @param load_assets Non-zero to resolve remaining dependencies through the asset manager.
/// @return Newly allocated glTF asset on success, or NULL on failure.
void *rt_gltf_load_preloaded(rt_string path,
                             uint8_t *preloaded_data,
                             size_t preloaded_size,
                             int load_assets) {
    return rt_gltf_load_impl(path, load_assets ? 1 : 0, preloaded_data, preloaded_size, NULL);
}

/// @brief Build a glTF asset on the main thread from a previously-staged preload bundle.
/// @details Consumes the bundle's staged dependencies (no file I/O), falling back to a normal
///          rt_gltf_load/rt_gltf_load_asset when @p bundle is NULL. Always frees @p bundle.
/// @param path Runtime source path used for diagnostics and dependency keys.
/// @param[in,out] bundle Owned preload bundle, consumed and freed; may be NULL.
/// @param load_assets Non-zero to select asset-manager fallback when @p bundle is NULL.
/// @return The loaded glTF asset handle, or NULL on failure.
void *rt_gltf_load_preloaded_bundle(rt_string path,
                                    rt_gltf_preload_bundle *bundle,
                                    int load_assets) {
    uint8_t *root_data;
    size_t root_size;
    void *asset;
    if (!bundle)
        return load_assets ? rt_gltf_load_asset(path) : rt_gltf_load(path);
    root_data = bundle->root_data;
    root_size = bundle->root_size;
    bundle->root_data = NULL;
    bundle->root_size = 0;
    asset = rt_gltf_load_impl(path, load_assets ? 1 : 0, root_data, root_size, bundle);
    rt_gltf_preload_bundle_free(bundle);
    return asset;
}

/// @brief Get the number of meshes extracted from a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe mesh count, or zero for an invalid handle/storage.
int64_t rt_gltf_mesh_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(a->meshes, a->mesh_count, a->mesh_capacity) : 0;
}

/// @brief Borrow a mesh by index from a loaded glTF asset.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based mesh index.
/// @return Borrowed Mesh3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_mesh(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t mesh_count = gltf_asset_safe_count(a->meshes, a->mesh_count, a->mesh_capacity);
    if (index < 0 || index >= mesh_count)
        return NULL;
    return rt_g3d_checked_or_null(a->meshes[index], RT_G3D_MESH3D_CLASS_ID);
}

/// @brief Get the number of materials extracted from a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe material count, or zero for an invalid handle/storage.
int64_t rt_gltf_material_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(a->materials, a->material_count, a->material_capacity) : 0;
}

/// @brief Borrow a material by index from a loaded glTF asset.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based material index.
/// @return Borrowed Material3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_material(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t material_count =
        gltf_asset_safe_count(a->materials, a->material_count, a->material_capacity);
    if (index < 0 || index >= material_count)
        return NULL;
    return rt_g3d_checked_or_null(a->materials[index], RT_G3D_MATERIAL3D_CLASS_ID);
}

/// @brief Get the number of skeletons extracted from a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe skeleton count, or zero for an invalid handle/storage.
int64_t rt_gltf_skeleton_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(a->skeletons, a->skeleton_count, a->skeleton_capacity) : 0;
}

/// @brief Borrow a skeleton by index from a loaded glTF asset.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based skeleton index.
/// @return Borrowed Skeleton3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_skeleton(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t skeleton_count =
        gltf_asset_safe_count(a->skeletons, a->skeleton_count, a->skeleton_capacity);
    if (index < 0 || index >= skeleton_count)
        return NULL;
    return rt_g3d_checked_or_null(a->skeletons[index], RT_G3D_SKELETON3D_CLASS_ID);
}

/// @brief Get the number of skeletal animation clips in a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe Animation3D count, or zero for an invalid handle/storage.
int64_t rt_gltf_animation_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(a->animations, a->animation_count, a->animation_capacity) : 0;
}

/// @brief Borrow a skeletal Animation3D clip by index.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based clip index.
/// @return Borrowed Animation3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_animation(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t animation_count =
        gltf_asset_safe_count(a->animations, a->animation_count, a->animation_capacity);
    if (index < 0 || index >= animation_count)
        return NULL;
    return rt_g3d_checked_or_null(a->animations[index], RT_G3D_ANIMATION3D_CLASS_ID);
}

/// @brief Return the number of node-animation clips in a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe NodeAnimation3D count, or zero for invalid handle/storage.
int64_t rt_gltf_node_animation_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(
                   a->node_animations, a->node_animation_count, a->node_animation_capacity)
             : 0;
}

/// @brief Borrow a node-animation clip by index.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based node-animation index.
/// @return Borrowed NodeAnimation3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_node_animation(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t node_animation_count = gltf_asset_safe_count(
        a->node_animations, a->node_animation_count, a->node_animation_capacity);
    if (index < 0 || index >= node_animation_count)
        return NULL;
    return rt_g3d_checked_or_null(a->node_animations[index], RT_G3D_NODEANIMATION3D_CLASS_ID);
}

/// @brief Return the number of cameras imported from the active scene.
/// @param obj Candidate glTF asset handle.
/// @return Safe active-scene camera count, or zero for invalid handle/storage.
int64_t rt_gltf_camera_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? gltf_asset_safe_count(a->cameras, a->camera_count, a->camera_capacity) : 0;
}

/// @brief Borrow a Camera3D imported from the active scene.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based active-scene camera index.
/// @return Borrowed Camera3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_camera(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a)
        return NULL;
    int32_t camera_count = gltf_asset_safe_count(a->cameras, a->camera_count, a->camera_capacity);
    if (index < 0 || index >= camera_count)
        return NULL;
    return rt_g3d_checked_or_null(a->cameras[index], RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Return the number of immutable scenes in a glTF asset.
/// @param obj Candidate glTF asset handle.
/// @return Safe scene count, or zero for invalid handle/storage.
int64_t rt_gltf_scene_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return gltf_asset_safe_scene_count(a);
}

/// @brief Return an imported scene name.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based immutable-scene index.
/// @return Runtime string containing the scene name, or an owned empty runtime string
///         for an invalid index.
rt_string rt_gltf_get_scene_name(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    int32_t scene_count = gltf_asset_safe_scene_count(a);
    if (!a || index < 0 || index >= scene_count || !a->scenes[index].name)
        return rt_const_cstr("");
    return rt_const_cstr(a->scenes[index].name);
}

/// @brief Borrow the root SceneNode3D for an immutable scene.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based immutable-scene index.
/// @return Borrowed SceneNode3D handle, or NULL for invalid asset/index/class.
void *rt_gltf_get_scene_root_at(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    int32_t scene_count = gltf_asset_safe_scene_count(a);
    if (!a || index < 0 || index >= scene_count)
        return NULL;
    return rt_g3d_checked_or_null(a->scenes[index].root, RT_G3D_SCENENODE3D_CLASS_ID);
}

/// @brief Return the number of cameras reachable from an immutable scene.
/// @param obj Candidate glTF asset handle.
/// @param scene_index Zero-based immutable-scene index.
/// @return Safe scene-local camera count, or zero for invalid asset/index/storage.
int64_t rt_gltf_scene_camera_count(void *obj, int64_t scene_index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    int32_t scene_count = gltf_asset_safe_scene_count(a);
    if (!a || scene_index < 0 || scene_index >= scene_count)
        return 0;
    return gltf_asset_safe_count(a->scenes[scene_index].cameras,
                                 a->scenes[scene_index].camera_count,
                                 a->scenes[scene_index].camera_capacity);
}

/// @brief Borrow a Camera3D from an immutable scene.
/// @param obj Candidate glTF asset handle.
/// @param scene_index Zero-based immutable-scene index.
/// @param index Zero-based camera index within that scene.
/// @return Borrowed Camera3D handle, or NULL for invalid asset/indices/class.
void *rt_gltf_get_scene_camera(void *obj, int64_t scene_index, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    gltf_scene_info_t *scene;
    int32_t scene_count = gltf_asset_safe_scene_count(a);
    if (!a || scene_index < 0 || scene_index >= scene_count)
        return NULL;
    scene = &a->scenes[scene_index];
    int32_t camera_count =
        gltf_asset_safe_count(scene->cameras, scene->camera_count, scene->camera_capacity);
    if (index < 0 || index >= camera_count)
        return NULL;
    return rt_g3d_checked_or_null(scene->cameras[index], RT_G3D_CAMERA3D_CLASS_ID);
}

/// @brief Return the number of nodes in the loaded active-scene tree.
/// @param obj Candidate glTF asset handle.
/// @return Stored positive node count when the active root is valid, otherwise zero.
int64_t rt_gltf_node_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a || a->node_count <= 0 || !rt_g3d_has_class(a->scene_root, RT_G3D_SCENENODE3D_CLASS_ID))
        return 0;
    return a->node_count;
}

/// @brief Borrow the active scene-root SceneNode3D of a loaded asset.
/// @param obj Candidate glTF asset handle.
/// @return Borrowed SceneNode3D handle, or NULL for invalid asset/root/class.
void *rt_gltf_get_scene_root(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    return a ? rt_g3d_checked_or_null(a->scene_root, RT_G3D_SCENENODE3D_CLASS_ID) : NULL;
}

/// @brief Return the number of imported KHR_materials_variants names.
/// @param obj Candidate glTF asset handle.
/// @return Positive variant-name count, or zero when absent/invalid.
int64_t rt_gltf_variant_count(void *obj) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a || !a->variant_names || a->variant_name_count <= 0)
        return 0;
    return a->variant_name_count;
}

/// @brief Return an imported material-variant name.
/// @param obj Candidate glTF asset handle.
/// @param index Zero-based variant index.
/// @return Runtime string containing the name, or an owned empty runtime string when invalid.
rt_string rt_gltf_get_variant_name(void *obj, int64_t index) {
    rt_gltf_asset *a = gltf_asset_checked(obj);
    if (!a || !a->variant_names || index < 0 || index >= a->variant_name_count ||
        !a->variant_names[index])
        return rt_const_cstr("");
    return rt_const_cstr(a->variant_names[index]);
}

/// @brief Probe whether graphics-enabled Draco mesh decoding accepts a payload.
/// @param data Candidate Draco bitstream.
/// @param size Number of readable bytes.
/// @return Non-zero when the payload decodes successfully; otherwise zero.
int rt_gltf_draco_decode_probe(const unsigned char *data, size_t size) {
    draco_mesh mesh;
    int unsupported = 0;
    int ok;
    if (!data || size == 0)
        return 0;
    ok = draco_decode_mesh(data, size, &mesh, &unsupported);
    if (ok)
        draco_mesh_free(&mesh);
    return ok;
}

#else
typedef int rt_graphics_disabled_tu_guard;

/// @brief Report Draco decoding as unavailable in a graphics-disabled build.
/// @param data Ignored candidate bytes.
/// @param size Ignored byte count.
/// @return Always zero.
int rt_gltf_draco_decode_probe(const unsigned char *data, size_t size) {
    (void)data;
    (void)size;
    return 0;
}
#endif /* ZANNA_ENABLE_GRAPHICS */
