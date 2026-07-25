//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/RuntimeComponents.hpp
// Purpose: Common runtime component classification for native code linking.
// Key invariants: Symbol prefix mappings must stay aligned with runtime
//                 library organization; archive names are generated from the
//                 runtime build graph.
// Ownership/Lifetime: Header-only stateless utilities with no global state.
// Links: src/tools/zanna/cmd_codegen_arm64.cpp
//        src/codegen/x86_64/CodegenPipeline.cpp
// Cross-platform touchpoints: runtime archive composition, native-link
//                             archive discovery, host-dependent optional
//                             runtime surfaces.
//
//===----------------------------------------------------------------------===//

/**
 * @file RuntimeComponents.hpp
 * @brief Classifies referenced runtime symbols and computes the component
 *        archives required by native executables.
 *
 * The mapping in this file bridges emitted `rt_*`/`Zanna.*` symbol names and
 * the component archive manifest.  It also closes the component set over
 * internal runtime dependencies so linkers receive a deterministic,
 * duplicate-free archive list.
 */

#pragma once

#include "zanna/runtime/RuntimeComponentManifest.hpp"

#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace zanna::codegen {

/// @brief Runtime library components for selective linking.
/// @details Native backends use these to determine which runtime archives
///          to link based on symbols referenced in generated assembly.
enum class RtComponent {
    Base,         ///< Core runtime (always linked)
    Arrays,       ///< Array operations (rt_arr_*)
    Oop,          ///< Object-oriented features (rt_obj_*, rt_type_*, etc.)
    Collections,  ///< Collections and containers (rt_list_*, rt_map_*, etc.)
    Game,         ///< Game dev utilities (rt_grid2d_*, rt_timer_*, rt_achievement_*, etc.)
    Text,         ///< Text processing (rt_codec_*, rt_csv_*, etc.)
    IoFs,         ///< File I/O (rt_file_*, rt_dir_*, etc.)
    Exec,         ///< Process execution (rt_exec_*, rt_process_*, rt_machine_*)
    Threads,      ///< Threading (rt_monitor_*, rt_thread_*, etc.)
    Graphics,     ///< Graphics (rt_canvas_*, rt_color_*, etc.)
    Audio,        ///< Audio (rt_audio_*, rt_playlist_*, ogg_reader_*)
    Network,      ///< Network (rt_network_*, rt_restclient_*, etc.)
    Localization, ///< Localization (rt_locale_*, locale manager, LocaleInfo)
    Count,
};

