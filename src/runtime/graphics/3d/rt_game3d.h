//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d.h
// Purpose: Runtime-backed Zanna.Game3D ergonomic layer over the lower-level
//   Graphics3D, Physics3D, input, audio, and post-FX primitives. Bundles a
//   window, camera, scene graph, physics world, input, audio, and effects into
//   a single World3D and exposes batteries-included entities, prefabs,
//   material/lighting/post-FX presets, and camera controllers.
//
// Key invariants:
//   - Every `void *` parameter/return is an opaque GC-managed runtime handle;
//     callers never dereference them directly.
//   - The integer-returning accessor families (layers/keys/mouse/etc.) surface
//     the RT_GAME3D_* enum constants below as callable functions so frontends
//     can bind them as read-only properties.
//   - Builder-style setters whose names lack `_prop` return their receiver to
//     allow fluent chaining; the `*_set_*_prop` variants are void property writers.
//   - Angles are in degrees, distances/positions in world units, time in seconds
//     unless a name says otherwise (e.g. `_ms`); colors are RGB channels 0.0–1.0.
//
// Ownership/Lifetime:
//   - World3D owns its canvas, camera, scene, physics, input, audio, and effects
//     sub-objects; destroying or GC-finalizing the world tears them all down.
//   - Entities, bodies, materials, meshes, and clips are GC-managed handles that
//     stay alive while referenced by the world or by frontend variables.
//   - Accessor functions return borrowed handles owned by their parent; callers
//     must not free them.
//
// Links: rt_game3d.c, rt_graphics3d_ids.h, render/rt_canvas3d.h,
//   physics/rt_physics3d.h, scene/rt_scene3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the complete runtime-backed Zanna.Game3D C ABI.
/// @details The API composes rendering, physics, input, audio, assets, entities, controllers,
///   streaming, effects, debugging, and deterministic run modes behind opaque GC-managed handles.
///   Unless a function explicitly creates or retains a result, object accessors return borrowed
///   handles owned by their parent object.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Enum constants — mirrored by the integer accessor families below
//=========================================================================

/// @brief Collision/render layer bits used to build LayerMask filters.
enum {
    RT_GAME3D_LAYER_WORLD = 1,   ///< Static world geometry (ground, walls).
    RT_GAME3D_LAYER_DYNAMIC = 2, ///< Movable dynamic bodies (props, crates).
    RT_GAME3D_LAYER_PLAYER = 4,  ///< Player-controlled entities.
    RT_GAME3D_LAYER_TRIGGER = 8, ///< Non-solid trigger volumes.
    RT_GAME3D_LAYER_DEBRIS = 16, ///< Short-lived debris/particles.
};

/// @brief Rigid-body collision shape kinds selectable on a BodyDef.
enum {
    RT_GAME3D_BODY_SHAPE_BOX = 0,     ///< Axis-aligned box collider.
    RT_GAME3D_BODY_SHAPE_SPHERE = 1,  ///< Sphere collider.
    RT_GAME3D_BODY_SHAPE_CAPSULE = 2, ///< Upright capsule collider.
};

/// @brief Transform synchronization direction between a physics body and its scene node.
enum {
    RT_GAME3D_SYNC_NODE_FROM_BODY = 0,             ///< Physics drives the node (dynamic bodies).
    RT_GAME3D_SYNC_BODY_FROM_NODE = 1,             ///< Node drives the body (scripted/kinematic).
    RT_GAME3D_SYNC_NODE_FROM_ANIM_ROOT_MOTION = 2, ///< Animation root motion drives the node.
    RT_GAME3D_SYNC_TWO_WAY_KINEMATIC = 3,          ///< Bidirectional kinematic sync.
};

/// @brief Material alpha-handling mode.
enum {
    RT_GAME3D_ALPHA_OPAQUE = 0, ///< Fully opaque, depth-write on.
    RT_GAME3D_ALPHA_MASK = 1,   ///< Alpha-tested cutout (binary coverage).
    RT_GAME3D_ALPHA_BLEND = 2,  ///< Order-dependent translucent blending.
};

/// @brief Built-in surface shading models selectable per material.
enum {
    RT_GAME3D_SHADING_PHONG = 0,    ///< Classic Blinn-Phong specular.
    RT_GAME3D_SHADING_TOON = 1,     ///< Banded/cel cartoon shading.
    RT_GAME3D_SHADING_PBR = 2,      ///< Physically based metallic/roughness workflow.
    RT_GAME3D_SHADING_UNLIT = 3,    ///< Flat albedo, no lighting applied.
    RT_GAME3D_SHADING_FRESNEL = 4,  ///< Rim/Fresnel emphasis.
    RT_GAME3D_SHADING_EMISSIVE = 5, ///< Self-illuminated, unlit by scene lights.
};

/// @brief Render quality presets trading fidelity against performance.
enum {
    RT_GAME3D_QUALITY_PERFORMANCE = 0, ///< Lowest cost, effects trimmed.
    RT_GAME3D_QUALITY_BALANCED = 1,    ///< Default middle ground.
    RT_GAME3D_QUALITY_CINEMATIC = 2,   ///< Highest fidelity.
};

/// @brief Collision-event phase selector used when querying World3D events.
enum {
    RT_GAME3D_COLLISION_ENTER = 0, ///< First frame two colliders touch.
    RT_GAME3D_COLLISION_STAY = 1,  ///< Continuing contact frames.
    RT_GAME3D_COLLISION_EXIT = 2,  ///< Frame contact ends.
    RT_GAME3D_COLLISION_ANY = 3,   ///< Match any of the above phases.
};

//=========================================================================
// Layers — collision-layer bit constants (Zanna.Game3D.Layers)
//=========================================================================

/// @brief Layer bit for static world geometry (RT_GAME3D_LAYER_WORLD).
/// @return The documented runtime integer constant.
int64_t rt_game3d_layers_world(void);
/// @brief Layer bit for movable dynamic bodies (RT_GAME3D_LAYER_DYNAMIC).
/// @return The documented runtime integer constant.
int64_t rt_game3d_layers_dynamic(void);
/// @brief Layer bit for player-controlled entities (RT_GAME3D_LAYER_PLAYER).
/// @return The documented runtime integer constant.
int64_t rt_game3d_layers_player(void);
/// @brief Layer bit for non-solid trigger volumes (RT_GAME3D_LAYER_TRIGGER).
/// @return The documented runtime integer constant.
int64_t rt_game3d_layers_trigger(void);
/// @brief Layer bit for short-lived debris/particles (RT_GAME3D_LAYER_DEBRIS).
/// @return The documented runtime integer constant.
int64_t rt_game3d_layers_debris(void);

//=========================================================================
// BodyShape — rigid-body shape kind constants (Zanna.Game3D.BodyShape)
//=========================================================================

/// @brief Box shape kind constant (RT_GAME3D_BODY_SHAPE_BOX).
/// @return The documented runtime integer constant.
int64_t rt_game3d_body_shape_box(void);
/// @brief Sphere shape kind constant (RT_GAME3D_BODY_SHAPE_SPHERE).
/// @return The documented runtime integer constant.
int64_t rt_game3d_body_shape_sphere(void);
/// @brief Capsule shape kind constant (RT_GAME3D_BODY_SHAPE_CAPSULE).
/// @return The documented runtime integer constant.
int64_t rt_game3d_body_shape_capsule(void);

//=========================================================================
// SyncMode — body/node transform sync constants (Zanna.Game3D.SyncMode)
//=========================================================================

/// @brief Sync constant: physics body drives the scene node.
/// @return The documented runtime integer constant.
int64_t rt_game3d_sync_mode_node_from_body(void);
/// @brief Sync constant: scene node drives the physics body.
/// @return The documented runtime integer constant.
int64_t rt_game3d_sync_mode_body_from_node(void);
/// @brief Sync constant: animation root motion drives the scene node.
/// @return The documented runtime integer constant.
int64_t rt_game3d_sync_mode_node_from_anim_root_motion(void);
/// @brief Sync constant: bidirectional kinematic body/node coupling.
/// @return The documented runtime integer constant.
int64_t rt_game3d_sync_mode_two_way_kinematic(void);

//=========================================================================
// AlphaMode — material alpha mode constants (Zanna.Game3D.AlphaMode)
//=========================================================================

/// @brief Opaque alpha-mode constant (RT_GAME3D_ALPHA_OPAQUE).
/// @return The documented runtime integer constant.
int64_t rt_game3d_alpha_mode_opaque(void);
/// @brief Alpha-tested cutout mode constant (RT_GAME3D_ALPHA_MASK).
/// @return The documented runtime integer constant.
int64_t rt_game3d_alpha_mode_mask(void);
/// @brief Translucent blend mode constant (RT_GAME3D_ALPHA_BLEND).
/// @return The documented runtime integer constant.
int64_t rt_game3d_alpha_mode_blend(void);

//=========================================================================
// ShadingModel — surface shading model constants (Zanna.Game3D.ShadingModel)
//=========================================================================

/// @brief Blinn-Phong shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_phong(void);
/// @brief Toon/cel shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_toon(void);
/// @brief Physically based (PBR) shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_pbr(void);
/// @brief Fresnel/rim shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_fresnel(void);
/// @brief Emissive (self-lit) shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_emissive(void);
/// @brief Unlit flat-albedo shading-model constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_shading_model_unlit(void);

//=========================================================================
// Quality — render quality preset constants (Zanna.Game3D.Quality)
//=========================================================================

/// @brief Performance quality preset constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_quality_performance(void);
/// @brief Balanced quality preset constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_quality_balanced(void);
/// @brief Cinematic quality preset constant.
/// @return The documented runtime integer constant.
int64_t rt_game3d_quality_cinematic(void);

//=========================================================================
// CollisionPhase — collision-event phase constants (Zanna.Game3D.CollisionPhase)
//=========================================================================

/// @brief Enter-phase constant (first contact frame).
/// @return The documented runtime integer constant.
int64_t rt_game3d_collision_enter(void);
/// @brief Stay-phase constant (ongoing contact frames).
/// @return The documented runtime integer constant.
int64_t rt_game3d_collision_stay(void);
/// @brief Exit-phase constant (contact-end frame).
/// @return The documented runtime integer constant.
int64_t rt_game3d_collision_exit(void);
/// @brief Wildcard constant matching any collision phase.
/// @return The documented runtime integer constant.
int64_t rt_game3d_collision_any(void);

//=========================================================================
// Keys — keyboard key-code constants (Zanna.Game3D.Keys)
//=========================================================================

/// @brief Key code for the W key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_w(void);
/// @brief Key code for the A key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_a(void);
/// @brief Key code for the S key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_s(void);
/// @brief Key code for the D key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_d(void);
/// @brief Key code for the spacebar.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_space(void);
/// @brief Key code for the Escape key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_escape(void);
/// @brief Key code for the Shift modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_shift(void);
/// @brief Key code for the Ctrl modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_ctrl(void);
/// @brief Key code for the Up arrow.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_up(void);
/// @brief Key code for the Down arrow.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_down(void);
/// @brief Key code for the Left arrow.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_left(void);
/// @brief Key code for the Right arrow.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_right(void);
/// @brief Key code for the F11 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f11(void);

// --- Full keyboard coverage: remaining A-Z / 0-9 / F1-F12 / navigation /
//     modifier / punctuation / numpad keys, completing Zanna.Game3D.Keys so any
//     physical key is reachable as Keys.get_<Name>(). Backing: rt_input table. ---
/// @brief Key code for the B key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_b(void);
/// @brief Key code for the C key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_c(void);
/// @brief Key code for the E key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_e(void);
/// @brief Key code for the F key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f(void);
/// @brief Key code for the G key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_g(void);
/// @brief Key code for the H key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_h(void);
/// @brief Key code for the I key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_i(void);
/// @brief Key code for the J key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_j(void);
/// @brief Key code for the K key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_k(void);
/// @brief Key code for the L key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_l(void);
/// @brief Key code for the M key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_m(void);
/// @brief Key code for the N key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_n(void);
/// @brief Key code for the O key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_o(void);
/// @brief Key code for the P key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_p(void);
/// @brief Key code for the Q key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_q(void);
/// @brief Key code for the R key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_r(void);
/// @brief Key code for the T key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_t(void);
/// @brief Key code for the U key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_u(void);
/// @brief Key code for the V key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_v(void);
/// @brief Key code for the X key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_x(void);
/// @brief Key code for the Y key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_y(void);
/// @brief Key code for the Z key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_z(void);
/// @brief Key code for the 0 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_0(void);
/// @brief Key code for the 1 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_1(void);
/// @brief Key code for the 2 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_2(void);
/// @brief Key code for the 3 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_3(void);
/// @brief Key code for the 4 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_4(void);
/// @brief Key code for the 5 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_5(void);
/// @brief Key code for the 6 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_6(void);
/// @brief Key code for the 7 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_7(void);
/// @brief Key code for the 8 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_8(void);
/// @brief Key code for the 9 key (top row).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_9(void);
/// @brief Key code for the F1 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f1(void);
/// @brief Key code for the F2 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f2(void);
/// @brief Key code for the F3 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f3(void);
/// @brief Key code for the F4 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f4(void);
/// @brief Key code for the F5 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f5(void);
/// @brief Key code for the F6 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f6(void);
/// @brief Key code for the F7 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f7(void);
/// @brief Key code for the F8 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f8(void);
/// @brief Key code for the F9 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f9(void);
/// @brief Key code for the F10 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f10(void);
/// @brief Key code for the F12 function key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_f12(void);
/// @brief Key code for the Enter/Return key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_enter(void);
/// @brief Key code for the Tab key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_tab(void);
/// @brief Key code for the Backspace key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_backspace(void);
/// @brief Key code for the Insert key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_insert(void);
/// @brief Key code for the Delete key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_delete(void);
/// @brief Key code for the Home key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_home(void);
/// @brief Key code for the End key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_end(void);
/// @brief Key code for the Page Up key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_pageup(void);
/// @brief Key code for the Page Down key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_pagedown(void);
/// @brief Key code for the Alt modifier (left).
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_alt(void);
/// @brief Key code for the left Shift modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_lshift(void);
/// @brief Key code for the right Shift modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_rshift(void);
/// @brief Key code for the left Ctrl modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_lctrl(void);
/// @brief Key code for the right Ctrl modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_rctrl(void);
/// @brief Key code for the left Alt modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_lalt(void);
/// @brief Key code for the right Alt modifier.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_ralt(void);
/// @brief Key code for the apostrophe/quote key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_quote(void);
/// @brief Key code for the comma key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_comma(void);
/// @brief Key code for the minus/hyphen key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_minus(void);
/// @brief Key code for the period key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_period(void);
/// @brief Key code for the forward-slash key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_slash(void);
/// @brief Key code for the semicolon key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_semicolon(void);
/// @brief Key code for the equals key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_equals(void);
/// @brief Key code for the left-bracket key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_lbracket(void);
/// @brief Key code for the backslash key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_backslash(void);
/// @brief Key code for the right-bracket key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_rbracket(void);
/// @brief Key code for the grave/backtick key.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_grave(void);
/// @brief Key code for the numpad 0.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num0(void);
/// @brief Key code for the numpad 1.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num1(void);
/// @brief Key code for the numpad 2.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num2(void);
/// @brief Key code for the numpad 3.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num3(void);
/// @brief Key code for the numpad 4.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num4(void);
/// @brief Key code for the numpad 5.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num5(void);
/// @brief Key code for the numpad 6.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num6(void);
/// @brief Key code for the numpad 7.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num7(void);
/// @brief Key code for the numpad 8.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num8(void);
/// @brief Key code for the numpad 9.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_num9(void);
/// @brief Key code for the numpad decimal point.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_numdot(void);
/// @brief Key code for the numpad divide.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_numdiv(void);
/// @brief Key code for the numpad multiply.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_nummul(void);
/// @brief Key code for the numpad subtract.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_numsub(void);
/// @brief Key code for the numpad add.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_numadd(void);
/// @brief Key code for the numpad Enter.
/// @return The documented runtime integer constant.
int64_t rt_game3d_key_numenter(void);

//=========================================================================
// MouseButtons — mouse-button code constants (Zanna.Game3D.MouseButtons)
//=========================================================================

/// @brief Button code for the left mouse button.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_mouse_left(void);
/// @brief Button code for the right mouse button.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_mouse_right(void);
/// @brief Button code for the middle (wheel) mouse button.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_mouse_middle(void);
/// @brief Button code for the first extra (X1) mouse button.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_mouse_x1(void);
/// @brief Button code for the second extra (X2) mouse button.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_mouse_x2(void);

//=========================================================================
// LayerMask — collision-filter bitmask object (Zanna.Game3D.LayerMask)
//=========================================================================

/// @brief Create an empty layer mask that matches no layers.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_layermask_none(void);
/// @brief Create a layer mask with every layer bit set.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_layermask_all(void);
/// @brief Create a layer mask containing exactly the given layer bit.
/// @param layer Positive single-bit collision or render layer.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_layermask_of(int64_t layer);
/// @brief Get the raw bitfield backing a layer mask.
/// @param mask Value supplied for the mask argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_layermask_get_bits(void *mask);
/// @brief Overwrite the raw bitfield backing a layer mask.
/// @param mask Value supplied for the mask argument.
/// @param bits Layer-mask bitfield.
void rt_game3d_layermask_set_bits(void *mask, int64_t bits);
/// @brief Return a mask with the given layer added (fluent; may return a new handle).
/// @param mask Value supplied for the mask argument.
/// @param layer Positive single-bit collision or render layer.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_layermask_include(void *mask, int64_t layer);
/// @brief Test whether the mask includes the given layer bit.
/// @param mask Value supplied for the mask argument.
/// @param layer Positive single-bit collision or render layer.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_layermask_includes(void *mask, int64_t layer);

//=========================================================================
// BodyDef — rigid-body construction descriptor (Zanna.Game3D.BodyDef)
//=========================================================================

/// @brief Build a dynamic box body definition from half-extents and mass.
/// @param half_x Value supplied for the half x argument.
/// @param half_y Value supplied for the half y argument.
/// @param half_z Value supplied for the half z argument.
/// @param mass Value supplied for the mass argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_box(double half_x, double half_y, double half_z, double mass);
/// @brief Build a dynamic sphere body definition from radius and mass.
/// @param radius Value supplied for the radius argument.
/// @param mass Value supplied for the mass argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_sphere(double radius, double mass);
/// @brief Build a dynamic capsule body definition from radius, height, and mass.
/// @param radius Value supplied for the radius argument.
/// @param height Value supplied for the height argument.
/// @param mass Value supplied for the mass argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_capsule(double radius, double height, double mass);
/// @brief Build a static (immovable) box body definition from half-extents.
/// @param half_x Value supplied for the half x argument.
/// @param half_y Value supplied for the half y argument.
/// @param half_z Value supplied for the half z argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_static_box(double half_x, double half_y, double half_z);
/// @brief Build a static ground-plane body definition spanning the given size.
/// @param size Value supplied for the size argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_static_plane(double size);
/// @brief Get the collision shape kind (RT_GAME3D_BODY_SHAPE_*) of a body def.
/// @param def Value supplied for the def argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_body_def_get_shape(void *def);
/// @brief Set the collision shape kind of a body def.
/// @param def Value supplied for the def argument.
/// @param shape Value supplied for the shape argument.
void rt_game3d_body_def_set_shape(void *def, int64_t shape);
/// @brief Get the body mass in kilograms.
/// @param def Value supplied for the def argument.
/// @return The documented floating-point result.
double rt_game3d_body_def_get_mass(void *def);
/// @brief Set the body mass in kilograms.
/// @param def Value supplied for the def argument.
/// @param mass Value supplied for the mass argument.
void rt_game3d_body_def_set_mass(void *def, double mass);
/// @brief Get the surface friction coefficient.
/// @param def Value supplied for the def argument.
/// @return The documented floating-point result.
double rt_game3d_body_def_get_friction(void *def);
/// @brief Set the surface friction coefficient.
/// @param def Value supplied for the def argument.
/// @param friction Value supplied for the friction argument.
void rt_game3d_body_def_set_friction(void *def, double friction);
/// @brief Get the restitution (bounciness) coefficient.
/// @param def Value supplied for the def argument.
/// @return The documented floating-point result.
double rt_game3d_body_def_get_restitution(void *def);
/// @brief Set the restitution (bounciness) coefficient.
/// @param def Value supplied for the def argument.
/// @param restitution Value supplied for the restitution argument.
void rt_game3d_body_def_set_restitution(void *def, double restitution);
/// @brief True if the body is static (never integrated by the solver).
/// @param def Value supplied for the def argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_body_def_get_static(void *def);
/// @brief Mark the body static (true) or dynamic (false).
/// @param def Value supplied for the def argument.
/// @param is_static Boolean state used by the operation.
void rt_game3d_body_def_set_static(void *def, int8_t is_static);
/// @brief True if the body is kinematic (script-driven, infinite mass).
/// @param def Value supplied for the def argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_body_def_get_kinematic(void *def);
/// @brief Mark the body kinematic (true) or simulated (false).
/// @param def Value supplied for the def argument.
/// @param is_kinematic Boolean state used by the operation.
void rt_game3d_body_def_set_kinematic(void *def, int8_t is_kinematic);
/// @brief True if the body is a non-solid trigger (overlap events only).
/// @param def Value supplied for the def argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_body_def_get_trigger(void *def);
/// @brief Mark the body as a trigger volume (true) or solid collider (false).
/// @param def Value supplied for the def argument.
/// @param is_trigger Boolean state used by the operation.
void rt_game3d_body_def_set_trigger(void *def, int8_t is_trigger);
/// @brief True if continuous collision detection (CCD) is enabled.
/// @param def Value supplied for the def argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_body_def_get_use_ccd(void *def);
/// @brief Enable or disable continuous collision detection for fast bodies.
/// @param def Value supplied for the def argument.
/// @param use_ccd Value supplied for the use ccd argument.
void rt_game3d_body_def_set_use_ccd(void *def, int8_t use_ccd);
/// @brief Get the collision layer this body belongs to.
/// @param def Value supplied for the def argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_body_def_get_layer(void *def);
/// @brief Set the collision layer this body belongs to (property setter).
/// @param def Value supplied for the def argument.
/// @param layer Positive single-bit collision or render layer.
void rt_game3d_body_def_set_layer_prop(void *def, int64_t layer);
/// @brief Get the collision mask selecting which layers this body collides with.
/// @param def Value supplied for the def argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_get_mask(void *def);
/// @brief Set the collision mask of this body (property setter).
/// @param def Value supplied for the def argument.
/// @param mask Value supplied for the mask argument.
void rt_game3d_body_def_set_mask_prop(void *def, void *mask);
/// @brief Get the body/node transform sync mode (RT_GAME3D_SYNC_*).
/// @param def Value supplied for the def argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_body_def_get_sync_mode(void *def);
/// @brief Set the body/node transform sync mode (property setter).
/// @param def Value supplied for the def argument.
/// @param sync_mode Value supplied for the sync mode argument.
void rt_game3d_body_def_set_sync_mode_prop(void *def, int64_t sync_mode);
/// @brief Fluent setter: assign the collision layer and return the def.
/// @param def Value supplied for the def argument.
/// @param layer Positive single-bit collision or render layer.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_body_def_with_layer(void *def, int64_t layer);
/// @brief Fluent setter: assign the collision mask and return the def.
/// @param def Value supplied for the def argument.
/// @param mask Value supplied for the mask argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_body_def_with_mask(void *def, void *mask);
/// @brief Fluent setter: mark the def as a trigger and return it.
/// @param def Value supplied for the def argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_body_def_as_trigger(void *def);
/// @brief Fluent setter: assign the sync mode and return the def.
/// @param def Value supplied for the def argument.
/// @param sync_mode Value supplied for the sync mode argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_body_def_with_sync(void *def, int64_t sync_mode);

