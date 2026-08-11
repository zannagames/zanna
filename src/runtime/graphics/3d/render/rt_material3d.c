//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_material3d.c
// Purpose: Zanna.Graphics3D.Material3D — legacy + PBR surface appearance.
//
// Key invariants:
//   - Defaults: diffuse=(1,1,1,1), specular=(1,1,1), shininess=32,
//     alpha=1.0, emissive=(0,0,0), reflectivity=0.0, unlit=false.
//   - PBR defaults: metallic=0.0, roughness=0.5, ao=1.0,
//     emissive_intensity=1.0, normal_scale=1.0, alpha_mode=opaque.
//   - Texture/normal_map/specular_map/emissive_map/metallic_roughness_map/
//     ao_map slots retain GC-managed Pixels, TextureAsset3D, or RenderTarget3D source objects;
//     draw submission resolves TextureAsset3D to the currently resident Pixels
//     fallback and forwards native compressed blocks to capable backends.
//     env_map retains a CubeMap3D object.
//   - Alpha [0.0=invisible, 1.0=opaque] controls transparency sorting
//     in Canvas3D.End() — opaque draws first, transparent back-to-front.
//   - env_map and reflectivity are forwarded through the backend draw command.
//     GPU backends consume them for cubemap reflections; the software backend
//     may still ignore them.
//
// Ownership/Lifetime:
//   - Material3D is GC-managed and retains every accepted texture/cubemap source.
//   - Backend command snapshots borrow only frame-owned resolved payloads.
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements legacy and physically based Material3D surface state.
/// @details This translation unit owns material initialization, cloning, texture-reference
///          lifetime management, sampler metadata, and the sanitized scalar state copied into
///          3D draw commands. Texture accessors return borrowed views; material texture and
///          cubemap slots retain accepted GC-managed source objects until replacement or
///          finalization.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_textureasset3d.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MATERIAL3D_UV_TRANSFORM_ABS_MAX 1000000.0
#define MATERIAL3D_CUSTOM_PARAM_ABS_MAX 1000000.0
#define MATERIAL3D_SHININESS_MAX 8192.0
#define MATERIAL3D_EMISSIVE_INTENSITY_MAX 1000000.0
#define MATERIAL3D_NORMAL_SCALE_MAX 1000.0
#define MATERIAL3D_DEPTH_BIAS_ABS_MAX 0.05
#define MATERIAL3D_SLOPE_DEPTH_BIAS_ABS_MAX 16.0
#define MATERIAL3D_TWO_PI 6.28318530717958647692
#define MATERIAL3D_MAX_ANISOTROPY 16

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"

/// @brief Apply compatibility sampler and UV metadata to one imported texture slot.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param slot Material texture-slot index.
/// @param uv_set Source UV-set index, normalized to zero or one.
/// @param offset_u Horizontal UV translation.
/// @param offset_v Vertical UV translation.
/// @param scale_u Horizontal UV scale.
/// @param scale_v Vertical UV scale.
/// @param rotation Counter-clockwise UV rotation in radians.
/// @param wrap_s Horizontal texture wrapping mode.
/// @param wrap_t Vertical texture wrapping mode.
/// @param filter Shared minification and magnification filtering mode.
void rt_material3d_set_import_texture_slot(void *obj,
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
static int material_texture_ref_supported(void *texture_ref);
static int32_t material_sanitize_filter(int64_t value);
static int32_t material_sanitize_mip_filter(int64_t value);

/// @brief Release the GC reference at `*slot` and NULL it. NULL-safe both ways (slot ==
/// NULL or *slot == NULL). Only frees the underlying object when the release drops its
/// retain count to zero — bystander references keep the texture alive.
/// @param slot Address of an owned reference slot to release and clear.
static void material_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release an owned material texture slot only when it still stores a supported texture.
/// @details Wrong-class private state is treated as borrowed corruption and is cleared without
///          releasing; matching texture slots are owned and released normally.
/// @param slot Address of the material texture slot to validate, release, and clear.
static void material_release_texture_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!material_texture_ref_supported(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    material_release_ref(slot);
}

/// @brief Release an owned environment-map slot only when it still stores a CubeMap3D.
/// @details Wrong-class private state is cleared without releasing; matching Cubemap3D slots are
///          owned and released normally.
/// @param slot Address of the environment-map slot to validate, release, and clear.
static void material_release_env_map_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    material_release_ref(slot);
}

/// @brief GC finalizer for Material3D. Walks all eight texture slots (diffuse, normal,
/// specular, emissive, metallic-roughness, AO, lightmap, environment) and releases each, regardless
/// of which subset the material actually used. Unused slots are NULL and release is
/// NULL-safe, so legacy-Phong materials pay the same teardown cost as full PBR ones.
/// @param obj Material3D instance being finalized; NULL is ignored.
static void rt_material3d_finalize(void *obj) {
    rt_material3d *mat = (rt_material3d *)obj;
    if (!mat)
        return;
    material_release_texture_slot(&mat->texture);
    material_release_texture_slot(&mat->normal_map);
    material_release_texture_slot(&mat->specular_map);
    material_release_texture_slot(&mat->emissive_map);
    material_release_texture_slot(&mat->metallic_roughness_map);
    material_release_texture_slot(&mat->ao_map);
    material_release_texture_slot(&mat->lightmap);
    material_release_env_map_slot(&mat->env_map);
}

/// @brief Retain-then-release swap for a texture slot: if `value` differs from the
/// current occupant, retain the new one *first*, then release the old one. Retain-
/// before-release keeps the refcount above zero through the transition so re-assigning
/// a slot to its own contents (or to a shared texture whose only owner is this slot)
/// can't briefly drop the refcount and trigger a finalize.
/// @param slot Address of the owned reference slot; NULL is ignored.
/// @param value New nullable GC-managed object, retained when different from the current value.
static void material_assign_ref(void **slot, void *value) {
    if (!slot)
        return;
    if (*slot == value)
        return;
    rt_obj_retain_maybe(value);
    material_release_ref(slot);
    *slot = value;
}

/// @brief Validate @p obj as a Material3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object handle.
/// @return Borrowed Material3D pointer, or NULL when @p obj is NULL or has another class.
static rt_material3d *material_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))
               ? (rt_material3d *)obj
               : NULL;
}

/// @brief Validate that @p pixels is a live `Zanna.Graphics.Pixels` handle.
/// @param pixels Candidate runtime object handle.
/// @return Nonzero when @p pixels is a valid Pixels object; otherwise zero.
static int material_pixels_handle_valid(void *pixels) {
    return rt_pixels_checked_impl_or_null(pixels) != NULL;
}

/// @brief Validate that @p cubemap is a live `Zanna.Graphics3D.Cubemap3D` handle.
/// @param cubemap Candidate runtime cubemap handle.
/// @return Nonzero only for a complete six-face CubeMap3D; otherwise zero.
static int material_cubemap_handle_valid(void *cubemap) {
    return cubemap && rt_cubemap3d_is_complete(cubemap);
}

/// @brief Resolve a texture reference to its RGBA8 Pixels source, if it has one.
/// @details A reference may be a Pixels handle (returned as-is) or a TextureAsset3D (its decoded
///          RGBA fallback is returned). Returns NULL when there is no drawable Pixels source.
/// @param texture_ref Nullable Pixels, TextureAsset3D, or RenderTarget3D source handle.
/// @return Borrowed drawable Pixels handle, or NULL when no decoded/render-target source exists.
void *rt_material3d_resolve_texture_pixels(void *texture_ref) {
    if (!texture_ref)
        return NULL;
    if (rt_g3d_has_class(texture_ref, RT_G3D_TEXTUREASSET3D_CLASS_ID))
        return rt_textureasset3d_get_pixels(texture_ref);
    if (rt_g3d_has_class(texture_ref, RT_G3D_RENDERTARGET3D_CLASS_ID))
        return rt_rendertarget3d_material_pixels(texture_ref);
    return material_pixels_handle_valid(texture_ref) ? texture_ref : NULL;
}

/// @brief Resolve a texture reference to a TextureAsset3D with uploadable native compressed blocks.
/// @param texture_ref Nullable candidate texture source.
/// @return The asset handle if it is a TextureAsset3D with a native cache key, else NULL.
void *rt_material3d_resolve_texture_native_asset(void *texture_ref) {
    if (!texture_ref || !rt_g3d_has_class(texture_ref, RT_G3D_TEXTUREASSET3D_CLASS_ID))
        return NULL;
    return rt_textureasset3d_get_native_cache_key(texture_ref) ? texture_ref : NULL;
}

/// @brief Borrow an unresolved texture reference in stable VSCN persistence order.
/// @param obj Material3D receiver.
/// @param slot Persisted texture-slot identifier.
/// @return Borrowed original source reference, or NULL for an invalid receiver, empty slot, or
///         unsupported slot identifier.
void *rt_material3d_get_persisted_texture_ref(void *obj, int64_t slot) {
    rt_material3d *material = material_checked(obj);
    void **texture_slot = NULL;
    if (!material)
        return NULL;
    switch (slot) {
        case RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR:
            texture_slot = &material->texture;
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_NORMAL:
            texture_slot = &material->normal_map;
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR:
            texture_slot = &material->specular_map;
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE:
            texture_slot = &material->emissive_map;
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS:
            texture_slot = &material->metallic_roughness_map;
            break;
        case RT_MATERIAL3D_TEXTURE_SLOT_AO:
            texture_slot = &material->ao_map;
            break;
        case RT_MATERIAL3D_PERSISTED_TEXTURE_SLOT_LIGHTMAP:
            texture_slot = &material->lightmap;
            break;
        default:
            return NULL;
    }
    if (*texture_slot && !material_texture_ref_supported(*texture_slot))
        rt_g3d_ref_slot_clear_unowned(texture_slot);
    return *texture_slot;
}

