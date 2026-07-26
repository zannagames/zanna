//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/RuntimeOwnership.hpp
// Purpose: Centralize runtime reference ownership metadata for optimizer queries.
// Key invariants: Unknown helpers are classified conservatively with no
//                 ownership facts. Bitmasks refer to explicit IL-visible
//                 arguments, not hidden bridge parameters.
// Ownership/Lifetime: Header-only table; no dynamic storage.
// Links: docs/il/il-passes.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines optimizer-facing ownership metadata for runtime helpers.
/// @details Classifications cover both canonical Zanna names and their native
///          `rt_*` aliases. Unknown names deliberately return an empty,
///          conservative effect set.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace il::runtime {

/// @brief Ownership effects attached to a runtime helper call.
struct RuntimeOwnershipEffects {
    std::uint64_t consumedArgMask{0}; ///< Arguments whose ownership is consumed.
    std::uint64_t retainedArgMask{0}; ///< Arguments whose reference count is retained.
    std::uint64_t ownedOutArgMask{0}; ///< Pointer args that receive an owned reference.
    bool returnsOwned{false};         ///< Result is an owned string/reference handle.
    bool mayAllocate{false};          ///< Helper may allocate runtime-managed storage.
    bool returnsKnownObject{false};   ///< Result is a heap object compatible with
                                      ///< object-specific retain/release helpers.
    bool knownNeutral{false};         ///< Helper borrows every argument, performs no
                                      ///< retain/release on any handle, and cannot
                                      ///< re-enter user code. Stronger than the mere
                                      ///< absence of masks (which may just mean
                                      ///< "unclassified").

