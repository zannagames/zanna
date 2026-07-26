//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_enums.c
// Purpose: Enum-mirror accessors for the Zanna.Game3D layer — each returns the
//   matching RT_GAME3D_* / input constant so frontends can bind them as
//   read-only Layers / BodyShape / SyncMode / AlphaMode / ShadingModel /
//   Quality / CollisionPhase / Keys / MouseButtons properties. Split out of
//   rt_game3d.c (which retains the world/entity/asset/simulation surface).
// Key invariants:
//   - Pure constant/forwarding accessors: stateless, no allocation, no failure.
// Ownership/Lifetime:
//   - None — every function is stateless and returns a scalar constant.
// Links: rt_game3d.h (declarations + RT_GAME3D_* constants), rt_input.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Exposes Game3D enum values and shared input codes through callable accessors.
/// @details Frontends bind these stateless functions as read-only properties for
///          collision layers, body shapes, synchronization and material modes,
///          quality levels, collision phases, keyboard/mouse input, render-pass
///          attribution, and hitch sources. Game3D-owned values are compile-time
///          constants; input accessors delegate to the canonical shared tables so
///          every runtime subsystem observes identical codes.

#include "rt_game3d.h"

#include "rt_input.h"

//=========================================================================
// Enum-mirror accessors — each returns the matching RT_GAME3D_* / input
// constant so frontends can bind them as read-only Layers/BodyShape/SyncMode/
// AlphaMode/ShadingModel/Quality/CollisionPhase/Keys/MouseButtons properties.
// Layer/shape/mode constants resolve at compile time; Keys/MouseButtons forward
// to the shared rt_keyboard/rt_mouse code tables so all subsystems agree.
//=========================================================================

/// @brief Layer bit for static world geometry (RT_GAME3D_LAYER_WORLD).
/// @return The corresponding single-bit Game3D collision-layer value.
int64_t rt_game3d_layers_world(void) {
    return RT_GAME3D_LAYER_WORLD;
}

/// @brief Layer bit for movable dynamic bodies (RT_GAME3D_LAYER_DYNAMIC).
/// @return The corresponding single-bit Game3D collision-layer value.
int64_t rt_game3d_layers_dynamic(void) {
    return RT_GAME3D_LAYER_DYNAMIC;
}

/// @brief Layer bit for player-controlled entities (RT_GAME3D_LAYER_PLAYER).
/// @return The corresponding single-bit Game3D collision-layer value.
int64_t rt_game3d_layers_player(void) {
    return RT_GAME3D_LAYER_PLAYER;
}

/// @brief Layer bit for non-solid trigger volumes (RT_GAME3D_LAYER_TRIGGER).
/// @return The corresponding single-bit Game3D collision-layer value.
int64_t rt_game3d_layers_trigger(void) {
    return RT_GAME3D_LAYER_TRIGGER;
}

/// @brief Layer bit for short-lived debris/particles (RT_GAME3D_LAYER_DEBRIS).
/// @return The corresponding single-bit Game3D collision-layer value.
int64_t rt_game3d_layers_debris(void) {
    return RT_GAME3D_LAYER_DEBRIS;
}

/// @brief Box shape kind constant (RT_GAME3D_BODY_SHAPE_BOX).
/// @return The corresponding Game3D body-shape value.
int64_t rt_game3d_body_shape_box(void) {
    return RT_GAME3D_BODY_SHAPE_BOX;
}

/// @brief Sphere shape kind constant (RT_GAME3D_BODY_SHAPE_SPHERE).
/// @return The corresponding Game3D body-shape value.
int64_t rt_game3d_body_shape_sphere(void) {
    return RT_GAME3D_BODY_SHAPE_SPHERE;
}

/// @brief Capsule shape kind constant (RT_GAME3D_BODY_SHAPE_CAPSULE).
/// @return The corresponding Game3D body-shape value.
int64_t rt_game3d_body_shape_capsule(void) {
    return RT_GAME3D_BODY_SHAPE_CAPSULE;
}

/// @brief Sync constant: physics body drives the scene node.
/// @return The corresponding Game3D node/body synchronization-mode value.
int64_t rt_game3d_sync_mode_node_from_body(void) {
    return RT_GAME3D_SYNC_NODE_FROM_BODY;
}