/// @brief Borrow the currently drawable Pixels fallback for one persisted material slot.
/// @details The returned object remains owned by the material's retained source. TextureAsset3D
///          and RenderTarget3D references are resolved without discarding their original source
///          identity, so inspection does not change later rendering or VSCN serialization.
/// @param obj Material3D receiver.
/// @param slot Persisted texture-slot identifier.
/// @return Borrowed resolved Pixels handle, or NULL when the receiver or source is unavailable.
static void *material_texture_pixels_for_slot(void *obj, int64_t slot) {
    return rt_material3d_resolve_texture_pixels(rt_material3d_get_persisted_texture_ref(obj, slot));
}

/// @brief Borrow the current base-color/albedo map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed base-color Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_texture_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR);
}

/// @brief Borrow the current tangent-space normal map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed normal-map Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_normal_map_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_NORMAL);
}

/// @brief Borrow the current legacy specular map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed specular-map Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_specular_map_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR);
}

/// @brief Borrow the current emissive map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed emissive-map Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_emissive_map_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE);
}

/// @brief Borrow the current packed metallic/roughness map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed packed-map Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_metallic_roughness_map_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS);
}

/// @brief Borrow the current ambient-occlusion map as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed ambient-occlusion Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_ao_map_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_TEXTURE_SLOT_AO);
}

/// @brief Borrow the current baked lightmap as decoded Pixels, or NULL.
/// @param obj Material3D receiver.
/// @return Borrowed lightmap Pixels fallback, or NULL when unavailable.
void *rt_material3d_get_lightmap_pixels(void *obj) {
    return material_texture_pixels_for_slot(obj, RT_MATERIAL3D_PERSISTED_TEXTURE_SLOT_LIGHTMAP);
}

/// @brief Whether a texture reference has any usable source (RGBA Pixels or native blocks).
/// @param texture_ref Nullable material texture source.
/// @return Nonzero when decoded pixels or uploadable native blocks are currently available.
static int material_texture_ref_has_drawable_source(void *texture_ref) {
    return rt_material3d_resolve_texture_pixels(texture_ref) ||
           rt_material3d_resolve_texture_native_asset(texture_ref);
}

/// @brief Whether a texture slot points at a supported material texture handle type.
/// @param texture_ref Candidate material texture source.
/// @return Nonzero for live Pixels, TextureAsset3D, or RenderTarget3D handles; otherwise zero.
static int material_texture_ref_supported(void *texture_ref) {
    return texture_ref && (material_pixels_handle_valid(texture_ref) ||
                           rt_g3d_has_class(texture_ref, RT_G3D_TEXTUREASSET3D_CLASS_ID) ||
                           rt_g3d_has_class(texture_ref, RT_G3D_RENDERTARGET3D_CLASS_ID));
}