//=========================================================================
// Collision3DEvent — per-contact collision event record (Zanna.Game3D.Collision3DEvent)
//=========================================================================

/// @brief Get the event phase (RT_GAME3D_COLLISION_*).
/// @param event Value supplied for the event argument.
/// @return One of the RT_GAME3D_COLLISION_* phase values.
int64_t rt_game3d_collision_event_get_phase(void *event);
/// @brief Get the first entity involved in the collision.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_get_a(void *event);
/// @brief Get the second entity involved in the collision.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_get_b(void *event);
/// @brief Get the underlying low-level physics collision event.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_get_raw(void *event);
/// @brief True if either body in the contact is a trigger volume.
/// @param event Value supplied for the event argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_collision_event_get_is_trigger(void *event);
/// @brief Get the relative approach speed of the two bodies at contact.
/// @param event Value supplied for the event argument.
/// @return The documented floating-point result.
double rt_game3d_collision_event_get_relative_speed(void *event);
/// @brief Get the normal impulse magnitude applied to resolve the contact.
/// @param event Value supplied for the event argument.
/// @return The documented floating-point result.
double rt_game3d_collision_event_get_normal_impulse(void *event);
/// @brief Get the number of contact points carried by the wrapped raw event.
/// @param event Value supplied for the event argument.
/// @return The number of contact points available through the indexed accessors.
int64_t rt_game3d_collision_event_get_contact_count(void *event);
/// @brief Get the world-space contact point as a Vec3.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_point(void *event);
/// @brief Get the world-space contact normal as a Vec3.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_normal(void *event);
/// @brief Get indexed world-space contact point as a Vec3.
/// @param event Value supplied for the event argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_contact_point(void *event, int64_t index);
/// @brief Get indexed world-space contact normal as a Vec3.
/// @param event Value supplied for the event argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_contact_normal(void *event, int64_t index);
/// @brief Get indexed signed contact separation.
/// @param event Value supplied for the event argument.
/// @param index Zero-based index of the requested item.
/// @return The documented floating-point result.
double rt_game3d_collision_event_contact_separation(void *event, int64_t index);
/// @brief Given one participating entity, return the other entity in the contact.
/// @param event Value supplied for the event argument.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_collision_event_other(void *event, void *entity);

//=========================================================================
// Input3D — keyboard/mouse input state (Zanna.Game3D.Input3D)
//=========================================================================

/// @brief Create a new input-state object bound to the active window.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_input_new(void);
/// @brief Get the mouse-look sensitivity multiplier.
/// @param input Input3D instance or snapshot used by the operation.
/// @return The documented floating-point result.
double rt_game3d_input_get_look_sensitivity(void *input);
/// @brief Set the mouse-look sensitivity multiplier.
/// @param input Input3D instance or snapshot used by the operation.
/// @param sensitivity Value supplied for the sensitivity argument.
void rt_game3d_input_set_look_sensitivity(void *input, double sensitivity);
/// @brief Sample fresh input and roll edge (pressed/released) state forward one frame.
/// @param input Input3D instance or snapshot used by the operation.
void rt_game3d_input_update(void *input);
/// @brief True while the given key is held down this frame.
/// @param input Input3D instance or snapshot used by the operation.
/// @param key Runtime keyboard key code.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_input_is_down(void *input, int64_t key);
/// @brief True only on the frame the given key transitions to down.
/// @param input Input3D instance or snapshot used by the operation.
/// @param key Runtime keyboard key code.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_input_pressed(void *input, int64_t key);
/// @brief True only on the frame the given key transitions to up.
/// @param input Input3D instance or snapshot used by the operation.
/// @param key Runtime keyboard key code.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_input_released(void *input, int64_t key);
/// @brief Get the per-frame mouse movement delta as a Vec2.
/// @param input Input3D instance or snapshot used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_input_mouse_delta(void *input);
/// @brief Absolute window-local cursor X for this frame's snapshot (ADR 0233).
/// @param input Input3D instance or snapshot used by the operation.
/// @return Cursor X in pixels, or zero when unavailable.
int64_t rt_game3d_input_get_mouse_x(void *input);
/// @brief Absolute window-local cursor Y for this frame's snapshot (ADR 0233).
/// @param input Input3D instance or snapshot used by the operation.
/// @return Cursor Y in pixels, or zero when unavailable.
int64_t rt_game3d_input_get_mouse_y(void *input);
/// @brief Absolute window-local cursor position as a fresh Vec2 (ADR 0233).
/// @param input Input3D instance or snapshot used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_input_mouse_position(void *input);
/// @brief True while the given mouse button is held down this frame.
/// @param input Input3D instance or snapshot used by the operation.
/// @param button Runtime mouse-button index.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_input_mouse_button(void *input, int64_t button);
/// @brief True only on the frame the given mouse button transitions to down.
/// @param input Input3D instance or snapshot used by the operation.
/// @param button Runtime mouse-button index.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_input_mouse_pressed(void *input, int64_t button);
/// @brief Get the mouse wheel scroll delta along Y for this frame.
/// @param input Input3D instance or snapshot used by the operation.
/// @return The documented floating-point result.
double rt_game3d_input_wheel_y(void *input);
/// @brief Get the WASD/arrow/space/shift movement axis as a normalized Vec3.
/// @param input Input3D instance or snapshot used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_input_move_axis(void *input);
/// @brief Get the mouse-look axis as a Vec2 (yaw/pitch delta scaled by sensitivity).
/// @param input Input3D instance or snapshot used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_input_look_axis(void *input);
/// @brief Capture and hide the OS cursor for relative mouse-look.
/// @param input Input3D instance or snapshot used by the operation.
void rt_game3d_input_capture_mouse(void *input);
/// @brief Release the captured cursor back to the OS.
/// @param input Input3D instance or snapshot used by the operation.
void rt_game3d_input_release_mouse(void *input);
/// @brief Enable/disable raw relative mouse-look (capture + OS raw deltas).
/// @param input Input3D instance or snapshot used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_input_set_relative_look(void *input, int8_t enabled);
/// @brief Bind a gamepad index into MoveAxis/LookAxis (-1 unbinds).
/// @param input Input3D instance or snapshot used by the operation.
/// @param pad Value supplied for the pad argument.
void rt_game3d_input_bind_pad(void *input, int64_t pad);
/// @brief Currently bound gamepad index (-1 when unbound).
/// @param input Input3D instance or snapshot used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_input_get_pad_bound(void *input);
/// @brief Set the right-stick look sensitivity (degrees/frame at full tilt).
/// @param input Input3D instance or snapshot used by the operation.
/// @param sensitivity Value supplied for the sensitivity argument.
void rt_game3d_input_set_pad_look_sensitivity(void *input, double sensitivity);
/// @brief Get the right-stick look sensitivity.
/// @param input Input3D instance or snapshot used by the operation.
/// @return The documented floating-point result.
double rt_game3d_input_get_pad_look_sensitivity(void *input);

//=========================================================================
// Entity3D — scene entity composing node, mesh, material, body, animator
//   (Zanna.Game3D.Entity3D)
//=========================================================================

/// @brief Create a new empty entity (a bare scene node with no renderable).
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_entity_new(void);
/// @brief Create an entity from an existing mesh and material.
/// @param mesh Mesh handle used by the operation.
/// @param material Material handle used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_entity_of(void *mesh, void *material);
/// @brief Wrap an existing scene node hierarchy as an entity (e.g. a loaded model).
/// @details If @p root is a Scene3D's implicit root, the source scene receives a new empty
///   implicit root and the complete former root hierarchy is transactionally transferred to the
///   entity. The source scene therefore remains valid, and no implicit scene root is reparented.
/// @param root Valid SceneNode3D hierarchy root to retain or transfer.
/// @return New group Entity3D that owns the hierarchy.
void *rt_game3d_entity_from_node(void *root);
/// @brief Get the entity's stable integer id.
/// @param entity Entity3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_entity_get_id(void *entity);
/// @brief Get the entity's underlying scene node.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_node(void *entity);
/// @brief Get the entity's mesh, or null if it has none.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_mesh(void *entity);
/// @brief Set the entity's mesh (property setter).
/// @param entity Entity3D instance used by the operation.
/// @param mesh Mesh handle used by the operation.
void rt_game3d_entity_set_mesh_prop(void *entity, void *mesh);
/// @brief Get the entity's material, or null if it has none.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_material(void *entity);
/// @brief Set the entity's material (property setter).
/// @param entity Entity3D instance used by the operation.
/// @param material Material handle used by the operation.
void rt_game3d_entity_set_material_prop(void *entity, void *material);
/// @brief Get the entity's attached physics body, or null if unattached.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_body(void *entity);
/// @brief Get the entity's attached animator, or null if none.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_anim(void *entity);
/// @brief Get the entity's collision layer.
/// @param entity Entity3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_entity_get_layer(void *entity);
/// @brief Set the entity's collision layer (property setter).
/// @param entity Entity3D instance used by the operation.
/// @param layer Positive single-bit collision or render layer.
void rt_game3d_entity_set_layer_prop(void *entity, int64_t layer);
/// @brief Get the entity's collision mask.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_collision_mask(void *entity);
/// @brief Set the entity's collision mask (property setter).
/// @param entity Entity3D instance used by the operation.
/// @param mask Value supplied for the mask argument.
void rt_game3d_entity_set_collision_mask_prop(void *entity, void *mask);
/// @brief Get the entity's display name.
/// @param entity Entity3D instance used by the operation.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_entity_get_name(void *entity);
/// @brief Set the entity's display name (property setter).
/// @param entity Entity3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
void rt_game3d_entity_set_name_prop(void *entity, rt_string name);
/// @brief Fluent: set local position from XYZ and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_position(void *entity, double x, double y, double z);
/// @brief Fluent: set local position from a Vec3 and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_position_v(void *entity, void *position);
/// @brief Fluent: set a uniform scale and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param scale Value supplied for the scale argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_scale(void *entity, double scale);
/// @brief Fluent: set a non-uniform XYZ scale and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_scale_xyz(void *entity, double x, double y, double z);
/// @brief Fluent: set rotation from Euler angles (degrees) and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param x_deg Value supplied for the x deg argument.
/// @param y_deg Value supplied for the y deg argument.
/// @param z_deg Value supplied for the z deg argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_rotation_euler(void *entity, double x_deg, double y_deg, double z_deg);
/// @brief Fluent: assign the mesh and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param mesh Mesh handle used by the operation.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_mesh(void *entity, void *mesh);
/// @brief Fluent: assign a mesh to every drawable node in this entity's scene-node subtree.
/// @param entity Entity3D instance used by the operation.
/// @param mesh Mesh handle used by the operation.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_mesh_recursive(void *entity, void *mesh);
/// @brief Fluent: assign the material and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param material Material handle used by the operation.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_material(void *entity, void *material);
/// @brief Fluent: assign a material to every node in this entity's scene-node subtree.
/// @param entity Entity3D instance used by the operation.
/// @param material Material handle used by the operation.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_material_recursive(void *entity, void *material);
/// @brief Fluent: parent the given child entity under this one and return this entity.
/// @param entity Entity3D instance used by the operation.
/// @param child Value supplied for the child argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_add_child(void *entity, void *child);
/// @brief True if the entity is a group (no own renderable, only children).
/// @param entity Entity3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_entity_is_group(void *entity);
/// @brief Fluent: assign the display name and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_name(void *entity, rt_string name);
/// @brief Fluent: assign the collision layer and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param layer Positive single-bit collision or render layer.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_layer(void *entity, int64_t layer);
/// @brief Fluent: assign the collision mask and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param mask Value supplied for the mask argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_entity_set_collision_mask(void *entity, void *mask);
/// @brief Fluent: attach a physics body (from a Body or BodyDef) and return the entity.
/// @param entity Entity3D instance used by the operation.
/// @param body_or_def Value supplied for the body or def argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_body(void *entity, void *body_or_def);
/// @brief Fluent: attach an animator, skeletal controller, or node animator and return entity.
/// @param entity Entity3D instance used by the operation.
/// @param animator_or_controller Value supplied for the animator or controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_animator(void *entity, void *animator_or_controller);
/// @brief Fluent: parent @p child under this entity and drive it from the named
///   bone of this entity's animated skeleton (world = bone pose each step).
/// @param entity Entity3D instance used by the operation.
/// @param child Value supplied for the child argument.
/// @param bone_name Value supplied for the bone name argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_to_bone(void *entity, void *child, rt_string bone_name);
/// @brief Build (cached) and activate a ragdoll from the entity's animator skeleton.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_enable_ragdoll(void *entity);
/// @brief Deactivate the entity's ragdoll with a blend back to animation.
/// @param entity Entity3D instance used by the operation.
/// @param blend_seconds Value supplied for the blend seconds argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_entity_disable_ragdoll(void *entity, double blend_seconds);
/// @brief Get the entity's cached Ragdoll3D (NULL before enableRagdoll).
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_ragdoll(void *entity);
/// @brief Fluent: bone attachment with a positional offset in bone space.
/// @param entity Entity3D instance used by the operation.
/// @param child Value supplied for the child argument.
/// @param bone_name Value supplied for the bone name argument.
/// @param offset_x Value supplied for the offset x argument.
/// @param offset_y Value supplied for the offset y argument.
/// @param offset_z Value supplied for the offset z argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_to_bone_offset(void *entity,
                                             void *child,
                                             rt_string bone_name,
                                             double offset_x,
                                             double offset_y,
                                             double offset_z);
/// @brief Fluent: remove this entity's bone-socket binding (stays parented).
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_detach_from_bone(void *entity);
/// @brief Fluent: attach a Behavior3D ticked by the world each simulation step
///   (null detaches).
/// @param entity Entity3D instance used by the operation.
/// @param behavior Value supplied for the behavior argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_behavior(void *entity, void *behavior);
/// @brief The entity's attached Behavior3D (NULL if none).
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_behavior(void *entity);

//=========================================================================
// Behavior3D — composable per-entity preset behaviors (Zanna.Game3D.Behavior3D)
//=========================================================================

/// @brief Create an empty behavior; compose presets with the fluent Add* calls.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_behavior_new(void);
/// @brief Fluent: continuous rotation about an axis at degrees/second.
/// @param behavior Value supplied for the behavior argument.
/// @param axis_x Value supplied for the axis x argument.
/// @param axis_y Value supplied for the axis y argument.
/// @param axis_z Value supplied for the axis z argument.
/// @param deg_per_sec Value supplied for the deg per sec argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_spin(
    void *behavior, double axis_x, double axis_y, double axis_z, double deg_per_sec);
/// @brief Fluent: circular XZ orbit around a world-space center.
/// @param behavior Value supplied for the behavior argument.
/// @param center_x Value supplied for the center x argument.
/// @param center_y Value supplied for the center y argument.
/// @param center_z Value supplied for the center z argument.
/// @param radius Value supplied for the radius argument.
/// @param deg_per_sec Value supplied for the deg per sec argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_orbit(void *behavior,
                                   double center_x,
                                   double center_y,
                                   double center_z,
                                   double radius,
                                   double deg_per_sec);
/// @brief Fluent: vertical sine bobbing around the height at first tick.
/// @param behavior Value supplied for the behavior argument.
/// @param amplitude Value supplied for the amplitude argument.
/// @param speed Value supplied for the speed argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_sine_float(void *behavior, double amplitude, double speed);
/// @brief Fluent: yaw so the entity's forward (-Z) axis points at the target.
/// @param behavior Value supplied for the behavior argument.
/// @param target_entity Value supplied for the target entity argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_face_target(void *behavior, void *target_entity);
/// @brief Fluent: move toward the target entity, stopping inside range
///   (direct XZ steer, or via a bound NavAgent3D).
/// @param behavior Value supplied for the behavior argument.
/// @param target_entity Value supplied for the target entity argument.
/// @param speed Value supplied for the speed argument.
/// @param range Value supplied for the range argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_chase(void *behavior, void *target_entity, double speed, double range);
/// @brief Fluent: follow a Path3D at constant arc-length speed (looping or one-shot).
/// @param behavior Value supplied for the behavior argument.
/// @param path Path3D whose control points are interpreted in world space.
/// @param speed Non-negative travel rate in world units per simulation second.
/// @param loop Value supplied for the loop argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_follow_path(void *behavior, void *path, double speed, int8_t loop);
/// @brief Fluent: despawn the entity after the given seconds of simulation time.
/// @param behavior Value supplied for the behavior argument.
/// @param seconds Value supplied for the seconds argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_lifetime(void *behavior, double seconds);
/// @brief C-internal one-shot preset: despawn @p target_entity on next update.
///   Not script-visible; exercises mid-sweep registry compaction in tests.
/// @param behavior Value supplied for the behavior argument.
/// @param target_entity Value supplied for the target entity argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_behavior_add_despawn_target_internal(void *behavior, void *target_entity);
/// @brief Fluent: route chase movement through a NavAgent3D (null clears).
/// @param behavior Value supplied for the behavior argument.
/// @param agent Value supplied for the agent argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_behavior_set_nav_agent(void *behavior, void *agent);
/// @brief Advance one behavior for one entity by dt seconds (world tick entry).
/// @param behavior Value supplied for the behavior argument.
/// @param entity Entity3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_behavior_update(void *behavior, void *entity, double dt);
/// @brief Apply an instantaneous linear impulse to the entity's body.
/// @param entity Entity3D instance used by the operation.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
void rt_game3d_entity_apply_impulse(void *entity, double x, double y, double z);
/// @brief Set the entity body's linear velocity directly.
/// @param entity Entity3D instance used by the operation.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
void rt_game3d_entity_set_velocity(void *entity, double x, double y, double z);
/// @brief Get the entity's local position as a Vec3.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_position(void *entity);
/// @brief Get the entity's world-space position as a Vec3.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_world_position(void *entity);
/// @brief True if the entity is currently spawned into a world.
/// @param entity Entity3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_entity_is_spawned(void *entity);
/// @brief True if the entity has been despawned/destroyed.
/// @param entity Entity3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_entity_is_destroyed(void *entity);

//=========================================================================
// Animator3D — animation state-machine driver (Zanna.Game3D.Animator3D)
//=========================================================================

/// @brief Create an animator driven by an AnimController3D or NodeAnimator3D.
/// @details Imported models can expose both skeletal and node animation. Use
/// ModelTemplate.Instantiate() to receive a combined wrapper automatically, or pass a raw
/// AnimController3D/NodeAnimator3D here for manual construction.
/// @param controller Value supplied for the controller argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_animator_new(void *controller);
/// @brief Get the animation controller backing this animator.
/// @param animator Value supplied for the animator argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_animator_get_controller(void *animator);
/// @brief Get the node animator backing this animator, or NULL for skeletal-only wrappers.
/// @param animator Value supplied for the animator argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_animator_get_node_animator(void *animator);
/// @brief Get the model-space matrix for a bone from the final composited pose
///   (freshly allocated Mat4, NULL when unavailable).
/// @param animator Value supplied for the animator argument.
/// @param bone_index Value supplied for the bone index argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_animator_get_bone_matrix(void *animator, int64_t bone_index);
/// @brief Resolve a bone index by name via the controller's skeleton (-1 if unknown).
/// @param animator Value supplied for the animator argument.
/// @param name Runtime string naming the requested object or property.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_animator_find_bone(void *animator, rt_string name);
/// @brief Play the named clip immediately; returns false if the clip is unknown.
/// @param animator Value supplied for the animator argument.
/// @param name Runtime string naming the requested object or property.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_play(void *animator, rt_string name);
/// @brief Cross-fade to the named clip over `seconds`; returns false if unknown.
/// @param animator Value supplied for the animator argument.
/// @param name Runtime string naming the requested object or property.
/// @param seconds Value supplied for the seconds argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_crossfade(void *animator, rt_string name, double seconds);
/// @brief Play a named clip as a true additive overlay layer.
/// @param animator Value supplied for the animator argument.
/// @param layer Positive single-bit collision or render layer.
/// @param name Runtime string naming the requested object or property.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_play_layer_additive(void *animator, int64_t layer, rt_string name);
/// @brief Cross-fade a named clip as a true additive overlay layer.
/// @param animator Value supplied for the animator argument.
/// @param layer Positive single-bit collision or render layer.
/// @param name Runtime string naming the requested object or property.
/// @param seconds Value supplied for the seconds argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_crossfade_layer_additive(void *animator,
                                                   int64_t layer,
                                                   rt_string name,
                                                   double seconds);
