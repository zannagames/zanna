//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTGraphicsSurfaceLinkTests.cpp
// Purpose: Link-smoke coverage for the graphics/runtime surface that must
//          remain exported in both full and graphics-disabled builds.
//
// Key invariants:
//   - Every declared graphics entry point resolves in both feature configurations.
//   - Additive runtime symbols never displace an established export.
// Ownership/Lifetime:
//   - The test owns no runtime objects; it validates function addresses only.
// Links: src/il/runtime/defs/graphics3d, src/runtime/graphics/common,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//

#include "rt_animcontroller3d.h"
#include "rt_audio.h"
#include "rt_blendtree3d.h"
#include "rt_canvas3d.h"
#include "rt_collider3d.h"
#include "rt_fbx_loader.h"
#include "rt_game3d.h"
#include "rt_gltf.h"
#include "rt_graphics.h"
#include "rt_gui.h"
#include "rt_iksolver3d.h"
#include "rt_joints3d.h"
#include "rt_mesh_simplify.h"
#include "rt_model3d.h"
#include "rt_navagent3d.h"
#include "rt_navmesh3d.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_skeleton3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace {

template <typename Fn> std::uintptr_t fn_bits(Fn fn) {
    static_assert(sizeof(fn) <= sizeof(std::uintptr_t));
    std::uintptr_t bits = 0;
    std::memcpy(&bits, &fn, sizeof(fn));
    return bits;
}

void test_filedialog_path_list_escaping() {
    const char *encoded = "alpha\\;semi;back\\\\slash;trailing\\;first;;third;tail\\";
    rt_string escaped = rt_string_from_bytes(encoded, std::strlen(encoded));
    assert(rt_filedialog_path_list_count(escaped) == 6);

    rt_string first = rt_filedialog_path_list_get(escaped, 0);
    rt_string second = rt_filedialog_path_list_get(escaped, 1);
    rt_string third = rt_filedialog_path_list_get(escaped, 2);
    rt_string fourth = rt_filedialog_path_list_get(escaped, 3);
    rt_string fifth = rt_filedialog_path_list_get(escaped, 4);
    rt_string sixth = rt_filedialog_path_list_get(escaped, 5);

    assert(std::strcmp(rt_string_cstr(first), "alpha;semi") == 0);
    assert(std::strcmp(rt_string_cstr(second), "back\\slash") == 0);
    assert(std::strcmp(rt_string_cstr(third), "trailing;first") == 0);
    assert(std::strcmp(rt_string_cstr(fourth), "") == 0);
    assert(std::strcmp(rt_string_cstr(fifth), "third") == 0);
    assert(std::strcmp(rt_string_cstr(sixth), "tail\\") == 0);
    assert(rt_str_len(rt_filedialog_path_list_get(escaped, 99)) == 0);
}

} // namespace