/// @brief Return whether a texture slot is currently drawable, clearing stale invalid refs.
/// @details TextureAsset3D refs are kept even when their residency window is empty; they may
///          become drawable again after streaming without rebinding the material.
/// @param slot Address of a retained material texture slot.
/// @return Nonzero when the slot currently supplies decoded pixels or native blocks.
static int material_texture_slot_has_drawable_source(void **slot) {
    if (!slot || !*slot)
        return 0;
    if (!material_texture_ref_supported(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return 0;
    }
    return material_texture_ref_has_drawable_source(*slot);
}

/// @brief Clear unsupported texture refs from all material texture slots.
/// @param mat Material whose texture references are repaired; NULL is ignored.
static void material_repair_texture_refs(rt_material3d *mat) {
    if (!mat)
        return;
    (void)material_texture_slot_has_drawable_source(&mat->texture);
    (void)material_texture_slot_has_drawable_source(&mat->normal_map);
    (void)material_texture_slot_has_drawable_source(&mat->specular_map);
    (void)material_texture_slot_has_drawable_source(&mat->emissive_map);
    (void)material_texture_slot_has_drawable_source(&mat->metallic_roughness_map);
    (void)material_texture_slot_has_drawable_source(&mat->ao_map);
    (void)material_texture_slot_has_drawable_source(&mat->lightmap);
}

/// @brief Clear an invalid env-map reference before exposing it to inspectors/renderers.
/// @param mat Material whose environment-map slot is repaired; NULL is ignored.
static void material_repair_env_map(rt_material3d *mat) {
    if (!mat || !mat->env_map || material_cubemap_handle_valid(mat->env_map))
        return;
    if (!rt_g3d_has_class(mat->env_map, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(&mat->env_map);
        return;
    }
    material_release_ref(&mat->env_map);
}

/// @brief Validate a texture reference, trapping with a descriptive message when unsupported.
/// @details NULL is accepted (clears the slot). TextureAsset3D handles are accepted even when
///          their current residency window is empty; draw submission resolves them lazily so
///          streaming can make the same material drawable again without rebinding. Non-texture
///          handles trap via @p method. Returns 1 if supported.
/// @param texture Nullable candidate texture source.
/// @param method Optional trap message used for an unsupported source.
/// @return One for NULL or a supported texture source; zero after trapping on invalid input.
static int material_texture_ref_valid_or_trap(void *texture, const char *method) {
    if (!texture)
        return 1;
    if (rt_g3d_has_class(texture, RT_G3D_TEXTUREASSET3D_CLASS_ID))
        return 1;
    if (rt_material3d_resolve_texture_pixels(texture))
        return 1;
    if (rt_material3d_resolve_texture_native_asset(texture))
        return 1;
    rt_trap(method ? method
                   : "Material3D: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
    return 0;
}

/// @brief Validate then retain-swap a texture reference into @p *slot; no-op-fails on invalid
/// input.
/// @param slot Address of the owned material texture slot.
/// @param texture Nullable replacement Pixels, TextureAsset3D, or RenderTarget3D handle.
/// @param method Optional trap message for an invalid slot or texture.
/// @return 1 on a successful assignment (including clearing with NULL), 0 if validation trapped.
static int material_assign_texture_ref_checked(void **slot, void *texture, const char *method) {
    if (!slot) {
        rt_trap(method ? method : "Material3D: internal texture slot is null");
        return 0;
    }
    if (*slot && !material_texture_ref_supported(*slot))
        rt_g3d_ref_slot_clear_unowned(slot);
    if (!material_texture_ref_valid_or_trap(texture, method))
        return 0;
    material_assign_ref(slot, texture);
    return 1;
}

/// @brief Internal hook used by rt_cubemap3d to assign an env-map cubemap on a material.
/// @details Validates the inputs (live Material3D + Cubemap3D handle), then performs the
///   retain-then-release swap on the env_map slot. NULL @p cubemap clears the slot.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param cubemap Nullable complete CubeMap3D to retain, or NULL to clear the slot.
void rt_material3d_assign_env_map_checked(void *obj, void *cubemap) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    material_repair_env_map(mat);
    if (cubemap && !material_cubemap_handle_valid(cubemap)) {
        rt_trap("Material3D.SetEnvMap: env map must be a complete CubeMap3D");
        return;
    }
    material_assign_ref(&mat->env_map, cubemap);
}

/// @brief Clamp into the closed `[0, 1]` range — the common normalized-parameter guard
/// used across PBR material setters (metallic, roughness, AO, alpha, reflectivity, etc.).
/// @param value Value to sanitize; non-finite values become zero.
/// @return Finite value clamped to the inclusive range from zero through one.
static double clamp01(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

/// @brief Clamp `value` into `[min_value, max_value]`; non-finite input maps to `min_value`.
/// @param value Value to sanitize.
/// @param min_value Inclusive lower bound and non-finite fallback.
/// @param max_value Inclusive upper bound.
/// @return @p value clamped to the requested interval.
static double clamp_range(double value, double min_value, double max_value) {
    if (!isfinite(value))
        return min_value;
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

/// @brief Clamp a signed value symmetrically around zero, using zero for NaN or infinity.
/// @details `clamp_range` deliberately uses its lower bound as the generic non-finite fallback,
///   which is appropriate for non-negative material fields. Signed shader parameters and depth
///   biases instead need a neutral zero fallback: mapping NaN to the negative magnitude limit
///   would turn malformed state into the strongest supported effect.
/// @param value Candidate signed value.
/// @param max_abs Non-negative maximum magnitude.
/// @return Finite value in `[-max_abs, max_abs]`, or zero for invalid input or bounds.
static double material_clamp_symmetric_or_zero(double value, double max_abs) {
    if (!isfinite(value) || !isfinite(max_abs) || max_abs < 0.0)
        return 0.0;
    if (value > max_abs)
        return max_abs;
    if (value < -max_abs)
        return -max_abs;
    return value;
}

/// @brief Clamp a UV-transform component into ±MATERIAL3D_UV_TRANSFORM_ABS_MAX, falling
///        back on non-finite input. Used to bound material UV scale/offset/rotation.
/// @param value Transform component to sanitize.
/// @param fallback Value returned for NaN or infinity.
/// @return Finite component bounded to the supported absolute limit.
static double material_clamp_uv_transform(double value, double fallback) {
    if (!isfinite(value))
        return fallback;
    if (value > MATERIAL3D_UV_TRANSFORM_ABS_MAX)
        return MATERIAL3D_UV_TRANSFORM_ABS_MAX;
    if (value < -MATERIAL3D_UV_TRANSFORM_ABS_MAX)
        return -MATERIAL3D_UV_TRANSFORM_ABS_MAX;
    return value;
}

static int32_t material_sanitize_anisotropy(int64_t value);

/// @brief Clamp a color component to the canonical `[0, 1]` range.
/// @details Thin wrapper over `clamp01` whose separate name documents
///   intent at call sites — a reader scanning a setter sees "this channel
///   is a color component with normalized range" rather than a generic
///   01 clamp that happens to be applied to RGBA.
/// @param value Color component to sanitize.
/// @return Finite component clamped to the inclusive range from zero through one.
static double sanitize_color(double value) {
    return clamp01(value);
}

/// @brief Sanitize an emissive color component while preserving HDR authoring values.
/// @details Emissive color is a light contribution multiplier, not a reflectance term.
///          Negative and non-finite inputs collapse to zero; large finite values are capped at
///          the same bounded scalar used for emissive intensity so shader constants remain
///          finite and predictable across CPU/GPU backends.
/// @param value HDR emissive component to sanitize.
/// @return Finite non-negative component capped at the material emissive limit.
static double sanitize_emissive_color(double value) {
    return clamp_range(value, 0.0, MATERIAL3D_EMISSIVE_INTENSITY_MAX);
}

/// @brief Initialize all six texture slots to neutral defaults so materials that
///        never set slot metadata render as they did before the per-slot machinery
///        was introduced.
/// @details Per slot: REPEAT wrap on both axes, LINEAR filter, UV set 0, and a 2×3
///          identity UV transform stored as six doubles in the layout
///          `[scale_u, shear_u, shear_v, scale_v, offset_u, offset_v]` — i.e. row-
///          major 2-row matrix with `(1,0,0,1,0,0)` = identity.
///
///          The legacy material-wide `texture_wrap_s` / `_t` / `_filter` scalars are
///          mirrored from slot 0 (base color) so older code reading those fields
///          directly sees a sensible default instead of an uninitialized value.
/// @param mat Fresh material storage to initialize; NULL is ignored.
static void material_init_texture_slots(rt_material3d *mat) {
    if (!mat)
        return;
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        mat->texture_slot_wrap_s[slot] = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
        mat->texture_slot_wrap_t[slot] = RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
        mat->texture_slot_filter[slot] = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
        mat->texture_slot_min_filter[slot] = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
        mat->texture_slot_mag_filter[slot] = RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
        mat->texture_slot_mip_filter[slot] = RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
        mat->texture_slot_anisotropy[slot] = 1;
        mat->texture_slot_uv_set[slot] = 0;
        /* texture_slot_uv_transform is a 6-element affine: indices [0..3] are the
         * 2x2 linear part (scale/rotation/shear) and [4..5] the UV translation.
         * Identity is [1,0,0,1,0,0]. */
        mat->texture_slot_uv_transform[slot][0] = 1.0;
        mat->texture_slot_uv_transform[slot][1] = 0.0;
        mat->texture_slot_uv_transform[slot][2] = 0.0;
        mat->texture_slot_uv_transform[slot][3] = 1.0;
        mat->texture_slot_uv_transform[slot][4] = 0.0;
        mat->texture_slot_uv_transform[slot][5] = 0.0;
    }
    mat->texture_wrap_s = mat->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_wrap_t = mat->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_filter = mat->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_min_filter = mat->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_mag_filter = mat->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_mip_filter = mat->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->anisotropy = mat->texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
}

/// @brief Initialize a freshly allocated material to the legacy-Phong default — white
/// diffuse (RGBA 1,1,1,1), white specular, shininess 32, zero emissive, metallic 0 /
/// roughness 0.5 (safe PBR fallback even though workflow starts legacy), opaque alpha
/// mode, no textures bound. Zeroes the custom-param scratch buffer so backends see a
/// stable initial state.
/// @param mat Freshly allocated Material3D storage; NULL is ignored.
static void material_init_defaults(rt_material3d *mat) {
    if (!mat)
        return;
    mat->vptr = NULL;
    mat->identity_serial = rt_g3d_next_identity_serial();
    mat->diffuse[0] = 1.0;
    mat->diffuse[1] = 1.0;
    mat->diffuse[2] = 1.0;
    mat->diffuse[3] = 1.0;
    mat->specular[0] = mat->specular[1] = mat->specular[2] = 1.0;
    mat->shininess = 32.0;
    mat->workflow = RT_MATERIAL3D_WORKFLOW_LEGACY;
    mat->texture = NULL;
    mat->normal_map = NULL;
    mat->specular_map = NULL;
    mat->emissive_map = NULL;
    mat->metallic_roughness_map = NULL;
    mat->ao_map = NULL;
    mat->lightmap = NULL;
    mat->emissive[0] = mat->emissive[1] = mat->emissive[2] = 0.0;
    mat->metallic = 0.0;
    mat->roughness = 0.5;
    mat->ao = 1.0;
    mat->emissive_intensity = 1.0;
    mat->normal_scale = 1.0;
    mat->alpha = 1.0;
    mat->alpha_cutoff = 0.5;
    mat->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    mat->alpha_mode_auto = 0;
    mat->alpha_mode_explicit = 0;
    mat->shadow_mode = RT_MATERIAL3D_SHADOW_MODE_AUTO;
    material_init_texture_slots(mat);
    mat->env_map = NULL;
    mat->reflectivity = 0.0;
    mat->unlit = 0;
    mat->double_sided = 0;
    mat->additive_blend = 0;
    mat->shading_model = 0;
    memset(mat->custom_params, 0, sizeof(mat->custom_params));
    mat->depth_bias = 0.0;
    mat->slope_scaled_depth_bias = 0.0;
    mat->soft_fade = 0.0;
    mat->ssr_enabled = 0;
}

/// @brief Re-sanitize copied material state that may have been imported through legacy direct
/// fields.
/// @param mat Material whose scalar, enum, sampler, transform, and reference state is normalized.
static void material_sanitize_state(rt_material3d *mat) {
    if (!mat)
        return;
    for (int i = 0; i < 3; i++) {
        mat->diffuse[i] = sanitize_color(mat->diffuse[i]);
        mat->specular[i] = sanitize_color(mat->specular[i]);
        mat->emissive[i] = sanitize_emissive_color(mat->emissive[i]);
    }
    mat->diffuse[3] = clamp01(mat->diffuse[3]);
    mat->shininess = clamp_range(mat->shininess, 0.0, MATERIAL3D_SHININESS_MAX);
    if (mat->workflow != RT_MATERIAL3D_WORKFLOW_PBR)
        mat->workflow = RT_MATERIAL3D_WORKFLOW_LEGACY;
    mat->metallic = clamp01(mat->metallic);
    mat->roughness = clamp01(mat->roughness);
    mat->ao = clamp01(mat->ao);
    mat->emissive_intensity =
        clamp_range(mat->emissive_intensity, 0.0, MATERIAL3D_EMISSIVE_INTENSITY_MAX);
    mat->normal_scale = clamp_range(mat->normal_scale, 0.0, MATERIAL3D_NORMAL_SCALE_MAX);
    mat->alpha = clamp01(mat->alpha);
    mat->alpha_cutoff = clamp01(mat->alpha_cutoff);
    if (mat->alpha_mode < RT_MATERIAL3D_ALPHA_MODE_OPAQUE ||
        mat->alpha_mode > RT_MATERIAL3D_ALPHA_MODE_BLEND)
        mat->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    mat->alpha_mode_auto = mat->alpha_mode_auto ? 1 : 0;
    mat->alpha_mode_explicit = mat->alpha_mode_explicit ? 1 : 0;
    if (mat->shadow_mode < RT_MATERIAL3D_SHADOW_MODE_AUTO ||
        mat->shadow_mode > RT_MATERIAL3D_SHADOW_MODE_CAST)
        mat->shadow_mode = RT_MATERIAL3D_SHADOW_MODE_AUTO;
    mat->reflectivity = clamp01(mat->reflectivity);
    mat->unlit = mat->unlit ? 1 : 0;
    mat->double_sided = mat->double_sided ? 1 : 0;
    mat->additive_blend = mat->additive_blend ? 1 : 0;
    if (mat->shading_model < 0 || mat->shading_model > 5)
        mat->shading_model = 0;
    mat->depth_bias =
        material_clamp_symmetric_or_zero(mat->depth_bias, MATERIAL3D_DEPTH_BIAS_ABS_MAX);
    mat->slope_scaled_depth_bias = material_clamp_symmetric_or_zero(
        mat->slope_scaled_depth_bias, MATERIAL3D_SLOPE_DEPTH_BIAS_ABS_MAX);
    mat->soft_fade = clamp_range(mat->soft_fade, 0.0, MATERIAL3D_EMISSIVE_INTENSITY_MAX);
    mat->ssr_enabled = mat->ssr_enabled ? 1 : 0;
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++) {
        int32_t wrap_s = mat->texture_slot_wrap_s[slot];
        int32_t wrap_t = mat->texture_slot_wrap_t[slot];
        mat->texture_slot_wrap_s[slot] = (wrap_s == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
                                          wrap_s == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
                                             ? wrap_s
                                             : RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
        mat->texture_slot_wrap_t[slot] = (wrap_t == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
                                          wrap_t == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
                                             ? wrap_t
                                             : RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
        mat->texture_slot_filter[slot] =
            mat->texture_slot_filter[slot] == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
        mat->texture_slot_min_filter[slot] =
            material_sanitize_filter(mat->texture_slot_min_filter[slot]);
        mat->texture_slot_mag_filter[slot] =
            material_sanitize_filter(mat->texture_slot_mag_filter[slot]);
        mat->texture_slot_mip_filter[slot] =
            mat->texture_slot_mip_filter[slot] == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST
                : (mat->texture_slot_mip_filter[slot] == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                       ? RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR
                       : RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
        mat->texture_slot_anisotropy[slot] =
            material_sanitize_anisotropy(mat->texture_slot_anisotropy[slot]);
        mat->texture_slot_uv_set[slot] = mat->texture_slot_uv_set[slot] == 1 ? 1 : 0;
        mat->texture_slot_uv_transform[slot][0] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][0], 1.0);
        mat->texture_slot_uv_transform[slot][1] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][1], 0.0);
        mat->texture_slot_uv_transform[slot][2] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][2], 0.0);
        mat->texture_slot_uv_transform[slot][3] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][3], 1.0);
        mat->texture_slot_uv_transform[slot][4] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][4], 0.0);
        mat->texture_slot_uv_transform[slot][5] =
            material_clamp_uv_transform(mat->texture_slot_uv_transform[slot][5], 0.0);
    }
    mat->texture_wrap_s = mat->texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_wrap_t = mat->texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_filter = mat->texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_min_filter = mat->texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_mag_filter = mat->texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->texture_mip_filter = mat->texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    mat->anisotropy = mat->texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR];
    for (int i = 0; i < 12; i++)
        mat->custom_params[i] = material_clamp_symmetric_or_zero(mat->custom_params[i],
                                                                 MATERIAL3D_CUSTOM_PARAM_ABS_MAX);
}