/// @brief Set a BlendTree3D as the wrapped controller's base pose source.
/// @param animator Value supplied for the animator argument.
/// @param blend_tree Value supplied for the blend tree argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_set_blend_tree(void *animator, void *blend_tree);
/// @brief Set an IKSolver3D as the wrapped controller's final-pose constraint.
/// @param animator Value supplied for the animator argument.
/// @param ik_solver Value supplied for the ik solver argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_set_ik_solver(void *animator, void *ik_solver);
/// @brief Set the playback speed multiplier for the named clip.
/// @param animator Value supplied for the animator argument.
/// @param name Runtime string naming the requested object or property.
/// @param speed Value supplied for the speed argument.
void rt_game3d_animator_set_speed(void *animator, rt_string name, double speed);
/// @brief True if the named clip is currently the active state.
/// @param animator Value supplied for the animator argument.
/// @param name Runtime string naming the requested object or property.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_animator_is_playing(void *animator, rt_string name);
/// @brief Get the elapsed time of the current animation state in seconds.
/// @param animator Value supplied for the animator argument.
/// @return The documented floating-point result.
double rt_game3d_animator_state_time(void *animator);
/// @brief Count animation events fired during the most recent update.
/// @param animator Value supplied for the animator argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_animator_event_count(void *animator);
/// @brief Get the name of the i-th animation event from the most recent update.
/// @param animator Value supplied for the animator argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_animator_event_name(void *animator, int64_t index);
/// @brief Advance the animator by `dt` seconds, sampling poses and firing events.
/// @param animator Value supplied for the animator argument.
/// @param dt Time interval in seconds.
void rt_game3d_animator_update(void *animator, double dt);

//=========================================================================
// Sound3D — spatial audio subsystem (Zanna.Game3D.Sound3D)
//=========================================================================

/// @brief Get the audio listener (ears) object.
/// @param audio Game3D audio subsystem used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_audio_get_listener(void *audio);
/// @brief True if the listener auto-tracks the active camera.
/// @param audio Game3D audio subsystem used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_audio_get_listener_follows_camera(void *audio);
/// @brief Get the attenuation reference distance (full-volume radius).
/// @param audio Game3D audio subsystem used by the operation.
/// @return The documented floating-point result.
double rt_game3d_audio_get_ref_distance(void *audio);
/// @brief Get the attenuation maximum distance (silence radius).
/// @param audio Game3D audio subsystem used by the operation.
/// @return The documented floating-point result.
double rt_game3d_audio_get_max_distance(void *audio);
/// @brief Get the master output volume (0–100).
/// @param audio Game3D audio subsystem used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_audio_get_volume(void *audio);
/// @brief Set the master output volume (0–100) for future and tracked positional sources.
/// @param audio Game3D audio subsystem used by the operation.
/// @param volume Value supplied for the volume argument.
void rt_game3d_audio_set_volume(void *audio, int64_t volume);
/// @brief Count currently active 3D sound sources after pruning finished voices.
/// @param audio Game3D audio subsystem used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_audio_get_source_count(void *audio);
/// @brief Enable or disable the listener auto-following the camera.
/// @param audio Game3D audio subsystem used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_audio_listener_follow_camera(void *audio, int8_t enabled);
/// @brief Set the listener pose explicitly from position/forward/up Vec3s.
/// @param audio Game3D audio subsystem used by the operation.
/// @param position Value supplied for the position argument.
/// @param forward Value supplied for the forward argument.
/// @param up Value supplied for the up argument.
void rt_game3d_audio_set_listener_pose(void *audio, void *position, void *forward, void *up);
/// @brief Set the distance-attenuation reference and maximum radii.
/// @param audio Game3D audio subsystem used by the operation.
/// @param ref_distance Value supplied for the ref distance argument.
/// @param max_distance Value supplied for the max distance argument.
void rt_game3d_audio_set_attenuation(void *audio, double ref_distance, double max_distance);
/// @brief Load an audio clip from a filesystem path.
/// @param audio Game3D audio subsystem used by the operation.
/// @param path Runtime path string naming the requested resource.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_audio_load(void *audio, rt_string path);
/// @brief Load an audio clip from a packed asset path.
/// @param audio Game3D audio subsystem used by the operation.
/// @param asset_path Value supplied for the asset path argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_audio_load_asset(void *audio, rt_string asset_path);
/// @brief Play a clip as a one-shot at a fixed world position; returns the source.
/// @param audio Game3D audio subsystem used by the operation.
/// @param clip Value supplied for the clip argument.
/// @param position Value supplied for the position argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_audio_play_at(void *audio, void *clip, void *position);
/// @brief Play a clip attached to an entity (follows it); returns the source.
/// @param audio Game3D audio subsystem used by the operation.
/// @param clip Value supplied for the clip argument.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_audio_play_attached(void *audio, void *clip, void *entity);
/// @brief Play a clip as non-spatial 2D audio; returns a source id.
/// @param audio Game3D audio subsystem used by the operation.
/// @param clip Value supplied for the clip argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_audio_play2d(void *audio, void *clip);
/// @brief Stop and remove all active sources.
/// @param audio Game3D audio subsystem used by the operation.
void rt_game3d_audio_clear_sources(void *audio);

//=========================================================================
// Effects3D registry — per-world particle/decal manager (Zanna.Game3D.EffectRegistry3D)
//=========================================================================

/// @brief Get the post-FX stack associated with this effect registry.
/// @param effects Game3D effect registry used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects_get_postfx(void *effects);
/// @brief Count total active effects (particles + decals).
/// @param effects Game3D effect registry used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_effects_get_count(void *effects);
/// @brief Count active particle systems.
/// @param effects Game3D effect registry used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_effects_get_particles_count(void *effects);
/// @brief Count active decals.
/// @param effects Game3D effect registry used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_effects_get_decal_count(void *effects);
/// @brief Register a particle system with an auto-expire lifetime; returns its handle.
/// @param effects Game3D effect registry used by the operation.
/// @param particles Value supplied for the particles argument.
/// @param lifetime Value supplied for the lifetime argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects_add_particles(void *effects, void *particles, double lifetime);
/// @brief Register a decal; returns its handle.
/// @param effects Game3D effect registry used by the operation.
/// @param decal Value supplied for the decal argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects_add_decal(void *effects, void *decal);
/// @brief Advance all registered effects by `dt` seconds and retire expired ones.
/// @param effects Game3D effect registry used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_effects_update(void *effects, double dt);
/// @brief Draw all registered effects through the given canvas and camera.
/// @param effects Game3D effect registry used by the operation.
/// @param canvas Canvas3D instance used by the operation.
/// @param camera Camera3D instance used by the operation.
void rt_game3d_effects_draw(void *effects, void *canvas, void *camera);
/// @brief Remove all registered effects immediately.
/// @param effects Game3D effect registry used by the operation.
void rt_game3d_effects_clear(void *effects);

//=========================================================================
// Effects3D presets — one-shot effect factories (Zanna.Game3D.Effects3D)
//=========================================================================

/// @brief Spawn an explosion effect at the given world position.
/// @param world World3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects3d_explosion(void *world, void *position);
/// @brief Spawn a directional spark burst at the given position.
/// @param world World3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @param direction Value supplied for the direction argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects3d_sparks(void *world, void *position, void *direction);
/// @brief Spawn a dust puff at the given position.
/// @param world World3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects3d_dust(void *world, void *position);
/// @brief Spawn a rising smoke plume at the given position.
/// @param world World3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects3d_smoke(void *world, void *position);
/// @brief Spawn an impact decal oriented to the given surface normal.
/// @param world World3D instance used by the operation.
/// @param position Value supplied for the position argument.
/// @param normal Value supplied for the normal argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_effects3d_impact_decal(void *world, void *position, void *normal);

//=========================================================================
// Lighting — scene lighting presets (Zanna.Game3D.Lighting)
//=========================================================================

/// @brief Apply a neutral three-point studio lighting rig to the world.
/// @param world World3D instance used by the operation.
void rt_game3d_lighting_studio(void *world);
/// @brief Apply outdoor sun+sky lighting using the given sun direction.
/// @param world World3D instance used by the operation.
/// @param sun_dir Value supplied for the sun dir argument.
void rt_game3d_lighting_outdoor(void *world, void *sun_dir);
/// @brief Apply a dim moonlit night lighting preset.
/// @param world World3D instance used by the operation.
void rt_game3d_lighting_night(void *world);
/// @brief Apply a warm indoor lighting preset.
/// @param world World3D instance used by the operation.
void rt_game3d_lighting_interior(void *world);
/// @brief Remove all preset lights from the world.
/// @param world World3D instance used by the operation.
void rt_game3d_lighting_clear(void *world);

//=========================================================================
// Materials — material presets (Zanna.Game3D.Materials)
//=========================================================================

/// @brief Create a matte plastic material of the given RGB color.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_plastic(double r, double g, double b);
/// @brief Create a metallic material of the given RGB color.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_metal(double r, double g, double b);
/// @brief Create a soft rubber material of the given RGB color.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_rubber(double r, double g, double b);
/// @brief Create a translucent glass material of the given RGB color and alpha.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @param alpha Value supplied for the alpha argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_glass(double r, double g, double b, double alpha);
/// @brief Create an emissive (self-lit) material of the given RGB color and intensity.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @param intensity Value supplied for the intensity argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_emissive(double r, double g, double b, double intensity);
/// @brief Create an unlit flat-albedo material of the given RGB color.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_unlit(double r, double g, double b);
/// @brief Create a material whose albedo is sampled from a Pixels texture.
/// @param pixels Pixels or texture handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_materials_from_albedo_map(void *pixels);

//=========================================================================
// PostFX — post-processing presets (Zanna.Game3D.PostFX)
//=========================================================================

/// @brief Apply a cinematic post-FX chain with bloom, tone-map, and subtle vignette.
/// @param world World3D instance used by the operation.
void rt_game3d_postfx_cinematic(void *world);
/// @brief Apply a crisp, minimal-grading post-FX chain to the world.
/// @param world World3D instance used by the operation.
void rt_game3d_postfx_crisp(void *world);
/// @brief Disable all post-processing for the world.
/// @param world World3D instance used by the operation.
void rt_game3d_postfx_none(void *world);

//=========================================================================
// Quality — quality preset application (Zanna.Game3D.Quality)
//=========================================================================

/// @brief Apply a render quality preset (RT_GAME3D_QUALITY_*) to the world.
/// @param world World3D instance used by the operation.
/// @param quality Value supplied for the quality argument.
void rt_game3d_quality_apply(void *world, int64_t quality);

//=========================================================================
// Prefab — primitive mesh-entity factories (Zanna.Game3D.Prefab)
//=========================================================================

/// @brief Create a cube entity of the given uniform size with a material.
/// @param size Value supplied for the size argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_box(double size, void *material);
/// @brief Create a box entity with explicit width/height/depth and a material.
/// @param width Value supplied for the width argument.
/// @param height Value supplied for the height argument.
/// @param depth Value supplied for the depth argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_box_xyz(double width, double height, double depth, void *material);
/// @brief Create a UV sphere entity with the given radius and segment count.
/// @param radius Value supplied for the radius argument.
/// @param segments Value supplied for the segments argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_sphere(double radius, int64_t segments, void *material);
/// @brief Create a cylinder entity with the given radius, height, and segments.
/// @param radius Value supplied for the radius argument.
/// @param height Value supplied for the height argument.
/// @param segments Value supplied for the segments argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_cylinder(double radius, double height, int64_t segments, void *material);
/// @brief Create a flat plane entity of the given width and depth.
/// @param width Value supplied for the width argument.
/// @param depth Value supplied for the depth argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_plane(double width, double depth, void *material);
/// @brief Create a large ground-plane entity of the given size.
/// @param size Value supplied for the size argument.
/// @param material Material handle used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_prefab_ground(double size, void *material);

//=========================================================================
// Assets3D — model loading and caching (Zanna.Game3D.Assets3D)
//=========================================================================

/// @brief Load a model from a filesystem path as a ready-to-spawn entity.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model(rt_string path);
/// @brief Load a model from a packed asset path as a ready-to-spawn entity.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_asset(rt_string path);
/// @brief Load a skeletal Animation3D clip from a model file by index.
/// @param path Runtime path string naming the requested resource.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_animation(rt_string path, int64_t index);
/// @brief Load a skeletal Animation3D clip from a packed model asset by index.
/// @param path Runtime path string naming the requested resource.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_animation_asset(rt_string path, int64_t index);
/// @brief Load a NodeAnimation3D clip from a model file by index.
/// @param path Runtime path string naming the requested resource.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_node_animation(rt_string path, int64_t index);
/// @brief Load a NodeAnimation3D clip from a packed model asset by index.
/// @param path Runtime path string naming the requested resource.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_node_animation_asset(rt_string path, int64_t index);
/// @brief Load a model from a filesystem path as a reusable instancing template.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_template(rt_string path);
/// @brief Load a model from a packed asset path as a reusable instancing template.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_template_asset(rt_string path);
/* Result-carrying loader peers (ADR 0233) — ok wraps the loaded handle, err
 * carries the asset-error diagnostic text. */
/// @brief Result peer of `rt_game3d_assets_load_model`. @param path Requested resource path.
void *rt_game3d_assets_load_model_result(rt_string path);
/// @brief Result peer of `rt_game3d_assets_load_model_asset`. @param path Requested resource path.
void *rt_game3d_assets_load_model_asset_result(rt_string path);
/// @brief Result peer of `rt_game3d_assets_load_animation`.
/// @param path Requested resource path. @param index Zero-based clip index.
void *rt_game3d_assets_load_animation_result(rt_string path, int64_t index);
/// @brief Result peer of `rt_game3d_assets_load_animation_asset`.
/// @param path Requested resource path. @param index Zero-based clip index.
void *rt_game3d_assets_load_animation_asset_result(rt_string path, int64_t index);
/// @brief Result peer of `rt_game3d_assets_load_node_animation`.
/// @param path Requested resource path. @param index Zero-based clip index.
void *rt_game3d_assets_load_node_animation_result(rt_string path, int64_t index);
/// @brief Result peer of `rt_game3d_assets_load_node_animation_asset`.
/// @param path Requested resource path. @param index Zero-based clip index.
void *rt_game3d_assets_load_node_animation_asset_result(rt_string path, int64_t index);
/// @brief Result peer of `rt_game3d_assets_load_model_template`. @param path Resource path.
void *rt_game3d_assets_load_model_template_result(rt_string path);
/// @brief Result peer of `rt_game3d_assets_load_model_template_asset`. @param path Resource path.
void *rt_game3d_assets_load_model_template_asset_result(rt_string path);
/// @brief Load a filesystem model through the AssetHandle3D contract.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_async(rt_string path);
/// @brief Load a packed-asset model through the AssetHandle3D contract.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_asset_async(rt_string path);
/// @brief Load a filesystem model template through the AssetHandle3D contract.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_template_async(rt_string path);
/// @brief Load a packed-asset model template through the AssetHandle3D contract.
/// @param path Runtime path string naming the requested resource.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_assets_load_model_template_asset_async(rt_string path);
/// @brief Set the process-wide cached-template residency budget; negative means unlimited.
/// @param bytes Value supplied for the bytes argument.
void rt_game3d_assets_set_residency_budget(int64_t bytes);
/// @brief Return estimated bytes currently resident in the shared cached-template store.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_assets_get_resident_bytes(void);
/// @brief Hint cache eviction: higher priority and lower distance survive pressure first.
/// @param model_template Value supplied for the model template argument.
/// @param priority Value supplied for the priority argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_assets_set_residency_hint(void *model_template, double priority, double distance);
/// @brief Set the per-drain async asset upload budget in decoded bytes; negative means unlimited.
/// @param bytes Value supplied for the bytes argument.
void rt_game3d_assets_set_upload_budget(int64_t bytes);
/// @brief Evict the cached template backing a ready template AssetHandle3D.
/// @param asset_handle Value supplied for the asset handle argument.
void rt_game3d_assets_evict(void *asset_handle);
/// @brief Warm the filesystem template cache through the background async load path.
/// @param path Runtime path string naming the requested resource.
void rt_game3d_assets_preload(rt_string path);
/// @brief Warm the packed-asset template cache through the background async load path.
/// @param path Runtime path string naming the requested resource.
void rt_game3d_assets_preload_asset(rt_string path);
/// @brief Drop all cached loaded models, freeing their memory.
void rt_game3d_assets_clear_cache(void);

//=========================================================================
// AssetHandle3D — asset-loading status/result handle (Zanna.Game3D.AssetHandle3D)
//=========================================================================

/// @brief True once the asset request has reached a terminal state.
/// @param asset_handle Value supplied for the asset handle argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_asset_handle_get_ready(void *asset_handle);
/// @brief Loading progress in the inclusive range [0, 1].
/// @param asset_handle Value supplied for the asset handle argument.
/// @return The documented floating-point result.
double rt_game3d_asset_handle_get_progress(void *asset_handle);
/// @brief Terminal error text, or an empty string on success / pending work.
/// @param asset_handle Value supplied for the asset handle argument.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_asset_handle_get_error(void *asset_handle);
/// @brief Cancel a pending request; completed requests are left unchanged.
/// @param asset_handle Value supplied for the asset handle argument.
void rt_game3d_asset_handle_cancel(void *asset_handle);
/// @brief Return the loaded entity for entity-mode requests, or NULL.
/// @param asset_handle Value supplied for the asset handle argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_asset_handle_get_entity(void *asset_handle);
/// @brief Return the loaded model template for template-mode requests, or NULL.
/// @param asset_handle Value supplied for the asset handle argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_asset_handle_get_template(void *asset_handle);

//=========================================================================
// ModelTemplate — cached model for instancing (Zanna.Game3D.ModelTemplate)
//=========================================================================

/// @brief Get the underlying loaded model backing the template.
/// @param model_template Value supplied for the model template argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_model_template_get_model(void *model_template);
/// @brief Get the source path the template was loaded from.
/// @param model_template Value supplied for the model template argument.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_model_template_get_path(void *model_template);
/// @brief True if the template was loaded from a packed asset (not the filesystem).
/// @param model_template Value supplied for the model template argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_model_template_get_is_asset(void *model_template);
/// @brief Number of scenes addressable from the underlying model.
/// @param model_template Value supplied for the model template argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_model_template_get_scene_count(void *model_template);
/// @brief Name of the imported scene at @p index, or empty when out of range.
/// @param model_template Value supplied for the model template argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_model_template_get_scene_name(void *model_template, int64_t index);
/// @brief Number of imported cameras in @p scene_index.
/// @param model_template Value supplied for the model template argument.
/// @param scene_index Value supplied for the scene index argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_model_template_get_camera_count(void *model_template, int64_t scene_index);
/// @brief Get an imported camera from @p scene_index.
/// @param model_template Value supplied for the model template argument.
/// @param scene_index Value supplied for the scene index argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_model_template_get_camera(void *model_template, int64_t scene_index, int64_t index);
/// @brief Instantiate a fresh entity from the template.
/// @param model_template Value supplied for the model template argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_model_template_instantiate(void *model_template);
/// @brief Instantiate a fresh entity from a specific imported scene.
/// @param model_template Value supplied for the model template argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_model_template_instantiate_scene_at(void *model_template, int64_t index);

//=========================================================================
// Environment / EnvHandle — environment presets and builder (Zanna.Game3D.Environment3D /
// EnvHandle)
//=========================================================================

/// @brief Apply a bright outdoor environment (sky, sun, fog) and return its handle.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_environment_outdoor(void *world);
/// @brief Apply a warm sunset environment and return its handle.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_environment_sunset(void *world);
/// @brief Apply a flat overcast environment and return its handle.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_environment_overcast(void *world);
/// @brief Apply a dark night environment and return its handle.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_environment_night(void *world);
/// @brief Fluent: add terrain of the given size/height to the environment and return the handle.
/// @param env Value supplied for the env argument.
/// @param size Value supplied for the size argument.
/// @param height Value supplied for the height argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_env_handle_with_terrain(void *env, double size, double height);
/// @brief Fluent: add a water plane at the given level and return the handle.
/// @param env Value supplied for the env argument.
/// @param level Value supplied for the level argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_env_handle_with_water(void *env, double level);
/// @brief Fluent: add distance fog between near/far planes and return the handle.
/// @param env Value supplied for the env argument.
/// @param near_plane Value supplied for the near plane argument.
/// @param far_plane Value supplied for the far plane argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_env_handle_with_fog(void *env, double near_plane, double far_plane);

/// @brief Fluent: enable exponential height fog (density pools below @p height).
/// @param obj Runtime object receiving or supplying this operation.
/// @param density Value supplied for the density argument.
/// @param height Value supplied for the height argument.
/// @param falloff Value supplied for the falloff argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_env_handle_with_height_fog(void *obj,
                                           double density,
                                           double height,
                                           double falloff);

//=========================================================================
// Debug3D — debug visualization toggles (Zanna.Game3D.Debug3D)
//=========================================================================

/// @brief Show or hide the on-screen debug stats overlay.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_debug_show_overlay(void *world, int8_t enabled);
/// @brief Draw a world-space coordinate axis gizmo of the given size at origin.
/// @param world World3D instance used by the operation.
/// @param origin Value supplied for the origin argument.
/// @param size Value supplied for the size argument.
void rt_game3d_debug_draw_axes(void *world, void *origin, double size);
/// @brief Enable or disable physics collider/contact debug drawing.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_debug_draw_physics(void *world, int8_t enabled);
/// @brief Enable or disable on-screen camera info readout.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_debug_draw_camera_info(void *world, int8_t enabled);
/// @brief Enable or disable the backend capabilities readout overlay.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_debug_draw_capabilities(void *world, int8_t enabled);