    /// @brief Query whether the helper consumes argument @p index.
    /// @param index Zero-based IL-visible argument position.
    /// @return True when ownership of that argument transfers to the helper.
    [[nodiscard]] constexpr bool consumesArg(unsigned index) const noexcept {
        return index < 64 && (consumedArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief Query whether the helper retains argument @p index.
    /// @param index Zero-based IL-visible argument position.
    /// @return True when the helper increments or stores that argument's
    ///         reference.
    [[nodiscard]] constexpr bool retainsArg(unsigned index) const noexcept {
        return index < 64 && (retainedArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief Query whether pointer argument @p index receives an owned reference.
    /// @param index Zero-based IL-visible argument position.
    /// @return True when the argument points to storage receiving an owned value.
    [[nodiscard]] constexpr bool writesOwnedOutArg(unsigned index) const noexcept {
        return index < 64 && (ownedOutArgMask & (std::uint64_t{1} << index)) != 0;
    }

    /// @brief True when any ownership fact is known.
    /// @return True when at least one mask or positive result/allocation property
    ///         is set; @ref knownNeutral alone is intentionally excluded.
    [[nodiscard]] constexpr bool hasAny() const noexcept {
        return consumedArgMask != 0 || retainedArgMask != 0 || ownedOutArgMask != 0 ||
               returnsOwned || mayAllocate || returnsKnownObject;
    }
};

namespace detail {

/// @brief Test whether a string begins with a prefix.
/// @param value String to inspect.
/// @param prefix Candidate leading substring.
/// @return True when @p prefix occurs at offset zero.
[[nodiscard]] constexpr bool startsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/// @brief Test whether a string ends with a suffix.
/// @param value String to inspect.
/// @param suffix Candidate trailing substring.
/// @return True when @p suffix occupies the end of @p value.
[[nodiscard]] constexpr bool endsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

/// @brief Test whether a string contains a substring.
/// @param value String to inspect.
/// @param needle Substring to locate.
/// @return True when @p needle occurs anywhere in @p value.
[[nodiscard]] constexpr bool contains(std::string_view value, std::string_view needle) noexcept {
    return value.find(needle) != std::string_view::npos;
}

/// @brief Match a qualified helper name against a fixed operation-name table.
/// @tparam N Number of entries in @p operations.
/// @param name Complete canonical or native helper name.
/// @param prefix Required namespace or C-symbol prefix.
/// @param operations Accepted suffixes after @p prefix.
/// @return True when @p name starts with @p prefix and its remainder exactly
///         matches one table entry.
template <std::size_t N>
[[nodiscard]] constexpr bool hasOperation(std::string_view name,
                                          std::string_view prefix,
                                          const std::string_view (&operations)[N]) noexcept {
    if (!startsWith(name, prefix))
        return false;
    const std::string_view operation = name.substr(prefix.size());
    for (const std::string_view candidate : operations) {
        if (operation == candidate)
            return true;
    }
    return false;
}

/// @brief True for value-oriented geometry helpers that allocate their result.
/// @details The runtime API manifest defines every object-returning operation on
///          these immutable math types as an owned value. Keep both canonical
///          names and C symbols here because frontend IL uses the former while
///          generated and hand-authored IL may use the latter.
/// @param name Canonical or native runtime helper name.
/// @return True when the helper allocates a supported immutable math value.
[[nodiscard]] constexpr bool returnsFreshMathValue(std::string_view name) noexcept {
    constexpr std::string_view vec2Operations[] = {
        "Add",
        "Div",
        "Lerp",
        "Mul",
        "Negate",
        "New",
        "Norm",
        "One",
        "Rotate",
        "Sub",
        "Zero",
    };
    constexpr std::string_view vec2Symbols[] = {
        "add",
        "div",
        "lerp",
        "mul",
        "neg",
        "new",
        "norm",
        "one",
        "rotate",
        "sub",
        "zero",
    };
    constexpr std::string_view vec3Operations[] = {
        "Add",
        "Cross",
        "Div",
        "Lerp",
        "Mul",
        "Negate",
        "New",
        "Norm",
        "One",
        "Sub",
        "Zero",
        "Reflect",
        "Project",
        "ClampLength",
        "MoveTowards",
        "Min",
        "Max",
    };
    constexpr std::string_view vec3Symbols[] = {
        "add",
        "cross",
        "div",
        "lerp",
        "mul",
        "neg",
        "new",
        "norm",
        "one",
        "sub",
        "zero",
        "reflect",
        "project",
        "clamp_len",
        "move_towards",
        "min",
        "max",
    };
    constexpr std::string_view mat3Operations[] = {
        "New",
        "Identity",
        "Zero",
        "Translate",
        "Scale",
        "ScaleUniform",
        "Rotate",
        "Shear",
        "Row",
        "Col",
        "Add",
        "Sub",
        "Mul",
        "MulScalar",
        "TransformPoint",
        "TransformVector",
        "Transpose",
        "Inverse",
        "Negate",
    };
    constexpr std::string_view mat3Symbols[] = {
        "new",           "identity",  "zero",    "translate",  "scale",
        "scale_uniform", "rotate",    "shear",   "row",        "col",
        "add",           "sub",       "mul",     "mul_scalar", "transform_point",
        "transform_vec", "transpose", "inverse", "neg",
    };
    constexpr std::string_view mat4Operations[] = {
        "New",
        "Identity",
        "Zero",
        "Translate",
        "Scale",
        "ScaleUniform",
        "RotateX",
        "RotateY",
        "RotateZ",
        "RotateAxis",
        "Perspective",
        "Orthographic",
        "LookAt",
        "Add",
        "Sub",
        "Mul",
        "MulScalar",
        "TransformPoint",
        "TransformVector",
        "Transpose",
        "Inverse",
        "Negate",
    };
    constexpr std::string_view mat4Symbols[] = {
        "new",           "identity",  "zero",     "translate",   "scale",       "scale_uniform",
        "rotate_x",      "rotate_y",  "rotate_z", "rotate_axis", "perspective", "ortho",
        "look_at",       "add",       "sub",      "mul",         "mul_scalar",  "transform_point",
        "transform_vec", "transpose", "inverse",  "neg",
    };
    constexpr std::string_view quatOperations[] = {
        "New",
        "Identity",
        "FromAxisAngle",
        "FromEuler",
        "Mul",
        "Conjugate",
        "Inverse",
        "Normalize",
        "Slerp",
        "Lerp",
        "RotateVec3",
        "ToMat4",
        "Axis",
    };
    constexpr std::string_view quatSymbols[] = {
        "new",
        "identity",
        "from_axis_angle",
        "from_euler",
        "mul",
        "conjugate",
        "inverse",
        "norm",
        "slerp",
        "lerp",
        "rotate_vec3",
        "to_mat4",
        "axis",
    };
    constexpr std::string_view splineOperations[] = {
        "CatmullRom",
        "Bezier",
        "Linear",
        "Eval",
        "Tangent",
        "PointAt",
        "Sample",
    };
    constexpr std::string_view splineSymbols[] = {
        "catmull_rom",
        "bezier",
        "linear",
        "eval",
        "tangent",
        "point_at",
        "sample",
    };

    return hasOperation(name, "Zanna.Math.Vec2.", vec2Operations) ||
           hasOperation(name, "rt_vec2_", vec2Symbols) ||
           hasOperation(name, "Zanna.Math.Vec3.", vec3Operations) ||
           hasOperation(name, "rt_vec3_", vec3Symbols) ||
           hasOperation(name, "Zanna.Math.Mat3.", mat3Operations) ||
           hasOperation(name, "rt_mat3_", mat3Symbols) ||
           hasOperation(name, "Zanna.Math.Mat4.", mat4Operations) ||
           hasOperation(name, "rt_mat4_", mat4Symbols) ||
           hasOperation(name, "Zanna.Math.Quat.", quatOperations) ||
           hasOperation(name, "rt_quat_", quatSymbols) ||
           hasOperation(name, "Zanna.Math.Spline.", splineOperations) ||
           hasOperation(name, "rt_spline_", splineSymbols);
}

/// @brief True for registered 3D APIs that materialize a fresh Vec3 result.
/// @details A typed `obj<Zanna.Math.Vec3>` return is not sufficient by itself:
///          OrbitController.Target and FollowController.Offset intentionally
///          expose borrowed stored vectors. Keep the allocating snapshot calls
///          explicit so those borrowed accessors remain unowned.
/// @param name Canonical or native runtime helper name.
/// @return True when the helper returns a newly allocated Vec3 snapshot.
[[nodiscard]] constexpr bool returnsFreshVec3(std::string_view name) noexcept {
    return name == "Zanna.Graphics3D.Light3D.get_Color" ||
           name == "Zanna.Graphics3D.Light3D.get_Direction" ||
           name == "Zanna.Graphics3D.Light3D.get_Position" ||
           name == "Zanna.Graphics3D.SceneNode.get_Position" ||
           name == "Zanna.Graphics3D.SceneNode.get_Scale" ||
           name == "Zanna.Graphics3D.SceneNode.get_WorldPosition" ||
           name == "Zanna.Graphics3D.SceneNode.get_WorldScale" ||
           name == "Zanna.Graphics3D.SceneNode.get_BoundsMin" ||
           name == "Zanna.Graphics3D.SceneNode.get_BoundsMax" ||
           name == "Zanna.Graphics3D.SoundListener3D.get_Position" ||
           name == "Zanna.Graphics3D.SoundListener3D.get_Forward" ||
           name == "Zanna.Graphics3D.SoundListener3D.get_Up" ||
           name == "Zanna.Graphics3D.SoundListener3D.get_Velocity" ||
           name == "Zanna.Graphics3D.SoundSource3D.get_Position" ||
           name == "Zanna.Graphics3D.SoundSource3D.get_Velocity" ||
           name == "Zanna.Graphics3D.PhysicsHit3D.get_Point" ||
           name == "Zanna.Graphics3D.PhysicsHit3D.get_Normal" ||
           name == "Zanna.Graphics3D.RayHit3D.get_Point" ||
           name == "Zanna.Graphics3D.RayHit3D.get_Normal" ||
           name == "Zanna.Graphics3D.LedgeHit3D.get_GrabPoint" ||
           name == "Zanna.Graphics3D.LedgeHit3D.get_SurfaceNormal" ||
           name == "Zanna.Graphics3D.LedgeHit3D.get_WallNormal" ||
           name == "Zanna.Graphics3D.LedgeHit3D.get_LandingPoint" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_Position" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_Scale" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_Velocity" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_AngularVelocity" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_GroundNormal" ||
           name == "Zanna.Graphics3D.Character3D.get_Position" ||
           name == "Zanna.Graphics3D.Camera3D.get_Position" ||
           name == "Zanna.Graphics3D.Camera3D.get_Forward" ||
           name == "Zanna.Graphics3D.Camera3D.get_Right" ||
           name == "Zanna.Graphics3D.Material3D.get_Color" ||
           name == "Zanna.Graphics3D.Transform3D.get_Position" ||
           name == "Zanna.Graphics3D.Transform3D.get_Scale" ||
           name == "Zanna.Graphics3D.LightProbeGrid3D.Sample" ||
           name == "Zanna.Graphics3D.TimeOfDay3D.get_SunDirection" ||
           name == "Zanna.Graphics3D.ReflectionProbe3D.get_Position" ||
           name == "Zanna.Graphics3D.NavAgent3D.get_Position" ||
           name == "Zanna.Graphics3D.NavAgent3D.get_Velocity" ||
           name == "Zanna.Graphics3D.NavAgent3D.get_DesiredVelocity" ||
           name == "Zanna.Graphics3D.AnimController3D.get_RootMotionDelta" ||
           name == "Zanna.Graphics3D.AnimController3D.ConsumeRootMotion" ||
           name == "Zanna.Game3D.Perception3D.LastKnownPosition" ||
           name == "Zanna.Game3D.Perception3D.HeardPosition" ||
           name == "Zanna.Game3D.TargetLock3D.LockedMoveBias" ||
           name == "Zanna.Game3D.HitEvent3D.Point" || name == "Zanna.Game3D.HitEvent3D.Normal" ||
           name == "Zanna.Game3D.World3D.get_WorldOrigin" ||
           name == "Zanna.Game3D.WorldStream3D.GetCellCenter" ||
           name == "Zanna.Game3D.WorldStream3D.GetTerrainTileCenter" ||
           name == "Zanna.Game3D.Input3D.MoveAxis" ||
           name == "Zanna.Game3D.World3D.GetPersistentPosition" ||
           name == "Zanna.Game3D.Entity3D.get_Position" ||
           name == "Zanna.Game3D.Entity3D.get_WorldPosition" ||
           name == "Zanna.Graphics3D.Cloth3D.GetPoint" ||
           name == "Zanna.Game3D.ThirdPersonController.get_ShoulderOffset" ||
           name == "rt_light3d_get_color" || name == "rt_light3d_get_direction" ||
           name == "rt_light3d_get_position" || name == "rt_scene_node3d_get_position" ||
           name == "rt_scene_node3d_get_scale" || name == "rt_scene_node3d_get_world_position" ||
           name == "rt_scene_node3d_get_world_scale" || name == "rt_scene_node3d_get_aabb_min" ||
           name == "rt_scene_node3d_get_aabb_max" || name == "rt_soundlistener3d_get_position" ||
           name == "rt_soundlistener3d_get_forward" || name == "rt_soundlistener3d_get_up" ||
           name == "rt_soundlistener3d_get_velocity" || name == "rt_soundsource3d_get_position" ||
           name == "rt_soundsource3d_get_velocity" || name == "rt_physics_hit3d_get_point" ||
           name == "rt_physics_hit3d_get_normal" || name == "rt_ray3d_hit_point" ||
           name == "rt_ray3d_hit_normal" || name == "rt_ledge_hit3d_get_grab_point" ||
           name == "rt_ledge_hit3d_get_surface_normal" ||
           name == "rt_ledge_hit3d_get_wall_normal" || name == "rt_ledge_hit3d_get_landing_point" ||
           name == "rt_body3d_get_position" || name == "rt_body3d_get_scale" ||
           name == "rt_body3d_get_velocity" || name == "rt_body3d_get_angular_velocity" ||
           name == "rt_body3d_get_ground_normal" || name == "rt_character3d_get_position" ||
           name == "rt_camera3d_get_position" || name == "rt_camera3d_get_forward" ||
           name == "rt_camera3d_get_right" || name == "rt_material3d_get_color" ||
           name == "rt_transform3d_get_position" || name == "rt_transform3d_get_scale" ||
           name == "rt_lightprobegrid3d_sample" || name == "rt_timeofday3d_get_sun_direction" ||
           name == "rt_reflectionprobe3d_get_position" || name == "rt_navagent3d_get_position" ||
           name == "rt_navagent3d_get_velocity" || name == "rt_navagent3d_get_desired_velocity" ||
           name == "rt_anim_controller3d_get_root_motion_delta" ||
           name == "rt_anim_controller3d_consume_root_motion" ||
           name == "rt_game3d_perception_last_known_position" ||
           name == "rt_game3d_perception_heard_position" ||
           name == "rt_game3d_targetlock_locked_move_bias" || name == "rt_game3d_hit_event_point" ||
           name == "rt_game3d_hit_event_normal" || name == "rt_game3d_world_get_world_origin" ||
           name == "rt_game3d_world_stream_get_cell_center" ||
           name == "rt_game3d_world_stream_get_terrain_tile_center" ||
           name == "rt_game3d_input_move_axis" ||
           name == "rt_game3d_world_get_persistent_position" ||
           name == "rt_game3d_entity_position" || name == "rt_game3d_entity_world_position" ||
           name == "rt_cloth3d_get_point" ||
           name == "rt_game3d_thirdperson_controller_get_shoulder_offset";
}

/// @brief True for registered 3D APIs that materialize fresh matrix/quaternion snapshots.
/// @param name Canonical or native runtime helper name.
/// @return True when the helper returns a newly allocated matrix or quaternion
///         snapshot.
[[nodiscard]] constexpr bool returnsFresh3DMathSnapshot(std::string_view name) noexcept {
    return name == "Zanna.Graphics3D.SceneNode.get_Rotation" ||
           name == "Zanna.Graphics3D.SceneNode.get_WorldMatrix" ||
           name == "Zanna.Graphics3D.SceneNode.get_WorldRotation" ||
           name == "Zanna.Graphics3D.PhysicsBody3D.get_Orientation" ||
           name == "Zanna.Graphics3D.Transform3D.get_Rotation" ||
           name == "Zanna.Graphics3D.Transform3D.get_Matrix" ||
           name == "Zanna.Graphics3D.AnimController3D.GetBoneMatrix" ||
           name == "Zanna.Game3D.Animator3D.GetBoneMatrix" ||
           name == "rt_scene_node3d_get_rotation" || name == "rt_scene_node3d_get_world_matrix" ||
           name == "rt_scene_node3d_get_world_rotation" || name == "rt_body3d_get_orientation" ||
           name == "rt_transform3d_get_rotation" || name == "rt_transform3d_get_matrix" ||
           name == "rt_anim_controller3d_get_bone_matrix" ||
           name == "rt_game3d_animator_get_bone_matrix";
}

} // namespace detail

/// @brief Classify string/reference ownership effects for known runtime helpers.
/// @details This metadata prevents optimizers from treating owned string,
///          object, array, and collection construction/consumption as ordinary
///          value operations. Names include both C runtime symbols and
///          high-level runtime namespace aliases produced by frontends.
/// @param name Canonical Zanna helper name or native `rt_*` symbol.
/// @return Known ownership effects, or an empty conservative classification
///         when @p name is unrecognized.
[[nodiscard]] inline RuntimeOwnershipEffects classifyRuntimeOwnership(std::string_view name) {
    RuntimeOwnershipEffects effects{};

    // High-traffic string helpers that only borrow: they read their arguments,
    // never retain, release, or store any handle, and never call back into
    // user code. Optimizers may hoist retain/release traffic across them.
    if (name == "rt_print_str" || name == "rt_str_len" || name == "rt_str_eq" ||
        name == "rt_str_cmp" || name == "rt_str_cmp_nocase" || name == "rt_str_is_empty" ||
        name == "rt_str_starts_with" || name == "rt_str_ends_with" || name == "rt_str_index_of" ||
        name == "Zanna.Terminal.PrintStr" || name == "Zanna.String.get_Length" ||
        name == "Zanna.String.Equals" || name == "Zanna.String.Cmp" ||
        name == "Zanna.String.CmpNoCase") {
        effects.knownNeutral = true;
        return effects;
    }

    if (name == "rt_str_concat" || name == "Zanna.String.Concat") {
        effects.consumedArgMask = 0b11;
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_str_release" || name == "rt_str_release_maybe" ||
        name == "rt_memory_release_str" || name == "Zanna.String.ReleaseMaybe" ||
        name == "Zanna.Memory.ReleaseStr" || name == "Zanna.Runtime.Unsafe.ReleaseStr") {
        effects.consumedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_str_retain" || name == "rt_str_retain_maybe" ||
        name == "rt_memory_retain_str" || name == "Zanna.String.RetainMaybe" ||
        name == "Zanna.Memory.RetainStr" || name == "Zanna.Runtime.Unsafe.RetainStr") {
        effects.retainedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_memory_release" || name == "Zanna.Memory.Release" ||
        name == "Zanna.Runtime.Unsafe.Release") {
        effects.consumedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_memory_retain" || name == "Zanna.Memory.Retain" ||
        name == "Zanna.Runtime.Unsafe.Retain") {
        effects.retainedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_str_empty") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_str_substr" || name == "rt_csv_quote_alloc" || name == "rt_str_split_fields" ||
        name == "rt_int_to_str" || name == "rt_f64_to_str" || name == "rt_str_i16_alloc" ||
        name == "rt_str_i32_alloc" || name == "rt_str_f_alloc" || name == "rt_const_cstr" ||
        name == "rt_str_from_lit" || name == "rt_str_left" || name == "rt_str_right" ||
        name == "rt_str_mid" || name == "rt_str_mid_len" || name == "rt_str_ltrim" ||
        name == "rt_str_rtrim" || name == "rt_str_trim" || name == "rt_str_ucase" ||
        name == "rt_str_lcase" || name == "rt_str_chr" || name == "rt_args_get" ||
        name == "rt_cmdline" || name == "rt_getkey_str" || name == "rt_inkey_str" ||
        name == "rt_term_read_line" || name == "rt_term_ask" || name == "rt_term_try_read_line" ||
        name == "rt_term_try_ask" || name == "rt_term_read_line_result" ||
        name == "rt_term_ask_result" || name == "Zanna.Terminal.TryReadLine" ||
        name == "Zanna.Terminal.TryAsk" || name == "Zanna.Terminal.ReadLineResult" ||
        name == "Zanna.Terminal.AskResult" || name == "rt_message_bundle_get_or" ||
        name == "Zanna.Localization.MessageBundle.GetOr" || name == "Zanna.String.Left" ||
        name == "Zanna.String.Right" || name == "Zanna.String.Mid2" ||
        name == "Zanna.String.Mid3" || name == "Zanna.String.LTrim" ||
        name == "Zanna.String.RTrim" || name == "Zanna.String.Trim" ||
        name == "Zanna.String.UCase" || name == "Zanna.String.LCase" ||
        name == "Zanna.String.Chr" || name == "Zanna.String.FromI64" ||
        name == "Zanna.String.FromF64") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_arr_i32_new" || name == "rt_arr_i64_new" || name == "rt_arr_f64_new" ||
        name == "rt_arr_str_alloc" || name == "rt_arr_obj_new") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_arr_i32_retain" || name == "rt_arr_i64_retain" || name == "rt_arr_f64_retain") {
        effects.retainedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_arr_i32_release" || name == "rt_arr_i64_release" ||
        name == "rt_arr_f64_release" || name == "rt_arr_str_release" ||
        name == "rt_arr_obj_release") {
        effects.consumedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_arr_str_get" || name == "rt_arr_obj_get") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_list_get" || name == "rt_list_first" || name == "rt_list_last" ||
        name == "rt_list_pop" || name == "Zanna.Collections.List.Get" ||
        name == "Zanna.Collections.List.First" || name == "Zanna.Collections.List.Last" ||
        name == "Zanna.Collections.List.Pop" || name == "rt_deque_get" ||
        name == "rt_deque_peek_front" || name == "rt_deque_peek_back" ||
        name == "rt_deque_pop_front" || name == "rt_deque_pop_back" ||
        name == "rt_deque_try_pop_front" || name == "rt_deque_try_pop_back" ||
        name == "Zanna.Collections.Deque.Get" || name == "Zanna.Collections.Deque.PeekFront" ||
        name == "Zanna.Collections.Deque.PeekBack" || name == "Zanna.Collections.Deque.PopFront" ||
        name == "Zanna.Collections.Deque.PopBack" ||
        name == "Zanna.Collections.Deque.TryPopFront" ||
        name == "Zanna.Collections.Deque.TryPopBack" || name == "rt_stack_pop" ||
        name == "rt_stack_try_pop" || name == "Zanna.Collections.Stack.Pop" ||
        name == "Zanna.Collections.Stack.TryPop" || name == "rt_queue_pop" ||
        name == "rt_queue_try_pop" || name == "Zanna.Collections.Queue.Pop" ||
        name == "Zanna.Collections.Queue.TryPop" || name == "rt_seq_pop" ||
        name == "rt_seq_remove" || name == "Zanna.Collections.Seq.Pop" ||
        name == "Zanna.Collections.Seq.RemoveAt" || name == "rt_multimap_get_first" ||
        name == "Zanna.Collections.MultiMap.GetFirst" || name == "rt_pqueue_pop" ||
        name == "rt_pqueue_try_pop" || name == "rt_pqueue_peek" || name == "rt_pqueue_try_peek" ||
        name == "Zanna.Collections.Heap.Pop" || name == "Zanna.Collections.Heap.TryPop" ||
        name == "Zanna.Collections.Heap.Peek" || name == "Zanna.Collections.Heap.TryPeek") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_iter_next" || name == "rt_iter_peek" ||
        name == "Zanna.Collections.Iterator.Next" || name == "Zanna.Collections.Iterator.Peek") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_weakmap_get" || name == "Zanna.Collections.WeakMap.Get") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_arr_str_put" || name == "rt_arr_obj_put") {
        effects.retainedArgMask = 0b100;
        return effects;
    }