/// @brief Sync constant: scene node drives the physics body.
/// @return The corresponding Game3D node/body synchronization-mode value.
int64_t rt_game3d_sync_mode_body_from_node(void) {
    return RT_GAME3D_SYNC_BODY_FROM_NODE;
}

/// @brief Sync constant: animation root motion drives the scene node.
/// @return The corresponding Game3D node/body synchronization-mode value.
int64_t rt_game3d_sync_mode_node_from_anim_root_motion(void) {
    return RT_GAME3D_SYNC_NODE_FROM_ANIM_ROOT_MOTION;
}

/// @brief Sync constant: bidirectional kinematic body/node coupling.
/// @return The corresponding Game3D node/body synchronization-mode value.
int64_t rt_game3d_sync_mode_two_way_kinematic(void) {
    return RT_GAME3D_SYNC_TWO_WAY_KINEMATIC;
}

/// @brief Opaque alpha-mode constant (RT_GAME3D_ALPHA_OPAQUE).
/// @return The corresponding Game3D material alpha-mode value.
int64_t rt_game3d_alpha_mode_opaque(void) {
    return RT_GAME3D_ALPHA_OPAQUE;
}

/// @brief Alpha-tested cutout mode constant (RT_GAME3D_ALPHA_MASK).
/// @return The corresponding Game3D material alpha-mode value.
int64_t rt_game3d_alpha_mode_mask(void) {
    return RT_GAME3D_ALPHA_MASK;
}

/// @brief Translucent blend mode constant (RT_GAME3D_ALPHA_BLEND).
/// @return The corresponding Game3D material alpha-mode value.
int64_t rt_game3d_alpha_mode_blend(void) {
    return RT_GAME3D_ALPHA_BLEND;
}

/// @brief Blinn-Phong shading-model constant (RT_GAME3D_SHADING_PHONG).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_phong(void) {
    return RT_GAME3D_SHADING_PHONG;
}

/// @brief Toon/cel shading-model constant (RT_GAME3D_SHADING_TOON).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_toon(void) {
    return RT_GAME3D_SHADING_TOON;
}

/// @brief Physically based (PBR) shading-model constant (RT_GAME3D_SHADING_PBR).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_pbr(void) {
    return RT_GAME3D_SHADING_PBR;
}

/// @brief Fresnel/rim shading-model constant (RT_GAME3D_SHADING_FRESNEL).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_fresnel(void) {
    return RT_GAME3D_SHADING_FRESNEL;
}

/// @brief Emissive (self-lit) shading-model constant (RT_GAME3D_SHADING_EMISSIVE).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_emissive(void) {
    return RT_GAME3D_SHADING_EMISSIVE;
}

/// @brief Unlit flat-albedo shading-model constant (RT_GAME3D_SHADING_UNLIT).
/// @return The corresponding Game3D shading-model value.
int64_t rt_game3d_shading_model_unlit(void) {
    return RT_GAME3D_SHADING_UNLIT;
}

/// @brief Performance quality preset constant (RT_GAME3D_QUALITY_PERFORMANCE).
/// @return The corresponding Game3D quality-preset value.
int64_t rt_game3d_quality_performance(void) {
    return RT_GAME3D_QUALITY_PERFORMANCE;
}

/// @brief Balanced quality preset constant (RT_GAME3D_QUALITY_BALANCED).
/// @return The corresponding Game3D quality-preset value.
int64_t rt_game3d_quality_balanced(void) {
    return RT_GAME3D_QUALITY_BALANCED;
}

/// @brief Cinematic quality preset constant (RT_GAME3D_QUALITY_CINEMATIC).
/// @return The corresponding Game3D quality-preset value.
int64_t rt_game3d_quality_cinematic(void) {
    return RT_GAME3D_QUALITY_CINEMATIC;
}

/// @brief Enter-phase constant: first frame two colliders touch (RT_GAME3D_COLLISION_ENTER).
/// @return The corresponding Game3D collision-phase value.
int64_t rt_game3d_collision_enter(void) {
    return RT_GAME3D_COLLISION_ENTER;
}

/// @brief Stay-phase constant: continuing contact frames (RT_GAME3D_COLLISION_STAY).
/// @return The corresponding Game3D collision-phase value.
int64_t rt_game3d_collision_stay(void) {
    return RT_GAME3D_COLLISION_STAY;
}