/// @brief Maps a runtime symbol to the component that defines it.
/// @details Recognizes both C ABI `rt_*` names and namespace-qualified
///          `Zanna.*` frontend names.  Classification is purely lexical and
///          does not consult the live runtime registry.
/// @param sym Symbol name to classify, for example `rt_list_add` or
///            `Zanna.Collections.List.Add`.
/// @return The defining component when a known prefix or exact name matches;
///         otherwise `std::nullopt`.
/// @note Keep this in sync with src/runtime/CMakeLists.txt library organization.
inline std::optional<RtComponent> componentForRuntimeSymbol(std::string_view sym) {
    /// Tests whether the symbol being classified begins with @p prefix.
    auto starts = [&](std::string_view prefix) -> bool {
        return sym.size() >= prefix.size() && sym.substr(0, prefix.size()) == prefix;
    };

    // Arrays component
    if (starts("rt_arr_"))
        return RtComponent::Arrays;

    // OOP component
    if (starts("rt_obj_") || starts("rt_type_") || starts("rt_register_") || starts("rt_cast_") ||
        starts("rt_ns_") || starts("rt_box_") || starts("rt_exc_") || starts("rt_result_") ||
        starts("rt_option_") || starts("rt_lazy") || starts("rt_oop_") ||
        sym == "rt_bind_interface")
        return RtComponent::Oop;

    // Collections component
    if (starts("rt_list_") || starts("rt_map_") || starts("rt_treemap_") || starts("rt_bag_") ||
        starts("rt_queue_") || starts("rt_ring_") || starts("rt_seq_") || starts("rt_stack_") ||
        starts("rt_bytes_") || starts("rt_set_") || starts("rt_sortedset_") ||
        starts("rt_deque_") || starts("rt_bitset_") || starts("rt_bloomfilter_") ||
        starts("rt_bimap_") || starts("rt_countmap_") || starts("rt_defaultmap_") ||
        starts("rt_frozenset_") || starts("rt_frozenmap_") || starts("rt_lrucache_") ||
        starts("rt_multimap_") || starts("rt_orderedmap_") || starts("rt_sparsearray_") ||
        starts("rt_weakmap_") || starts("rt_pqueue_") || starts("rt_trie_") ||
        starts("rt_unionfind_") || starts("rt_convert_") || starts("rt_inputmanager_") ||
        starts("rt_inputaction_"))
        return RtComponent::Collections;

    // Game component (game dev utilities — lives in src/runtime/game/)
    if (starts("rt_grid2d_") || starts("rt_timer_") || starts("rt_statemachine_") ||
        starts("rt_animstate_") || starts("rt_tween_") || starts("rt_buttongroup_") ||
        starts("rt_smoothvalue_") || starts("rt_particle_") || starts("rt_spriteanim_") ||
        starts("rt_collision_") || starts("rt_objpool_") || starts("rt_screenfx_") ||
        starts("rt_pathfollow_") || starts("rt_quadtree_") || starts("rt_debugoverlay_") ||
        starts("rt_gameui_") || starts("rt_uilabel_") || starts("rt_uibar_") ||
        starts("rt_uipanel_") || starts("rt_uinineslice_") || starts("rt_uimenulist_") ||
        starts("rt_pathfinder_") || starts("rt_dialogue_") || starts("rt_lighting2d_") ||
        starts("rt_platformer_ctrl_") || starts("rt_achievement_") || starts("rt_typewriter_") ||
        starts("rt_quests_") || starts("rt_game_scene_"))
        return RtComponent::Game;

    // Text component
    if (starts("rt_codec_") || starts("rt_csv_") || starts("rt_guid_") || starts("rt_hash_") ||
        starts("rt_parse_") || starts("rt_json") || starts("rt_xml_") || starts("rt_yaml_") ||
        starts("rt_ini_") || starts("rt_toml_") || starts("rt_html_") || starts("rt_markdown_") ||
        starts("rt_regex_") || starts("rt_compiled_pattern_") || starts("rt_scanner_") ||
        starts("rt_template_") || starts("rt_textwrap_") || starts("rt_diff_") ||
        starts("rt_numfmt_") || starts("rt_pluralize_") || starts("rt_version_") ||
        starts("rt_keyderive_") || starts("rt_aes_") || starts("rt_cipher_") ||
        starts("rt_password_") || starts("rt_rand_") || starts("rt_fuzzy_match_"))
        return RtComponent::Text;

    // I/O and filesystem component
    if (starts("rt_file_") || starts("rt_dir_") || starts("rt_path_") || starts("rt_binfile_") ||
        starts("rt_linereader_") || starts("rt_linewriter_") || starts("rt_io_file_") ||
        starts("rt_memstream_") || starts("rt_stream_") || starts("rt_watcher_") ||
        starts("rt_compress_") || starts("rt_archive_") || starts("rt_glob_") ||
        starts("rt_tempfile_") || starts("rt_savedata_") || starts("rt_workspace_") ||
        starts("rt_asset_resolver_") || starts("rt_project_manifest_") || sym == "rt_eof_ch" ||
        sym == "rt_lof_ch" || sym == "rt_loc_ch" || sym == "rt_close_err" ||
        sym == "rt_seek_ch_err" || sym == "rt_write_ch_err" || sym == "rt_println_ch_err" ||
        sym == "rt_line_input_ch_err" || sym == "rt_open_err_vstr")
        return RtComponent::IoFs;

    // Exec component. rt_shutdown_* lives in the same rt_exec archive, so native
    // programs using Zanna.System.Shutdown must pull it in too — omitting it left
    // every native Shutdown user with an undefined-symbol link error (VDOC-210).
    if (starts("rt_exec_") || starts("rt_process_") || starts("rt_machine_") ||
        starts("rt_shutdown_"))
        return RtComponent::Exec;

    // Threads component
    if (starts("rt_monitor_") || starts("rt_thread_") || starts("rt_safe_") ||
        starts("rt_channel_") || starts("rt_future_") || starts("rt_parallel_") ||
        starts("rt_concqueue_") || starts("rt_cancellation_") || starts("rt_debounce_") ||
        starts("rt_scheduler_") || starts("rt_pool_"))
        return RtComponent::Threads;

    // Graphics component
    if (starts("rt_canvas_") || starts("rt_color_") || starts("rt_vec2_") || starts("rt_vec3_") ||
        starts("rt_pixels_") || starts("rt_sprite_") || starts("rt_spritebatch_") ||
        starts("rt_tilemap_") || starts("rt_camera_") || starts("rt_scene_") ||
        starts("rt_font_") || starts("rt_gui_") || starts("rt_checkbox_") ||
        starts("rt_codeeditor_") || starts("rt_widget_") || starts("rt_treeview_") ||
        starts("rt_virtual_") || starts("rt_command_state_") || starts("rt_accessibility_") ||
        starts("rt_radiobutton_") || starts("rt_menuitem_") || starts("rt_contextmenu_") ||
        starts("rt_statusbar_") || starts("rt_toolbar_") || starts("rt_findbar_") ||
        starts("rt_commandpalette_") || starts("rt_scrollview_") || starts("rt_action_") ||
        starts("rt_input_") || starts("rt_inputmgr_") || starts("rt_mat3_") || starts("rt_mat4_") ||
        starts("rt_graphics_") || starts("rt_aabb3d_") || starts("rt_anim_blend3d_") ||
        starts("rt_anim_controller3d_") || starts("rt_anim_player3d_") ||
        starts("rt_animation3d_") || starts("rt_assets3d_") || starts("rt_blend_tree3d_") ||
        starts("rt_body3d_") || starts("rt_camera3d_") || starts("rt_canvas3d_") ||
        starts("rt_capsule3d_") || starts("rt_character3d_") || starts("rt_collider3d_") ||
        starts("rt_collision_event3d_") || starts("rt_contact_point3d_") ||
        starts("rt_cubemap3d_") || starts("rt_decal3d_") || starts("rt_distance_joint3d_") ||
        starts("rt_fbx_") || starts("rt_game3d_") || starts("rt_gltf_") ||
        starts("rt_hinge_joint3d_") || starts("rt_ik_solver3d_") || starts("rt_instbatch3d_") ||
        starts("rt_joints3d_") || starts("rt_light3d_") || starts("rt_material3d_") ||
        starts("rt_mesh3d_") || starts("rt_model3d_") || starts("rt_morphtarget3d_") ||
        starts("rt_navagent3d_") || starts("rt_navmesh3d_") || starts("rt_node_animation3d_") ||
        starts("rt_node_animator3d_") || starts("rt_particles3d_") || starts("rt_path3d_") ||
        starts("rt_physics_hit3d_") || starts("rt_physics_hit_list3d_") || starts("rt_postfx3d_") ||
        starts("rt_ray3d_") || starts("rt_rendertarget3d_") || starts("rt_rope_joint3d_") ||
        starts("rt_scene3d_") || starts("rt_scene_node3d_") || starts("rt_segment3d_") ||
        starts("rt_sixdof_joint3d_") || starts("rt_skeleton3d_") || starts("rt_sphere3d_") ||
        starts("rt_spring_joint3d_") || starts("rt_sprite3d_") || starts("rt_terrain3d_") ||
        starts("rt_texatlas3d_") || starts("rt_textureasset3d_") || starts("rt_transform3d_") ||
        starts("rt_trigger3d_") || starts("rt_vegetation3d_") || starts("rt_water3d_") ||
        starts("rt_world3d_"))
        return RtComponent::Graphics;

    // Audio component
    if (starts("rt_audio_") || starts("rt_playlist_") || starts("rt_sound_") ||
        starts("rt_soundbank_") || starts("rt_synth_") || starts("rt_music_") ||
        starts("rt_voice_") || starts("rt_sound3d_") || starts("rt_soundlistener3d_") ||
        starts("rt_soundsource3d_") || starts("ogg_reader_"))
        return RtComponent::Audio;

    // Network component
    if (starts("rt_network_") || starts("rt_restclient_") || starts("rt_retry_") ||
        starts("rt_ratelimit_") || starts("rt_websocket_") || starts("rt_crypto_") ||
        starts("rt_tls_") || starts("rt_http_") || starts("rt_tcp_") || starts("rt_udp_") ||
        starts("rt_dns_") || starts("rt_url_"))
        return RtComponent::Network;

    // Localization component
    if (starts("rt_locale_") || starts("rt_numformat_") || starts("rt_dateformat_") ||
        starts("rt_reltimefmt_") || starts("rt_message_bundle_") || starts("rt_list_format_") ||
        starts("rt_text_direction_") || starts("rt_collator_") || starts("rt_plural_rules_"))
        return RtComponent::Localization;

    // Base component (time, math, formatting, etc.)
    if (starts("rt_context_") || starts("rt_crc32_") || starts("rt_error_") || starts("rt_trap_") ||
        sym == "rt_trap" || sym == "rt_init_stack_safety" || sym == "rt_trap_stack_overflow" ||
        starts("rt_fp_") || starts("rt_memory_") || starts("rt_string_") || starts("rt_io_") ||
        starts("rt_math_") || starts("rt_perlin_") || starts("rt_random_") || starts("rt_bits_") ||
        starts("rt_numeric_") || starts("rt_bigint_") || starts("rt_debug_") || starts("rt_fmt_") ||
        starts("rt_format_") || starts("rt_int_format_") || starts("rt_printf_") ||
        starts("rt_term_") || starts("rt_time_") || starts("rt_datetime_") ||
        starts("rt_dateonly_") || starts("rt_daterange_") || starts("rt_duration_") ||
        starts("rt_reltime_") || starts("rt_stopwatch_") || starts("rt_countdown_") ||
        starts("rt_easing_") || starts("rt_modvar_") || starts("rt_args_") || starts("rt_log_") ||
        starts("rt_msgbus_") || starts("rt_heap_") || starts("rt_output_"))
        return RtComponent::Base;

    // -------------------------------------------------------------------------
    // Zanna.* namespace-qualified symbols (from OOP-style IL / Zia frontend).
    // These are emitted as extern calls like @Zanna.String.Left in IL and
    // become assembly symbols like _Zanna.String.Left.
    // -------------------------------------------------------------------------
    if (starts("Zanna.Terminal.") || starts("Zanna.String.") || starts("Zanna.Math.") ||
        starts("Zanna.Core.") || starts("Zanna.System.Environment."))
        return RtComponent::Base;
    if (starts("Zanna.Collections."))
        return RtComponent::Collections;
    if (starts("Zanna.Game."))
        return RtComponent::Game;
    if (starts("Zanna.Text."))
        return RtComponent::Text;
    if (starts("Zanna.IO.") || starts("Zanna.File.") || starts("Zanna.Dir.") ||
        starts("Zanna.Path.") || starts("Zanna.Workspace.") || starts("Zanna.Assets.") ||
        starts("Zanna.Project."))
        return RtComponent::IoFs;
    if (starts("Zanna.Net.") || starts("Zanna.Http.") || starts("Zanna.WebSocket."))
        return RtComponent::Network;
    if (starts("Zanna.Canvas.") || starts("Zanna.Input.") || starts("Zanna.GUI.") ||
        starts("Zanna.Graphics.") || starts("Zanna.Graphics3D.") || starts("Zanna.Color."))
        return RtComponent::Graphics;
    if (starts("Zanna.Sound.") || starts("Zanna.Music."))
        return RtComponent::Audio;
    if (starts("Zanna.Thread.") || starts("Zanna.Channel.") || starts("Zanna.Future."))
        return RtComponent::Threads;
    if (starts("Zanna.System.Exec.") || starts("Zanna.System.Machine.") ||
        starts("Zanna.System.Process.") || starts("Zanna.System.Shutdown."))
        return RtComponent::Exec;
    if (starts("Zanna.Debug."))
        return RtComponent::Base;
    if (starts("Zanna.Localization."))
        return RtComponent::Localization;

    return std::nullopt;
}