int main() {
    volatile std::uintptr_t surface[] = {
        fn_bits(&rt_canvas_is_available),
        fn_bits(&rt_audio_is_available),
        fn_bits(&rt_canvas3d_new_offscreen),
        fn_bits(&rt_canvas3d_get_is_offscreen),
        fn_bits(&rt_canvas3d_get_draw_count),
        fn_bits(&rt_canvas3d_get_occluded_draw_count),
        fn_bits(&rt_mesh3d_clear),
        fn_bits(&rt_mesh3d_from_stl),
        fn_bits(&rt_mesh3d_simplify),
        fn_bits(&rt_mesh3d_get_simplify_requested_triangles),
        fn_bits(&rt_mesh3d_get_simplify_achieved_triangles),
        fn_bits(&rt_mesh3d_get_simplify_status),
        fn_bits(&rt_mesh3d_get_bone_count),
        fn_bits(&rt_mesh3d_get_skeleton),
        fn_bits(&rt_camera3d_new_ortho),
        fn_bits(&rt_camera3d_is_ortho),
        fn_bits(&rt_light3d_new_spot),
        fn_bits(&rt_scene3d_save),
        fn_bits(&rt_scene3d_sync_bindings),
        fn_bits(&rt_scene_node3d_bind_body),
        fn_bits(&rt_scene_node3d_clear_body_binding),
        fn_bits(&rt_scene_node3d_get_body),
        fn_bits(&rt_scene_node3d_set_sync_mode),
        fn_bits(&rt_scene_node3d_get_selected_lod),
        fn_bits(&rt_scene_node3d_get_sync_mode),
        fn_bits(&rt_scene_node3d_bind_animator),
        fn_bits(&rt_scene_node3d_clear_animator_binding),
        fn_bits(&rt_scene_node3d_get_animator),
        fn_bits(&rt_fbx_load_recoverable),
        fn_bits(&rt_fbx_get_morph_target),
        fn_bits(&rt_fbx_node_animation_count),
        fn_bits(&rt_fbx_get_node_animation),
        fn_bits(&rt_fbx_get_node_animation_name),
        fn_bits(&rt_fbx_camera_count),
        fn_bits(&rt_fbx_get_camera),
        fn_bits(&rt_gltf_load),
        fn_bits(&rt_gltf_load_asset),
        fn_bits(&rt_gltf_mesh_count),
        fn_bits(&rt_gltf_get_mesh),
        fn_bits(&rt_gltf_material_count),
        fn_bits(&rt_gltf_get_material),
        fn_bits(&rt_gltf_camera_count),
        fn_bits(&rt_gltf_get_camera),
        fn_bits(&rt_gltf_scene_count),
        fn_bits(&rt_gltf_get_scene_name),
        fn_bits(&rt_gltf_get_scene_root_at),
        fn_bits(&rt_gltf_scene_camera_count),
        fn_bits(&rt_gltf_get_scene_camera),
        fn_bits(&rt_gltf_node_count),
        fn_bits(&rt_gltf_get_scene_root),
        fn_bits(&rt_anim_controller3d_new),
        fn_bits(&rt_anim_controller3d_add_state),
        fn_bits(&rt_anim_controller3d_add_transition),
        fn_bits(&rt_anim_controller3d_play),
        fn_bits(&rt_anim_controller3d_crossfade),
        fn_bits(&rt_anim_controller3d_stop),
        fn_bits(&rt_anim_controller3d_update),
        fn_bits(&rt_anim_controller3d_get_current_state),
        fn_bits(&rt_anim_controller3d_get_previous_state),
        fn_bits(&rt_anim_controller3d_get_is_transitioning),
        fn_bits(&rt_anim_controller3d_get_blend_tree_weight),
        fn_bits(&rt_anim_controller3d_get_animation_lod_skips),
        fn_bits(&rt_anim_controller3d_get_state_count),
        fn_bits(&rt_anim_controller3d_set_state_speed),
        fn_bits(&rt_anim_controller3d_set_state_looping),
        fn_bits(&rt_anim_controller3d_set_animation_lod),
        fn_bits(&rt_anim_controller3d_set_blend_tree),
        fn_bits(&rt_anim_controller3d_set_ik_solver),
        fn_bits(&rt_anim_controller3d_add_event),
        fn_bits(&rt_anim_controller3d_poll_event),
        fn_bits(&rt_anim_controller3d_set_root_motion_bone),
        fn_bits(&rt_anim_controller3d_get_root_motion_delta),
        fn_bits(&rt_anim_controller3d_consume_root_motion),
        fn_bits(&rt_anim_controller3d_set_layer_weight),
        fn_bits(&rt_anim_controller3d_set_layer_mask),
        fn_bits(&rt_anim_controller3d_play_layer),
        fn_bits(&rt_anim_controller3d_play_layer_additive),
        fn_bits(&rt_anim_controller3d_crossfade_layer),
        fn_bits(&rt_anim_controller3d_crossfade_layer_additive),
        fn_bits(&rt_anim_controller3d_stop_layer),
        fn_bits(&rt_anim_controller3d_get_bone_matrix),
        fn_bits(&rt_anim_controller3d_get_final_palette_data),
        fn_bits(&rt_anim_controller3d_get_previous_palette_data),
        fn_bits(&rt_game3d_animator_set_blend_tree),
        fn_bits(&rt_game3d_animator_set_ik_solver),
        fn_bits(&rt_game3d_animator_crossfade_layer_additive),
        fn_bits(&rt_game3d_world_get_entity_count),
        fn_bits(&rt_game3d_world_get_body_count),
        fn_bits(&rt_game3d_world_get_draw_count),
        fn_bits(&rt_game3d_world_get_visible_node_count),
        fn_bits(&rt_game3d_world_get_occluded_draw_count),
        fn_bits(&rt_game3d_world_get_stream_resident_bytes),
        fn_bits(&rt_game3d_world_bake_nav_mesh),
        fn_bits(&rt_game3d_world_bake_tiled_nav_mesh),
        fn_bits(&rt_game3d_world_stream_get_resident_terrain_tile),
        fn_bits(&rt_game3d_world_stream_get_cell_count),
        fn_bits(&rt_game3d_world_stream_get_cell_name),
        fn_bits(&rt_game3d_world_stream_get_cell_center),
        fn_bits(&rt_game3d_world_stream_get_cell_resident),
        fn_bits(&rt_game3d_world_stream_get_cell_bytes),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_count),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_name),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_heightmap),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_center),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_resident),
        fn_bits(&rt_game3d_world_stream_get_terrain_tile_bytes),
        fn_bits(&rt_animation3d_retarget),
        fn_bits(&rt_ik_solver3d_two_bone),
        fn_bits(&rt_ik_solver3d_look_at),
        fn_bits(&rt_ik_solver3d_fabrik),
        fn_bits(&rt_ik_solver3d_set_target),
        fn_bits(&rt_ik_solver3d_set_weight),
        fn_bits(&rt_ik_solver3d_solve),
        fn_bits(&rt_ik_solver3d_get_skeleton),
        fn_bits(&rt_ik_solver3d_apply_to_pose),
        fn_bits(&rt_blend_tree3d_new_1d),
        fn_bits(&rt_blend_tree3d_new_2d),
        fn_bits(&rt_blend_tree3d_add_sample),
        fn_bits(&rt_blend_tree3d_set_param),
        fn_bits(&rt_blend_tree3d_update),
        fn_bits(&rt_blend_tree3d_get_sample_count),
        fn_bits(&rt_blend_tree3d_get_blend),
        fn_bits(&rt_model3d_load),
        fn_bits(&rt_model3d_load_asset),
        fn_bits(&rt_model3d_get_mesh_count),
        fn_bits(&rt_model3d_get_material_count),
        fn_bits(&rt_model3d_get_skeleton_count),
        fn_bits(&rt_model3d_get_animation_count),
        fn_bits(&rt_model3d_get_node_count),
        fn_bits(&rt_model3d_get_mesh),
        fn_bits(&rt_model3d_get_material),
        fn_bits(&rt_model3d_get_skeleton),
        fn_bits(&rt_model3d_get_animation),
        fn_bits(&rt_model3d_find_node),
        fn_bits(&rt_model3d_instantiate),
        fn_bits(&rt_model3d_instantiate_scene),
        fn_bits(&rt_material3d_new_pbr),
        fn_bits(&rt_material3d_clone),
        fn_bits(&rt_material3d_make_instance),
        fn_bits(&rt_material3d_set_albedo_map),
        fn_bits(&rt_material3d_set_metallic),
        fn_bits(&rt_material3d_get_metallic),
        fn_bits(&rt_material3d_set_roughness),
        fn_bits(&rt_material3d_get_roughness),
        fn_bits(&rt_material3d_set_ao),
        fn_bits(&rt_material3d_get_ao),
        fn_bits(&rt_material3d_set_emissive_intensity),
        fn_bits(&rt_material3d_get_emissive_intensity),
        fn_bits(&rt_material3d_set_metallic_roughness_map),
        fn_bits(&rt_material3d_set_ao_map),
        fn_bits(&rt_material3d_set_normal_scale),
        fn_bits(&rt_material3d_get_normal_scale),
        fn_bits(&rt_material3d_set_alpha_mode),
        fn_bits(&rt_material3d_get_alpha_mode),
        fn_bits(&rt_material3d_set_double_sided),
        fn_bits(&rt_material3d_get_double_sided),
        fn_bits(&rt_distance_joint3d_new),
        fn_bits(&rt_distance_joint3d_get_distance),
        fn_bits(&rt_distance_joint3d_set_distance),
        fn_bits(&rt_spring_joint3d_new),
        fn_bits(&rt_spring_joint3d_get_stiffness),
        fn_bits(&rt_spring_joint3d_set_stiffness),
        fn_bits(&rt_spring_joint3d_get_damping),
        fn_bits(&rt_spring_joint3d_set_damping),
        fn_bits(&rt_spring_joint3d_get_rest_length),
        fn_bits(&rt_world3d_add_joint),
        fn_bits(&rt_world3d_remove_joint),
        fn_bits(&rt_world3d_joint_count),
        fn_bits(&rt_world3d_get_collision_count),
        fn_bits(&rt_world3d_get_collision_body_a),
        fn_bits(&rt_world3d_get_collision_body_b),
        fn_bits(&rt_world3d_get_collision_normal),
        fn_bits(&rt_world3d_get_collision_depth),
        fn_bits(&rt_world3d_get_collision_event_count),
        fn_bits(&rt_world3d_get_collision_event),
        fn_bits(&rt_world3d_get_enter_event_count),
        fn_bits(&rt_world3d_get_enter_event),
        fn_bits(&rt_world3d_get_stay_event_count),
        fn_bits(&rt_world3d_get_stay_event),
        fn_bits(&rt_world3d_get_exit_event_count),
        fn_bits(&rt_world3d_get_exit_event),
        fn_bits(&rt_world3d_raycast),
        fn_bits(&rt_world3d_raycast_all),
        fn_bits(&rt_world3d_sweep_sphere),
        fn_bits(&rt_world3d_sweep_capsule),
        fn_bits(&rt_world3d_overlap_sphere),
        fn_bits(&rt_world3d_overlap_aabb),
        fn_bits(&rt_physics_hit3d_get_body),
        fn_bits(&rt_physics_hit3d_get_collider),
        fn_bits(&rt_physics_hit3d_get_point),
        fn_bits(&rt_physics_hit3d_get_normal),
        fn_bits(&rt_physics_hit3d_get_distance),
        fn_bits(&rt_physics_hit3d_get_fraction),
        fn_bits(&rt_physics_hit3d_get_started_penetrating),
        fn_bits(&rt_physics_hit3d_get_is_trigger),
        fn_bits(&rt_physics_hit_list3d_get_count),
        fn_bits(&rt_physics_hit_list3d_get),
        fn_bits(&rt_collision_event3d_get_body_a),
        fn_bits(&rt_collision_event3d_get_body_b),
        fn_bits(&rt_collision_event3d_get_collider_a),
        fn_bits(&rt_collision_event3d_get_collider_b),
        fn_bits(&rt_collision_event3d_get_is_trigger),
        fn_bits(&rt_collision_event3d_get_contact_count),
        fn_bits(&rt_collision_event3d_get_relative_speed),
        fn_bits(&rt_collision_event3d_get_normal_impulse),
        fn_bits(&rt_collision_event3d_get_contact),
        fn_bits(&rt_collision_event3d_get_contact_point),
        fn_bits(&rt_collision_event3d_get_contact_normal),
        fn_bits(&rt_collision_event3d_get_contact_separation),
        fn_bits(&rt_contact_point3d_get_point),
        fn_bits(&rt_contact_point3d_get_normal),
        fn_bits(&rt_contact_point3d_get_separation),
        fn_bits(&rt_collider3d_new_box),
        fn_bits(&rt_collider3d_new_sphere),
        fn_bits(&rt_collider3d_new_capsule),
        fn_bits(&rt_collider3d_new_convex_hull),
        fn_bits(&rt_collider3d_new_mesh),
        fn_bits(&rt_collider3d_new_heightfield),
        fn_bits(&rt_collider3d_new_compound),
        fn_bits(&rt_collider3d_add_child),
        fn_bits(&rt_collider3d_get_type),
        fn_bits(&rt_collider3d_get_local_bounds_min),
        fn_bits(&rt_collider3d_get_local_bounds_max),
        fn_bits(&rt_body3d_new),
        fn_bits(&rt_body3d_set_collider),
        fn_bits(&rt_body3d_get_collider),
        fn_bits(&rt_body3d_set_orientation),
        fn_bits(&rt_body3d_get_orientation),
        fn_bits(&rt_body3d_set_angular_velocity),
        fn_bits(&rt_body3d_get_angular_velocity),
        fn_bits(&rt_body3d_apply_torque),
        fn_bits(&rt_body3d_apply_angular_impulse),
        fn_bits(&rt_body3d_set_linear_damping),
        fn_bits(&rt_body3d_get_linear_damping),
        fn_bits(&rt_body3d_set_angular_damping),
        fn_bits(&rt_body3d_get_angular_damping),
        fn_bits(&rt_body3d_set_kinematic),
        fn_bits(&rt_body3d_is_kinematic),
        fn_bits(&rt_body3d_set_can_sleep),
        fn_bits(&rt_body3d_can_sleep),
        fn_bits(&rt_body3d_is_sleeping),
        fn_bits(&rt_body3d_wake),
        fn_bits(&rt_body3d_sleep),
        fn_bits(&rt_body3d_set_use_ccd),
        fn_bits(&rt_body3d_get_use_ccd),
        fn_bits(&rt_textureasset3d_load_ktx2),
        fn_bits(&rt_textureasset3d_load_ktx2_asset),
        fn_bits(&rt_textureasset3d_get_width),
        fn_bits(&rt_textureasset3d_get_height),
        fn_bits(&rt_textureasset3d_get_mip_count),
        fn_bits(&rt_textureasset3d_get_format),
        fn_bits(&rt_textureasset3d_get_compressed),
        fn_bits(&rt_textureasset3d_get_resident_mip_start),
        fn_bits(&rt_textureasset3d_get_resident_mip_count),
        fn_bits(&rt_textureasset3d_get_resident_bytes),
        fn_bits(&rt_textureasset3d_set_resident_mip_range),
        fn_bits(&rt_terrain3d_set_splat_map),
        fn_bits(&rt_terrain3d_set_layer_texture),
        fn_bits(&rt_terrain3d_set_layer_scale),
        fn_bits(&rt_navmesh3d_bake),
        fn_bits(&rt_navmesh3d_bake_tiled),
        fn_bits(&rt_navmesh3d_add_offmesh_link),
        fn_bits(&rt_navmesh3d_get_offmesh_link_count),
        fn_bits(&rt_navmesh3d_add_obstacle),
        fn_bits(&rt_navmesh3d_get_obstacle_count),
        fn_bits(&rt_navmesh3d_rebuild_tile),
        fn_bits(&rt_navagent3d_new),
        fn_bits(&rt_navagent3d_set_target),
        fn_bits(&rt_navagent3d_clear_target),
        fn_bits(&rt_navagent3d_update),
        fn_bits(&rt_navagent3d_update_batch),
        fn_bits(&rt_navagent3d_warp),
        fn_bits(&rt_navagent3d_get_position),
        fn_bits(&rt_navagent3d_get_velocity),
        fn_bits(&rt_navagent3d_get_desired_velocity),
        fn_bits(&rt_navagent3d_get_has_path),
        fn_bits(&rt_navagent3d_get_remaining_distance),
        fn_bits(&rt_navagent3d_get_stopping_distance),
        fn_bits(&rt_navagent3d_set_stopping_distance),
        fn_bits(&rt_navagent3d_get_desired_speed),
        fn_bits(&rt_navagent3d_set_desired_speed),
        fn_bits(&rt_navagent3d_get_auto_repath),
        fn_bits(&rt_navagent3d_set_auto_repath),
        fn_bits(&rt_navagent3d_get_avoidance_enabled),
        fn_bits(&rt_navagent3d_set_avoidance_enabled),
        fn_bits(&rt_navagent3d_get_avoidance_radius),
        fn_bits(&rt_navagent3d_set_avoidance_radius),
        fn_bits(&rt_navagent3d_bind_character),
        fn_bits(&rt_navagent3d_bind_node),
        fn_bits(&rt_sound3d_sync_bindings),
        fn_bits(&rt_soundlistener3d_new),
        fn_bits(&rt_soundlistener3d_get_position),
        fn_bits(&rt_soundlistener3d_set_position),
        fn_bits(&rt_soundlistener3d_get_forward),
        fn_bits(&rt_soundlistener3d_set_forward),
        fn_bits(&rt_soundlistener3d_get_velocity),
        fn_bits(&rt_soundlistener3d_set_velocity),
        fn_bits(&rt_soundlistener3d_get_is_active),
        fn_bits(&rt_soundlistener3d_set_is_active),
        fn_bits(&rt_soundlistener3d_bind_node),
        fn_bits(&rt_soundlistener3d_clear_node_binding),
        fn_bits(&rt_soundlistener3d_bind_camera),
        fn_bits(&rt_soundlistener3d_clear_camera_binding),
        fn_bits(&rt_soundsource3d_new),
        fn_bits(&rt_soundsource3d_get_position),
        fn_bits(&rt_soundsource3d_set_position),
        fn_bits(&rt_soundsource3d_get_velocity),
        fn_bits(&rt_soundsource3d_set_velocity),
        fn_bits(&rt_soundsource3d_get_doppler_factor),
        fn_bits(&rt_soundsource3d_get_ref_distance),
        fn_bits(&rt_soundsource3d_set_ref_distance),
        fn_bits(&rt_soundsource3d_get_max_distance),
        fn_bits(&rt_soundsource3d_set_max_distance),
        fn_bits(&rt_soundsource3d_get_volume),
        fn_bits(&rt_soundsource3d_set_volume),
        fn_bits(&rt_soundsource3d_get_looping),
        fn_bits(&rt_soundsource3d_set_looping),
        fn_bits(&rt_soundsource3d_get_is_playing),
        fn_bits(&rt_soundsource3d_get_voice_id),
        fn_bits(&rt_soundsource3d_play),
        fn_bits(&rt_soundsource3d_stop),
        fn_bits(&rt_soundsource3d_bind_node),
        fn_bits(&rt_soundsource3d_clear_node_binding),
    };

    for (std::uintptr_t bits : surface) {
        assert(bits != 0);
    }

    test_filedialog_path_list_escaping();

    return 0;
}