/// @brief Exit-phase constant: frame contact ends (RT_GAME3D_COLLISION_EXIT).
/// @return The corresponding Game3D collision-phase value.
int64_t rt_game3d_collision_exit(void) {
    return RT_GAME3D_COLLISION_EXIT;
}

/// @brief Wildcard constant matching any collision phase (RT_GAME3D_COLLISION_ANY).
/// @return The corresponding Game3D collision-phase value.
int64_t rt_game3d_collision_any(void) {
    return RT_GAME3D_COLLISION_ANY;
}

/// @brief Key code for the W key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_w(void) {
    return rt_keyboard_key_w();
}

/// @brief Key code for the A key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_a(void) {
    return rt_keyboard_key_a();
}

/// @brief Key code for the S key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_s(void) {
    return rt_keyboard_key_s();
}

/// @brief Key code for the D key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_d(void) {
    return rt_keyboard_key_d();
}

/// @brief Key code for the spacebar (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_space(void) {
    return rt_keyboard_key_space();
}

/// @brief Key code for the Escape key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_escape(void) {
    return rt_keyboard_key_escape();
}

/// @brief Key code for the left Shift modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_shift(void) {
    return rt_keyboard_key_lshift();
}

/// @brief Key code for the left Ctrl modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_ctrl(void) {
    return rt_keyboard_key_lctrl();
}

/// @brief Key code for the Up arrow (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_up(void) {
    return rt_keyboard_key_up();
}

/// @brief Key code for the Down arrow (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_down(void) {
    return rt_keyboard_key_down();
}

/// @brief Key code for the Left arrow (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_left(void) {
    return rt_keyboard_key_left();
}

/// @brief Key code for the Right arrow (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_right(void) {
    return rt_keyboard_key_right();
}

/// @brief Key code for the F11 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f11(void) {
    return rt_keyboard_key_f11();
}

// --- Full keyboard coverage wrappers (forward to the shared rt_keyboard table) ---

/// @brief Key code for the B key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_b(void) {
    return rt_keyboard_key_b();
}

/// @brief Key code for the C key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_c(void) {
    return rt_keyboard_key_c();
}

/// @brief Key code for the E key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_e(void) {
    return rt_keyboard_key_e();
}

/// @brief Key code for the F key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f(void) {
    return rt_keyboard_key_f();
}

/// @brief Key code for the G key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_g(void) {
    return rt_keyboard_key_g();
}

/// @brief Key code for the H key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_h(void) {
    return rt_keyboard_key_h();
}

/// @brief Key code for the I key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_i(void) {
    return rt_keyboard_key_i();
}

/// @brief Key code for the J key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_j(void) {
    return rt_keyboard_key_j();
}

/// @brief Key code for the K key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_k(void) {
    return rt_keyboard_key_k();
}

/// @brief Key code for the L key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_l(void) {
    return rt_keyboard_key_l();
}

/// @brief Key code for the M key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_m(void) {
    return rt_keyboard_key_m();
}

/// @brief Key code for the N key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_n(void) {
    return rt_keyboard_key_n();
}

/// @brief Key code for the O key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_o(void) {
    return rt_keyboard_key_o();
}

/// @brief Key code for the P key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_p(void) {
    return rt_keyboard_key_p();
}

/// @brief Key code for the Q key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_q(void) {
    return rt_keyboard_key_q();
}

/// @brief Key code for the R key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_r(void) {
    return rt_keyboard_key_r();
}

/// @brief Key code for the T key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_t(void) {
    return rt_keyboard_key_t();
}

/// @brief Key code for the U key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_u(void) {
    return rt_keyboard_key_u();
}

/// @brief Key code for the V key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_v(void) {
    return rt_keyboard_key_v();
}

/// @brief Key code for the X key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_x(void) {
    return rt_keyboard_key_x();
}

/// @brief Key code for the Y key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_y(void) {
    return rt_keyboard_key_y();
}

/// @brief Key code for the Z key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_z(void) {
    return rt_keyboard_key_z();
}

/// @brief Key code for the 0 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_0(void) {
    return rt_keyboard_key_0();
}

/// @brief Key code for the 1 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_1(void) {
    return rt_keyboard_key_1();
}

/// @brief Key code for the 2 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_2(void) {
    return rt_keyboard_key_2();
}

/// @brief Key code for the 3 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_3(void) {
    return rt_keyboard_key_3();
}

/// @brief Key code for the 4 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_4(void) {
    return rt_keyboard_key_4();
}