//=========================================================================
// CharacterController3D — kinematic character mover (Zanna.Game3D.CharacterController3D)
//=========================================================================

/// @brief Create a capsule character controller bound to an entity in the world.
/// @param world World3D instance used by the operation.
/// @param entity Entity3D instance used by the operation.
/// @param radius Value supplied for the radius argument.
/// @param height Value supplied for the height argument.
/// @param mass Value supplied for the mass argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_character_controller_new(
    void *world, void *entity, double radius, double height, double mass);
/// @brief Get the underlying low-level character object.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_character_controller_get_character(void *controller);
/// @brief Get the entity driven by this controller.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_character_controller_get_entity(void *controller);
/// @brief Get the horizontal move speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_character_controller_get_speed(void *controller);
/// @brief Set the horizontal move speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @param speed Value supplied for the speed argument.
void rt_game3d_character_controller_set_speed(void *controller, double speed);
/// @brief Get the initial jump speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_character_controller_get_jump_speed(void *controller);
/// @brief Set the initial jump speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @param jump_speed Value supplied for the jump speed argument.
void rt_game3d_character_controller_set_jump_speed(void *controller, double jump_speed);
/// @brief Get the downward gravity acceleration magnitude in units/second².
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_character_controller_get_gravity(void *controller);
/// @brief Set the downward gravity acceleration magnitude in units/second².
/// @param controller Value supplied for the controller argument.
/// @param gravity Value supplied for the gravity argument.
void rt_game3d_character_controller_set_gravity(void *controller, double gravity);
/// @brief Advance the controller using input and camera orientation over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param input Input3D instance or snapshot used by the operation.
/// @param camera Camera3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_character_controller_update(void *controller, void *input, void *camera, double dt);
/// @brief Teleport the character to an absolute world position, clearing velocity.
/// @param controller Value supplied for the controller argument.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
void rt_game3d_character_controller_teleport(void *controller, double x, double y, double z);
/// @brief True if the character is currently standing on ground.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_character_controller_grounded(void *controller);
/// @brief Get the crouch capsule height applied by SetCrouching(true).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_character_controller_get_crouch_height(void *controller);
/// @brief Set the crouch capsule height.
/// @param controller Value supplied for the controller argument.
/// @param height Value supplied for the height argument.
void rt_game3d_character_controller_set_crouch_height(void *controller, double height);
/// @brief Toggle crouch; standing back up returns false when blocked by a ceiling.
/// @param controller Value supplied for the controller argument.
/// @param crouching Value supplied for the crouching argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_character_controller_set_crouching(void *controller, int8_t crouching);
/// @brief True while the crouch state is engaged.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_character_controller_is_crouching(void *controller);
/// @brief Get the dynamic push impulse scale (0 = block only).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_character_controller_get_push_strength(void *controller);
/// @brief Set the dynamic push impulse scale.
/// @param controller Value supplied for the controller argument.
/// @param strength Value supplied for the strength argument.
void rt_game3d_character_controller_set_push_strength(void *controller, double strength);
/// @brief Whether the controller rides moving kinematic platforms.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_character_controller_get_ride_platforms(void *controller);
/// @brief Enable/disable riding moving platforms.
/// @param controller Value supplied for the controller argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_character_controller_set_ride_platforms(void *controller, int8_t enabled);
/// @brief True while the character rests on a too-steep surface.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_character_controller_is_sliding(void *controller);
/// @brief Entity owning the body under the character's feet (NULL if unmanaged).
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_character_controller_ground_entity(void *controller);
/// @brief Probe for a grabbable ledge ahead (LedgeHit3D or NULL); defaults
///   origin/forward/radius from the character pose and entity facing.
/// @param controller Value supplied for the controller argument.
/// @param max_height Value supplied for the max height argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_character_controller_probe_ledge(void *controller, double max_height);
/// @brief Probe for a vaultable obstacle ahead (LedgeHit3D or NULL).
/// @param controller Value supplied for the controller argument.
/// @param max_height Value supplied for the max height argument.
/// @param max_thickness Value supplied for the max thickness argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_character_controller_probe_vault(void *controller,
                                                 double max_height,
                                                 double max_thickness);

//=========================================================================
// Combat volumes and health (Zanna.Game3D.Hitbox3D / Health3D / events)
//=========================================================================

/// @brief Create an entity-space combat volume (kind Hurt, team 0, channel 1).
/// @param entity Entity3D instance used by the operation.
/// @param collider Value supplied for the collider argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_hitbox_new(void *entity, void *collider);
/// @brief Create a bone-attached combat volume; traps on unknown bone names.
/// @param entity Entity3D instance used by the operation.
/// @param bone_name Value supplied for the bone name argument.
/// @param collider Value supplied for the collider argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hitbox_new_on_bone(void *entity, rt_string bone_name, void *collider);
/// @brief Get the volume kind (HitboxKind.Hurt = 0, HitboxKind.Hit = 1).
/// @param hitbox Value supplied for the hitbox argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_hitbox_get_kind(void *hitbox);
/// @brief Set the volume kind (Hurt or Hit).
/// @param hitbox Value supplied for the hitbox argument.
/// @param kind Value supplied for the kind argument.
void rt_game3d_hitbox_set_kind(void *hitbox, int64_t kind);
/// @brief Get the team id (same-team pairs are skipped unless friendly fire).
/// @param hitbox Value supplied for the hitbox argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_hitbox_get_team(void *hitbox);
/// @brief Set the team id.
/// @param hitbox Value supplied for the hitbox argument.
/// @param team Value supplied for the team argument.
void rt_game3d_hitbox_set_team(void *hitbox, int64_t team);
/// @brief Get the channel bitmask (hit and hurt channels must overlap).
/// @param hitbox Value supplied for the hitbox argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_hitbox_get_channel(void *hitbox);
/// @brief Set the channel bitmask.
/// @param hitbox Value supplied for the hitbox argument.
/// @param channel Value supplied for the channel argument.
void rt_game3d_hitbox_set_channel(void *hitbox, int64_t channel);
/// @brief Get the manual activation switch.
/// @param hitbox Value supplied for the hitbox argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_hitbox_get_active(void *hitbox);
/// @brief Set the manual activation switch (scripted attacks).
/// @param hitbox Value supplied for the hitbox argument.
/// @param active Value supplied for the active argument.
void rt_game3d_hitbox_set_active(void *hitbox, int8_t active);
/// @brief Get the friendly-fire flag.
/// @param hitbox Value supplied for the hitbox argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_hitbox_get_friendly_fire(void *hitbox);
/// @brief Set the friendly-fire flag (allow same-team hits from this attacker).
/// @param hitbox Value supplied for the hitbox argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_hitbox_set_friendly_fire(void *hitbox, int8_t enabled);
/// @brief Fluent: bind an animation activation window (state name + time range).
/// @param hitbox Value supplied for the hitbox argument.
/// @param state_name Value supplied for the state name argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hitbox_bind_window(void *hitbox, rt_string state_name, double t0, double t1);
/// @brief Fluent: set the shape offset in bone/entity space.
/// @param hitbox Value supplied for the hitbox argument.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_hitbox_set_local_offset(void *hitbox, double x, double y, double z);
/// @brief HitboxKind.Hurt constant (0).
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_hitbox_kind_hurt(void);
/// @brief HitboxKind.Hit constant (1).
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_hitbox_kind_hit(void);

/// @brief Create a health component with the given maximum hit points.
/// @param max_hp Value supplied for the max hp argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_health_new(double max_hp);
/// @brief Fluent: attach a Health3D component (one per entity; reattach replaces).
/// @param entity Entity3D instance used by the operation.
/// @param health Value supplied for the health argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_attach_health(void *entity, void *health);
/// @brief Get the entity's Health3D component (NULL when none).
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_health(void *entity);
/// @brief Current hit points.
/// @param health Value supplied for the health argument.
/// @return The documented floating-point result.
double rt_game3d_health_get_current(void *health);
/// @brief Maximum hit points.
/// @param health Value supplied for the health argument.
/// @return The documented floating-point result.
double rt_game3d_health_get_max(void *health);
/// @brief Set maximum hit points.
/// @param health Value supplied for the health argument.
/// @param max_hp Value supplied for the max hp argument.
void rt_game3d_health_set_max(void *health, double max_hp);
/// @brief True once hp reached 0 (until Revive).
/// @param health Value supplied for the health argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_health_is_dead(void *health);
/// @brief I-frame duration granted per applied damage.
/// @param health Value supplied for the health argument.
/// @return The documented floating-point result.
double rt_game3d_health_get_invuln_seconds(void *health);
/// @brief Set the i-frame duration granted per applied damage.
/// @param health Value supplied for the health argument.
/// @param seconds Value supplied for the seconds argument.
void rt_game3d_health_set_invuln_seconds(void *health, double seconds);
/// @brief True while i-frames are active.
/// @param health Value supplied for the health argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_health_get_invulnerable(void *health);
/// @brief Apply damage; returns the applied amount (0 while invulnerable/dead).
/// @param health Value supplied for the health argument.
/// @param amount Value supplied for the amount argument.
/// @param source_entity Value supplied for the source entity argument.
/// @param tag Value supplied for the tag argument.
/// @return The documented floating-point result.
double rt_game3d_health_damage(void *health, double amount, void *source_entity, int64_t tag);
/// @brief Heal (clamped to max; no effect while dead).
/// @param health Value supplied for the health argument.
/// @param amount Value supplied for the amount argument.
void rt_game3d_health_heal(void *health, double amount);
/// @brief Clear the death latch and restore hp.
/// @param health Value supplied for the health argument.
/// @param hp Value supplied for the hp argument.
void rt_game3d_health_revive(void *health, double hp);
/// @brief One-shot: true for the step after hp crossed to 0.
/// @param health Value supplied for the health argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_health_just_died(void *health);
/// @brief One-shot: true for the step after damage applied.
/// @param health Value supplied for the health argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_health_just_damaged(void *health);
/// @brief Most recent applied damage amount.
/// @param health Value supplied for the health argument.
/// @return The documented floating-point result.
double rt_game3d_health_last_damage(void *health);
/// @brief Most recent caller-supplied damage tag.
/// @param health Value supplied for the health argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_health_last_tag(void *health);
/// @brief Impulse knockback on the owner's dynamic body (false for kinematic/none).
/// @param health Value supplied for the health argument.
/// @param direction Value supplied for the direction argument.
/// @param strength Value supplied for the strength argument.
/// @param point Value supplied for the point argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_health_apply_knockback(void *health,
                                        void *direction,
                                        double strength,
                                        void *point);

/// @brief Get the world time multiplier (default 1.0, clamped [0, 4]).
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_time_scale(void *world);
/// @brief Set the world time multiplier.
/// @param world World3D instance used by the operation.
/// @param scale Value supplied for the scale argument.
void rt_game3d_world_set_time_scale(void *world, double scale);
/// @brief Get the latched pause state.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_get_paused(void *world);
/// @brief Set the latched pause state (simulation freezes; rendering continues).
/// @param world World3D instance used by the operation.
/// @param paused Value supplied for the paused argument.
void rt_game3d_world_set_paused(void *world, int8_t paused);
/// @brief One-shot hit-stop for @p seconds of real time (max-latched).
/// @param world World3D instance used by the operation.
/// @param seconds Value supplied for the seconds argument.
void rt_game3d_world_hit_stop(void *world, double seconds);
/// @brief Real (unscaled) clamped frame step for UI/menus.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_unscaled_dt(void *world);
/// @brief Real (unscaled) elapsed seconds for UI/menus.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_unscaled_elapsed(void *world);
/// @brief Live DOF focus pull through the world post-FX chain; false when the
///   chain has no DOF effect.
/// @param world World3D instance used by the operation.
/// @param distance Value supplied for the distance argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_set_dof_focus(void *world, double distance);

/// @brief Number of hit events buffered by the most recent step.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_hit_event_count(void *world);
/// @brief Get a buffered hit event as a boxed HitEvent3D (NULL out of range).
/// @param world World3D instance used by the operation.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_hit_event(void *world, int64_t index);
/// @brief Clear buffered hit and damage events without stepping.
/// @param world World3D instance used by the operation.
void rt_game3d_world_clear_hit_events(void *world);
/// @brief Number of damage events buffered since the last step.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_damage_event_count(void *world);
/// @brief Get a buffered damage event as a boxed DamageEvent3D.
/// @param world World3D instance used by the operation.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_damage_event(void *world, int64_t index);
/// @brief HitEvent3D.Attacker accessor.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_get_attacker(void *event);
/// @brief HitEvent3D.Victim accessor.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_get_victim(void *event);
/// @brief HitEvent3D.Hitbox accessor (attacking volume).
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_get_hitbox(void *event);
/// @brief HitEvent3D.Hurtbox accessor (victim volume).
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_get_hurtbox(void *event);
/// @brief HitEvent3D.Point — witness point (fresh Vec3).
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_point(void *event);
/// @brief HitEvent3D.Normal — contact normal (fresh Vec3, +Y fallback).
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_hit_event_normal(void *event);
/// @brief DamageEvent3D.Victim accessor.
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_damage_event_get_victim(void *event);
/// @brief DamageEvent3D.Source accessor (NULL when absent/stale).
/// @param event Value supplied for the event argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_damage_event_get_source(void *event);
/// @brief DamageEvent3D.Amount accessor.
/// @param event Value supplied for the event argument.
/// @return The documented floating-point result.
double rt_game3d_damage_event_get_amount(void *event);
/// @brief DamageEvent3D.Tag accessor.
/// @param event Value supplied for the event argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_damage_event_get_tag(void *event);
/// @brief DamageEvent3D.WasLethal accessor.
/// @param event Value supplied for the event argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_damage_event_get_was_lethal(void *event);

//=========================================================================
// FirstPersonController — FPS camera/movement rig (Zanna.Game3D.FirstPersonController)
//=========================================================================

/// @brief Create a first-person controller bound to the world's camera.
/// @param world World3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_first_person_controller_new(void *world);
/// @brief Get the character controller driving movement.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_first_person_controller_get_character(void *controller);
/// @brief Set the character controller driving movement.
/// @param controller Value supplied for the controller argument.
/// @param character_controller Value supplied for the character controller argument.
void rt_game3d_first_person_controller_set_character(void *controller, void *character_controller);
/// @brief Get the move speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_first_person_controller_get_speed(void *controller);
/// @brief Set the move speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @param speed Value supplied for the speed argument.
void rt_game3d_first_person_controller_set_speed(void *controller, double speed);
/// @brief Get the mouse-look sensitivity multiplier.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_first_person_controller_get_look_sensitivity(void *controller);
/// @brief Set the mouse-look sensitivity multiplier.
/// @param controller Value supplied for the controller argument.
/// @param sensitivity Value supplied for the sensitivity argument.
void rt_game3d_first_person_controller_set_look_sensitivity(void *controller, double sensitivity);
/// @brief Capture and hide the cursor for relative mouse-look.
/// @param controller Value supplied for the controller argument.
void rt_game3d_first_person_controller_capture_mouse(void *controller);
/// @brief Release the captured cursor.
/// @param controller Value supplied for the controller argument.
void rt_game3d_first_person_controller_release_mouse(void *controller);
/// @brief Update movement and look from input over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_first_person_controller_update(void *controller, void *world, double dt);
/// @brief Late-update the camera pose after physics over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_first_person_controller_late_update(void *controller, void *world, double dt);

//=========================================================================
// FreeFlyController — unconstrained spectator camera (Zanna.Game3D.FreeFlyController)
//=========================================================================

/// @brief Create a free-fly (noclip spectator) controller for the world's camera.
/// @param world World3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_free_fly_controller_new(void *world);
/// @brief Get the fly speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_free_fly_controller_get_speed(void *controller);
/// @brief Set the fly speed in units/second.
/// @param controller Value supplied for the controller argument.
/// @param speed Value supplied for the speed argument.
void rt_game3d_free_fly_controller_set_speed(void *controller, double speed);
/// @brief Get the mouse-look sensitivity multiplier.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_free_fly_controller_get_look_sensitivity(void *controller);
/// @brief Set the mouse-look sensitivity multiplier.
/// @param controller Value supplied for the controller argument.
/// @param sensitivity Value supplied for the sensitivity argument.
void rt_game3d_free_fly_controller_set_look_sensitivity(void *controller, double sensitivity);
/// @brief Capture and hide the cursor for relative mouse-look.
/// @param controller Value supplied for the controller argument.
void rt_game3d_free_fly_controller_capture_mouse(void *controller);
/// @brief Release the captured cursor.
/// @param controller Value supplied for the controller argument.
void rt_game3d_free_fly_controller_release_mouse(void *controller);
/// @brief Update fly movement and look from input over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_free_fly_controller_update(void *controller, void *world, double dt);
/// @brief Late-update the camera pose after physics over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_free_fly_controller_late_update(void *controller, void *world, double dt);

//=========================================================================
// OrbitController — target-orbiting camera (Zanna.Game3D.OrbitController)
//=========================================================================

/// @brief Create an orbit controller circling the given target for the world's camera.
/// @param world World3D instance used by the operation.
/// @param target Value supplied for the target argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_orbit_controller_new(void *world, void *target);
/// @brief Get the orbit target (entity or point) being circled.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_orbit_controller_get_target(void *controller);
/// @brief Set the orbit target being circled.
/// @param controller Value supplied for the controller argument.
/// @param target Value supplied for the target argument.
void rt_game3d_orbit_controller_set_target(void *controller, void *target);
/// @brief Get the orbit radius (distance from target) in world units.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_orbit_controller_get_distance(void *controller);
/// @brief Set the orbit radius (distance from target) in world units.
/// @param controller Value supplied for the controller argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_orbit_controller_set_distance(void *controller, double distance);
/// @brief Get the horizontal orbit angle (yaw) in degrees.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_orbit_controller_get_yaw(void *controller);
/// @brief Set the horizontal orbit angle (yaw) in degrees.
/// @param controller Value supplied for the controller argument.
/// @param yaw Value supplied for the yaw argument.
void rt_game3d_orbit_controller_set_yaw(void *controller, double yaw);
/// @brief Get the vertical orbit angle (pitch) in degrees.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_orbit_controller_get_pitch(void *controller);
/// @brief Set the vertical orbit angle (pitch) in degrees.
/// @param controller Value supplied for the controller argument.
/// @param pitch Value supplied for the pitch argument.
void rt_game3d_orbit_controller_set_pitch(void *controller, double pitch);
/// @brief Update orbit yaw/pitch/zoom from input over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_orbit_controller_update(void *controller, void *world, double dt);
/// @brief Late-update the camera pose after physics over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_orbit_controller_late_update(void *controller, void *world, double dt);

//=========================================================================
// FollowController — smoothed third-person chase camera (Zanna.Game3D.FollowController)
//=========================================================================

/// @brief Create a follow controller chasing the target entity at the given offset.
/// @param world World3D instance used by the operation.
/// @param target_entity Value supplied for the target entity argument.
/// @param offset Value supplied for the offset argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_follow_controller_new(void *world, void *target_entity, void *offset);
/// @brief Get the entity being followed.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_follow_controller_get_target(void *controller);
/// @brief Set the entity being followed.
/// @param controller Value supplied for the controller argument.
/// @param target_entity Value supplied for the target entity argument.
void rt_game3d_follow_controller_set_target(void *controller, void *target_entity);
/// @brief Get the follow offset (camera position relative to target) as a Vec3.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_follow_controller_get_offset(void *controller);
/// @brief Set the follow offset (camera position relative to target).
/// @param controller Value supplied for the controller argument.
/// @param offset Value supplied for the offset argument.
void rt_game3d_follow_controller_set_offset(void *controller, void *offset);
/// @brief Get the position-smoothing damping factor.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_follow_controller_get_damping(void *controller);
/// @brief Set the position-smoothing damping factor.
/// @param controller Value supplied for the controller argument.
/// @param damping Value supplied for the damping argument.
void rt_game3d_follow_controller_set_damping(void *controller, double damping);
/// @brief Update the smoothed follow position over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_follow_controller_update(void *controller, void *world, double dt);
/// @brief Late-update the camera pose after physics over `dt` seconds.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_follow_controller_late_update(void *controller, void *world, double dt);

//=========================================================================
// ThirdPersonController — collision-aware spring-arm over-the-shoulder camera
// with camera-relative character drive (Zanna.Game3D.ThirdPersonController)
//=========================================================================