    if (name == "rt_obj_new_i64" || name == "rt_box_i64" || name == "rt_box_f64" ||
        name == "rt_box_i1" || name == "rt_box_i1_bool" || name == "rt_box_value_type" ||
        name == "Zanna.Core.Box.I64" || name == "Zanna.Core.Box.F64" ||
        name == "Zanna.Core.Box.I1" || name == "Zanna.Core.Box.ValueType") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    // Graphics3D spatial queries allocate result objects. Keep these explicit:
    // nearby accessors such as PhysicsHit3D.Body and PhysicsHitList3D.Get are
    // borrowed references owned by the hit/list and must not be released as
    // call-result temporaries.
    if (name == "Zanna.Graphics3D.PhysicsWorld3D.Raycast" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.RaycastAll" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.SweepSphere" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.SweepCapsule" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.OverlapSphere" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.OverlapAABB" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.ProbeLedge" ||
        name == "Zanna.Graphics3D.PhysicsWorld3D.ProbeVault" || name == "rt_world3d_raycast" ||
        name == "rt_world3d_raycast_all" || name == "rt_world3d_sweep_sphere" ||
        name == "rt_world3d_sweep_capsule" || name == "rt_world3d_overlap_sphere" ||
        name == "rt_world3d_overlap_aabb" || name == "rt_world3d_probe_ledge" ||
        name == "rt_world3d_probe_vault") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    // Snapshot/value APIs allocate a fresh Vec3 even when their names look
    // like ordinary getters. The caller owns that returned reference.
    if (detail::returnsFreshVec3(name)) {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    // Geometry math operations and 3D value snapshots return newly allocated
    // immutable objects. This mirrors the public runtime manifest's owned-math
    // contract and lets the frontend release nested transform temporaries.
    if (detail::returnsFreshMathValue(name) || detail::returnsFresh3DMathSnapshot(name)) {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    // Game3D audio playback returns a newly created SoundSource3D in addition
    // to retaining it in the subsystem's active-source list. The caller owns
    // the original reference.
    if (name == "Zanna.Game3D.Sound3D.PlayAt" || name == "Zanna.Game3D.Sound3D.PlayAttached" ||
        name == "rt_game3d_audio_play_at" || name == "rt_game3d_audio_play_attached") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    if (name == "rt_box_str" || name == "Zanna.Core.Box.Str") {
        effects.retainedArgMask = 0b1;
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        effects.returnsKnownObject = true;
        return effects;
    }

    if (name == "rt_box_try_to_str") {
        effects.ownedOutArgMask = 0b10;
        return effects;
    }

    if (name == "rt_line_input_ch_err") {
        effects.ownedOutArgMask = 0b10;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_bitset_to_string" || name == "rt_bytes_to_str" || name == "rt_bytes_to_hex" ||
        name == "rt_bytes_to_base64" || name == "rt_orderedmap_key_at" ||
        name == "rt_trie_longest_prefix" || name == "rt_sortedset_first" ||
        name == "rt_sortedset_last" || name == "rt_sortedset_floor" ||
        name == "rt_sortedset_ceil" || name == "rt_sortedset_lower" ||
        name == "rt_sortedset_higher" || name == "rt_sortedset_at" ||
        name == "Zanna.Collections.BitSet.ToString" || name == "Zanna.Collections.Bytes.ToStr" ||
        name == "Zanna.Collections.Bytes.ToHex" || name == "Zanna.Collections.Bytes.ToBase64" ||
        name == "Zanna.Collections.OrderedMap.KeyAt" ||
        name == "Zanna.Collections.Trie.LongestPrefix" ||
        name == "Zanna.Collections.SortedSet.First" || name == "Zanna.Collections.SortedSet.Last" ||
        name == "Zanna.Collections.SortedSet.Floor" || name == "Zanna.Collections.SortedSet.Ceil" ||
        name == "Zanna.Collections.SortedSet.Lower" ||
        name == "Zanna.Collections.SortedSet.Higher" || name == "Zanna.Collections.SortedSet.At") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_map_keys" || name == "rt_map_values" || name == "rt_orderedmap_keys" ||
        name == "rt_orderedmap_values" || name == "rt_frozenmap_keys" ||
        name == "rt_frozenmap_values" || name == "rt_frozenset_items" || name == "rt_bag_items" ||
        name == "rt_bag_union" || name == "rt_bag_intersect" || name == "rt_bag_diff" ||
        name == "rt_set_items" || name == "rt_set_union" || name == "rt_set_intersect" ||
        name == "rt_set_diff" || name == "rt_sparse_indices" || name == "rt_sparse_values" ||
        name == "rt_multimap_get" || name == "rt_multimap_keys" || name == "rt_intmap_keys" ||
        name == "rt_intmap_values" || name == "rt_countmap_keys" ||
        name == "rt_countmap_most_common" || name == "rt_lrucache_keys" ||
        name == "rt_lrucache_values" || name == "rt_weakmap_keys" || name == "rt_pqueue_to_seq" ||
        name == "rt_ring_to_seq" || name == "rt_deque_to_seq" || name == "rt_deque_to_list" ||
        name == "rt_stack_to_seq" || name == "rt_stack_to_list" || name == "rt_queue_to_seq" ||
        name == "rt_queue_to_list" || name == "rt_list_to_seq" || name == "rt_list_to_set" ||
        name == "rt_list_to_stack" || name == "rt_list_to_queue" || name == "rt_seq_to_list" ||
        name == "rt_seq_to_set" || name == "rt_seq_to_stack" || name == "rt_seq_to_queue" ||
        name == "rt_seq_to_deque" || name == "rt_seq_to_bag" || name == "rt_seq_slice" ||
        name == "rt_seq_take" || name == "rt_seq_drop" || name == "rt_seq_keep_wrapper" ||
        name == "rt_seq_reject_wrapper" || name == "rt_seq_apply_wrapper" ||
        name == "rt_seq_take_while_wrapper" || name == "rt_seq_drop_while_wrapper" ||
        name == "rt_trie_keys" || name == "rt_trie_with_prefix" || name == "rt_sortedset_items" ||
        name == "rt_sortedset_range" || name == "rt_sortedset_take" ||
        name == "rt_sortedset_skip" || name == "rt_sortedset_union" ||
        name == "rt_sortedset_intersect" || name == "rt_sortedset_diff" ||
        name == "rt_iter_to_seq") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_weakref_new" || name == "Zanna.Memory.WeakRef.New") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_weakref_get" || name == "Zanna.Memory.WeakRef.Get") {
        effects.returnsOwned = true;
        return effects;
    }