/// @brief Key code for the 5 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_5(void) {
    return rt_keyboard_key_5();
}

/// @brief Key code for the 6 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_6(void) {
    return rt_keyboard_key_6();
}

/// @brief Key code for the 7 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_7(void) {
    return rt_keyboard_key_7();
}

/// @brief Key code for the 8 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_8(void) {
    return rt_keyboard_key_8();
}

/// @brief Key code for the 9 key (top row) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_9(void) {
    return rt_keyboard_key_9();
}

/// @brief Key code for the F1 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f1(void) {
    return rt_keyboard_key_f1();
}

/// @brief Key code for the F2 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f2(void) {
    return rt_keyboard_key_f2();
}

/// @brief Key code for the F3 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f3(void) {
    return rt_keyboard_key_f3();
}

/// @brief Key code for the F4 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f4(void) {
    return rt_keyboard_key_f4();
}

/// @brief Key code for the F5 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f5(void) {
    return rt_keyboard_key_f5();
}

/// @brief Key code for the F6 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f6(void) {
    return rt_keyboard_key_f6();
}

/// @brief Key code for the F7 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f7(void) {
    return rt_keyboard_key_f7();
}

/// @brief Key code for the F8 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f8(void) {
    return rt_keyboard_key_f8();
}

/// @brief Key code for the F9 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f9(void) {
    return rt_keyboard_key_f9();
}

/// @brief Key code for the F10 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f10(void) {
    return rt_keyboard_key_f10();
}

/// @brief Key code for the F12 function key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_f12(void) {
    return rt_keyboard_key_f12();
}

/// @brief Key code for the Enter/Return key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_enter(void) {
    return rt_keyboard_key_enter();
}

/// @brief Key code for the Tab key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_tab(void) {
    return rt_keyboard_key_tab();
}

/// @brief Key code for the Backspace key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_backspace(void) {
    return rt_keyboard_key_backspace();
}

/// @brief Key code for the Insert key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_insert(void) {
    return rt_keyboard_key_insert();
}

/// @brief Key code for the Delete key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_delete(void) {
    return rt_keyboard_key_delete();
}

/// @brief Key code for the Home key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_home(void) {
    return rt_keyboard_key_home();
}

/// @brief Key code for the End key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_end(void) {
    return rt_keyboard_key_end();
}

/// @brief Key code for the Page Up key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_pageup(void) {
    return rt_keyboard_key_pageup();
}

/// @brief Key code for the Page Down key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_pagedown(void) {
    return rt_keyboard_key_pagedown();
}

/// @brief Key code for the Alt modifier (left) (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_alt(void) {
    return rt_keyboard_key_lalt();
}

/// @brief Key code for the left Shift modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_lshift(void) {
    return rt_keyboard_key_lshift();
}

/// @brief Key code for the right Shift modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_rshift(void) {
    return rt_keyboard_key_rshift();
}

/// @brief Key code for the left Ctrl modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_lctrl(void) {
    return rt_keyboard_key_lctrl();
}

/// @brief Key code for the right Ctrl modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_rctrl(void) {
    return rt_keyboard_key_rctrl();
}

/// @brief Key code for the left Alt modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_lalt(void) {
    return rt_keyboard_key_lalt();
}

/// @brief Key code for the right Alt modifier (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_ralt(void) {
    return rt_keyboard_key_ralt();
}

/// @brief Key code for the apostrophe/quote key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_quote(void) {
    return rt_keyboard_key_quote();
}

/// @brief Key code for the comma key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_comma(void) {
    return rt_keyboard_key_comma();
}

/// @brief Key code for the minus/hyphen key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_minus(void) {
    return rt_keyboard_key_minus();
}

/// @brief Key code for the period key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_period(void) {
    return rt_keyboard_key_period();
}

/// @brief Key code for the forward-slash key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_slash(void) {
    return rt_keyboard_key_slash();
}

/// @brief Key code for the semicolon key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_semicolon(void) {
    return rt_keyboard_key_semicolon();
}

/// @brief Key code for the equals key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_equals(void) {
    return rt_keyboard_key_equals();
}

/// @brief Key code for the left-bracket key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_lbracket(void) {
    return rt_keyboard_key_lbracket();
}

/// @brief Key code for the backslash key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_backslash(void) {
    return rt_keyboard_key_backslash();
}