/// @brief Switch the material from legacy-Phong to PBR workflow. Called implicitly by
/// any PBR-specific setter (metallic / roughness / metallic-roughness-map / AO / etc.)
/// so a caller that touches those fields automatically opts into the PBR lighting path
/// without having to call a separate "SetPBR" entry. Legacy-only setters leave the
/// workflow alone so a pure-Phong material stays in that mode.
/// @param mat Material to promote; NULL is ignored.
static void material_promote_to_pbr(rt_material3d *mat) {
    if (!mat)
        return;
    mat->workflow = RT_MATERIAL3D_WORKFLOW_PBR;
}

/// @brief Deep-copy the material shell, retaining (not cloning) the underlying texture
/// source objects. Scalar / color / flag fields are memcpy-like copied; texture pointers go
/// through `material_assign_ref` so the clone holds its own retain count. Shared texture
/// data keeps GPU uploads cheap — callers who want independent textures clone them
/// separately. Returns NULL if `rt_material3d_new` fails to allocate.
/// @param obj Source Material3D handle.
/// @return New independently mutable Material3D retaining the source's texture objects, or NULL
///         for an invalid source or allocation failure.
static void *material_clone_like(void *obj) {
    rt_material3d *src = material_checked(obj);
    rt_material3d *dst;
    if (!src)
        return NULL;
    material_repair_texture_refs(src);
    material_repair_env_map(src);
    dst = (rt_material3d *)rt_material3d_new();
    if (!dst)
        return NULL;

    dst->diffuse[0] = src->diffuse[0];
    dst->diffuse[1] = src->diffuse[1];
    dst->diffuse[2] = src->diffuse[2];
    dst->diffuse[3] = src->diffuse[3];
    dst->specular[0] = src->specular[0];
    dst->specular[1] = src->specular[1];
    dst->specular[2] = src->specular[2];
    dst->shininess = src->shininess;
    dst->workflow = src->workflow;
    dst->emissive[0] = src->emissive[0];
    dst->emissive[1] = src->emissive[1];
    dst->emissive[2] = src->emissive[2];
    dst->metallic = src->metallic;
    dst->roughness = src->roughness;
    dst->ao = src->ao;
    dst->emissive_intensity = src->emissive_intensity;
    dst->normal_scale = src->normal_scale;
    dst->alpha = src->alpha;
    dst->alpha_cutoff = src->alpha_cutoff;
    dst->alpha_mode = src->alpha_mode;
    dst->alpha_mode_auto = src->alpha_mode_auto;
    dst->alpha_mode_explicit = src->alpha_mode_explicit;
    dst->shadow_mode = src->shadow_mode;
    dst->texture_wrap_s = src->texture_wrap_s;
    dst->texture_wrap_t = src->texture_wrap_t;
    dst->texture_filter = src->texture_filter;
    dst->texture_min_filter = src->texture_min_filter;
    dst->texture_mag_filter = src->texture_mag_filter;
    dst->texture_mip_filter = src->texture_mip_filter;
    dst->anisotropy = src->anisotropy;
    memcpy(dst->texture_slot_wrap_s, src->texture_slot_wrap_s, sizeof(dst->texture_slot_wrap_s));
    memcpy(dst->texture_slot_wrap_t, src->texture_slot_wrap_t, sizeof(dst->texture_slot_wrap_t));
    memcpy(dst->texture_slot_filter, src->texture_slot_filter, sizeof(dst->texture_slot_filter));
    memcpy(dst->texture_slot_min_filter,
           src->texture_slot_min_filter,
           sizeof(dst->texture_slot_min_filter));
    memcpy(dst->texture_slot_mag_filter,
           src->texture_slot_mag_filter,
           sizeof(dst->texture_slot_mag_filter));
    memcpy(dst->texture_slot_mip_filter,
           src->texture_slot_mip_filter,
           sizeof(dst->texture_slot_mip_filter));
    memcpy(dst->texture_slot_anisotropy,
           src->texture_slot_anisotropy,
           sizeof(dst->texture_slot_anisotropy));
    memcpy(dst->texture_slot_uv_set, src->texture_slot_uv_set, sizeof(dst->texture_slot_uv_set));
    memcpy(dst->texture_slot_uv_transform,
           src->texture_slot_uv_transform,
           sizeof(dst->texture_slot_uv_transform));
    dst->reflectivity = src->reflectivity;
    dst->unlit = src->unlit;
    dst->double_sided = src->double_sided;
    dst->additive_blend = src->additive_blend;
    dst->shading_model = src->shading_model;
    memcpy(dst->custom_params, src->custom_params, sizeof(dst->custom_params));
    dst->depth_bias = src->depth_bias;
    dst->slope_scaled_depth_bias = src->slope_scaled_depth_bias;
    dst->soft_fade = src->soft_fade;
    dst->ssr_enabled = src->ssr_enabled;
    material_sanitize_state(dst);

    material_assign_ref(&dst->texture, src->texture);
    material_assign_ref(&dst->normal_map, src->normal_map);
    material_assign_ref(&dst->specular_map, src->specular_map);
    material_assign_ref(&dst->emissive_map, src->emissive_map);
    material_assign_ref(&dst->metallic_roughness_map, src->metallic_roughness_map);
    material_assign_ref(&dst->ao_map, src->ao_map);
    material_assign_ref(&dst->lightmap, src->lightmap);
    material_assign_ref(&dst->env_map, src->env_map);
    return dst;
}

/// @brief Create a new material with default white diffuse, shininess 32, fully opaque.
/// @details Materials define how light interacts with a mesh surface. The default
///          state is a white, opaque, lit Phong material with no textures. Any
///          assigned texture pointers/assets (diffuse, normal, specular, emissive, env)
///          are retained so they stay alive with the material.
/// @return Opaque material handle, or NULL on allocation failure.
void *rt_material3d_new(void) {
    rt_material3d *mat =
        (rt_material3d *)rt_obj_new_i64(RT_G3D_MATERIAL3D_CLASS_ID, (int64_t)sizeof(rt_material3d));
    if (!mat) {
        rt_trap("Material3D.New: memory allocation failed");
        return NULL;
    }
    material_init_defaults(mat);
    rt_obj_set_finalizer(mat, rt_material3d_finalize);
    return mat;
}

/// @brief Create a material with a solid diffuse color (no texture).
/// @param r Red diffuse component [0.0–1.0].
/// @param g Green diffuse component [0.0–1.0].
/// @param b Blue diffuse component [0.0–1.0].
/// @return Opaque material handle, or NULL on failure.
void *rt_material3d_new_color(double r, double g, double b) {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    if (!mat)
        return NULL;
    mat->diffuse[0] = sanitize_color(r);
    mat->diffuse[1] = sanitize_color(g);
    mat->diffuse[2] = sanitize_color(b);
    mat->diffuse[3] = 1.0;
    return mat;
}

