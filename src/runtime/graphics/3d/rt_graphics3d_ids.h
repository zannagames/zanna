//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_graphics3d_ids.h
// Purpose: Stable runtime class identifiers for Zanna.Graphics3D and Zanna.Game3D
//   objects, plus small inline helpers that validate/cast opaque object handles
//   against those ids before the 3D runtime modules dereference them.
//
// Key invariants:
//   - Class id constants are permanent ABI; never renumber an existing id.
//   - Ids are negative sentinels distinct from user/runtime object class ids.
//   - With RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE plus the explicit unsafe opt-in
//     RT_G3D_TRUSTED_STRUCT_HANDLES, checks degrade to a non-NULL test for
//     trusted internal call sites only.
//
// Ownership/Lifetime:
//   - Header-only; no allocation. Helpers never take ownership of @p obj.
//
// Links: src/runtime/core/rt_heap.h (heap header introspection),
//        src/runtime/graphics/3d/render/rt_canvas3d.c and sibling rt_*3d.c consumers
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares stable Graphics3D/Game3D class identifiers and handle validators.
/// @details Opaque runtime handles must pass these helpers before being cast to
/// private 3D payloads. Tagged math values additionally validate their heap
/// shape so malformed or unrelated heap objects cannot be read as Vec3/Quat
/// component arrays.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rt_heap.h"
#include "rt_quat.h"
#include "rt_vec3.h"

/// @brief Return the runtime class id of an object (0 if none/plain value).
/// @param p Borrowed object handle to inspect.
/// @return Stable runtime class identifier, or zero for a null or class-less value.
#ifdef __cplusplus
extern "C" {
#endif
extern int64_t rt_obj_class_id(void *p);
#ifdef __cplusplus
}
#endif