/// @brief Key code for the right-bracket key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_rbracket(void) {
    return rt_keyboard_key_rbracket();
}

/// @brief Key code for the grave/backtick key (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_grave(void) {
    return rt_keyboard_key_grave();
}

/// @brief Key code for the numpad 0 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num0(void) {
    return rt_keyboard_key_num0();
}

/// @brief Key code for the numpad 1 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num1(void) {
    return rt_keyboard_key_num1();
}

/// @brief Key code for the numpad 2 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num2(void) {
    return rt_keyboard_key_num2();
}

/// @brief Key code for the numpad 3 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num3(void) {
    return rt_keyboard_key_num3();
}

/// @brief Key code for the numpad 4 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num4(void) {
    return rt_keyboard_key_num4();
}

/// @brief Key code for the numpad 5 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num5(void) {
    return rt_keyboard_key_num5();
}

/// @brief Key code for the numpad 6 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num6(void) {
    return rt_keyboard_key_num6();
}

/// @brief Key code for the numpad 7 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num7(void) {
    return rt_keyboard_key_num7();
}

/// @brief Key code for the numpad 8 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num8(void) {
    return rt_keyboard_key_num8();
}

/// @brief Key code for the numpad 9 (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_num9(void) {
    return rt_keyboard_key_num9();
}

/// @brief Key code for the numpad decimal point (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_numdot(void) {
    return rt_keyboard_key_numdot();
}

/// @brief Key code for the numpad divide (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_numdiv(void) {
    return rt_keyboard_key_numdiv();
}

/// @brief Key code for the numpad multiply (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_nummul(void) {
    return rt_keyboard_key_nummul();
}

/// @brief Key code for the numpad subtract (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_numsub(void) {
    return rt_keyboard_key_numsub();
}

/// @brief Key code for the numpad add (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_numadd(void) {
    return rt_keyboard_key_numadd();
}

/// @brief Key code for the numpad Enter (forwards to the shared keyboard table).
/// @return The matching code from the shared keyboard table.
int64_t rt_game3d_key_numenter(void) {
    return rt_keyboard_key_numenter();
}

/// @brief Button code for the left mouse button (forwards to the shared mouse table).
/// @return The matching code from the shared mouse-button table.
int64_t rt_game3d_mouse_left(void) {
    return rt_mouse_button_left();
}

/// @brief Button code for the right mouse button (forwards to the shared mouse table).
/// @return The matching code from the shared mouse-button table.
int64_t rt_game3d_mouse_right(void) {
    return rt_mouse_button_right();
}

/// @brief Button code for the middle (wheel) mouse button (forwards to the shared mouse table).
/// @return The matching code from the shared mouse-button table.
int64_t rt_game3d_mouse_middle(void) {
    return rt_mouse_button_middle();
}

/// @brief Button code for the first extra (X1) mouse button (forwards to the shared mouse table).
/// @return The matching code from the shared mouse-button table.
int64_t rt_game3d_mouse_x1(void) {
    return rt_mouse_button_x1();
}

/// @brief Button code for the second extra (X2) mouse button (forwards to the shared mouse table).
/// @return The matching code from the shared mouse-button table.
int64_t rt_game3d_mouse_x2(void) {
    return rt_mouse_button_x2();
}

/// @brief RenderPass.Shadow constant (plan 30 pass-attribution ids).
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_shadow(void) {
    return 0;
}

/// @brief RenderPass.Opaque constant.
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_opaque(void) {
    return 1;
}

/// @brief RenderPass.Transparent constant.
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_transparent(void) {
    return 2;
}

/// @brief RenderPass.PostFX constant.
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_postfx(void) {
    return 3;
}

/// @brief RenderPass.Overlay constant.
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_overlay(void) {
    return 4;
}

/// @brief RenderPass.Present constant.
/// @return The stable Game3D render-pass attribution identifier.
int64_t rt_game3d_renderpass_present(void) {
    return 5;
}

/// @brief HitchSource.StreamCommit constant.
/// @return The stable Game3D hitch-source attribution identifier.
int64_t rt_game3d_hitchsource_stream_commit(void) {
    return 0;
}

/// @brief HitchSource.FrameTotal constant.
/// @return The stable Game3D hitch-source attribution identifier.
int64_t rt_game3d_hitchsource_frame_total(void) {
    return 3;
}