/// @brief Create a material with a diffuse texture map.
/// @details Pixels, TextureAsset3D, or RenderTarget3D is retained through the material's source
/// slot.
/// @param pixels Pixels, TextureAsset3D, or RenderTarget3D handle for the diffuse texture.
/// @return Opaque material handle, or NULL on failure.
void *rt_material3d_new_textured(void *pixels) {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new();
    if (!mat)
        return NULL;
    if (!material_assign_texture_ref_checked(
            &mat->texture,
            pixels,
            "Material3D.SetTexture: texture must be Pixels, TextureAsset3D, or RenderTarget3D")) {
        if (rt_obj_release_check0(mat))
            rt_obj_free(mat);
        return NULL;
    }
    return mat;
}

/// @brief Create a metallic-roughness PBR material with a solid base color.
/// @param r Red base-color component, clamped to the inclusive normalized range.
/// @param g Green base-color component, clamped to the inclusive normalized range.
/// @param b Blue base-color component, clamped to the inclusive normalized range.
/// @return New opaque PBR material handle, or NULL on allocation failure.
void *rt_material3d_new_pbr(double r, double g, double b) {
    rt_material3d *mat = (rt_material3d *)rt_material3d_new_color(r, g, b);
    if (!mat)
        return NULL;
    material_promote_to_pbr(mat);
    mat->metallic = 0.0;
    mat->roughness = 0.5;
    mat->ao = 1.0;
    mat->emissive_intensity = 1.0;
    mat->normal_scale = 1.0;
    mat->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    mat->shadow_mode = RT_MATERIAL3D_SHADOW_MODE_AUTO;
    return mat;
}

/// @brief Deep-copy a material, including all texture references (which are retained).
/// @param obj Source Material3D handle.
/// @return Independent material shell sharing retained texture sources, or NULL on invalid input
///         or allocation failure.
void *rt_material3d_clone(void *obj) {
    return material_clone_like(obj);
}

/// @brief Alias for `_clone` — produce an independent material instance for per-renderable
/// tweaks without affecting the source. Same texture-retention semantics.
/// @param obj Source Material3D handle.
/// @return Independent material instance sharing retained texture sources, or NULL on failure.
void *rt_material3d_make_instance(void *obj) {
    return material_clone_like(obj);
}

/// @brief Set the diffuse color of a material (overrides existing color, keeps texture).
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param r Red diffuse component, clamped to the normalized range.
/// @param g Green diffuse component, clamped to the normalized range.
/// @param b Blue diffuse component, clamped to the normalized range.
void rt_material3d_set_color(void *obj, double r, double g, double b) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->diffuse[0] = sanitize_color(r);
    mat->diffuse[1] = sanitize_color(g);
    mat->diffuse[2] = sanitize_color(b);
}

/// @brief Read the material diffuse/base color as a fresh Vec3.
/// @param obj Material3D receiver.
/// @return Newly allocated Vec3 containing sanitized RGB channels; invalid materials yield white.
void *rt_material3d_get_color(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return rt_vec3_new(1.0, 1.0, 1.0);
    return rt_vec3_new(sanitize_color(mat->diffuse[0]),
                       sanitize_color(mat->diffuse[1]),
                       sanitize_color(mat->diffuse[2]));
}

/// @brief Set or replace the diffuse texture map.
/// @details The material retain-swaps a supported source. Passing NULL clears the slot; a non-null
///          unsupported runtime object traps and leaves the existing source unchanged.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source.
void rt_material3d_set_texture(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    (void)material_assign_texture_ref_checked(
        &mat->texture,
        pixels,
        "Material3D.SetTexture: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
}

/// @brief Bind a RenderTarget3D's live contents as the albedo texture.
/// @details The material samples the target's last completed frame; the mirror
///   refreshes automatically whenever a frame rendered into the target ends, so
///   security-monitor/scope setups need no per-frame readback or rebinding.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param target RenderTarget3D source to retain; NULL or a wrong-class object traps.
void rt_material3d_set_albedo_render_target(void *obj, void *target) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (!target || !rt_g3d_has_class(target, RT_G3D_RENDERTARGET3D_CLASS_ID)) {
        rt_trap("Material3D.SetAlbedoRenderTarget: target must be a RenderTarget3D");
        return;
    }
    (void)material_assign_texture_ref_checked(
        &mat->texture, target, "Material3D.SetAlbedoRenderTarget: target must be a RenderTarget3D");
}

/// @brief Detach a render-target albedo binding (leaves the material textureless).
/// @details A Pixels or TextureAsset3D albedo source is deliberately left unchanged.
/// @param obj Material3D receiver; invalid handles are ignored.
void rt_material3d_clear_albedo_render_target(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (mat->texture && rt_g3d_has_class(mat->texture, RT_G3D_RENDERTARGET3D_CLASS_ID))
        material_assign_ref(&mat->texture, NULL);
}

/// @brief Bind a RenderTarget3D's live contents as the emissive map (glowing monitors).
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param target RenderTarget3D source to retain; NULL or a wrong-class object traps.
void rt_material3d_set_emissive_render_target(void *obj, void *target) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (!target || !rt_g3d_has_class(target, RT_G3D_RENDERTARGET3D_CLASS_ID)) {
        rt_trap("Material3D.SetEmissiveRenderTarget: target must be a RenderTarget3D");
        return;
    }
    (void)material_assign_texture_ref_checked(
        &mat->emissive_map,
        target,
        "Material3D.SetEmissiveRenderTarget: target must be a RenderTarget3D");
}

/// @brief Coerce an incoming texture-wrap mode to a known-valid enum value.
/// @details Accepts CLAMP_TO_EDGE and MIRRORED_REPEAT; everything else (including
///   unknown future modes or out-of-range values from importers) falls back to
///   REPEAT, which is the safe default and matches the texture-slot init in
///   material_init_texture_slots.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_WRAP_*` value.
/// @return A valid wrapping constant, defaulting to repeat.
static int32_t material_sanitize_wrap(int64_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE ||
        value == RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT)
        return (int32_t)value;
    return RT_MATERIAL3D_TEXTURE_WRAP_REPEAT;
}

/// @brief Coerce an incoming texture-filter mode to LINEAR or NEAREST.
/// @details Only NEAREST is accepted as an explicit override; everything else
///   maps to LINEAR so unknown future filter modes degrade gracefully rather
///   than producing undefined backend behaviour.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_FILTER_*` value.
/// @return Nearest for the explicit nearest constant; otherwise linear.
static int32_t material_sanitize_filter(int64_t value) {
    return value == RT_MATERIAL3D_TEXTURE_FILTER_NEAREST ? RT_MATERIAL3D_TEXTURE_FILTER_NEAREST
                                                         : RT_MATERIAL3D_TEXTURE_FILTER_LINEAR;
}

/// @brief Coerce an imported mip-selection filter to none, nearest, or linear.
/// @param value Candidate `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` value.
/// @return A valid mip-filter constant; unknown values disable mip sampling.
static int32_t material_sanitize_mip_filter(int64_t value) {
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST;
    if (value == RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR)
        return RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR;
    return RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE;
}

/// @brief Clamp requested texture anisotropy into the script-facing [1,16] range.
/// @param value Requested anisotropy level.
/// @return Integer anisotropy level clamped from one through the backend-independent maximum.
static int32_t material_sanitize_anisotropy(int64_t value) {
    if (value < 1)
        return 1;
    if (value > MATERIAL3D_MAX_ANISOTROPY)
        return MATERIAL3D_MAX_ANISOTROPY;
    return (int32_t)value;
}

/// @brief Validate a texture-slot index and narrow it to int32_t.
/// @details Returns -1 for any out-of-range value so that callers can perform a
///   single negative-check before indexing the per-slot arrays, keeping bounds
///   enforcement in one place rather than scattered across every importer hook.
/// @param slot Candidate texture-slot index.
/// @return Narrowed valid index, or negative one when out of bounds.
static int32_t material_sanitize_texture_slot(int64_t slot) {
    if (slot < 0 || slot >= RT_MATERIAL3D_TEXTURE_SLOT_COUNT)
        return -1;
    return (int32_t)slot;
}

/// @brief Internal importer hook: store glTF-style sampler state on the material.
/// @details Applies the sampler to the base-color slot with identity UV transform and no mip
///          filter, preserving the behavior of importers using the legacy material-wide fields.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param wrap_s Horizontal texture wrapping mode.
/// @param wrap_t Vertical texture wrapping mode.
/// @param filter Shared minification and magnification filter.
void rt_material3d_set_import_sampler(void *obj, int64_t wrap_s, int64_t wrap_t, int64_t filter) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    rt_material3d_set_import_texture_slot(obj,
                                          RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR,
                                          0,
                                          0.0,
                                          0.0,
                                          1.0,
                                          1.0,
                                          0.0,
                                          wrap_s,
                                          wrap_t,
                                          filter);
}

/**
 * @brief Store independent imported sampler axes and UV metadata for one material texture slot.
 * @details This is the lossless glTF importer boundary. Minification and magnification use the
 * existing nearest/linear constants; mip selection additionally supports none. The legacy
 * material-wide filter mirrors magnification for compatibility, while all three axes are retained
 * in the per-slot and base-color mirrors consumed by draw snapshots.
 * @param obj Material3D receiver.
 * @param slot Texture slot in `[0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT)`.
 * @param uv_set Imported UV set (only 0/1 are representable after prior glTF validation).
 * @param offset_u Horizontal UV translation.
 * @param offset_v Vertical UV translation.
 * @param scale_u Horizontal UV scale.
 * @param scale_v Vertical UV scale.
 * @param rotation UV rotation in radians.
 * @param wrap_s Horizontal wrap mode.
 * @param wrap_t Vertical wrap mode.
 * @param min_filter Independent minification filter.
 * @param mag_filter Independent magnification filter.
 * @param mip_filter Independent mip-selection filter.
 */