/// @brief Create a third-person spring-arm controller orbiting the target entity.
/// @param world World3D instance used by the operation.
/// @param target_entity Value supplied for the target entity argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_thirdperson_controller_new(void *world, void *target_entity);
/// @brief Get the orbited target entity.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_thirdperson_controller_get_target(void *controller);
/// @brief Set the orbited target entity.
/// @param controller Value supplied for the controller argument.
/// @param target_entity Value supplied for the target entity argument.
void rt_game3d_thirdperson_controller_set_target(void *controller, void *target_entity);
/// @brief Get the optional CharacterController3D drive slot.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_thirdperson_controller_get_character(void *controller);
/// @brief Set the CharacterController3D driven camera-relatively each Update.
/// @param controller Value supplied for the controller argument.
/// @param character_controller Value supplied for the character controller argument.
void rt_game3d_thirdperson_controller_set_character(void *controller, void *character_controller);
/// @brief Get the desired (pre-collision) boom length.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_distance(void *controller);
/// @brief Set the desired boom length (clamped to [MinDistance, MaxDistance]).
/// @param controller Value supplied for the controller argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_thirdperson_controller_set_distance(void *controller, double distance);
/// @brief Get the boom pull-in floor.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_min_distance(void *controller);
/// @brief Set the boom pull-in floor.
/// @param controller Value supplied for the controller argument.
/// @param min_distance Value supplied for the min distance argument.
void rt_game3d_thirdperson_controller_set_min_distance(void *controller, double min_distance);
/// @brief Get the boom length ceiling.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_max_distance(void *controller);
/// @brief Set the boom length ceiling.
/// @param controller Value supplied for the controller argument.
/// @param max_distance Value supplied for the max distance argument.
void rt_game3d_thirdperson_controller_set_max_distance(void *controller, double max_distance);
/// @brief Get the local-space shoulder offset as a Vec3.
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_thirdperson_controller_get_shoulder_offset(void *controller);
/// @brief Set the local-space shoulder offset (x = lateral, y = up, z = forward).
/// @param controller Value supplied for the controller argument.
/// @param offset Value supplied for the offset argument.
void rt_game3d_thirdperson_controller_set_shoulder_offset(void *controller, void *offset);
/// @brief Get the pivot height above the target entity origin.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_pivot_height(void *controller);
/// @brief Set the pivot height above the target entity origin.
/// @param controller Value supplied for the controller argument.
/// @param height Value supplied for the height argument.
void rt_game3d_thirdperson_controller_set_pivot_height(void *controller, double height);
/// @brief Get the boom-release smoothing factor.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_damping(void *controller);
/// @brief Set the boom-release smoothing factor (pull-in stays instant).
/// @param controller Value supplied for the controller argument.
/// @param damping Value supplied for the damping argument.
void rt_game3d_thirdperson_controller_set_damping(void *controller, double damping);
/// @brief Get the camera orbit yaw in degrees (yaw 0 looks down -Z, 90 down -X).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_yaw(void *controller);
/// @brief Set the camera orbit yaw in degrees.
/// @param controller Value supplied for the controller argument.
/// @param yaw Value supplied for the yaw argument.
void rt_game3d_thirdperson_controller_set_yaw(void *controller, double yaw);
/// @brief Get the camera orbit pitch in degrees (positive = camera above pivot).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_pitch(void *controller);
/// @brief Set the camera orbit pitch in degrees (clamped to the pitch range).
/// @param controller Value supplied for the controller argument.
/// @param pitch Value supplied for the pitch argument.
void rt_game3d_thirdperson_controller_set_pitch(void *controller, double pitch);
/// @brief Get the boom sweep sphere radius.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_collision_radius(void *controller);
/// @brief Set the boom sweep sphere radius.
/// @param controller Value supplied for the controller argument.
/// @param radius Value supplied for the radius argument.
void rt_game3d_thirdperson_controller_set_collision_radius(void *controller, double radius);
/// @brief Get the boom collision layer mask.
/// @param controller Value supplied for the controller argument.
/// @return The bit mask of layers tested by the camera boom sweep.
int64_t rt_game3d_thirdperson_controller_get_collision_mask(void *controller);
/// @brief Set the boom collision layer mask (exclude character/projectile layers).
/// @param controller Value supplied for the controller argument.
/// @param mask Value supplied for the mask argument.
void rt_game3d_thirdperson_controller_set_collision_mask(void *controller, int64_t mask);
/// @brief Get whether occluder fading is enabled.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_thirdperson_controller_get_occlusion_fade(void *controller);
/// @brief Enable/disable occluder fading (disable restores faded materials).
/// @param controller Value supplied for the controller argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_thirdperson_controller_set_occlusion_fade(void *controller, int8_t enabled);
/// @brief Get the aim-mode request flag.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_thirdperson_controller_get_aiming(void *controller);
/// @brief Set the aim-mode request flag (distance/FOV blend animates smoothly).
/// @param controller Value supplied for the controller argument.
/// @param aiming Value supplied for the aiming argument.
void rt_game3d_thirdperson_controller_set_aiming(void *controller, int8_t aiming);
/// @brief Get the aim-mode boom length.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_aim_distance(void *controller);
/// @brief Set the aim-mode boom length.
/// @param controller Value supplied for the controller argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_thirdperson_controller_set_aim_distance(void *controller, double distance);
/// @brief Get the aim-mode camera FOV in degrees.
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_thirdperson_controller_get_aim_fov(void *controller);
/// @brief Set the aim-mode camera FOV in degrees.
/// @param controller Value supplied for the controller argument.
/// @param fov Value supplied for the fov argument.
void rt_game3d_thirdperson_controller_set_aim_fov(void *controller, double fov);
/// @brief Get the installed TargetLock3D framing source (NULL if none).
/// @param controller Value supplied for the controller argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_thirdperson_controller_get_lock_target(void *controller);
/// @brief Install a TargetLock3D framing source (NULL to clear).
/// @param controller Value supplied for the controller argument.
/// @param lock Value supplied for the lock argument.
void rt_game3d_thirdperson_controller_set_lock_target(void *controller, void *lock);
/// @brief Pre-physics update: look input, aim blend, character drive.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_thirdperson_controller_update(void *controller, void *world, double dt);
/// @brief Post-sync late update: swept spring-arm camera, aim FOV, occluder fade.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_thirdperson_controller_late_update(void *controller, void *world, double dt);

//=========================================================================
// RailCamera3D — spline camera on a Path3D (Zanna.Game3D.RailCamera3D)
//=========================================================================

/// @brief Create a rail camera riding a Path3D (arclength-constant traversal).
/// @param world World3D instance used by the operation.
/// @param path Runtime path string naming the requested resource.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_rail_camera_new(void *world, void *path);
/// @brief Get the requested arclength-normalized progress [0,1].
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_rail_camera_get_progress(void *controller);
/// @brief Set the requested progress (damped when PositionDamping > 0).
/// @param controller Value supplied for the controller argument.
/// @param progress Value supplied for the progress argument.
void rt_game3d_rail_camera_set_progress(void *controller, double progress);
/// @brief Get the auto-advance speed in units/sec along arclength (0 = manual).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_rail_camera_get_speed(void *controller);
/// @brief Set the auto-advance speed.
/// @param controller Value supplied for the controller argument.
/// @param speed Value supplied for the speed argument.
void rt_game3d_rail_camera_set_speed(void *controller, double speed);
/// @brief Get the progress damping factor (0 = snap).
/// @param controller Value supplied for the controller argument.
/// @return The documented floating-point result.
double rt_game3d_rail_camera_get_position_damping(void *controller);
/// @brief Set the progress damping factor.
/// @param controller Value supplied for the controller argument.
/// @param damping Value supplied for the damping argument.
void rt_game3d_rail_camera_set_position_damping(void *controller, double damping);
/// @brief Look at an entity's post-physics position (clears other look modes).
/// @param controller Value supplied for the controller argument.
/// @param entity Entity3D instance used by the operation.
void rt_game3d_rail_camera_set_look_entity(void *controller, void *entity);
/// @brief Look at a fixed Vec3 point (clears other look modes).
/// @param controller Value supplied for the controller argument.
/// @param point Value supplied for the point argument.
void rt_game3d_rail_camera_set_look_point(void *controller, void *point);
/// @brief Look along a second path evaluated at the same t (clears other modes).
/// @param controller Value supplied for the controller argument.
/// @param path Runtime path string naming the requested resource.
void rt_game3d_rail_camera_set_look_path(void *controller, void *path);
/// @brief Fluent: add an FOV key at arclength t.
/// @param controller Value supplied for the controller argument.
/// @param t Value supplied for the t argument.
/// @param fov Value supplied for the fov argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_rail_camera_add_fov_key(void *controller, double t, double fov);
/// @brief Fluent: add a roll key (degrees about the view axis) at arclength t.
/// @param controller Value supplied for the controller argument.
/// @param t Value supplied for the t argument.
/// @param degrees Value supplied for the degrees argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_rail_camera_add_roll_key(void *controller, double t, double degrees);
/// @brief Whether keys interpolate with smoothstep instead of linearly.
/// @param controller Value supplied for the controller argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_rail_camera_get_key_ease(void *controller);
/// @brief Choose smoothstep (true) or linear (false) key interpolation.
/// @param controller Value supplied for the controller argument.
/// @param smooth Value supplied for the smooth argument.
void rt_game3d_rail_camera_set_key_ease(void *controller, int8_t smooth);
/// @brief Pre-physics update: auto-advance and damp progress.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_rail_camera_update(void *controller, void *world, double dt);
/// @brief Post-sync late update: evaluate the spline + keys, write the camera.
/// @param controller Value supplied for the controller argument.
/// @param world World3D instance used by the operation.
/// @param dt Time interval in seconds.
void rt_game3d_rail_camera_late_update(void *controller, void *world, double dt);

//=========================================================================
// TargetLock3D — lock-on target acquisition, cycling, and framing source
// (Zanna.Game3D.TargetLock3D)
//=========================================================================

/// @brief Create a lock-on helper scoring candidates around the owner entity.
/// @param world World3D instance used by the operation.
/// @param owner_entity Value supplied for the owner entity argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_targetlock_new(void *world, void *owner_entity);
/// @brief Get the currently locked live entity (NULL when unlocked, destroyed, or health-dead).
/// @param lock Value supplied for the lock argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_targetlock_get_target(void *lock);
/// @brief Get the acquisition radius.
/// @param lock Value supplied for the lock argument.
/// @return The documented floating-point result.
double rt_game3d_targetlock_get_max_distance(void *lock);
/// @brief Set the acquisition radius.
/// @param lock Value supplied for the lock argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_targetlock_set_max_distance(void *lock, double distance);
/// @brief Get the half-angle acquisition cone in degrees.
/// @param lock Value supplied for the lock argument.
/// @return The documented floating-point result.
double rt_game3d_targetlock_get_cone_degrees(void *lock);
/// @brief Set the half-angle acquisition cone in degrees.
/// @param lock Value supplied for the lock argument.
/// @param degrees Value supplied for the degrees argument.
void rt_game3d_targetlock_set_cone_degrees(void *lock, double degrees);
/// @brief Get the targetable layer mask.
/// @param lock Value supplied for the lock argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_targetlock_get_candidate_mask(void *lock);
/// @brief Set the targetable layer mask.
/// @param lock Value supplied for the lock argument.
/// @param mask Value supplied for the mask argument.
void rt_game3d_targetlock_set_candidate_mask(void *lock, int64_t mask);
/// @brief Get whether candidates must have line of sight.
/// @param lock Value supplied for the lock argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_targetlock_get_require_los(void *lock);
/// @brief Set whether candidates must have line of sight.
/// @param lock Value supplied for the lock argument.
/// @param require Value supplied for the require argument.
void rt_game3d_targetlock_set_require_los(void *lock, int8_t require);
/// @brief Get the current-target score multiplier.
/// @param lock Value supplied for the lock argument.
/// @return The documented floating-point result.
double rt_game3d_targetlock_get_stickiness(void *lock);
/// @brief Set the current-target score multiplier.
/// @param lock Value supplied for the lock argument.
/// @param stickiness Value supplied for the stickiness argument.
void rt_game3d_targetlock_set_stickiness(void *lock, double stickiness);
/// @brief Get the auto-release distance.
/// @param lock Value supplied for the lock argument.
/// @return The documented floating-point result.
double rt_game3d_targetlock_get_break_distance(void *lock);
/// @brief Set the auto-release distance.
/// @param lock Value supplied for the lock argument.
/// @param distance Value supplied for the distance argument.
void rt_game3d_targetlock_set_break_distance(void *lock, double distance);
/// @brief Get the LoS-break grace period in seconds.
/// @param lock Value supplied for the lock argument.
/// @return The documented floating-point result.
double rt_game3d_targetlock_get_los_grace_seconds(void *lock);
/// @brief Set the LoS-break grace period in seconds (0 = instant release).
/// @param lock Value supplied for the lock argument.
/// @param seconds Value supplied for the seconds argument.
void rt_game3d_targetlock_set_los_grace_seconds(void *lock, double seconds);
/// @brief Acquire the best live, health-nondead candidate in view; true when a target is locked.
/// @param lock Value supplied for the lock argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_targetlock_acquire(void *lock);
/// @brief Release the current target and clear transition flags without firing JustLost.
/// @param lock Value supplied for the lock argument.
void rt_game3d_targetlock_clear(void *lock);
/// @brief Cycle to the nearest candidate left (-1) / right (+1); true on change.
/// @param lock Value supplied for the lock argument.
/// @param direction Value supplied for the direction argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_targetlock_cycle(void *lock, int64_t direction);
/// @brief Per-step maintenance: auto-release on death/distance/LoS-grace.
/// @param lock Value supplied for the lock argument.
/// @param dt Time interval in seconds.
void rt_game3d_targetlock_update(void *lock, double dt);
/// @brief One-shot: true for the frame after a target was acquired.
/// @param lock Value supplied for the lock argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_targetlock_just_acquired(void *lock);
/// @brief One-shot: true for the frame after the target was auto-released.
/// @param lock Value supplied for the lock argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_targetlock_just_lost(void *lock);
/// @brief Rotate a planar move vector up to 12° toward the target bearing.
/// @param lock Value supplied for the lock argument.
/// @param move Value supplied for the move argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_targetlock_locked_move_bias(void *lock, void *move);

//=========================================================================
// Timeline3D — in-engine cutscene sequencer (Zanna.Game3D.Timeline3D)
//=========================================================================

/// @brief Create an empty timeline bound to a world (start via playTimeline).
/// @param world World3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_timeline_new(void *world);
/// @brief Fluent: camera cut (pose applied at t, held until the next camera key).
/// @param tl Value supplied for the tl argument.
/// @param t Value supplied for the t argument.
/// @param pos Value supplied for the pos argument.
/// @param look Value supplied for the look argument.
/// @param fov Value supplied for the fov argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_camera_cut(void *tl, double t, void *pos, void *look, double fov);
/// @brief Fluent: camera spline move over [t0,t1] (look = Vec3|Entity3D|Path3D|NULL).
/// @param tl Value supplied for the tl argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @param path Runtime path string naming the requested resource.
/// @param look_target Value supplied for the look target argument.
/// @param ease Value supplied for the ease argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_camera_move(
    void *tl, double t0, double t1, void *path, void *look_target, int64_t ease);
/// @brief Fluent: FOV ramp lerped over [t0,t1].
/// @param tl Value supplied for the tl argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @param fov0 Value supplied for the fov0 argument.
/// @param fov1 Value supplied for the fov1 argument.
/// @param ease Value supplied for the ease argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_fov_ramp(
    void *tl, double t0, double t1, double fov0, double fov1, int64_t ease);
/// @brief Fluent: fire Animator3D.crossfade on a named entity at t.
/// @param tl Value supplied for the tl argument.
/// @param t Value supplied for the t argument.
/// @param entity_name Value supplied for the entity name argument.
/// @param state_name Value supplied for the state name argument.
/// @param crossfade_seconds Value supplied for the crossfade seconds argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_anim(
    void *tl, double t, rt_string entity_name, rt_string state_name, double crossfade_seconds);
/// @brief Fluent: fire an audio clip at t (2D, or positional at a Vec3).
/// @param tl Value supplied for the tl argument.
/// @param t Value supplied for the t argument.
/// @param clip Value supplied for the clip argument.
/// @param positional Value supplied for the positional argument.
/// @param pos Value supplied for the pos argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_audio(void *tl, double t, void *clip, int8_t positional, void *pos);
/// @brief Fluent: subtitle text shown over [t0,t1].
/// @param tl Value supplied for the tl argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @param text Value supplied for the text argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_subtitle(void *tl, double t0, double t1, rt_string text);
/// @brief Fluent: letterbox bars covering a height fraction over [t0,t1].
/// @param tl Value supplied for the tl argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @param amount Value supplied for the amount argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_letterbox(void *tl, double t0, double t1, double amount);
/// @brief Fluent: full-screen fade from alpha a0 to a1 over [t0,t1].
/// @param tl Value supplied for the tl argument.
/// @param t0 Value supplied for the t0 argument.
/// @param t1 Value supplied for the t1 argument.
/// @param a0 Value supplied for the a0 argument.
/// @param a1 Value supplied for the a1 argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_fade(void *tl, double t0, double t1, double a0, double a1);
/// @brief Fluent: polled event marker fired when the playhead crosses t.
/// @param tl Value supplied for the tl argument.
/// @param t Value supplied for the t argument.
/// @param id Value supplied for the id argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_timeline_add_marker(void *tl, double t, int64_t id);
/// @brief Total duration (max track end time).
/// @param tl Value supplied for the tl argument.
/// @return The documented floating-point result.
double rt_game3d_timeline_get_duration(void *tl);
/// @brief Current playhead time.
/// @param tl Value supplied for the tl argument.
/// @return The documented floating-point result.
double rt_game3d_timeline_get_time(void *tl);
/// @brief True while playing.
/// @param tl Value supplied for the tl argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_timeline_get_playing(void *tl);
/// @brief True once the playhead reached the end (until re-play).
/// @param tl Value supplied for the tl argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_timeline_get_finished(void *tl);
/// @brief Whether skip() is allowed (default true).
/// @param tl Value supplied for the tl argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_timeline_get_skippable(void *tl);
/// @brief Gate skip().
/// @param tl Value supplied for the tl argument.
/// @param skippable Value supplied for the skippable argument.
void rt_game3d_timeline_set_skippable(void *tl, int8_t skippable);
/// @brief One-shot: true for the step after the timeline finished.
/// @param tl Value supplied for the tl argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_timeline_just_finished(void *tl);
/// @brief Markers fired during the most recent step.
/// @param tl Value supplied for the tl argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_timeline_events_fired_count(void *tl);
/// @brief Marker id at index within the most recent step's fired set.
/// @param tl Value supplied for the tl argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_timeline_event_fired_id(void *tl, int64_t index);
/// @brief Currently displayed subtitle text ("" when none).
/// @param tl Value supplied for the tl argument.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_timeline_active_subtitle(void *tl);
/// @brief Skip to the end (final anim states, silent audio, markers fire).
/// @param tl Value supplied for the tl argument.
void rt_game3d_timeline_skip(void *tl);
/// @brief Stop playback and uninstall from the world (controller resumes).
/// @param tl Value supplied for the tl argument.
void rt_game3d_timeline_stop(void *tl);
/// @brief Install and start a timeline on the world (one at a time).
/// @param world World3D instance used by the operation.
/// @param tl Value supplied for the tl argument.
void rt_game3d_world_play_timeline(void *world, void *tl);
/// @brief The world's active timeline (NULL when none).
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_active_timeline(void *world);
/// @brief Stop and uninstall the world's active timeline.
/// @param world World3D instance used by the operation.
void rt_game3d_world_stop_timeline(void *world);

//=========================================================================
// Dialogue3D — 3D conversation surface (Zanna.Game3D.Dialogue3D)
//=========================================================================

/// @brief Create a conversation bound to a world (shown via Show).
/// @param world World3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_dialogue_new(void *world);
/// @brief Fluent: queue a line (text may be a localization key).
/// @param dialogue Value supplied for the dialogue argument.
/// @param speaker Value supplied for the speaker argument.
/// @param text Value supplied for the text argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_dialogue_say(void *dialogue, rt_string speaker, rt_string text);
/// @brief Fluent: queue a voiced line (a valid Sound clip plays when the line starts).
/// @param dialogue Value supplied for the dialogue argument.
/// @param speaker Value supplied for the speaker argument.
/// @param text Value supplied for the text argument.
/// @param clip Value supplied for the clip argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_dialogue_say_voiced(void *dialogue, rt_string speaker, rt_string text, void *clip);
/// @brief Fluent: queue a blocking choice prompt (seq<str> of 1..8 options).
/// @param dialogue Value supplied for the dialogue argument.
/// @param options_seq Value supplied for the options seq argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_dialogue_ask_choice(void *dialogue, void *options_seq);
/// @brief Show: install as the world's active conversation.
/// @details Empty dialogues remain inactive; choice-only dialogues show their prompt immediately.
/// @param dialogue Value supplied for the dialogue argument.
void rt_game3d_dialogue_show(void *dialogue);
/// @brief Hide: release the world slot without clearing queued lines.
/// @param dialogue Value supplied for the dialogue argument.
void rt_game3d_dialogue_hide(void *dialogue);
/// @brief Advance (or complete the reveal first — two-stage skip).
/// @param dialogue Value supplied for the dialogue argument.
void rt_game3d_dialogue_advance(void *dialogue);
/// @brief Complete the current line's reveal instantly.
/// @param dialogue Value supplied for the dialogue argument.
void rt_game3d_dialogue_skip_reveal(void *dialogue);
/// @brief Move the choice highlight (clamped).
/// @param dialogue Value supplied for the dialogue argument.
/// @param delta Value supplied for the delta argument.
void rt_game3d_dialogue_move_choice(void *dialogue, int64_t delta);
/// @brief Confirm the highlighted choice (latches choiceMade/lastChoice).
/// @param dialogue Value supplied for the dialogue argument.
void rt_game3d_dialogue_confirm_choice(void *dialogue);
/// @brief True while shown.
/// @param dialogue Value supplied for the dialogue argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_dialogue_get_active(void *dialogue);
/// @brief Queued line count.
/// @param dialogue Value supplied for the dialogue argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_dialogue_get_line_count(void *dialogue);
/// @brief True while a choice prompt blocks advance.
/// @param dialogue Value supplied for the dialogue argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_dialogue_get_choice_pending(void *dialogue);
/// @brief One-shot: a choice was confirmed since the last query.
/// @param dialogue Value supplied for the dialogue argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_dialogue_choice_made(void *dialogue);
/// @brief Index confirmed by the last choice prompt (-1 when none).
/// @param dialogue Value supplied for the dialogue argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_dialogue_last_choice(void *dialogue);
/// @brief Currently revealed text of the active line.
/// @param dialogue Value supplied for the dialogue argument.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_dialogue_current_text(void *dialogue);
/// @brief Anchor bubbles above a live entity (NULL clears; later destruction releases the anchor).
/// @param dialogue Value supplied for the dialogue argument.
/// @param entity Entity3D instance used by the operation.
void rt_game3d_dialogue_set_speaker_entity(void *dialogue, void *entity);
/// @brief Anchored-bubble mode (falls back to the bottom panel off-screen).
/// @param dialogue Value supplied for the dialogue argument.
/// @param anchored Value supplied for the anchored argument.
void rt_game3d_dialogue_set_anchored(void *dialogue, int8_t anchored);
/// @brief Auto-advance lines after the reveal completes plus a hold.
/// @param dialogue Value supplied for the dialogue argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_dialogue_set_auto_advance(void *dialogue, int8_t enabled);
/// @brief Typewriter speed in characters/second (default 40).
/// @param dialogue Value supplied for the dialogue argument.
/// @param chars_per_second Value supplied for the chars per second argument.
void rt_game3d_dialogue_set_reveal_speed(void *dialogue, double chars_per_second);
/// @brief Bind a MessageBundle for localization-key resolution (NULL unbinds).
/// @param dialogue Value supplied for the dialogue argument.
/// @param bundle Value supplied for the bundle argument.
void rt_game3d_dialogue_set_locale(void *dialogue, void *bundle);
/// @brief Style knobs: panel alpha and speaker-name color.
/// @param dialogue Value supplied for the dialogue argument.
/// @param panel_alpha Value supplied for the panel alpha argument.
/// @param name_color Value supplied for the name color argument.
void rt_game3d_dialogue_set_style(void *dialogue, double panel_alpha, int64_t name_color);