/// @name Permanent Graphics3D and Game3D class identifiers
/// @brief Frozen negative ABI sentinels used to tag opaque runtime objects.
/// @details Append identifiers at the end of this sequence; never renumber,
/// reuse, or reinterpret an existing value.
/// @{
#define RT_G3D_CUBEMAP3D_CLASS_ID INT64_C(-0x603001)
#define RT_G3D_RENDERTARGET3D_CLASS_ID INT64_C(-0x603002)
#define RT_G3D_CANVAS3D_CLASS_ID INT64_C(-0x603003)
#define RT_G3D_MESH3D_CLASS_ID INT64_C(-0x603004)
#define RT_G3D_CAMERA3D_CLASS_ID INT64_C(-0x603005)
#define RT_G3D_MATERIAL3D_CLASS_ID INT64_C(-0x603006)
#define RT_G3D_LIGHT3D_CLASS_ID INT64_C(-0x603007)
#define RT_G3D_SCENE3D_CLASS_ID INT64_C(-0x603008)
#define RT_G3D_SCENENODE3D_CLASS_ID INT64_C(-0x603009)
#define RT_G3D_NODEANIMATION3D_CLASS_ID INT64_C(-0x60300A)
#define RT_G3D_NODEANIMATOR3D_CLASS_ID INT64_C(-0x60300B)
#define RT_G3D_SKELETON3D_CLASS_ID INT64_C(-0x60300C)
#define RT_G3D_ANIMATION3D_CLASS_ID INT64_C(-0x60300D)
#define RT_G3D_ANIMPLAYER3D_CLASS_ID INT64_C(-0x60300E)
#define RT_G3D_ANIMBLEND3D_CLASS_ID INT64_C(-0x60300F)
#define RT_G3D_ANIMCONTROLLER3D_CLASS_ID INT64_C(-0x603010)
#define RT_G3D_FBX_ASSET_CLASS_ID INT64_C(-0x603011)
#define RT_G3D_GLTF_ASSET_CLASS_ID INT64_C(-0x603012)
#define RT_G3D_MODEL3D_CLASS_ID INT64_C(-0x603013)
#define RT_G3D_MORPHTARGET3D_CLASS_ID INT64_C(-0x603014)
#define RT_G3D_PARTICLES3D_CLASS_ID INT64_C(-0x603015)
#define RT_G3D_POSTFX3D_CLASS_ID INT64_C(-0x603016)
#define RT_G3D_RAYHIT3D_CLASS_ID INT64_C(-0x603017)
#define RT_G3D_SOUNDLISTENER3D_CLASS_ID INT64_C(-0x603018)
#define RT_G3D_SOUNDSOURCE3D_CLASS_ID INT64_C(-0x603019)
#define RT_G3D_WORLD3D_CLASS_ID INT64_C(-0x60301A)
#define RT_G3D_PHYSICSHIT3D_CLASS_ID INT64_C(-0x60301B)
#define RT_G3D_PHYSICSHITLIST3D_CLASS_ID INT64_C(-0x60301C)
#define RT_G3D_CONTACTPOINT3D_CLASS_ID INT64_C(-0x60301D)
#define RT_G3D_COLLISIONEVENT3D_CLASS_ID INT64_C(-0x60301E)
#define RT_G3D_COLLIDER3D_CLASS_ID INT64_C(-0x60301F)
#define RT_G3D_BODY3D_CLASS_ID INT64_C(-0x603020)
#define RT_G3D_CHARACTER3D_CLASS_ID INT64_C(-0x603021)
#define RT_G3D_TRIGGER3D_CLASS_ID INT64_C(-0x603022)
#define RT_G3D_DISTANCEJOINT3D_CLASS_ID INT64_C(-0x603023)
#define RT_G3D_SPRINGJOINT3D_CLASS_ID INT64_C(-0x603024)
#define RT_G3D_TRANSFORM3D_CLASS_ID INT64_C(-0x603025)
#define RT_G3D_PATH3D_CLASS_ID INT64_C(-0x603026)
#define RT_G3D_INSTANCEBATCH3D_CLASS_ID INT64_C(-0x603027)
#define RT_G3D_TERRAIN3D_CLASS_ID INT64_C(-0x603028)
#define RT_G3D_NAVMESH3D_CLASS_ID INT64_C(-0x603029)
#define RT_G3D_NAVAGENT3D_CLASS_ID INT64_C(-0x60302A)
#define RT_G3D_DECAL3D_CLASS_ID INT64_C(-0x60302B)
#define RT_G3D_SPRITE3D_CLASS_ID INT64_C(-0x60302C)
#define RT_G3D_WATER3D_CLASS_ID INT64_C(-0x60302D)
#define RT_G3D_VEGETATION3D_CLASS_ID INT64_C(-0x60302E)
#define RT_G3D_TEXTUREATLAS3D_CLASS_ID INT64_C(-0x60302F)
#define RT_G3D_GAME3D_LAYERMASK_CLASS_ID INT64_C(-0x603030)
#define RT_G3D_GAME3D_INPUT_CLASS_ID INT64_C(-0x603031)
#define RT_G3D_GAME3D_ENTITY_CLASS_ID INT64_C(-0x603032)
#define RT_G3D_GAME3D_SOUND_CLASS_ID INT64_C(-0x603033)
#define RT_G3D_GAME3D_EFFECTS_CLASS_ID INT64_C(-0x603034)
#define RT_G3D_GAME3D_WORLD_CLASS_ID INT64_C(-0x603035)
#define RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID INT64_C(-0x603036)
#define RT_G3D_GAME3D_FREEFLY_CLASS_ID INT64_C(-0x603037)
#define RT_G3D_GAME3D_ORBIT_CLASS_ID INT64_C(-0x603038)
#define RT_G3D_GAME3D_FOLLOW_CLASS_ID INT64_C(-0x603039)
#define RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID INT64_C(-0x60303A)
#define RT_G3D_GAME3D_ENV_HANDLE_CLASS_ID INT64_C(-0x60303B)
#define RT_G3D_GAME3D_BODYDEF_CLASS_ID INT64_C(-0x60303C)
#define RT_G3D_GAME3D_COLLISION_EVENT_CLASS_ID INT64_C(-0x60303D)
#define RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID INT64_C(-0x60303E)
#define RT_G3D_GAME3D_ANIMATOR3D_CLASS_ID INT64_C(-0x60303F)
#define RT_G3D_HINGEJOINT3D_CLASS_ID INT64_C(-0x603040)
#define RT_G3D_ROPEJOINT3D_CLASS_ID INT64_C(-0x603041)
#define RT_G3D_SIXDOFJOINT3D_CLASS_ID INT64_C(-0x603042)
#define RT_G3D_GAME3D_ASSET_HANDLE3D_CLASS_ID INT64_C(-0x603043)
#define RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID INT64_C(-0x603044)
#define RT_G3D_TEXTUREASSET3D_CLASS_ID INT64_C(-0x603045)
#define RT_G3D_BLENDTREE3D_CLASS_ID INT64_C(-0x603046)
#define RT_G3D_IKSOLVER3D_CLASS_ID INT64_C(-0x603047)
#define RT_G3D_GAME3D_DIAGNOSTICS_CLASS_ID INT64_C(-0x603048)
#define RT_G3D_GAME3D_BEHAVIOR3D_CLASS_ID INT64_C(-0x603049)
#define RT_G3D_LENSFLARE3D_CLASS_ID INT64_C(-0x60304A)
#define RT_G3D_GAME3D_THIRDPERSON_CLASS_ID INT64_C(-0x60304B)
#define RT_G3D_GAME3D_TARGETLOCK_CLASS_ID INT64_C(-0x60304C)
#define RT_G3D_LEDGEHIT3D_CLASS_ID INT64_C(-0x60304D)
#define RT_G3D_GAME3D_HITBOX_CLASS_ID INT64_C(-0x60304E)
#define RT_G3D_GAME3D_HITEVENT_CLASS_ID INT64_C(-0x60304F)
#define RT_G3D_GAME3D_HEALTH_CLASS_ID INT64_C(-0x603050)
#define RT_G3D_GAME3D_DAMAGEEVENT_CLASS_ID INT64_C(-0x603051)
#define RT_G3D_RAGDOLL3D_CLASS_ID INT64_C(-0x603052)
#define RT_G3D_GAME3D_RAILCAMERA_CLASS_ID INT64_C(-0x603053)
#define RT_G3D_GAME3D_TIMELINE_CLASS_ID INT64_C(-0x603054)
#define RT_G3D_GAME3D_DIALOGUE_CLASS_ID INT64_C(-0x603055)
#define RT_G3D_GAME3D_LIPSYNC_CLASS_ID INT64_C(-0x603056)
#define RT_G3D_LIGHTBAKER3D_CLASS_ID INT64_C(-0x603057)
#define RT_G3D_LIGHTPROBEGRID3D_CLASS_ID INT64_C(-0x603058)
#define RT_G3D_REFLECTIONPROBE3D_CLASS_ID INT64_C(-0x603059)
#define RT_G3D_SKY3D_CLASS_ID INT64_C(-0x60305A)
#define RT_G3D_TIMEOFDAY3D_CLASS_ID INT64_C(-0x60305B)
#define RT_G3D_GAME3D_SURFACETABLE_CLASS_ID INT64_C(-0x60305C)
#define RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID INT64_C(-0x60305D)
#define RT_G3D_GAME3D_INTERACTABLE_CLASS_ID INT64_C(-0x60305E)
#define RT_G3D_GAME3D_INTERACTOR_CLASS_ID INT64_C(-0x60305F)
#define RT_G3D_GAME3D_PERCEPTION_CLASS_ID INT64_C(-0x603060)
#define RT_G3D_GAME3D_BTREE_CLASS_ID INT64_C(-0x603061)
#define RT_G3D_GAME3D_BTINSTANCE_CLASS_ID INT64_C(-0x603062)
#define RT_G3D_GAME3D_REVERBZONE_CLASS_ID INT64_C(-0x603063)
#define RT_G3D_GAME3D_AMBIENTBED_CLASS_ID INT64_C(-0x603064)
#define RT_G3D_CLOTH3D_CLASS_ID INT64_C(-0x603065)
#define RT_G3D_GAME3D_MINIMAP_CLASS_ID INT64_C(-0x603066)
#define RT_G3D_VEHICLE3D_CLASS_ID INT64_C(-0x603067)
/// @}