void rt_material3d_set_import_texture_slot_sampler_axes(void *obj,
                                                        int64_t slot,
                                                        int64_t uv_set,
                                                        double offset_u,
                                                        double offset_v,
                                                        double scale_u,
                                                        double scale_v,
                                                        double rotation,
                                                        int64_t wrap_s,
                                                        int64_t wrap_t,
                                                        int64_t min_filter,
                                                        int64_t mag_filter,
                                                        int64_t mip_filter) {
    rt_material3d *mat = material_checked(obj);
    int32_t slot_index = material_sanitize_texture_slot(slot);
    double c;
    double s;
    if (!mat || slot_index < 0)
        return;
    offset_u = material_clamp_uv_transform(offset_u, 0.0);
    offset_v = material_clamp_uv_transform(offset_v, 0.0);
    scale_u = material_clamp_uv_transform(scale_u, 1.0);
    scale_v = material_clamp_uv_transform(scale_v, 1.0);
    if (!isfinite(rotation))
        rotation = 0.0;
    else
        rotation = fmod(rotation, MATERIAL3D_TWO_PI);
    c = cos(rotation);
    s = sin(rotation);
    mat->texture_slot_wrap_s[slot_index] = material_sanitize_wrap(wrap_s);
    mat->texture_slot_wrap_t[slot_index] = material_sanitize_wrap(wrap_t);
    mat->texture_slot_min_filter[slot_index] = material_sanitize_filter(min_filter);
    mat->texture_slot_mag_filter[slot_index] = material_sanitize_filter(mag_filter);
    mat->texture_slot_mip_filter[slot_index] = material_sanitize_mip_filter(mip_filter);
    mat->texture_slot_filter[slot_index] = mat->texture_slot_mag_filter[slot_index];
    mat->texture_slot_uv_set[slot_index] = uv_set == 1 ? 1 : 0;
    mat->texture_slot_uv_transform[slot_index][0] = material_clamp_uv_transform(scale_u * c, 1.0);
    mat->texture_slot_uv_transform[slot_index][1] = material_clamp_uv_transform(-scale_v * s, 0.0);
    mat->texture_slot_uv_transform[slot_index][2] = material_clamp_uv_transform(scale_u * s, 0.0);
    mat->texture_slot_uv_transform[slot_index][3] = material_clamp_uv_transform(scale_v * c, 1.0);
    mat->texture_slot_uv_transform[slot_index][4] = offset_u;
    mat->texture_slot_uv_transform[slot_index][5] = offset_v;
    if (slot_index == RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR) {
        mat->texture_wrap_s = mat->texture_slot_wrap_s[slot_index];
        mat->texture_wrap_t = mat->texture_slot_wrap_t[slot_index];
        mat->texture_filter = mat->texture_slot_filter[slot_index];
        mat->texture_min_filter = mat->texture_slot_min_filter[slot_index];
        mat->texture_mag_filter = mat->texture_slot_mag_filter[slot_index];
        mat->texture_mip_filter = mat->texture_slot_mip_filter[slot_index];
        mat->anisotropy = mat->texture_slot_anisotropy[slot_index];
    }
}

/**
 * @brief Compatibility importer hook that applies one filter to minification/magnification.
 * @details Existing callers retain their non-mipmapped behavior. Lossless glTF import calls
 * `rt_material3d_set_import_texture_slot_sampler_axes` instead.
 * @param obj Material3D receiver.
 * @param slot Texture slot index.
 * @param uv_set UV set index.
 * @param offset_u Horizontal UV translation.
 * @param offset_v Vertical UV translation.
 * @param scale_u Horizontal UV scale.
 * @param scale_v Vertical UV scale.
 * @param rotation UV rotation in radians.
 * @param wrap_s Horizontal wrap mode.
 * @param wrap_t Vertical wrap mode.
 * @param filter Shared nearest/linear minification and magnification filter.
 */
void rt_material3d_set_import_texture_slot(void *obj,
                                           int64_t slot,
                                           int64_t uv_set,
                                           double offset_u,
                                           double offset_v,
                                           double scale_u,
                                           double scale_v,
                                           double rotation,
                                           int64_t wrap_s,
                                           int64_t wrap_t,
                                           int64_t filter) {
    rt_material3d_set_import_texture_slot_sampler_axes(obj,
                                                       slot,
                                                       uv_set,
                                                       offset_u,
                                                       offset_v,
                                                       scale_u,
                                                       scale_v,
                                                       rotation,
                                                       wrap_s,
                                                       wrap_t,
                                                       filter,
                                                       filter,
                                                       RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE);
}

/// @brief Alias for `_set_texture` using PBR-style "albedo" terminology.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source.
void rt_material3d_set_albedo_map(void *obj, void *pixels) {
    rt_material3d_set_texture(obj, pixels);
}

/// @brief Set the Phong specular shininess exponent (higher = tighter highlight).
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param s Exponent clamped to the supported range; non-finite input becomes zero.
void rt_material3d_set_shininess(void *obj, double s) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->shininess = clamp_range(s, 0.0, MATERIAL3D_SHININESS_MAX);
}

/// @brief Enable or disable unlit mode (ignores scene lighting, renders flat color/texture).
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param unlit Zero to restore lighting, or any nonzero value to enable unlit rendering.
void rt_material3d_set_unlit(void *obj, int8_t unlit) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->unlit = unlit ? 1 : 0;
}

/// @brief Opt this material into screen-space reflections (Plan 10).
/// @details SSR composites scene reflections over the env-map term for surfaces
///          flagged here (water, glossy floors) on backends that support it;
///          the flag is ignored (env-map only) elsewhere.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param enabled Zero to disable SSR, or nonzero to enable it.
void rt_material3d_set_ssr_enabled(void *obj, int8_t enabled) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->ssr_enabled = enabled ? 1 : 0;
}

/// @brief Return whether screen-space reflections are enabled for this material.
/// @param obj Material3D receiver.
/// @return One when enabled, or zero for disabled and invalid materials.
int8_t rt_material3d_get_ssr_enabled(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0;
    return mat->ssr_enabled ? 1 : 0;
}

/// @brief Return whether unlit mode is enabled.
/// @param obj Material3D receiver.
/// @return One when unlit rendering is enabled, or zero for disabled and invalid materials.
int8_t rt_material3d_get_unlit(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0;
    return mat->unlit ? 1 : 0;
}

/// @brief Select the renderer shading model (0=Phong, 1=Toon, 3=Unlit, 4=Fresnel, 5=Emissive).
/// @details Model values outside [0,5] are clamped to 0. Model 2 is kept for
///          Game3D's PBR enum and promotes the material to the PBR workflow.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param model Shading-model identifier; unsupported values select Phong.
void rt_material3d_set_shading_model(void *obj, int64_t model) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (model < 0 || model > 5)
        model = 0;
    mat->shading_model = (int32_t)model;
    if (model == 2)
        material_promote_to_pbr(mat);
}

/// @brief Read the current shading model.
/// @param obj Material3D receiver.
/// @return Sanitized model identifier from zero through five; invalid state returns Phong zero.
int64_t rt_material3d_get_shading_model(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0;
    if (mat->shading_model < 0 || mat->shading_model > 5)
        return 0;
    return mat->shading_model;
}

/// @brief Set a custom shader parameter by index (0–11) for advanced shading effects.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param index Custom parameter index from zero through eleven; invalid indices are ignored.
/// @param value Finite parameter value, clamped to the backend-safe absolute limit.
void rt_material3d_set_custom_param(void *obj, int64_t index, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat || index < 0 || index >= 12)
        return;
    mat->custom_params[index] =
        material_clamp_symmetric_or_zero(value, MATERIAL3D_CUSTOM_PARAM_ABS_MAX);
}

/// @brief Set the transparency level (0.0 = invisible, 1.0 = fully opaque).
/// @details Values are clamped to [0,1]. Setting alpha below 1.0 promotes an
///          opaque material to BLEND so the visible result matches the scalar.
///          Call SetAlphaMode afterwards when explicit MASK/OPAQUE behavior is needed.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param alpha Opacity value, clamped to the inclusive normalized range.
void rt_material3d_set_alpha(void *obj, double alpha) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->alpha = clamp01(alpha);
    if (mat->alpha < 0.999 && mat->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_OPAQUE) {
        mat->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_BLEND;
        mat->alpha_mode_auto = 1;
    } else if (mat->alpha >= 0.999 && mat->alpha_mode == RT_MATERIAL3D_ALPHA_MODE_BLEND &&
               mat->alpha_mode_auto) {
        mat->alpha_mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
        mat->alpha_mode_auto = 0;
    }
}

/// @brief Get the current transparency level of the material.
/// @param obj Material3D receiver.
/// @return Sanitized opacity in the normalized range; invalid materials return fully opaque.
double rt_material3d_get_alpha(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 1.0;
    return clamp01(mat->alpha);
}