//=========================================================================
// LipSync3D — amplitude visemes + blink/gaze layer (Zanna.Game3D.LipSync3D)
//=========================================================================

/// @brief Create and attach a facial component to an entity (one per entity).
/// @param entity Entity3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_lipsync_new(void *entity);
/// @brief Entity accessor for the attached LipSync3D (NULL when none).
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_entity_get_lipsync(void *entity);
/// @brief Fluent: bind the MorphTarget3D the shape bindings drive.
/// @param lipsync Value supplied for the lipsync argument.
/// @param morph Value supplied for the morph argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_lipsync_bind_morph(void *lipsync, void *morph);
/// @brief Fluent: bind a mouth shape (up to 4) with a per-shape weight scale.
/// @param lipsync Value supplied for the lipsync argument.
/// @param shape_name Value supplied for the shape name argument.
/// @param scale Value supplied for the scale argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_lipsync_bind_mouth_shape(void *lipsync, rt_string shape_name, double scale);
/// @brief Fluent: create the gaze LookAt solver for a named head bone.
/// @param lipsync Value supplied for the lipsync argument.
/// @param bone_name Value supplied for the bone name argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_lipsync_bind_head_bone(void *lipsync, rt_string bone_name);
/// @brief Drive from a playing voice (enables per-voice metering).
/// @param lipsync Value supplied for the lipsync argument.
/// @param voice_id Value supplied for the voice id argument.
void rt_game3d_lipsync_drive(void *lipsync, int64_t voice_id);
/// @brief Inject one explicit 0..1 level sample; subsequent ticks release toward zero.
/// @param lipsync Value supplied for the lipsync argument.
/// @param level Value supplied for the level argument.
void rt_game3d_lipsync_drive_level(void *lipsync, double level);
/// @brief Release the drive (mouth eases closed).
/// @param lipsync Value supplied for the lipsync argument.
void rt_game3d_lipsync_stop(void *lipsync);
/// @brief Configure the seeded procedural blink layer.
/// @param lipsync Value supplied for the lipsync argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
/// @param shape_name Value supplied for the shape name argument.
/// @param min_interval Value supplied for the min interval argument.
/// @param max_interval Value supplied for the max interval argument.
void rt_game3d_lipsync_set_blink(
    void *lipsync, int8_t enabled, rt_string shape_name, double min_interval, double max_interval);
/// @brief Ease gaze toward an Entity3D/Vec3 target (NULL clears).
/// @param lipsync Value supplied for the lipsync argument.
/// @param target Value supplied for the target argument.
void rt_game3d_lipsync_set_gaze(void *lipsync, void *target);
/// @brief True while a voice drive is active.
/// @param lipsync Value supplied for the lipsync argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_lipsync_get_driving(void *lipsync);
/// @brief Current smoothed envelope level.
/// @param lipsync Value supplied for the lipsync argument.
/// @return The documented floating-point result.
double rt_game3d_lipsync_get_level(void *lipsync);

//=========================================================================
// World3D — top-level game world bundling all subsystems (Zanna.Game3D.World3D)
//=========================================================================

/// @brief Create a game world (window + default camera/scene/physics/etc.).
/// @param title Value supplied for the title argument.
/// @param width Value supplied for the width argument.
/// @param height Value supplied for the height argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_world_new(rt_string title, int64_t width, int64_t height);
/// @brief Create a game world with explicit camera FOV and near/far clip planes.
/// @param title Value supplied for the title argument.
/// @param width Value supplied for the width argument.
/// @param height Value supplied for the height argument.
/// @param fov_deg Value supplied for the fov deg argument.
/// @param near_plane Value supplied for the near plane argument.
/// @param far_plane Value supplied for the far plane argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_world_new_with_camera(rt_string title,
                                      int64_t width,
                                      int64_t height,
                                      double fov_deg,
                                      double near_plane,
                                      double far_plane);
/// @brief Create a game world with a camera FOV authored in horizontal degrees.
/// @details The runtime converts @p horizontal_fov_deg to the vertical FOV used by the renderer
///   based on the initial window aspect ratio. This avoids the wide-angle/fisheye look that can
///   happen when a conventional horizontal FOV value is passed to the vertical-FOV constructor.
/// @param title Value supplied for the title argument.
/// @param width Value supplied for the width argument.
/// @param height Value supplied for the height argument.
/// @param horizontal_fov_deg Value supplied for the horizontal fov deg argument.
/// @param near_plane Value supplied for the near plane argument.
/// @param far_plane Value supplied for the far plane argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_world_new_with_horizontal_camera(rt_string title,
                                                 int64_t width,
                                                 int64_t height,
                                                 double horizontal_fov_deg,
                                                 double near_plane,
                                                 double far_plane);
/// @brief Create a fullscreen world at desktop resolution (no windowed flash).
/// @param title Value supplied for the title argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_new_fullscreen(rt_string title);
/// @brief Create a world that renders inside an existing 2D canvas's window
/// (single-window mode); the window is borrowed and survives world teardown.
void *rt_game3d_world_new_with_canvas_camera(void *canvas2d,
                                             double fov_deg,
                                             double near_plane,
                                             double far_plane);
/// @brief Create a fullscreen world with a camera FOV authored in horizontal degrees.
/// @param title Value supplied for the title argument.
/// @param horizontal_fov_deg Value supplied for the horizontal fov deg argument.
/// @param near_plane Value supplied for the near plane argument.
/// @param far_plane Value supplied for the far plane argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_world_new_fullscreen_with_horizontal_camera(rt_string title,
                                                            double horizontal_fov_deg,
                                                            double near_plane,
                                                            double far_plane);
/// @brief Destroy the world and all owned subsystems, closing its window.
/// @param world World3D instance used by the operation.
void rt_game3d_world_destroy(void *world);
/// @brief True if the world has been destroyed.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_is_destroyed(void *world);
/// @brief Get the world's rendering canvas.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_canvas(void *world);
/// @brief Get the world's active camera.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_camera(void *world);
/// @brief Get the world's scene graph.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_scene(void *world);
/// @brief Get the world's physics simulation.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_physics(void *world);
/// @brief Get the world's input-state object.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_input(void *world);
/// @brief Get the world's audio subsystem.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_audio(void *world);
/// @brief Get the world's effects registry.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_effects(void *world);
/// @brief Get the world's owned streaming controller, creating it on first access.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_stream(void *world);
/// @brief Get the most recent frame's delta time in seconds.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_dt(void *world);
/// @brief Get total elapsed wall-clock time since the world started, in seconds.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_elapsed(void *world);
/// @brief Get the current frame counter.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_frame(void *world);
/// @brief Count fixed-timestep updates discarded by the spiral-of-death guard.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_dropped_fixed_steps(void *world);
/// @brief Get the render interpolation fraction left in the fixed-timestep accumulator.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_fixed_interpolation_alpha(void *world);
/// @brief Enable/disable built-in fixed-step render interpolation of entity poses.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_world_set_render_interpolation(void *world, int8_t enabled);
/// @brief Whether built-in fixed-step render interpolation is enabled.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_get_render_interpolation(void *world);
/// @brief Count spawned Entity3D objects currently owned by the world.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_entity_count(void *world);
/// @brief Count physics bodies currently registered through spawned entities.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_body_count(void *world);
/// @brief Count main 3D draw submissions queued by the latest ended frame.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_draw_count(void *world);
/// @brief Count drawable scene nodes submitted by the latest scene draw.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_visible_node_count(void *world);
/// @brief Count draw submissions skipped by latest visibility culling.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_occluded_draw_count(void *world);
/// @brief Count draw submissions skipped specifically by CPU frustum culling.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_frustum_culled_draw_count(void *world);
/// @brief Count draw submissions skipped specifically by the CPU occlusion grid.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_cpu_occluded_draw_count(void *world);
/// @brief Count bytes resident in the world-owned stream controller, if any.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_stream_resident_bytes(void *world);
/// @brief Get the configured worker count for internal deterministic jobs.
/// @param world World3D instance used by the operation.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_get_worker_count(void *world);
/// @brief True when internal jobs are allowed to use worker threads.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_get_jobs_enabled(void *world);
/// @brief Set the internal worker count; values <= 1 keep jobs disabled.
/// @param world World3D instance used by the operation.
/// @param worker_count Requested worker count.
void rt_game3d_world_set_worker_count(void *world, int64_t worker_count);
/// @brief True when camera-relative floating-origin rebasing is enabled.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_get_floating_origin(void *world);
/// @brief Enable or disable camera-relative floating-origin rebasing.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_world_set_floating_origin(void *world, int8_t enabled);
/// @brief Get the accumulated world-origin offset as a Vec3.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_get_world_origin(void *world);
/// @brief Set the camera-distance threshold that triggers a floating-origin rebase.
/// @param world World3D instance used by the operation.
/// @param meters Value supplied for the meters argument.
void rt_game3d_world_set_origin_rebase_threshold(void *world, double meters);
/// @brief Apply a manual floating-origin rebase. Must be called between frames.
/// @param world World3D instance used by the operation.
/// @param dx Value supplied for the dx argument.
/// @param dy Value supplied for the dy argument.
/// @param dz Value supplied for the dz argument.
void rt_game3d_world_rebase_origin(void *world, double dx, double dy, double dz);
/// @brief Spawn an entity into the world (adds it to scene + physics); returns the entity.
/// @param world World3D instance used by the operation.
/// @param entity Entity3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_spawn(void *world, void *entity);
/// @brief Spawn an invisible static heightfield collider (from a Pixels heightmap) so a
///   standalone Terrain3D becomes solid to physics/character controllers. The heightfield is
///   centered on the entity; pass the terrain center as (pos_x,pos_y,pos_z). Returns the entity.
/// @param world World3D instance used by the operation.
/// @param heightmap Value supplied for the heightmap argument.
/// @param scale_x Value supplied for the scale x argument.
/// @param scale_y Value supplied for the scale y argument.
/// @param scale_z Value supplied for the scale z argument.
/// @param pos_x Value supplied for the pos x argument.
/// @param pos_y Value supplied for the pos y argument.
/// @param pos_z Value supplied for the pos z argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_spawn_heightfield_collider(void *world,
                                                 void *heightmap,
                                                 double scale_x,
                                                 double scale_y,
                                                 double scale_z,
                                                 double pos_x,
                                                 double pos_y,
                                                 double pos_z);
/// @brief Despawn an entity, removing it from scene and physics.
/// @param world World3D instance used by the operation.
/// @param entity Entity3D instance used by the operation.
void rt_game3d_world_despawn(void *world, void *entity);
/// @brief Find a scene node by name, or null if absent.
/// @param world World3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_find_node(void *world, rt_string name);
/// @brief Find a scene node by name as Some(SceneNode3D), or None when absent.
/// @param world World3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_find_node_option(void *world, rt_string name);
/// @brief Find a spawned entity by name, or null if absent.
/// @param world World3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_find_entity(void *world, rt_string name);
/// @brief Find a spawned entity by name as Some(Entity3D), or None when absent.
/// @param world World3D instance used by the operation.
/// @param name Runtime string naming the requested object or property.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_find_entity_option(void *world, rt_string name);
/// @brief Install a camera controller to drive the world's camera each frame.
/// @param world World3D instance used by the operation.
/// @param controller Value supplied for the controller argument.
void rt_game3d_world_set_camera_controller(void *world, void *controller);
/// @brief Point the camera to look at the given target entity or point.
/// @param world World3D instance used by the operation.
/// @param target Value supplied for the target argument.
void rt_game3d_world_look_at(void *world, void *target);
/// @brief Notify the world its window was resized to the given dimensions.
/// @param world World3D instance used by the operation.
/// @param width Value supplied for the width argument.
/// @param height Value supplied for the height argument.
void rt_game3d_world_on_resize(void *world, int64_t width, int64_t height);
/// @brief Set the global ambient light color.
/// @param world World3D instance used by the operation.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
void rt_game3d_world_set_ambient(void *world, double r, double g, double b);
/// @brief Enable/disable image-based lighting from the world's skybox environment.
/// @param world World3D instance used by the operation.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_world_set_ibl_enabled(void *world, int8_t enabled);
/// @brief True when image-based lighting is enabled for the world's canvas.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_get_ibl_enabled(void *world);
/// @brief Scale the environment lighting contribution (default 1.0).
/// @param world World3D instance used by the operation.
/// @param intensity Value supplied for the intensity argument.
void rt_game3d_world_set_ibl_intensity(void *world, double intensity);
/// @brief Current environment lighting intensity for the world's canvas.
/// @param world World3D instance used by the operation.
/// @return The documented floating-point result.
double rt_game3d_world_get_ibl_intensity(void *world);
/// @brief Bind a light into the given light slot.
/// @param world World3D instance used by the operation.
/// @param slot Value supplied for the slot argument.
/// @param light Value supplied for the light argument.
void rt_game3d_world_add_light(void *world, int64_t slot, void *light);
/// @brief Clear all bound light slots.
/// @param world World3D instance used by the operation.
void rt_game3d_world_clear_lights(void *world);
/// @brief Set the world's skybox from a cubemap.
/// @param world World3D instance used by the operation.
/// @param cubemap Value supplied for the cubemap argument.
void rt_game3d_world_set_skybox(void *world, void *cubemap);
/// @brief Configure distance fog: RGB color plus near/far planes.
/// @param world World3D instance used by the operation.
/// @param r Red color channel.
/// @param g Green color channel.
/// @param b Blue color channel.
/// @param near_plane Value supplied for the near plane argument.
/// @param far_plane Value supplied for the far plane argument.
void rt_game3d_world_set_fog(
    void *world, double r, double g, double b, double near_plane, double far_plane);
/// @brief Apply a render quality preset (RT_GAME3D_QUALITY_*) to the world.
/// @param world World3D instance used by the operation.
/// @param quality Value supplied for the quality argument.
void rt_game3d_world_set_quality(void *world, int64_t quality);
/// @brief Bake a NavMesh3D from the world's current Scene3D.
/// @param world World3D instance used by the operation.
/// @param agent_radius Value supplied for the agent radius argument.
/// @param agent_height Value supplied for the agent height argument.
/// @param max_slope Value supplied for the max slope argument.
/// @param cell_size Requested cell size.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_bake_nav_mesh(
    void *world, double agent_radius, double agent_height, double max_slope, double cell_size);
/// @brief Bake a tiled NavMesh3D from the world's current Scene3D.
/// @param world World3D instance used by the operation.
/// @param tile_size Requested tile size.
/// @param agent_radius Value supplied for the agent radius argument.
/// @param agent_height Value supplied for the agent height argument.
/// @param max_slope Value supplied for the max slope argument.
/// @param cell_size Requested cell size.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_bake_tiled_nav_mesh(void *world,
                                          double tile_size,
                                          double agent_radius,
                                          double agent_height,
                                          double max_slope,
                                          double cell_size);
/// @brief Count collision events recorded this frame for the given phase.
/// @param world World3D instance used by the operation.
/// @param phase Value supplied for the phase argument.
/// @return The number of recorded events whose phase matches @p phase.
int64_t rt_game3d_world_collision_event_count(void *world, int64_t phase);
/// @brief Get the i-th collision event for the given phase.
/// @param world World3D instance used by the operation.
/// @param phase Value supplied for the phase argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_collision_event(void *world, int64_t phase, int64_t index);
/// @brief Clear the recorded collision-event buffers.
/// @param world World3D instance used by the operation.
void rt_game3d_world_clear_collision_events(void *world);
/// @brief Set the physics gravity vector.
/// @param world World3D instance used by the operation.
/// @param x X-axis value.
/// @param y Y-axis value.
/// @param z Z-axis value.
void rt_game3d_world_set_gravity(void *world, double x, double y, double z);
/// @brief Run the blocking game loop, calling `update(dt)` each frame until the window closes.
/// @details Native callers pass a C-callable function pointer. VM/BytecodeVM callers pass a script
/// function reference; the Game3D VM bridge resolves it and calls the runtime with a native
/// trampoline. The update callback signature is `(Float) -> Unit`.
/// @param world World3D instance used by the operation.
/// @param update Optional callback handle validated by the runtime.
void rt_game3d_world_run(void *world, void *update);
/// @brief Run the game loop with per-frame `update(dt)` and 2D `overlay()` callbacks.
/// @details Native callers pass C-callable function pointers. VM/BytecodeVM callers pass script
/// function references, which the Game3D VM bridge invokes through native trampolines. The update
/// signature is `(Float) -> Unit`; the overlay signature is `() -> Unit`.
/// @param world World3D instance used by the operation.
/// @param update Optional callback handle validated by the runtime.
/// @param overlay Optional callback handle validated by the runtime.
void rt_game3d_world_run_with_overlay(void *world, void *update, void *overlay);
/// @brief Run a fixed-timestep game loop with the given step and `update(dt)` callback.
/// @details Native callers pass a C-callable function pointer. VM/BytecodeVM callers pass a script
/// function reference, which the Game3D VM bridge invokes through a native trampoline. The update
/// callback signature is `(Float) -> Unit`.
/// @param world World3D instance used by the operation.
/// @param step_sec Time interval in seconds.
/// @param update Optional callback handle validated by the runtime.
void rt_game3d_world_run_fixed(void *world, double step_sec, void *update);
/// @brief Run a fixed-timestep loop with `update(dt)` and 2D `overlay()` callbacks.
/// @details Native callers pass C-callable function pointers. VM/BytecodeVM callers pass script
/// function references, which the Game3D VM bridge invokes through native trampolines. The update
/// signature is `(Float) -> Unit`; the overlay signature is `() -> Unit`.
/// @param world World3D instance used by the operation.
/// @param step_sec Time interval in seconds.
/// @param update Optional callback handle validated by the runtime.
/// @param overlay Optional callback handle validated by the runtime.
void rt_game3d_world_run_fixed_with_overlay(void *world,
                                            double step_sec,
                                            void *update,
                                            void *overlay);
/// @brief Run a deterministic fixed number of frames at a fixed step using `update(dt)`.
/// @details Native callers pass a C-callable function pointer. VM/BytecodeVM callers pass a script
/// function reference, which the Game3D VM bridge invokes through a native trampoline. The update
/// callback signature is `(Float) -> Unit`.
/// @param world World3D instance used by the operation.
/// @param frame_count Number of frames requested.
/// @param step_sec Time interval in seconds.
/// @param update Optional callback handle validated by the runtime.
void rt_game3d_world_run_frames(void *world, int64_t frame_count, double step_sec, void *update);
/// @brief Run a fixed number of frames with no update callback (pure simulation/render).
/// @param world World3D instance used by the operation.
/// @param frame_count Number of frames requested.
/// @param step_sec Time interval in seconds.
void rt_game3d_world_run_frames_only(void *world, int64_t frame_count, double step_sec);
/// @brief Advance the world one frame manually; returns false when the window should close.
/// @param world World3D instance used by the operation.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_tick(void *world);
/// @brief Step physics simulation by the given fixed step in seconds.
/// @param world World3D instance used by the operation.
/// @param step_sec Time interval in seconds.
void rt_game3d_world_step_simulation(void *world, double step_sec);
/// @brief Begin a frame: poll input, update timing, and prepare the canvas.
/// @param world World3D instance used by the operation.
void rt_game3d_world_begin_frame(void *world);
/// @brief Draw the scene graph into the current frame.
/// @param world World3D instance used by the operation.
void rt_game3d_world_draw_scene(void *world);
/// @brief Draw registered effects into the current frame.
/// @param world World3D instance used by the operation.
void rt_game3d_world_draw_effects(void *world);
/// @brief End the 3D scene pass for the current frame.
/// @param world World3D instance used by the operation.
void rt_game3d_world_end_scene(void *world);
/// @brief Draw a 2D `overlay()` callback over the current frame.
/// @details Native callers pass a C-callable function pointer. VM/BytecodeVM callers pass a script
/// function reference, which the Game3D VM bridge invokes through a native trampoline. The overlay
/// callback signature is `() -> Unit`.
/// @param world World3D instance used by the operation.
/// @param overlay Optional callback handle validated by the runtime.
void rt_game3d_world_draw_overlay(void *world, void *overlay);
/// @brief Finalize and capture the rendered frame as a Pixels image; returns it.
/// @param world World3D instance used by the operation.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_capture_final_frame(void *world);
/// @brief Present the finished frame to the window (flip buffers).
/// @param world World3D instance used by the operation.
void rt_game3d_world_present(void *world);

//=========================================================================
// WorldStream3D — deterministic streaming state (Zanna.Game3D.WorldStream3D)
//=========================================================================