    if (name == "rt_weakref_free" || name == "Zanna.Memory.WeakRef.Free") {
        effects.consumedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_weakref_reset" || name == "Zanna.Memory.WeakRef.Reset") {
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_unbox_str" || name == "Zanna.Core.Box.ToStr" ||
        ((detail::startsWith(name, "rt_cipher_") || detail::startsWith(name, "rt_aes_")) &&
         (detail::contains(name, "_decrypt") &&
          (detail::endsWith(name, "_result") || detail::contains(name, "_try_")))) ||
        (detail::startsWith(name, "Zanna.Crypto.") &&
         (detail::contains(name, ".Decrypt") || detail::contains(name, ".TryDecrypt")) &&
         (detail::endsWith(name, "Result") || detail::contains(name, ".TryDecrypt"))) ||
        name == "rt_box_to_i64_option" || name == "rt_box_to_f64_option" ||
        name == "rt_box_to_i1_option" || name == "rt_box_to_str_option" ||
        name == "Zanna.Core.Box.ToI64Option" || name == "Zanna.Core.Box.ToF64Option" ||
        name == "Zanna.Core.Box.ToI1Option" || name == "Zanna.Core.Box.ToStrOption" ||
        name == "rt_obj_to_string" || name == "Zanna.Core.Object.ToString" ||
        name == "rt_obj_type_name" || name == "Zanna.Core.Object.TypeName" ||
        name == "Zanna.Core.Object.get_TypeName" || name == "rt_parse_double_option" ||
        name == "rt_parse_int64_option" || name == "rt_parse_bool_option" ||
        name == "Zanna.Core.Parse.TryDouble" || name == "Zanna.Core.Parse.TryDouble" ||
        name == "Zanna.Core.Parse.TryInt" || name == "Zanna.Core.Parse.TryBool" ||
        name == "rt_datetime_try_parse_option" || name == "Zanna.Time.DateTime.TryParseOption" ||
        name == "rt_queue_try_pop_option" || name == "Zanna.Collections.Queue.TryPopOption" ||
        name == "rt_stack_try_pop_option" || name == "Zanna.Collections.Stack.TryPopOption" ||
        name == "rt_pqueue_try_pop_option" || name == "Zanna.Collections.Heap.TryPopOption" ||
        name == "rt_pqueue_try_peek_option" || name == "Zanna.Collections.Heap.TryPeekOption" ||
        name == "rt_deque_try_pop_front_option" ||
        name == "Zanna.Collections.Deque.TryPopFrontOption" ||
        name == "rt_deque_try_pop_back_option" ||
        name == "Zanna.Collections.Deque.TryPopBackOption" ||
        name == "rt_concqueue_try_dequeue_option" ||
        name == "Zanna.Threads.ConcurrentQueue.TryDequeueOption" ||
        name == "rt_channel_try_recv_option" || name == "Zanna.Threads.Channel.TryRecvOption" ||
        name == "rt_future_try_get_option" || name == "Zanna.Threads.Future.TryGetOption" ||
        name == "rt_locale_try_parse_option" ||
        name == "Zanna.Localization.Locale.TryParseOption" ||
        name == "rt_message_bundle_try_get_option" ||
        name == "Zanna.Localization.MessageBundle.TryGetOption" ||
        name == "rt_numformat_try_parse_decimal" ||
        name == "Zanna.Localization.NumberFormat.TryParseDecimal" ||
        name == "rt_numformat_try_parse_integer" ||
        name == "Zanna.Localization.NumberFormat.TryParseInteger" ||
        name == "rt_numformat_try_parse_currency" ||
        name == "Zanna.Localization.NumberFormat.TryParseCurrency" ||
        name == "rt_promise_get_future" || name == "Zanna.Threads.Promise.GetFuture" ||
        name == "rt_async_run" || name == "Zanna.Threads.Async.Run" ||
        name == "rt_async_run_owned" || name == "Zanna.Threads.Async.RunOwned" ||
        name == "rt_async_run_cancellable" || name == "Zanna.Threads.Async.RunCancellable" ||
        name == "rt_async_run_cancellable_owned" ||
        name == "Zanna.Threads.Async.RunCancellableOwned" || name == "rt_async_delay" ||
        name == "Zanna.Threads.Async.Delay" || name == "rt_async_all" ||
        name == "Zanna.Threads.Async.All" || name == "rt_async_any" ||
        name == "Zanna.Threads.Async.Any" || name == "rt_async_map" ||
        name == "Zanna.Threads.Async.Map" || name == "rt_async_map_owned" ||
        name == "Zanna.Threads.Async.MapOwned" || name == "Zanna.Core.Convert.ToString_Int" ||
        name == "Zanna.Core.Convert.ToString_Double" || name == "Zanna.Core.Convert.ToStringInt" ||
        name == "Zanna.Core.Convert.ToStringDouble") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_msgbus_new" || name == "Zanna.Core.MessageBus.New" ||
        name == "rt_msgbus_callback_new" || name == "Zanna.Core.MessageBus.Callback" ||
        name == "rt_msgbus_topics" || name == "Zanna.Core.MessageBus.Topics") {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (detail::startsWith(name, "Zanna.Collections.") &&
        (detail::endsWith(name, ".Items") || detail::endsWith(name, ".Keys") ||
         detail::endsWith(name, ".Values") || detail::endsWith(name, ".ToSeq") ||
         detail::endsWith(name, ".ToList") || detail::endsWith(name, ".ToSet") ||
         detail::endsWith(name, ".ToStack") || detail::endsWith(name, ".ToQueue") ||
         detail::endsWith(name, ".Union") || detail::endsWith(name, ".Intersect") ||
         detail::endsWith(name, ".Diff") || detail::endsWith(name, ".Merge") ||
         detail::endsWith(name, ".Range") || detail::endsWith(name, ".Take") ||
         detail::endsWith(name, ".Skip") || detail::endsWith(name, ".And") ||
         detail::endsWith(name, ".Or") || detail::endsWith(name, ".Xor") ||
         detail::endsWith(name, ".Not") || detail::endsWith(name, ".Empty") ||
         detail::endsWith(name, ".WithCapacity") || detail::endsWith(name, ".NewMax") ||
         detail::endsWith(name, ".NewDefault"))) {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_msgbus_subscribe" || name == "Zanna.Core.MessageBus.Subscribe") {
        effects.retainedArgMask = 0b110;
        effects.mayAllocate = true;
        return effects;
    }

    if (name == "rt_obj_retain_maybe" || name == "rt_obj_retain_known") {
        effects.retainedArgMask = 0b1;
        return effects;
    }

    if (name == "rt_obj_release_check0" || name == "rt_obj_release_known_check0") {
        effects.consumedArgMask = 0b1;
        return effects;
    }

    if (detail::startsWith(name, "rt_") &&
        (detail::endsWith(name, "_new") || detail::endsWith(name, "_clone") ||
         detail::contains(name, "_from_"))) {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    if (detail::startsWith(name, "Zanna.") &&
        (detail::endsWith(name, ".New") || detail::endsWith(name, ".Clone") ||
         detail::contains(name, ".From"))) {
        effects.returnsOwned = true;
        effects.mayAllocate = true;
        return effects;
    }

    return effects;
}

} // namespace il::runtime