/// @brief Set PBR metallic factor [0, 1]. 0 = dielectric (plastic, wood), 1 = pure metal. Auto-
/// promotes the material to PBR shading model on first use.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param value Metallic factor, clamped to the inclusive normalized range.
void rt_material3d_set_metallic(void *obj, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    material_promote_to_pbr(mat);
    mat->metallic = clamp01(value);
}

/// @brief Read the metallic factor (default 0).
/// @param obj Material3D receiver.
/// @return Sanitized metallic factor, or zero for an invalid material.
double rt_material3d_get_metallic(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0.0;
    return clamp01(mat->metallic);
}

/// @brief Set PBR roughness [0, 1]. 0 = mirror smooth, 1 = fully diffuse. Auto-promotes to PBR.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param value Roughness factor, clamped to the inclusive normalized range.
void rt_material3d_set_roughness(void *obj, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    material_promote_to_pbr(mat);
    mat->roughness = clamp01(value);
}

/// @brief Read the roughness factor (default 0.5).
/// @param obj Material3D receiver.
/// @return Sanitized roughness factor, or the 0.5 default for an invalid material.
double rt_material3d_get_roughness(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0.5;
    return clamp01(mat->roughness);
}

/// @brief Set ambient-occlusion factor [0, 1]. 1 = no occlusion, 0 = fully shadowed in cavities.
/// Multiplied into the indirect lighting. Auto-promotes to PBR.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param value Ambient-occlusion factor, clamped to the inclusive normalized range.
void rt_material3d_set_ao(void *obj, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    material_promote_to_pbr(mat);
    mat->ao = clamp01(value);
}

/// @brief Read the AO factor (default 1.0 = no occlusion).
/// @param obj Material3D receiver.
/// @return Sanitized ambient-occlusion factor, or one for an invalid material.
double rt_material3d_get_ao(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 1.0;
    return clamp01(mat->ao);
}

/// @brief Multiply the emissive output by `value` (≥ 0). Useful for HDR/bloom: values > 1 push
/// emissive surfaces past clamping range.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param value Non-negative HDR multiplier, finite-clamped to the emissive scalar limit.
void rt_material3d_set_emissive_intensity(void *obj, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->emissive_intensity = clamp_range(value, 0.0, MATERIAL3D_EMISSIVE_INTENSITY_MAX);
}

/// @brief Read the emissive intensity multiplier (default 1.0).
/// @param obj Material3D receiver.
/// @return Sanitized non-negative emissive multiplier, or one for an invalid material.
double rt_material3d_get_emissive_intensity(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 1.0;
    return clamp_range(mat->emissive_intensity, 0.0, MATERIAL3D_EMISSIVE_INTENSITY_MAX);
}

/// @brief Assign a normal map texture for per-pixel bump/detail lighting.
/// @details The material retains the supported source; NULL clears the slot and unsupported
///          non-null runtime objects trap without replacing the current source.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source.
void rt_material3d_set_normal_map(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    (void)material_assign_texture_ref_checked(
        &mat->normal_map,
        pixels,
        "Material3D.SetNormalMap: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
}

/// @brief Return whether the base-color/albedo texture slot is populated.
/// @details Stale wrong-class references are cleared. A streaming asset with no current residency
///          remains retained but reports false until decoded pixels or native blocks are present.
/// @param obj Material3D receiver.
/// @return One when the slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_texture(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->texture)) ? 1 : 0;
}

/// @brief Return whether the normal-map slot is populated.
/// @param obj Material3D receiver.
/// @return One when the slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_normal_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->normal_map)) ? 1 : 0;
}

/// @brief Assign a glTF-style metallic-roughness texture (B = metallic, G = roughness, R/A
/// unused). Auto-promotes the material to PBR shading.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source retained by the slot.
void rt_material3d_set_metallic_roughness_map(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (material_assign_texture_ref_checked(&mat->metallic_roughness_map,
                                            pixels,
                                            "Material3D.SetMetallicRoughnessMap: texture must be "
                                            "Pixels, TextureAsset3D, or RenderTarget3D"))
        material_promote_to_pbr(mat);
}

/// @brief Return whether the metallic-roughness texture slot is populated.
/// @param obj Material3D receiver.
/// @return One when the packed slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_metallic_roughness_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->metallic_roughness_map)) ? 1 : 0;
}

/// @brief Assign an ambient-occlusion texture (R channel). Multiplied into indirect lighting.
/// Auto-promotes the material to PBR shading.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source retained by the slot.
void rt_material3d_set_ao_map(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (material_assign_texture_ref_checked(
            &mat->ao_map,
            pixels,
            "Material3D.SetAOMap: texture must be Pixels, TextureAsset3D, or RenderTarget3D"))
        material_promote_to_pbr(mat);
}

/// @brief Assign a baked lightmap atlas sampled with TEXCOORD_1. When present, the
///   draw's flat-ambient term is replaced by lightmap radiance x albedo (values are
///   stored with 2x headroom by the baker: texel 255 = radiance 2.0). Pass NULL to clear.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D lightmap source.
void rt_material3d_set_lightmap(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    (void)material_assign_texture_ref_checked(
        &mat->lightmap,
        pixels,
        "Material3D.SetLightmap: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
}

/// @brief Return whether the baked lightmap slot is populated.
/// @param obj Material3D receiver.
/// @return One when the lightmap slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_lightmap(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->lightmap)) ? 1 : 0;
}

/// @brief Return whether the ambient-occlusion texture slot is populated.
/// @param obj Material3D receiver.
/// @return One when the ambient-occlusion slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_ao_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->ao_map)) ? 1 : 0;
}

/// @brief Assign a specular map texture to control per-pixel highlight intensity.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source retained by the slot.
void rt_material3d_set_specular_map(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    (void)material_assign_texture_ref_checked(
        &mat->specular_map,
        pixels,
        "Material3D.SetSpecularMap: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
}

/// @brief Return whether the specular texture slot is populated.
/// @param obj Material3D receiver.
/// @return One when the specular slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_specular_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->specular_map)) ? 1 : 0;
}

/// @brief Assign an emissive map texture for self-illuminated surface regions.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param pixels Nullable Pixels, TextureAsset3D, or RenderTarget3D source retained by the slot.
void rt_material3d_set_emissive_map(void *obj, void *pixels) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    (void)material_assign_texture_ref_checked(
        &mat->emissive_map,
        pixels,
        "Material3D.SetEmissiveMap: texture must be Pixels, TextureAsset3D, or RenderTarget3D");
}

/// @brief Return whether the emissive texture slot is populated.
/// @param obj Material3D receiver.
/// @return One when the emissive slot has a drawable source; otherwise zero.
int8_t rt_material3d_get_has_emissive_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    return (mat && material_texture_slot_has_drawable_source(&mat->emissive_map)) ? 1 : 0;
}

/// @brief Return whether an environment cubemap is populated.
/// @details Incomplete cubemaps are released and wrong-class stale state is cleared before the
///          result is reported.
/// @param obj Material3D receiver.
/// @return One for a retained complete CubeMap3D, or zero otherwise.
int8_t rt_material3d_get_has_env_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    material_repair_env_map(mat);
    return (mat && mat->env_map) ? 1 : 0;
}

/// @brief Set the emissive (self-illumination) color, independent of scene lights.
/// @details Emissive channels intentionally accept HDR values above 1.0. Use
///          `Material3D.SetEmissiveIntensity` as an additional scalar multiplier; both are
///          sanitized to finite, non-negative ranges before rendering.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param r Red emissive component.
/// @param g Green emissive component.
/// @param b Blue emissive component.
void rt_material3d_set_emissive_color(void *obj, double r, double g, double b) {
    rt_material3d *m = material_checked(obj);
    if (!m)
        return;
    m->emissive[0] = sanitize_emissive_color(r);
    m->emissive[1] = sanitize_emissive_color(g);
    m->emissive[2] = sanitize_emissive_color(b);
}

/// @brief Set normal-map intensity (≥ 0). 1.0 = no scaling, > 1 amplifies bumps, < 1 flattens.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param value Non-negative scale, finite-clamped to the supported normal-map limit.
void rt_material3d_set_normal_scale(void *obj, double value) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->normal_scale = clamp_range(value, 0.0, MATERIAL3D_NORMAL_SCALE_MAX);
}

/// @brief Read the normal-map scale factor (default 1.0).
/// @param obj Material3D receiver.
/// @return Sanitized normal-map scale, or one for an invalid material.
double rt_material3d_get_normal_scale(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 1.0;
    return clamp_range(mat->normal_scale, 0.0, MATERIAL3D_NORMAL_SCALE_MAX);
}

/// @brief Set material texture anisotropy (1 disables anisotropic filtering; clamps to 16).
/// @details The sanitized value is copied to every texture slot and the legacy material-wide
///          mirror so all samplers use a consistent setting.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param anisotropy Requested anisotropy level.
void rt_material3d_set_anisotropy(void *obj, int64_t anisotropy) {
    rt_material3d *mat = material_checked(obj);
    int32_t sanitized;
    if (!mat)
        return;
    sanitized = material_sanitize_anisotropy(anisotropy);
    mat->anisotropy = sanitized;
    for (int slot = 0; slot < RT_MATERIAL3D_TEXTURE_SLOT_COUNT; slot++)
        mat->texture_slot_anisotropy[slot] = sanitized;
}