/// @brief Create a stream controller bound to a live World3D.
/// @param world World3D instance used by the operation.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_world_stream_new(void *world);
/// @brief Get resident scene-cell count.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_resident_cell_count(void *stream);
/// @brief Get resident terrain-tile count.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_resident_terrain_tile_count(void *stream);
/// @brief Return the nth currently resident Terrain3D tile, or NULL.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_stream_get_resident_terrain_tile(void *stream, int64_t index);
/// @brief Get parsed scene-cell entry count.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_cell_count(void *stream);
/// @brief Get a parsed scene-cell name, or "" for an invalid index.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_cell_name(void *stream, int64_t index);
/// @brief Get a parsed scene-cell center, or NULL for an invalid index.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_stream_get_cell_center(void *stream, int64_t index);
/// @brief Return whether a parsed scene-cell entry is currently resident.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_get_cell_resident(void *stream, int64_t index);
/// @brief Get the byte estimate for a parsed scene-cell entry.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_cell_bytes(void *stream, int64_t index);
/// @brief Get parsed scene-cell material metadata, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_cell_material(void *stream, int64_t index);
/// @brief Get parsed scene-cell optional binary sidecar path, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_cell_sidecar(void *stream, int64_t index);
/// @brief Get resident bytes of a scene-cell's loaded binary sidecar payload (0 if none/unloaded).
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_cell_sidecar_bytes(void *stream, int64_t index);
/// @brief Get parsed scene-cell collision/render layer metadata, or 0 if unset/invalid.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_cell_layer(void *stream, int64_t index);
/// @brief Get parsed scene-cell collision mask metadata, or all bits if unset.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The cell collision mask, or the all-layers mask when the metadata is unset.
int64_t rt_game3d_world_stream_get_cell_collision_mask(void *stream, int64_t index);
/// @brief Return whether parsed scene-cell collision is enabled.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_get_cell_collision_enabled(void *stream, int64_t index);
/// @brief Get parsed scene-cell navigation area metadata, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_cell_nav_area(void *stream, int64_t index);
/// @brief Get parsed scene-cell traversal cost metadata.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented floating-point result.
double rt_game3d_world_stream_get_cell_traversal_cost(void *stream, int64_t index);
/// @brief Get parsed terrain-tile entry count.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_terrain_tile_count(void *stream);
/// @brief Get a parsed terrain-tile name, or "" for an invalid index.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_terrain_tile_name(void *stream, int64_t index);
/// @brief Get a parsed terrain-tile heightmap sidecar path, or "" for an invalid index/missing
/// path.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_terrain_tile_heightmap(void *stream, int64_t index);
/// @brief Get a parsed terrain-tile center, or NULL for an invalid index.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_world_stream_get_terrain_tile_center(void *stream, int64_t index);
/// @brief Return whether a parsed terrain-tile entry is currently resident.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_get_terrain_tile_resident(void *stream, int64_t index);
/// @brief Get the byte estimate for a parsed terrain-tile entry.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_terrain_tile_bytes(void *stream, int64_t index);
/// @brief Get parsed terrain-tile material metadata, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_terrain_tile_material(void *stream, int64_t index);
/// @brief Get parsed terrain-tile optional binary sidecar path, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_terrain_tile_sidecar(void *stream, int64_t index);
/// @brief Get resident bytes of a terrain-tile's loaded binary sidecar payload (0 if
/// none/unloaded).
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_terrain_tile_sidecar_bytes(void *stream, int64_t index);
/// @brief Get parsed terrain-tile collision/render layer metadata.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_terrain_tile_layer(void *stream, int64_t index);
/// @brief Get parsed terrain-tile collision mask metadata.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The collision-layer mask parsed for the indexed terrain tile.
int64_t rt_game3d_world_stream_get_terrain_tile_collision_mask(void *stream, int64_t index);
/// @brief Return whether parsed terrain-tile collision is enabled.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_get_terrain_tile_collision_enabled(void *stream, int64_t index);
/// @brief Get parsed terrain-tile navigation area metadata, or "" for invalid/missing.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_terrain_tile_nav_area(void *stream, int64_t index);
/// @brief Get parsed terrain-tile traversal cost metadata.
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return The documented floating-point result.
double rt_game3d_world_stream_get_terrain_tile_traversal_cost(void *stream, int64_t index);
/// @brief Get pending stream request count.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_pending_request_count(void *stream);
/// @brief Get estimated resident stream bytes.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_resident_bytes(void *stream);
/// @brief Set the streaming focus point.
/// @param stream Value supplied for the stream argument.
/// @param position Value supplied for the position argument.
void rt_game3d_world_stream_set_center(void *stream, void *position);
/// @brief Set load/unload radii in world units.
/// @param stream Value supplied for the stream argument.
/// @param load_radius Value supplied for the load radius argument.
/// @param unload_radius Value supplied for the unload radius argument.
void rt_game3d_world_stream_set_radii(void *stream, double load_radius, double unload_radius);
/// @brief Bound resident stream bytes; negative means unlimited.
/// @param stream Value supplied for the stream argument.
/// @param bytes Value supplied for the bytes argument.
void rt_game3d_world_stream_set_residency_budget(void *stream, int64_t bytes);
/// @brief Mount a tiled terrain streaming manifest.
/// @param stream Value supplied for the stream argument.
/// @param manifest_path Value supplied for the manifest path argument.
void rt_game3d_world_stream_mount_tiled_terrain(void *stream, rt_string manifest_path);
/// @brief Mount a scene-cell streaming manifest.
/// @param stream Value supplied for the stream argument.
/// @param manifest_path Value supplied for the manifest path argument.
void rt_game3d_world_stream_mount_cells(void *stream, rt_string manifest_path);
/// @brief Advance stream scheduling/telemetry.
/// @param stream Value supplied for the stream argument.
/// @param dt Time interval in seconds.
void rt_game3d_world_stream_update(void *stream, double dt);

/* Game3D.Surfaces — process-global surface-tag registry (plan 20). */
/// @brief Register (or look up) a surface name; ids stable from 1; idempotent.
/// @param name Runtime string naming the requested object or property.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_surfaces_register(rt_string name);
/// @brief Name for a surface id, or "" when unknown.
/// @param id Value supplied for the id argument.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_surfaces_name_of(int64_t id);
/// @brief Id for a surface name, or 0 when unregistered.
/// @param name Runtime string naming the requested object or property.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_surfaces_id_of(rt_string name);
/// @brief Number of registered surfaces.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_surfaces_count(void);

/* Footsteps — SurfaceTable3D + Footsteps3D (plan 23). */
/// @brief Create an empty per-surface footstep table (row 0 = untyped default).
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_surface_table_new(void);
/// @brief Append a Sound clip variant for a surface id (fluent; up to 8 per row).
/// @param table Value supplied for the table argument.
/// @param surface_id Value supplied for the surface id argument.
/// @param clip Value supplied for the clip argument.
/// @return The runtime handle described above, or NULL when unavailable.
void *rt_game3d_surface_table_add_clip(void *table, int64_t surface_id, void *clip);
/// @brief Hearing-stimulus loudness scale for a surface row (fluent; default 1).
/// @param table Value supplied for the table argument.
/// @param surface_id Value supplied for the surface id argument.
/// @param loudness Value supplied for the loudness argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_surface_table_set_loudness(void *table, int64_t surface_id, double loudness);
/// @brief Configured clip count for a surface row.
/// @param table Value supplied for the table argument.
/// @param surface_id Value supplied for the surface id argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_surface_table_clip_count(void *table, int64_t surface_id);
/// @brief Bind footsteps to an entity (requires an animator for event mode).
/// @param entity Entity3D instance used by the operation.
/// @param table Value supplied for the table argument.
/// @return A new GC-managed runtime handle, or NULL on failure.
void *rt_game3d_footsteps_new(void *entity, void *table);
/// @brief Animator event-name prefix consumed as steps (fluent; default "footstep").
/// @param steps Value supplied for the steps argument.
/// @param prefix Value supplied for the prefix argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_footsteps_set_event_prefix(void *steps, rt_string prefix);
/// @brief Ground raycast mask (fluent; default -1 = everything).
/// @param steps Value supplied for the steps argument.
/// @param mask Value supplied for the mask argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_footsteps_set_ground_mask(void *steps, int64_t mask);
/// @brief Playback volume scale applied to the audio master volume (fluent; 0..4, default 1).
/// @param steps Value supplied for the steps argument.
/// @param scale Value supplied for the scale argument.
/// @return The receiver handle for fluent chaining, or NULL when unavailable.
void *rt_game3d_footsteps_set_volume_scale(void *steps, double scale);
/// @brief Lifetime steps fired (telemetry/tests).
/// @param steps Value supplied for the steps argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_footsteps_get_step_count(void *steps);
/// @brief Surface id resolved by the most recent step (0 = untyped).
/// @param steps Value supplied for the steps argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_footsteps_get_last_surface(void *steps);

/* Interaction — Interactable3D + Interactor3D (plan 21). */
/// @brief Attach a new interactable component to an entity.
/// @details The component starts enabled with prompt `"Use"` and a two-unit focus radius.
/// @param entity Entity3D that will own the component.
/// @return A new Interactable3D handle, or NULL when the entity is invalid or allocation fails.
void *rt_game3d_interactable_new(void *entity);
/// @brief Set the prompt displayed while an interactable has focus.
/// @param item Interactable3D component to configure.
/// @param prompt Runtime string containing the new prompt.
/// @return @p item for fluent chaining.
void *rt_game3d_interactable_with_prompt(void *item, rt_string prompt);
/// @brief Get an interactable's current prompt.
/// @param item Interactable3D component to query.
/// @return A retained runtime string, or the empty string for an invalid component.
rt_string rt_game3d_interactable_get_prompt(void *item);
/// @brief Set the application-defined interaction kind.
/// @param item Interactable3D component to configure.
/// @param kind Application-defined kind value reported with the interaction.
/// @return @p item for fluent chaining.
void *rt_game3d_interactable_with_kind(void *item, int64_t kind);
/// @brief Get an interactable's application-defined kind.
/// @param item Interactable3D component to query.
/// @return The configured kind, or 0 for an invalid component.
int64_t rt_game3d_interactable_get_kind(void *item);
/// @brief Set the maximum distance from which the item can receive focus.
/// @details Positive finite values are accepted and clamped to 64 world units.
/// @param item Interactable3D component to configure.
/// @param radius Focus radius in world units.
/// @return @p item for fluent chaining.
void *rt_game3d_interactable_with_radius(void *item, double radius);
/// @brief Get an interactable's focus radius.
/// @param item Interactable3D component to query.
/// @return The radius in world units, or 0 for an invalid component.
double rt_game3d_interactable_get_radius(void *item);
/// @brief Enable or disable an interactable for focus scans.
/// @param item Interactable3D component to configure.
/// @param enabled Non-zero to enable interaction; zero to disable it.
void rt_game3d_interactable_set_enabled(void *item, int8_t enabled);
/// @brief Test whether an interactable participates in focus scans.
/// @param item Interactable3D component to query.
/// @return 1 when enabled, or 0 when disabled or invalid.
int8_t rt_game3d_interactable_get_enabled(void *item);
/// @brief Set the priority used to break competing focus candidates.
/// @param item Interactable3D component to configure.
/// @param priority Finite focus-priority value, bounded to the runtime coordinate limit.
void rt_game3d_interactable_set_focus_priority(void *item, double priority);
/// @brief Get an interactable's focus priority.
/// @param item Interactable3D component to query.
/// @return The configured priority, or 0 for an invalid component.
double rt_game3d_interactable_get_focus_priority(void *item);
/// @brief Attach a new interaction scanner to an entity.
/// @details The scanner starts with a 70-degree cone, requires line of sight, and tests all layers.
/// @param entity Entity3D that will own the scanner.
/// @return A new Interactor3D handle, or NULL when the entity is invalid or allocation fails.
void *rt_game3d_interactor_new(void *entity);
/// @brief Set the scanner's angular focus cone.
/// @details Finite values greater than one degree are accepted and clamped to 180 degrees.
/// @param scanner Interactor3D component to configure.
/// @param degrees Full cone angle in degrees.
void rt_game3d_interactor_set_cone_degrees(void *scanner, double degrees);
/// @brief Get the scanner's angular focus cone.
/// @param scanner Interactor3D component to query.
/// @return The full cone angle in degrees, or 0 for an invalid scanner.
double rt_game3d_interactor_get_cone_degrees(void *scanner);
/// @brief Control whether focus candidates must have an unobstructed line of sight.
/// @param scanner Interactor3D component to configure.
/// @param required Non-zero to require a line-of-sight test; zero to skip it.
void rt_game3d_interactor_set_require_los(void *scanner, int8_t required);
/// @brief Test whether line of sight is required for focus.
/// @param scanner Interactor3D component to query.
/// @return 1 when required, or 0 when disabled or invalid.
int8_t rt_game3d_interactor_get_require_los(void *scanner);
/// @brief Set the collision-layer mask used by line-of-sight queries.
/// @param scanner Interactor3D component to configure.
/// @param mask Bit mask of layers that can obstruct interaction focus.
void rt_game3d_interactor_set_los_mask(void *scanner, int64_t mask);
/// @brief Get the collision-layer mask used by line-of-sight queries.
/// @param scanner Interactor3D component to query.
/// @return The configured obstruction-layer mask, or 0 for an invalid scanner.
int64_t rt_game3d_interactor_get_los_mask(void *scanner);
/// @brief Get the interactable currently selected by the scanner.
/// @param scanner Interactor3D component to query.
/// @return A retained Interactable3D handle, or NULL when no target has focus.
void *rt_game3d_interactor_get_focused(void *scanner);
/// @brief Consume the one-shot notification that the focused target changed.
/// @param scanner Interactor3D component to query.
/// @return 1 when focus changed since the previous call; otherwise 0.
int8_t rt_game3d_interactor_focus_changed(void *scanner);
/// @brief Record an interaction with the currently focused target.
/// @details Successful requests update the interaction count and last-interacted handle.
/// @param scanner Interactor3D component issuing the interaction.
/// @return 1 when a target was focused and recorded; otherwise 0.
int8_t rt_game3d_interactor_interact(void *scanner);
/// @brief Get the lifetime number of successful interaction requests.
/// @param scanner Interactor3D component to query.
/// @return The saturating interaction count, or 0 for an invalid scanner.
int64_t rt_game3d_interactor_get_interact_count(void *scanner);
/// @brief Get the target of the most recent successful interaction.
/// @param scanner Interactor3D component to query.
/// @return A retained Interactable3D handle, or NULL when no interaction has succeeded.
void *rt_game3d_interactor_get_last_interacted(void *scanner);

/* AI — Perception3D + BehaviorTree3D (plan 22). */
/// @brief Attach a sight-and-hearing perception component to an entity.
/// @details Defaults to a 15-unit, 110-degree sight cone with all target and occluder layers.
/// @param entity Entity3D that will own the component.
/// @return A new Perception3D handle, or NULL when the entity is invalid or allocation fails.
void *rt_game3d_perception_new(void *entity);
/// @brief Configure the component's sight volume and eye offset.
/// @param sense Perception3D component to configure.
/// @param range Maximum sight distance in world units; positive values are clamped to 512.
/// @param fov_degrees Full field-of-view angle; valid values are clamped to 360 degrees.
/// @param eye_height Finite vertical sight-origin offset, bounded to the coordinate limit.
void rt_game3d_perception_set_sight(void *sense,
                                    double range,
                                    double fov_degrees,
                                    double eye_height);
/// @brief Configure how far a unit-loudness world sound can be heard.
/// @param sense Perception3D component to configure.
/// @param range_at_loudness1 Non-negative base range, clamped to 512 world units.
void rt_game3d_perception_set_hearing(void *sense, double range_at_loudness1);
/// @brief Set the entity-layer mask eligible for visual perception.
/// @param sense Perception3D component to configure.
/// @param mask Bit mask of target entity layers.
void rt_game3d_perception_set_target_mask(void *sense, int64_t mask);
/// @brief Set the collision-layer mask that can occlude visual perception.
/// @param sense Perception3D component to configure.
/// @param mask Bit mask of line-of-sight obstruction layers.
void rt_game3d_perception_set_los_mask(void *sense, int64_t mask);
/// @brief Count targets currently considered visible.
/// @param sense Perception3D component to query.
/// @return The number of live visible targets, or 0 for an invalid component.
int64_t rt_game3d_perception_seen_count(void *sense);
/// @brief Get a visible target by its compact visible-list index.
/// @param sense Perception3D component to query.
/// @param index Zero-based index in the currently visible target list.
/// @return A retained live Entity3D handle, or NULL for an invalid index or stale stable id.
void *rt_game3d_perception_seen_target(void *sense, int64_t index);
/// @brief Get the most recently observed world position of a tracked target.
/// @details While the target is visible, the returned position follows its live position.
/// @param sense Perception3D component holding the target track.
/// @param target Entity3D whose track should be queried.
/// @return A new Vec3 containing the last-known position, or the zero vector when unavailable.
void *rt_game3d_perception_last_known_position(void *sense, void *target);
/// @brief Consume the one-shot notification that any target became seen or lost.
/// @param sense Perception3D component to query.
/// @return 1 when visibility changed since the previous call; otherwise 0.
int8_t rt_game3d_perception_seen_changed(void *sense);
/// @brief Count sound stimuli heard during the current perception interval.
/// @param sense Perception3D component to query.
/// @return The number of buffered heard events, or 0 for an invalid component.
int64_t rt_game3d_perception_heard_count(void *sense);
/// @brief Get the world position of a buffered heard event.
/// @param sense Perception3D component to query.
/// @param index Zero-based heard-event index.
/// @return A new Vec3 containing the event position, or the zero vector when out of range.
void *rt_game3d_perception_heard_position(void *sense, int64_t index);
/// @brief Get the application-defined tag of a buffered heard event.
/// @param sense Perception3D component to query.
/// @param index Zero-based heard-event index.
/// @return The sound tag, or 0 when @p index is out of range.
int64_t rt_game3d_perception_heard_tag(void *sense, int64_t index);
/// @brief Deliver a positional sound stimulus to hearing components in range.
/// @details Each listener's effective reach is its unit-loudness hearing range times @p loudness.
/// @param world World3D whose active perceivers receive the stimulus.
/// @param position Vec3 world-space origin of the sound.
/// @param loudness Positive finite loudness multiplier.
/// @param tag Application-defined value stored with each delivered event.
void rt_game3d_world_report_sound(void *world, void *position, double loudness, int64_t tag);
/// @brief Create an empty behavior-tree definition.
/// @return A new BehaviorTree3D handle, or NULL when allocation fails.
void *rt_game3d_btree_new(void);
/// @brief Append a sequence composite node to a behavior tree.
/// @param tree BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_sequence(void *tree);
/// @brief Append a selector composite node to a behavior tree.
/// @param tree BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_selector(void *tree);
/// @brief Append a result-inverting decorator node to a behavior tree.
/// @param tree BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_inverter(void *tree);
/// @brief Append a leaf that succeeds while the instance's target is visible.
/// @param tree BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_can_see(void *tree);
/// @brief Append a timed wait leaf to a behavior tree.
/// @param tree BehaviorTree3D definition to extend.
/// @param seconds Non-negative duration before the leaf succeeds.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_wait(void *tree, double seconds);
/// @brief Append a leaf that moves the owner toward the current target.
/// @param tree BehaviorTree3D definition to extend.
/// @param speed Movement speed in world units per second; invalid values use the runtime default.
/// @param arrive_distance Distance at which the leaf succeeds; invalid values use the default.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_move_to_target(void *tree, double speed, double arrive_distance);
/// @brief Append a leaf that moves the owner toward the target's last-known position.
/// @param tree BehaviorTree3D definition to extend.
/// @param speed Movement speed in world units per second; invalid values use the runtime default.
/// @param arrive_distance Distance at which the leaf succeeds; invalid values use the default.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_move_to_last_known(void *tree, double speed, double arrive_distance);
/// @brief Append an application-resolved custom leaf to a behavior tree.
/// @param tree BehaviorTree3D definition to extend.
/// @param id Nonzero application-defined identifier exposed while the leaf is pending.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_custom(void *tree, int64_t id);
/// @brief Append an acyclic child relationship to a composite or decorator node.
/// @details Duplicate edges and leaf parents are rejected; an inverter accepts exactly one child.
/// @param tree BehaviorTree3D containing both nodes.
/// @param parent Index of the node that will own the child.
/// @param child Index of the node to append.
void rt_game3d_btree_add_child(void *tree, int64_t parent, int64_t child);
/// @brief Select the node at which behavior-tree evaluation begins.
/// @param tree BehaviorTree3D definition to configure.
/// @param node Valid node index to use as the root.
void rt_game3d_btree_set_root(void *tree, int64_t node);
/// @brief Attach an executable instance of a rooted behavior tree to an entity.
/// @param entity Entity3D that will own and tick the instance.
/// @param tree Rooted BehaviorTree3D definition retained by the instance.
/// @return A new BehaviorTreeInstance3D handle, or NULL when the inputs are invalid.
void *rt_game3d_bt_instance_new(void *entity, void *tree);
/// @brief Set the entity targeted by target-aware behavior leaves.
/// @param instance BehaviorTreeInstance3D to configure.
/// @param target_entity Entity3D to target, or NULL to clear the current target.
void rt_game3d_bt_instance_set_target(void *instance, void *target_entity);
/// @brief Get the custom-leaf identifier awaiting application resolution.
/// @param instance BehaviorTreeInstance3D to query.
/// @return The pending custom identifier, or 0 when no custom leaf is awaiting resolution.
int64_t rt_game3d_bt_instance_pending_custom(void *instance);
/// @brief Complete the custom leaf currently awaiting application resolution.
/// @param instance BehaviorTreeInstance3D with a pending custom leaf.
/// @param success Non-zero to resolve the leaf as success; zero to resolve it as failure.
void rt_game3d_bt_instance_resolve(void *instance, int8_t success);