/// @brief Obtains the manifest archive name for a runtime component.
/// @param comp Component whose static-library base name is required.
/// @return The corresponding manifest entry, such as
///         `zanna_rt_collections`.  An invalid value (including `Count`)
///         falls back to the Base archive.
inline std::string_view archiveNameForComponent(RtComponent comp) {
    static_assert(zanna::runtime_manifest::kRuntimeComponentArchives.size() ==
                  static_cast<size_t>(RtComponent::Count));
    const size_t index = static_cast<size_t>(comp);
    if (index < zanna::runtime_manifest::kRuntimeComponentArchives.size()) {
        return zanna::runtime_manifest::kRuntimeComponentArchives[index];
    }
    return zanna::runtime_manifest::kRuntimeComponentArchives[0];
}

/// @brief Resolves the complete runtime component set required by symbols.
/// @details Classifies recognized symbols, applies transitive internal
///          dependency rules, and emits each required component once in a
///          stable link order.  Unrecognized symbols do not add a component.
/// @tparam SymbolRange A range type whose elements are convertible to std::string_view
///                     (e.g., std::unordered_set<std::string>, std::vector<std::string>).
/// @param symbols The runtime symbols referenced by generated code.
/// @return Dependency-closed, duplicate-free component list with Base first.
template <typename SymbolRange>
inline std::vector<RtComponent> resolveRequiredComponents(const SymbolRange &symbols) {
    // Classify symbols into components.
    std::unordered_set<int> needed;
    for (const auto &sym : symbols) {
        const auto comp = componentForRuntimeSymbol(sym);
        if (comp)
            needed.insert(static_cast<int>(*comp));
    }

    /// Reports whether @p c is already present in the dependency closure.
    auto has = [&](RtComponent c) { return needed.count(static_cast<int>(c)) != 0; };
    /// Inserts @p c into the dependency closure; duplicate insertions are harmless.
    auto add = [&](RtComponent c) { needed.insert(static_cast<int>(c)); };

    // Apply dependency rules (internal runtime calls between components).
    // Note: Base's calls to rt_audio_shutdown and rt_file_state_cleanup use
    // weak symbols, so Audio/IoFs are NOT unconditionally required.
    if (has(RtComponent::Text) || has(RtComponent::IoFs) || has(RtComponent::Exec) ||
        has(RtComponent::Network))
        add(RtComponent::Collections);
    if (has(RtComponent::Game)) {
        add(RtComponent::Collections); // Game depends on Collections + OOP
        add(RtComponent::Text);        // LevelData/scene editor use JSON helpers
    }
    if (has(RtComponent::Graphics))
        add(RtComponent::Localization); // Game3D Dialogue3D resolves text via MessageBundle
    if (has(RtComponent::Localization))
        add(RtComponent::Collections); // MessageBundle.FromMap reads rt_map entries
    if (has(RtComponent::IoFs)) {
        add(RtComponent::Text);    // SaveData depends on rt_json_stream_*
        add(RtComponent::Network); // IO uses the shared OS entropy adapter.
    }
    if (has(RtComponent::Collections))
        add(RtComponent::Arrays);
    if (has(RtComponent::Collections) || has(RtComponent::Arrays) || has(RtComponent::Graphics) ||
        has(RtComponent::Threads) || has(RtComponent::Audio) || has(RtComponent::Network) ||
        has(RtComponent::Game) || has(RtComponent::Localization))
        add(RtComponent::Oop);
    if (has(RtComponent::Oop))
        add(RtComponent::Threads);

    // Build ordered list (Base always first).
    std::vector<RtComponent> result;
    result.push_back(RtComponent::Base);

    // Add remaining components in a stable order.
    static constexpr RtComponent order[] = {
        RtComponent::Oop,
        RtComponent::Arrays,
        RtComponent::Collections,
        RtComponent::Game,
        RtComponent::Text,
        RtComponent::IoFs,
        RtComponent::Exec,
        RtComponent::Threads,
        RtComponent::Graphics,
        RtComponent::Audio,
        RtComponent::Network,
        RtComponent::Localization,
    };
    for (auto c : order) {
        if (has(c))
            result.push_back(c);
    }
    return result;
}

} // namespace zanna::codegen