#if defined(RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE) && RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE &&       \
    !defined(RT_G3D_TRUSTED_STRUCT_HANDLES)
#error                                                                                             \
    "RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE disables public Graphics3D class checks; define RT_G3D_TRUSTED_STRUCT_HANDLES only for private trusted fixtures."
#endif

/// @brief True if @p obj is a live object of runtime class @p class_id.
/// @details Under RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE this only checks for
///          non-NULL (trusted-handle fast path).
/// @param obj Borrowed opaque runtime handle.
/// @param class_id Expected stable runtime class identifier.
/// @return Nonzero when the handle is non-null and matches the expected class.
static inline int32_t rt_g3d_has_class(void *obj, int64_t class_id) {
#if defined(RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE) && RT_G3D_INTERNAL_ASSUME_STRUCT_HANDLE
    (void)class_id;
    return obj != NULL;
#else
    return obj && rt_obj_class_id(obj) == class_id;
#endif
}

/// @brief Return @p obj if it is class @p class_id, else NULL (safe cast).
/// @param obj Borrowed opaque runtime handle.
/// @param class_id Expected stable runtime class identifier.
/// @return The unchanged borrowed handle on a class match, otherwise `NULL`.
static inline void *rt_g3d_checked_or_null(void *obj, int64_t class_id) {
    return rt_g3d_has_class(obj, class_id) ? obj : NULL;
}