/* Audio immersion — reverb zones, occlusion, ambient beds, dialogue (plan 24). */
/// @brief Create an axis-aligned reverb zone from two world-space corners.
/// @details Corner components are sorted automatically; the zone starts with balanced reverb
/// values.
/// @param min Vec3 containing one corner of the zone.
/// @param max Vec3 containing the opposite corner.
/// @return A new ReverbZone3D handle, or NULL when the inputs or allocation are invalid.
void *rt_game3d_reverbzone_new(void *min, void *max);
/// @brief Configure a reverb zone's room size, damping, and wet mix.
/// @param zone ReverbZone3D to configure.
/// @param room Room-size control, clamped to the range 0 through 1.
/// @param damping High-frequency damping control, clamped to the range 0 through 1.
/// @param wet Wet-signal mix, clamped to the range 0 through 1.
/// @return @p zone for fluent chaining.
void *rt_game3d_reverbzone_set_reverb(void *zone, double room, double damping, double wet);
/// @brief Set the precedence used when reverb zones overlap.
/// @param zone ReverbZone3D to configure.
/// @param priority Priority value; the containing zone with the highest value wins.
void rt_game3d_reverbzone_set_priority(void *zone, int64_t priority);
/// @brief Get a reverb zone's overlap priority.
/// @param zone ReverbZone3D to query.
/// @return The configured priority, or 0 for an invalid zone.
int64_t rt_game3d_reverbzone_get_priority(void *zone);
/// @brief Register a reverb zone with a world's positional-audio subsystem.
/// @details Registration lazily creates the reverb mixer group used by future positional playback.
/// @param audio Sound3D subsystem that will own the zone reference.
/// @param zone ReverbZone3D to register.
void rt_game3d_audio_add_reverb_zone(void *audio, void *zone);
/// @brief Set the time used to blend between active reverb-zone parameters.
/// @param audio Sound3D subsystem to configure.
/// @param seconds Non-negative finite blend duration; the default is 0.5 seconds.
void rt_game3d_audio_set_reverb_blend(void *audio, double seconds);
/// @brief Get the currently eased reverb wet mix.
/// @param audio Sound3D subsystem to query.
/// @return The current wet mix, or 0 outside all zones or for an invalid subsystem.
double rt_game3d_audio_get_reverb_wet(void *audio);
/// @brief Control whether future positional voices route through the reverb group.
/// @param audio Sound3D subsystem to configure.
/// @param enabled Non-zero to enable reverb routing; zero to bypass it.
void rt_game3d_audio_set_reverb_routing(void *audio, int8_t enabled);
/// @brief Configure listener-to-source occlusion for tracked positional voices.
/// @details Disabling occlusion immediately clears retained occlusion on every tracked source.
/// @param audio Sound3D subsystem to configure.
/// @param enabled Non-zero to perform occlusion raycasts; zero to disable them.
/// @param mask Collision-layer mask used by the occlusion rays.
/// @param amount Occlusion applied to blocked sources, clamped to the range 0 through 1.
void rt_game3d_audio_set_occlusion(void *audio, int8_t enabled, int64_t mask, double amount);
/// @brief Set the maximum number of occlusion raycasts performed per world step.
/// @param audio Sound3D subsystem to configure.
/// @param budget Positive raycast count, clamped to 256; the default is 8.
void rt_game3d_audio_set_occlusion_budget(void *audio, int64_t budget);
/// @brief Play a dialogue clip through the ducking-trigger mixer group.
/// @param audio Sound3D subsystem that supplies volume and dialogue routing.
/// @param clip Sound handle to play.
/// @return A positive voice identifier on success, or 0 on failure.
int64_t rt_game3d_audio_play_dialogue(void *audio, void *clip);
/// @brief Create an ambient-bed crossfader bound to a world's audio subsystem.
/// @details The bed starts with a two-second crossfade and no active zone.
/// @param world World3D whose Sound3D subsystem will own the bed.
/// @return A new AmbientBed3D handle, or NULL when no audio subsystem is available.
void *rt_game3d_ambientbed_new(void *world);
/// @brief Add an axis-aligned zone whose ambient clip loops while the listener is inside.
/// @param bed AmbientBed3D to extend.
/// @param min Vec3 containing one corner of the zone.
/// @param max Vec3 containing the opposite corner.
/// @param clip Sound handle to loop for the zone.
/// @param volume Playback volume clamped to the range 0 through 100.
/// @return @p bed for fluent chaining.
void *rt_game3d_ambientbed_add_zone(void *bed, void *min, void *max, void *clip, int64_t volume);
/// @brief Set the ambient clip used while the listener is outside every zone.
/// @param bed AmbientBed3D to configure.
/// @param clip Sound handle to loop, or NULL for silence.
/// @param volume Playback volume clamped to the range 0 through 100.
/// @return @p bed for fluent chaining.
void *rt_game3d_ambientbed_set_default(void *bed, void *clip, int64_t volume);
/// @brief Set the duration used to crossfade between ambient beds.
/// @param bed AmbientBed3D to configure.
/// @param seconds Non-negative finite crossfade duration.
void rt_game3d_ambientbed_set_crossfade(void *bed, double seconds);
/// @brief Get the ambient-bed crossfade duration.
/// @param bed AmbientBed3D to query.
/// @return The duration in seconds, or 0 for an invalid bed.
double rt_game3d_ambientbed_get_crossfade(void *bed);
/// @brief Get the zone currently selecting the ambient bed.
/// @param bed AmbientBed3D to query.
/// @return The zero-based zone index, or -1 while the default bed is active.
int64_t rt_game3d_ambientbed_get_active_zone(void *bed);

/* World persistence — entity-state deltas, cell flags, VW3DSAV1 (plan 17). */
/// @brief Opt an entity into world persistence under a stable game-defined key.
/// @details Existing live state for the key is applied immediately; duplicate live keys trap.
/// @param entity Entity3D whose pose, alive state, and state tag should persist.
/// @param key Non-empty key of at most 255 bytes with no embedded NUL, stable across sessions.
/// @return @p entity for fluent chaining.
void *rt_game3d_entity_set_persistent(void *entity, rt_string key);
/// @brief Get the stable persistence key assigned to an entity.
/// @param entity Entity3D to query.
/// @return A retained key string, or the empty string when the entity is not persistent.
rt_string rt_game3d_entity_get_persistent_key(void *entity);
/// @brief Set the application-defined integer stored with a persistent entity.
/// @param entity Entity3D whose persisted record should carry the tag.
/// @param tag Free-form application state value.
void rt_game3d_entity_set_state_tag(void *entity, int64_t tag);
/// @brief Get an entity's application-defined persisted state tag.
/// @param entity Entity3D to query.
/// @return The current state tag, or 0 for an invalid entity.
int64_t rt_game3d_entity_get_state_tag(void *entity);
/// @brief Test the last recorded alive state for a persistence key.
/// @param world World3D containing the persistence store.
/// @param key Stable persistence key to query.
/// @return 1 when the record is alive or absent, or 0 when recorded dead or invalid.
int8_t rt_game3d_world_get_persistent_alive(void *world, rt_string key);
/// @brief Get the last recorded world position for a persistence key.
/// @param world World3D containing the persistence store.
/// @param key Stable persistence key to query.
/// @return A new Vec3 containing the recorded position, or the zero vector when unknown.
void *rt_game3d_world_get_persistent_position(void *world, rt_string key);
/// @brief Atomically serialize world deltas and stream flags to a VW3DSAV1 slot.
/// @param world World3D whose persistence store should be saved.
/// @param app_name Application name used to locate the platform data directory.
/// @param slot Save-slot name used as the `.vw3dsav` file stem.
/// @return 1 on success, or 0 on path, encoding, 64 MiB limit, allocation, or I/O failure.
int8_t rt_game3d_world_save_state(void *world, rt_string app_name, rt_string slot);
/// @brief Load a VW3DSAV1 slot and apply its persistent state to the world.
/// @details Success replaces the delta store and cell flags and updates resident persistent
/// entities.
/// @param world World3D that will receive the loaded state.
/// @param app_name Application name used to locate the platform data directory.
/// @param slot Save-slot name used as the `.vw3dsav` file stem.
/// @return 1 on success, or 0 when the file is missing or invalid; failure leaves state unchanged.
int8_t rt_game3d_world_load_state(void *world, rt_string app_name, rt_string slot);
/// @brief Store an application-defined integer flag for a streamed cell.
/// @param stream WorldStream3D whose persistent cell flags are updated.
/// @param cell Non-empty stable cell name no longer than 255 bytes and containing no embedded NUL.
/// @param key Non-empty flag name within the cell no longer than 255 bytes and containing no NUL.
/// @param value Integer value to store.
void rt_game3d_world_stream_set_cell_flag(void *stream,
                                          rt_string cell,
                                          rt_string key,
                                          int64_t value);
/// @brief Get an application-defined integer flag for a streamed cell.
/// @param stream WorldStream3D containing the persistent cell flags.
/// @param cell Stable cell name.
/// @param key Flag name within the cell.
/// @return The stored value, or 0 when the flag is absent or the inputs are invalid.
int64_t rt_game3d_world_stream_get_cell_flag(void *stream, rt_string cell, rt_string key);
/// @brief Count cell-loaded notifications currently buffered for polling.
/// @param stream WorldStream3D to query.
/// @return The number of buffered loaded-cell events, or 0 for an invalid stream.
int64_t rt_game3d_world_stream_loaded_event_count(void *stream);
/// @brief Get the cell name from a buffered cell-loaded notification.
/// @param stream WorldStream3D to query.
/// @param index Zero-based buffered-event index.
/// @return A retained cell-name string, or the empty string when @p index is out of range.
rt_string rt_game3d_world_stream_loaded_event(void *stream, int64_t index);
/// @brief Release and clear all buffered cell-loaded notifications.
/// @param stream WorldStream3D whose notification buffer should be emptied.
void rt_game3d_world_stream_clear_loaded_events(void *stream);
/// @brief Validate one exact canonical VW3DSAV1 buffer without applying it.
/// @param data Borrowed snapshot bytes.
/// @param size Exact byte count, from 16 bytes through 64 MiB.
/// @return One for a complete bounded snapshot with canonical keys/booleans and finite transforms;
/// zero for invalid, truncated, or trailing data.
int8_t rt_game3d_persistence_validate(const void *data, int64_t size);

/* Minimap3D — authored-map minimap, markers, compass, indicators (plan 28). */
/// @brief Create a world-bound minimap with a square default viewport.
/// @param world World3D providing the canvas, camera, and entity state.
/// @param size_px Initial width and height in pixels, from 32 through 2048.
/// @return A new Minimap3D handle, or NULL when the inputs or allocation are invalid.
void *rt_game3d_minimap_new(void *world, int64_t size_px);
/// @brief Set the authored north-up map image and its world-space bounds.
/// @param minimap Minimap3D to configure.
/// @param pixels Pixels resource drawn as the map backdrop, or NULL for the fallback panel.
/// @param min_x Minimum world X coordinate represented by the image.
/// @param min_z Minimum world Z coordinate represented by the image.
/// @param max_x Maximum world X coordinate represented by the image.
/// @param max_z Maximum world Z coordinate represented by the image.
void rt_game3d_minimap_set_map_image(
    void *minimap, void *pixels, double min_x, double min_z, double max_x, double max_z);
/// @brief Select the entity used for the player arrow and objective bearings.
/// @param minimap Minimap3D to configure.
/// @param entity Entity3D to track, or NULL to clear the tracked entity.
void rt_game3d_minimap_set_tracked_entity(void *minimap, void *entity);
/// @brief Set the minimap's screen-space viewport.
/// @param minimap Minimap3D to configure.
/// @param x Finite bounded left edge in canvas pixels.
/// @param y Finite bounded top edge in canvas pixels.
/// @param w Positive finite bounded viewport width in pixels.
/// @param h Positive finite bounded viewport height in pixels.
void rt_game3d_minimap_set_viewport(void *minimap, double x, double y, double w, double h);
/// @brief Configure the optional top-center compass strip.
/// @param minimap Minimap3D to configure.
/// @param enabled Non-zero to draw the compass; zero to hide it.
/// @param width_px Compass width in pixels; finite values of at least 64 are accepted.
void rt_game3d_minimap_set_compass(void *minimap, int8_t enabled, double width_px);
/// @brief Add a minimap marker that follows an entity.
/// @param minimap Minimap3D that will own the marker.
/// @param entity Entity3D whose world position drives the marker.
/// @param icon Optional Pixels resource; NULL selects the fallback marker shape.
/// @param color Packed runtime color used to tint or draw the marker.
/// @return A positive marker identifier, or 0 when the marker cannot be created.
int64_t rt_game3d_minimap_add_marker(void *minimap, void *entity, void *icon, int64_t color);
/// @brief Add a minimap marker fixed at a world-space position.
/// @param minimap Minimap3D that will own the marker.
/// @param point Vec3 containing the fixed marker position.
/// @param icon Optional Pixels resource; NULL selects the fallback marker shape.
/// @param color Packed runtime color used to tint or draw the marker.
/// @return A positive marker identifier, or 0 when the marker cannot be created.
int64_t rt_game3d_minimap_add_marker_at(void *minimap, void *point, void *icon, int64_t color);
/// @brief Remove a marker and release its retained resources.
/// @param minimap Minimap3D containing the marker.
/// @param id Marker identifier returned by an add operation; unknown ids are ignored.
void rt_game3d_minimap_remove_marker(void *minimap, int64_t id);
/// @brief Control whether an off-map marker clamps to the minimap rim.
/// @param minimap Minimap3D containing the marker.
/// @param id Marker identifier to configure.
/// @param clamp Non-zero to clamp to the rim; zero to leave the marker unclamped.
void rt_game3d_minimap_set_marker_edge_clamp(void *minimap, int64_t id, int8_t clamp);
/// @brief Set a marker's visual scale.
/// @param minimap Minimap3D containing the marker.
/// @param id Marker identifier to configure.
/// @param scale Positive finite multiplier, clamped to a maximum of 8.
void rt_game3d_minimap_set_marker_scale(void *minimap, int64_t id, double scale);
/// @brief Control whether a marker is projected onto the compass by bearing.
/// @param minimap Minimap3D containing the marker.
/// @param id Marker identifier to configure.
/// @param enabled Non-zero to include the marker on the compass; zero to omit it.
void rt_game3d_minimap_set_marker_on_compass(void *minimap, int64_t id, int8_t enabled);
/// @brief Control whether a marker also renders as a screen-space objective indicator.
/// @param minimap Minimap3D containing the marker.
/// @param id Marker identifier to configure.
/// @param enabled Non-zero to draw the objective indicator; zero to hide it.
void rt_game3d_minimap_set_objective_indicator(void *minimap, int64_t id, int8_t enabled);
/// @brief Count the minimap's currently live markers.
/// @param minimap Minimap3D to query.
/// @return The number of occupied marker slots, or 0 for an invalid minimap.
int64_t rt_game3d_minimap_get_marker_count(void *minimap);
/// @brief Map a world X/Z coordinate to the viewport's horizontal coordinate.
/// @details The affine result is not clamped to the viewport, but is finite and coordinate-bounded.
/// @param minimap Minimap3D supplying the world bounds and viewport.
/// @param world_x World-space X coordinate.
/// @param world_z World-space Z coordinate; accepted for API symmetry and otherwise ignored.
/// @return The corresponding canvas X coordinate, or 0 for an invalid minimap.
double rt_game3d_minimap_map_x(void *minimap, double world_x, double world_z);
/// @brief Map a world X/Z coordinate to the viewport's vertical coordinate.
/// @details The affine result is not clamped to the viewport, but is finite and coordinate-bounded.
/// @param minimap Minimap3D supplying the world bounds and viewport.
/// @param world_x World-space X coordinate; accepted for API symmetry and otherwise ignored.
/// @param world_z World-space Z coordinate.
/// @return The corresponding canvas Y coordinate, or 0 for an invalid minimap.
double rt_game3d_minimap_map_y(void *minimap, double world_x, double world_z);
/// @brief Draw the map, markers, compass, and objective indicators through Canvas3D.
/// @param minimap Minimap3D to render during the application's HUD pass.
void rt_game3d_minimap_draw(void *minimap);

/* Profiling depth — hitch tracer + pass/hitch constants (plan 30). */
/// @brief Set the duration above which total-frame work is recorded as a hitch.
/// @param world World3D whose telemetry threshold is changed.
/// @param ms Non-negative finite threshold in milliseconds; the default is 25.
void rt_game3d_world_set_hitch_threshold(void *world, double ms);
/// @brief Count chronological entries in the world's bounded hitch ring.
/// @param world World3D whose hitch telemetry is queried.
/// @return The buffered entry count, from 0 through 256.
int64_t rt_game3d_world_hitch_count(void *world);
/// @brief Get the world-frame number associated with a hitch entry.
/// @param world World3D whose hitch telemetry is queried.
/// @param index Zero-based chronological index, where zero is the oldest buffered entry.
/// @return The recorded frame number, or -1 when @p index is out of range.
int64_t rt_game3d_world_hitch_frame(void *world, int64_t index);
/// @brief Get the HitchSource identifier associated with a hitch entry.
/// @param world World3D whose hitch telemetry is queried.
/// @param index Zero-based chronological index, where zero is the oldest buffered entry.
/// @return The recorded HitchSource value, or -1 when @p index is out of range.
int64_t rt_game3d_world_hitch_source(void *world, int64_t index);
/// @brief Get the measured duration associated with a hitch entry.
/// @param world World3D whose hitch telemetry is queried.
/// @param index Zero-based chronological index, where zero is the oldest buffered entry.
/// @return The duration in milliseconds, or 0 when @p index is out of range.
double rt_game3d_world_hitch_ms(void *world, int64_t index);
/// @brief Discard all buffered hitch telemetry for a world.
/// @param world World3D whose hitch ring should be cleared.
void rt_game3d_world_clear_hitches(void *world);
/// @brief Get the RenderPass identifier for shadow rendering.
/// @return The stable RenderPass.Shadow constant.
int64_t rt_game3d_renderpass_shadow(void);
/// @brief Get the RenderPass identifier for opaque rendering.
/// @return The stable RenderPass.Opaque constant.
int64_t rt_game3d_renderpass_opaque(void);
/// @brief Get the RenderPass identifier for transparent rendering.
/// @return The stable RenderPass.Transparent constant.
int64_t rt_game3d_renderpass_transparent(void);
/// @brief Get the RenderPass identifier for post-processing.
/// @return The stable RenderPass.PostFX constant.
int64_t rt_game3d_renderpass_postfx(void);
/// @brief Get the RenderPass identifier for overlay rendering.
/// @return The stable RenderPass.Overlay constant.
int64_t rt_game3d_renderpass_overlay(void);
/// @brief Get the RenderPass identifier for presentation.
/// @return The stable RenderPass.Present constant.
int64_t rt_game3d_renderpass_present(void);
/// @brief Get the HitchSource identifier for staged stream commits.
/// @return The stable HitchSource.StreamCommit constant.
int64_t rt_game3d_hitchsource_stream_commit(void);
/// @brief Get the HitchSource identifier for unattributed total-frame work.
/// @return The stable HitchSource.FrameTotal constant.
int64_t rt_game3d_hitchsource_frame_total(void);

/// @brief Toggle worker-backed streaming (default on); off restores blocking inline loads.
/// @param stream Value supplied for the stream argument.
/// @param enabled Non-zero to enable the documented feature, or zero to disable it.
void rt_game3d_world_stream_set_async_streaming(void *stream, int8_t enabled);

/// @brief True when worker-backed streaming is enabled for this stream.
/// @param stream Value supplied for the stream argument.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_get_async_streaming(void *stream);

/// @brief Cap staged bytes committed per update (-1 = unlimited, 0 = hold commits).
/// @param stream Value supplied for the stream argument.
/// @param bytes Value supplied for the bytes argument.
void rt_game3d_world_stream_set_commit_budget(void *stream, int64_t bytes);

/// @brief Seconds of smoothed center velocity to prefetch along (0 disables prefetch).
/// @param stream Value supplied for the stream argument.
/// @param seconds Value supplied for the seconds argument.
void rt_game3d_world_stream_set_prefetch_lookahead(void *stream, double seconds);

/// @brief Worst single staged-commit slice in wall milliseconds since mount.
/// @param stream Value supplied for the stream argument.
/// @return The documented floating-point result.
double rt_game3d_world_stream_get_stream_stall_ms(void *stream);

/// @brief Cells currently staged or staging purely from velocity prefetch.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_prefetched_cell_count(void *stream);

/// @brief Set the HLOD proxy ring radius (<= 0 restores the 4x-load-radius default).
/// @param stream Value supplied for the stream argument.
/// @param radius Value supplied for the radius argument.
void rt_game3d_world_stream_set_proxy_radius(void *stream, double radius);

/// @brief The cell's manifest/baked proxy path ("" when the cell has no proxy).
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return A runtime string containing the documented result.
rt_string rt_game3d_world_stream_get_cell_proxy(void *stream, int64_t index);

/// @brief Number of cells currently holding only their HLOD proxy subtree.
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_proxy_resident_count(void *stream);

/// @brief Measured bytes of resident proxy subtrees (also included in ResidentBytes).
/// @param stream Value supplied for the stream argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_get_proxy_resident_bytes(void *stream);

/// @brief Bake a merged low-poly proxy .vscn for a resident cell (authoring hook).
/// @param stream Value supplied for the stream argument.
/// @param index Zero-based index of the requested item.
/// @return 1 when the documented condition holds, or 0 otherwise.
int8_t rt_game3d_world_stream_bake_cell_proxy(void *stream, int64_t index);

/// @brief Generate yaw-strip impostors for cells holding proxies; returns count.
/// @param stream Value supplied for the stream argument.
/// @param distance Value supplied for the distance argument.
/// @return The documented integer result, including any sentinel described above.
int64_t rt_game3d_world_stream_generate_impostors(void *stream, double distance);

#ifdef __cplusplus
}
#endif