/// @brief Read material texture anisotropy, clamped to the public [1,16] range.
/// @param obj Material3D receiver.
/// @return Sanitized anisotropy level, or one for an invalid material.
int64_t rt_material3d_get_anisotropy(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 1;
    return material_sanitize_anisotropy(mat->anisotropy);
}

/// @brief Set the alpha-handling mode (RT_MATERIAL3D_ALPHA_MODE_OPAQUE / MASK / BLEND).
/// OPAQUE: ignore alpha. MASK: discard fragments below cutoff. BLEND: depth-sorted transparency.
/// Out-of-range values are clamped to OPAQUE.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param mode Requested alpha-mode constant; invalid values select opaque.
void rt_material3d_set_alpha_mode(void *obj, int64_t mode) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (mode < RT_MATERIAL3D_ALPHA_MODE_OPAQUE || mode > RT_MATERIAL3D_ALPHA_MODE_BLEND)
        mode = RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    mat->alpha_mode = (int32_t)mode;
    mat->alpha_mode_auto = 0;
    mat->alpha_mode_explicit = 1;
}

/// @brief Read the alpha mode (default OPAQUE).
/// @param obj Material3D receiver.
/// @return Valid alpha-mode constant, defaulting to opaque for invalid material or state.
int64_t rt_material3d_get_alpha_mode(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    if (mat->alpha_mode < RT_MATERIAL3D_ALPHA_MODE_OPAQUE ||
        mat->alpha_mode > RT_MATERIAL3D_ALPHA_MODE_BLEND)
        return RT_MATERIAL3D_ALPHA_MODE_OPAQUE;
    return mat->alpha_mode;
}

/// @brief Set the material's shadow casting mode.
/// @details AUTO preserves legacy behavior: opaque and alpha-masked draws cast shadows, while
///          alpha-blended draws are skipped. NONE disables shadow casting for this material.
///          CAST forces the shadow pass to include the material even when it is alpha-blended,
///          useful for glass, translucent cloth, and other authored cases where a shadow is
///          visually expected despite the main pass requiring blending.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param mode Requested shadow-mode constant; invalid values select automatic behavior.
void rt_material3d_set_shadow_mode(void *obj, int64_t mode) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    if (mode < RT_MATERIAL3D_SHADOW_MODE_AUTO || mode > RT_MATERIAL3D_SHADOW_MODE_CAST)
        mode = RT_MATERIAL3D_SHADOW_MODE_AUTO;
    mat->shadow_mode = (int32_t)mode;
}

/// @brief Read the material's shadow casting mode.
/// @param obj Material3D receiver.
/// @return One of RT_MATERIAL3D_SHADOW_MODE_AUTO, NONE, or CAST.
int64_t rt_material3d_get_shadow_mode(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return RT_MATERIAL3D_SHADOW_MODE_AUTO;
    if (mat->shadow_mode < RT_MATERIAL3D_SHADOW_MODE_AUTO ||
        mat->shadow_mode > RT_MATERIAL3D_SHADOW_MODE_CAST)
        return RT_MATERIAL3D_SHADOW_MODE_AUTO;
    return mat->shadow_mode;
}

/// @brief Toggle double-sided rendering. Disables backface culling for this material — useful
/// for foliage, banners, anything that should look correct from both sides. Increases fillrate.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param enabled Zero to enable backface culling, or nonzero to render both sides.
void rt_material3d_set_double_sided(void *obj, int8_t enabled) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->double_sided = enabled ? 1 : 0;
}

/// @brief Returns 1 if double-sided rendering is enabled, 0 otherwise.
/// @param obj Material3D receiver.
/// @return One when double-sided rendering is enabled, or zero for invalid or single-sided state.
int8_t rt_material3d_get_double_sided(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0;
    return mat->double_sided ? 1 : 0;
}

/// @brief Configure constant and slope-scaled depth bias for this material.
/// @details The bias is copied into each draw command and interpreted by the active backend's
///          native depth-bias mechanism. Negative constant values pull coplanar geometry
///          slightly toward the camera and are useful for decals, painted lines, and other
///          overlays that intentionally sit on top of an existing surface. Positive values push
///          geometry away. The slope term is added by GPU backends on steep polygons where
///          constant bias alone often fails to separate two nearly coplanar surfaces. Both
///          inputs are finite-clamped so malformed values cannot poison backend raster state.
/// @param obj Material3D receiver; invalid handles are ignored.
/// @param constant_bias Constant depth offset, clamped to the supported signed range.
/// @param slope_scaled_bias Slope-dependent multiplier, clamped to the supported signed range.
void rt_material3d_set_depth_bias(void *obj, double constant_bias, double slope_scaled_bias) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return;
    mat->depth_bias =
        material_clamp_symmetric_or_zero(constant_bias, MATERIAL3D_DEPTH_BIAS_ABS_MAX);
    mat->slope_scaled_depth_bias =
        material_clamp_symmetric_or_zero(slope_scaled_bias, MATERIAL3D_SLOPE_DEPTH_BIAS_ABS_MAX);
}

/* Map-slot and scalar readback (ADR 0233). Slot getters return the borrowed
 * retained source handle (Pixels, TextureAsset3D, or RenderTarget3D), NULL
 * when unbound. `SetAlbedoMap`/`SetAlbedoRenderTarget` alias the diffuse slot
 * (`Texture`); `SetEmissiveRenderTarget` aliases `EmissiveMap`. */

/// @brief `Material3D.Texture` — borrowed diffuse/albedo source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_texture(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->texture);
    return mat ? mat->texture : NULL;
}

/// @brief `Material3D.NormalMap` — borrowed normal-map source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_normal_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->normal_map);
    return mat ? mat->normal_map : NULL;
}

/// @brief `Material3D.SpecularMap` — borrowed specular-map source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_specular_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->specular_map);
    return mat ? mat->specular_map : NULL;
}

/// @brief `Material3D.EmissiveMap` — borrowed emissive-map source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_emissive_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->emissive_map);
    return mat ? mat->emissive_map : NULL;
}

/// @brief `Material3D.MetallicRoughnessMap` — borrowed ORM-style source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_metallic_roughness_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->metallic_roughness_map);
    return mat ? mat->metallic_roughness_map : NULL;
}

/// @brief `Material3D.AmbientOcclusionMap` — borrowed AO-map source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_ao_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->ao_map);
    return mat ? mat->ao_map : NULL;
}

/// @brief `Material3D.Lightmap` — borrowed baked-GI atlas source (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed source handle, or NULL when unbound.
void *rt_material3d_get_lightmap(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (mat)
        (void)material_texture_slot_has_drawable_source(&mat->lightmap);
    return mat ? mat->lightmap : NULL;
}

/// @brief `Material3D.EnvMap` — borrowed environment cubemap (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Borrowed CubeMap3D handle, or NULL when unbound.
void *rt_material3d_get_env_map(void *obj) {
    rt_material3d *mat = material_checked(obj);
    material_repair_env_map(mat);
    return mat ? mat->env_map : NULL;
}

/// @brief `Material3D.EmissiveColor` — fresh Vec3 of the emissive multiplier (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Newly allocated color snapshot; origin for an invalid handle.
void *rt_material3d_get_emissive_color(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return rt_vec3_new(0.0, 0.0, 0.0);
    for (int i = 0; i < 3; i++)
        mat->emissive[i] = sanitize_emissive_color(mat->emissive[i]);
    return rt_vec3_new(mat->emissive[0], mat->emissive[1], mat->emissive[2]);
}

/// @brief `Material3D.Shininess` — read the retained specular exponent (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Retained shininess, or zero for an invalid handle.
double rt_material3d_get_shininess(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0.0;
    mat->shininess = clamp_range(mat->shininess, 0.0, MATERIAL3D_SHININESS_MAX);
    return mat->shininess;
}

/// @brief `Material3D.DepthBias` — read the retained constant depth bias (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Retained constant bias, or zero for an invalid handle.
double rt_material3d_get_depth_bias(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0.0;
    mat->depth_bias =
        material_clamp_symmetric_or_zero(mat->depth_bias, MATERIAL3D_DEPTH_BIAS_ABS_MAX);
    return mat->depth_bias;
}

/// @brief `Material3D.DepthSlopeBias` — read the slope-scaled depth bias (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @return Retained slope-scaled bias, or zero for an invalid handle.
double rt_material3d_get_depth_slope_bias(void *obj) {
    rt_material3d *mat = material_checked(obj);
    if (!mat)
        return 0.0;
    mat->slope_scaled_depth_bias = material_clamp_symmetric_or_zero(
        mat->slope_scaled_depth_bias, MATERIAL3D_SLOPE_DEPTH_BIAS_ABS_MAX);
    return mat->slope_scaled_depth_bias;
}

/// @brief `Material3D.GetCustomParam(index)` — read one custom parameter (ADR 0233).
/// @param obj Borrowed Material3D handle.
/// @param index Zero-based parameter index; out-of-range reads return zero.
/// @return Retained parameter value, or zero.
double rt_material3d_get_custom_param(void *obj, int64_t index) {
    rt_material3d *mat = material_checked(obj);
    if (!mat || index < 0 || (size_t)index >= sizeof(mat->custom_params) / sizeof(double))
        return 0.0;
    mat->custom_params[index] = material_clamp_symmetric_or_zero(mat->custom_params[index],
                                                                 MATERIAL3D_CUSTOM_PARAM_ABS_MAX);
    return mat->custom_params[index];
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