/// @brief True if @p obj is a plain (class-less) heap value of exactly
///        @p payload_bytes — used to recognize boxed Vec3/Quat passed as void*.
/// @param obj Borrowed opaque heap handle to inspect.
/// @param payload_bytes Exact byte length required for the class-less payload.
/// @return Nonzero only for a plain object with the exact requested payload shape.
static inline int32_t rt_g3d_is_plain_value_object(void *obj, size_t payload_bytes) {
    if (!obj)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(obj, &heap_info))
        return 0;
    return heap_info.kind == RT_HEAP_OBJECT && heap_info.elem_kind == RT_ELEM_NONE &&
           heap_info.class_id == 0 && heap_info.len == payload_bytes &&
           heap_info.cap == payload_bytes;
}

/// @brief True if @p obj is a tagged heap value with at least @p payload_bytes bytes.
/// @details Vec3 and Quat started life as class-less heap values. Newer constructors tag them
///          with explicit math class ids, while some internal fixtures and serialized surfaces
///          still expose the old class id 0 shape. The helper accepts only the requested tag and
///          enough inline payload space; legacy id 0 is handled separately by
///          @ref rt_g3d_is_plain_value_object.
/// @param obj Borrowed opaque heap handle to inspect.
/// @param class_id Required tagged math class identifier.
/// @param payload_bytes Minimum inline payload capacity in bytes.
/// @return Nonzero only when heap kind, element kind, tag, and capacity are valid.
static inline int32_t rt_g3d_is_tagged_value_object(void *obj,
                                                    int64_t class_id,
                                                    size_t payload_bytes) {
    if (!obj)
        return 0;
    rt_heap_info_t heap_info;
    if (!rt_heap_get_info(obj, &heap_info))
        return 0;
    return heap_info.kind == RT_HEAP_OBJECT && heap_info.elem_kind == RT_ELEM_NONE &&
           heap_info.class_id == class_id && heap_info.cap >= payload_bytes;
}

/// @brief True if @p obj is a Vec3, accepting both tagged and legacy class-less payloads.
/// @param obj Borrowed opaque heap handle to inspect.
/// @return Nonzero for a valid three-double tagged or legacy Vec3 payload.
static inline int32_t rt_g3d_is_vec3(void *obj) {
    return rt_g3d_is_tagged_value_object(obj, RT_VEC3_CLASS_ID, sizeof(double) * 3u) ||
           rt_g3d_is_plain_value_object(obj, sizeof(double) * 3u);
}

/// @brief True if @p obj is a Quat, accepting both tagged and legacy class-less payloads.
/// @param obj Borrowed opaque heap handle to inspect.
/// @return Nonzero for a valid four-double tagged or legacy quaternion payload.
static inline int32_t rt_g3d_is_quat(void *obj) {
    return rt_g3d_is_tagged_value_object(obj, RT_QUAT_CLASS_ID, sizeof(double) * 4u) ||
           rt_g3d_is_plain_value_object(obj, sizeof(double) * 4u);
}
