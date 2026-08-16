set(RT_BASE_SOURCES
        core/rt_context.c
        core/rt_crc32.c
        core/rt_error.c
        core/rt_trap.c
        core/rt_stack_safety.c
        core/rt_fp.c
        core/rt_memory.c
        core/rt_output.c
        core/rt_string_ops.c
        core/rt_string_advanced.c
        core/rt_string_specialized.c
        core/rt_string_builder.c
        core/rt_string_format.c
        core/rt_string_encode.c
        core/rt_string_intern.c
        core/rt_io.c
        core/rt_math.c
        core/rt_perlin.c
        core/rt_random.c
        core/rt_bits.c
        core/rt_pool.c
        core/rt_heap.c
        core/rt_numeric.c
        core/rt_numeric_conv.c
        core/rt_bigint.c
        core/rt_debug.c
        core/rt_fmt.c
        core/rt_format.c
        core/rt_int_format.c
        core/rt_printf_compat.c
        core/rt_term.c
        core/rt_time.c
        core/rt_datetime.c
        core/rt_dateonly.c
        core/rt_easing.c
        core/rt_duration.c
        core/rt_reltime.c
        core/rt_daterange.c
        core/rt_stopwatch.c
        core/rt_countdown.c
        core/rt_testing.c
        core/rt_modvar.c
        core/rt_args.c
        core/rt_log.c
        core/rt_msgbus.c
        core/rt_gc.c
        core/rt_zia_completion_stub.c
        core/rt_zia_highlight_stub.c
        core/rt_basic_completion_stub.c
)

set(RT_ARRAY_SOURCES
        arrays/rt_array.c
        arrays/rt_array_i64.c
        arrays/rt_array_f64.c
        arrays/rt_array_str.c
        arrays/rt_array_obj.c
        arrays/rt_list_i64.c
)

set(RT_OOP_SOURCES
        oop/rt_box.c
        oop/rt_object.c
        oop/rt_oop_dispatch.c
        oop/rt_type_registry.c
        oop/rt_sb_bridge.c
        oop/rt_exc.c
        oop/rt_result.c
        oop/rt_option.c
        oop/rt_lazy.c
        oop/rt_lazyseq.c
)

set(RT_COLLECTIONS_SOURCES
        collections/rt_bag.c
        collections/rt_convert_coll.c
        collections/rt_iter.c
        collections/rt_bitset.c
        collections/rt_deque.c
        collections/rt_list.c
        collections/rt_pqueue.c
        collections/rt_seq.c
        collections/rt_seq_ops.c
        collections/rt_seq_functional.c
        collections/rt_set.c
        collections/rt_sortedset.c
        collections/rt_stack.c
        collections/rt_queue.c
        collections/rt_ring.c
        collections/rt_lrucache.c
        collections/rt_bloomfilter.c
        collections/rt_unionfind.c
        collections/rt_orderedmap.c
        collections/rt_defaultmap.c
        collections/rt_frozenset.c
        collections/rt_frozenmap.c
        collections/rt_sparsearray.c
        collections/rt_weakmap.c
        collections/rt_map.c
        collections/rt_intmap.c
        collections/rt_numbuf.c
        collections/rt_bimap.c
        collections/rt_countmap.c
        collections/rt_multimap.c
        collections/rt_treemap.c
        collections/rt_trie.c
        collections/rt_bytes.c
        collections/rt_binbuf.c
)

set(RT_GAME_SOURCES
        game/rt_grid2d.c
        game/rt_timer.c
        game/rt_statemachine.c
        game/rt_animation_events.c
        game/rt_animstate.c
        game/rt_animtimeline.c
        game/rt_tween.c
        game/rt_buttongroup.c
        game/rt_smoothvalue.c
        game/rt_particle.c
        game/rt_spriteanim.c
        game/rt_collision.c
        game/rt_world_projection.c
        game/rt_objpool.c
        game/rt_screenfx.c
        game/rt_pathfollow.c
        game/rt_quadtree.c
        game/rt_debugoverlay.c
        game/rt_gameui.c
        game/rt_gameui_draw.c
        game/rt_gameui_widgets.c
        game/rt_gameui_textinput.c
        game/rt_pathfinder.c
        game/rt_dialogue.c
        game/rt_lighting2d.c
        game/rt_platformer_ctrl.c
        game/rt_achievement.c
        game/rt_quests.c
        game/rt_typewriter.c
        game/rt_entity.c
        game/rt_behavior.c
        game/rt_leveldata.c
        game/rt_raycast2d.c
        game/rt_scenemanager.c
        game/rt_config.c
        game/rt_scene_editor.cpp
        game/rt_tiled_import.cpp
)

set(RT_TEXT_SOURCES
        text/rt_codec.c
        text/rt_csv.c
        text/rt_ini.c
        text/rt_guid.c
        text/rt_hash.c
        text/rt_table.c
        text/rt_hash_util.c
        text/rt_json_parse.c
        text/rt_json_format.c
        text/rt_json_validate.c
        text/rt_json_stream.c
        text/rt_xml.c
        text/rt_xml_format.c
        text/rt_yaml.c
        text/rt_yaml_format.c
        text/rt_keyderive.c
        text/rt_aes.c
        text/rt_cipher.c
        text/rt_password.c
        text/rt_rand.c
        text/rt_regex.c
        text/rt_regex_parse.c
        text/rt_regex_match.c
        text/rt_compiled_pattern.c
        text/rt_scanner.c
        text/rt_template.c
        text/rt_textwrap.c
        text/rt_diff.c
        text/rt_numfmt.c
        text/rt_parse.c
        text/rt_pluralize.c
        text/rt_version.c
        text/rt_toml.c
        text/rt_markdown.c
        text/rt_jsonpath.c
        text/rt_html.c
        text/rt_serialize.c
        text/rt_fuzzy_match.cpp
)

set(RT_IO_FS_SOURCES
        io/rt_file.c
        io/rt_file_io.c
        io/rt_file_path.c
        io/rt_file_ext.c
        io/rt_binfile.c
        io/rt_memstream.c
        io/rt_stream.c
        io/rt_linereader.c
        io/rt_linewriter.c
        io/rt_path.c
        io/rt_dir.c
        io/rt_dir_list.c
        io/rt_dir_page.cpp
        io/rt_glob.c
        io/rt_tempfile.c
        io/rt_watcher.c
        io/rt_compress.c
        io/rt_zstd.c
        io/rt_deflate.c
        io/rt_archive.c
        io/rt_archive_fs.c
        io/rt_archive_read.c
        io/rt_savedata.c
        io/rt_zpak_reader.c
        io/rt_path_exe.c
        io/rt_asset.c
        io/rt_asset_decode.c
        io/rt_ide_primitives.cpp
)

set(RT_EXEC_SOURCES
        system/rt_exec.c
        system/rt_process.c
        system/rt_pty.c
        system/rt_machine.c
        system/rt_machine_linux_cgroup.c
        system/rt_shutdown.c
)

set(RT_GRAPHICS_SOURCES
        graphics/common/rt_embed_host.c
        graphics/2d/rt_action.c
        graphics/2d/rt_action_presets.c
        graphics/2d/rt_action_io.c
        graphics/text/rt_font.c
        graphics/2d/rt_canvas.c
        graphics/2d/rt_drawing.c
        graphics/2d/rt_drawing_advanced.c
        graphics/2d/rt_color.c
        graphics/gui/rt_gui_app.c
        graphics/gui/rt_gui_app_font.c
        graphics/gui/rt_gui_font_platform.c
        graphics/gui/rt_gui_accessibility.c
        graphics/gui/rt_gui_codeeditor.c
        graphics/gui/rt_gui_codeeditor_syntax.c
        graphics/gui/rt_gui_messagebox.c
        graphics/gui/rt_gui_filedialog.c
        graphics/gui/rt_gui_findbar.c
        graphics/gui/rt_gui_features.c
        graphics/gui/rt_gui_navaids.c
        graphics/gui/rt_gui_menus.c
        graphics/gui/rt_gui_bars.c
        graphics/gui/rt_gui_system.c
        graphics/gui/rt_gui_widgets.c
        graphics/gui/rt_gui_subhandle.c
        graphics/gui/rt_gui_layout.c
        graphics/gui/rt_gui_theme.c
        graphics/gui/rt_gui_widgets_complex.c
        graphics/gui/rt_gui_image.c
        graphics/gui/rt_gui_controls.c
        graphics/gui/rt_gui_color.c
        graphics/gui/rt_gui_constants.c
        graphics/gui/rt_gui_ide.cpp
        graphics/input/rt_input.c
        graphics/input/rt_input_pad.c
        graphics/input/rt_inputmgr.c
        graphics/input/rt_keychord.c
        graphics/math/rt_mat3.c
        graphics/math/rt_mat4.c
        graphics/math/rt_vec2.c
        graphics/math/rt_vec3.c
        graphics/math/rt_quat.c
        graphics/math/rt_spline.c
        graphics/2d/rt_physics2d.c
        graphics/2d/rt_physics2d_collision.c
        graphics/2d/rt_physics2d_joint.c
        graphics/3d/physics/rt_cloth3d.c
        graphics/3d/physics/rt_collider3d.c
        graphics/3d/physics/rt_quickhull3d.c
        graphics/3d/assets/rt_mesh_simplify.c
        graphics/2d/rt_pixels.c
        graphics/2d/rt_pixels_io.c
        graphics/2d/rt_pixels_png.c
        graphics/2d/rt_pixels_jpeg.c
        graphics/2d/rt_pixels_transform.c
        graphics/2d/rt_pixels_draw.c
        graphics/media/rt_gif.c
        graphics/3d/assets/rt_asset_error.c
        graphics/3d/assets/rt_gltf.c
        graphics/3d/assets/rt_gltf_trs.c
        graphics/3d/assets/rt_gltf_json.c
        graphics/3d/assets/rt_textureasset3d.c
        graphics/2d/rt_sprite.c
        graphics/2d/rt_spritesheet.c
        graphics/2d/rt_camera.c
        graphics/2d/rt_tilemap.c
        graphics/2d/rt_tilemap_io.c
        graphics/2d/rt_scene.c
        graphics/2d/rt_spritebatch.c
        graphics/2d/rt_texatlas.c
        graphics/text/rt_bitmapfont.c
        graphics/text/rt_ttf_font.c
        graphics/2d/rt_graphics2d.c
        graphics/2d/rt_graphics2d_tilemap.c
        graphics/2d/rt_graphics2d_render.c
        graphics/text/fonts/embedded_font.c  # JetBrains Mono Regular (MIT) — embedded for portable font rendering
        graphics/3d/render/rt_canvas3d.c
        graphics/3d/render/rt_canvas3d_overlay.c
        graphics/3d/render/rt_canvas3d_gameui.c
        graphics/3d/render/rt_canvas3d_clusters.c
        graphics/3d/render/rt_canvas3d_tempmgr.c
        graphics/3d/render/rt_canvas3d_snapshot.c
        graphics/3d/render/rt_canvas3d_motion.c
        graphics/3d/render/rt_canvas3d_skybox.c
        graphics/3d/render/rt_canvas3d_lighting.c
        graphics/3d/backend/vgfx3d_backend_sw.c
        graphics/3d/rt_g3d_commit_queue.c
        graphics/3d/rt_game3d.c
        graphics/3d/rt_game3d_enums.c
        graphics/3d/rt_game3d_controllers.c
        graphics/3d/rt_game3d_thirdperson.c
        graphics/3d/rt_game3d_targetlock.c
        graphics/3d/rt_game3d_combat.c
        graphics/3d/rt_game3d_railcamera.c
        graphics/3d/rt_game3d_timeline.c
        graphics/3d/rt_game3d_dialogue.c
        graphics/3d/rt_game3d_facial.c
        graphics/3d/rt_game3d_bodydef.c
        graphics/3d/rt_game3d_audio.c
        graphics/3d/rt_game3d_effects.c
        graphics/3d/rt_game3d_animator.c
        graphics/3d/rt_game3d_behavior.c
        graphics/3d/rt_game3d_entity.c
        graphics/3d/rt_game3d_input.c
        graphics/3d/rt_game3d_presets.c
        graphics/3d/rt_game3d_diagnostics.c
        graphics/3d/rt_game3d_surfaces.c
        graphics/3d/rt_game3d_footsteps.c
        graphics/3d/rt_game3d_interact.c
        graphics/3d/rt_game3d_persistence.c
        graphics/3d/rt_game3d_minimap.c
        graphics/3d/rt_game3d_ai.c
        graphics/3d/render/rt_mesh3d.c
        graphics/3d/render/rt_camera3d.c
        graphics/3d/render/rt_material3d.c
        graphics/3d/render/rt_light3d.c
        graphics/3d/render/rt_cubemap3d.c
        graphics/3d/render/rt_rendertarget3d.c
        graphics/3d/scene/rt_scene3d.c
        graphics/3d/scene/rt_scene3d_node.c
        graphics/3d/scene/rt_scene3d_metadata.c
        graphics/3d/scene/rt_scene3d_nodeanim.c
        graphics/3d/scene/rt_scene3d_query.c
        graphics/3d/scene/rt_scene3d_spatial.c
        graphics/3d/scene/rt_scene3d_vscn_save.c
        graphics/3d/scene/rt_scene3d_vscn_load.c
        graphics/3d/anim/rt_animcontroller3d.c
        graphics/3d/anim/rt_blendtree3d.c
        graphics/3d/anim/rt_iksolver3d.c
        graphics/3d/render/rt_model3d.c
        graphics/3d/anim/rt_skeleton3d.c
        graphics/3d/assets/rt_fbx_loader.c
        graphics/3d/anim/rt_morphtarget3d.c
        graphics/3d/world/rt_particles3d.c
        graphics/3d/render/rt_postfx3d.c
        graphics/3d/render/rt_lightbaker3d.c
        graphics/3d/render/rt_reflectionprobe3d.c
        graphics/3d/world/rt_sky3d.c
        graphics/3d/world/rt_timeofday3d.c
        graphics/3d/scene/rt_raycast3d.c
        graphics/3d/physics/rt_physics3d.c
        graphics/3d/physics/rt_physics3d_character.c
        graphics/3d/physics/rt_physics3d_probes.c
        graphics/3d/physics/rt_ragdoll3d.c
        graphics/3d/physics/rt_vehicle3d.c
        graphics/3d/physics/rt_physics3d_collision.c
        graphics/3d/physics/rt_physics3d_query.c
        graphics/3d/physics/rt_physics3d_raycast.c
        graphics/3d/physics/rt_physics3d_solver.c
        graphics/3d/physics/rt_joints3d.c
        graphics/3d/physics/rt_joints3d_math.c
        graphics/3d/scene/rt_transform3d.c
        graphics/3d/nav/rt_path3d.c
        graphics/3d/render/rt_instbatch3d.c
        graphics/3d/world/rt_terrain3d.c
        graphics/3d/nav/rt_navmesh3d.c
        graphics/3d/nav/rt_navagent3d.c
        graphics/3d/render/rt_decal3d.c
        graphics/3d/render/rt_lensflare3d.c
        graphics/3d/render/rt_sprite3d.c
        graphics/3d/world/rt_water3d.c
        graphics/3d/world/rt_vegetation3d.c
        graphics/media/rt_avi.c
        graphics/media/rt_videoplayer.c
        graphics/media/rt_theora.c
        graphics/media/rt_theora_recon.c
        graphics/media/rt_ycbcr.c
        graphics/gui/rt_videowidget.c
        graphics/3d/render/rt_texatlas3d.c
        graphics/3d/backend/vgfx3d_frustum.c
        graphics/3d/backend/vgfx3d_brdf_lut.c
        graphics/3d/backend/vgfx3d_skinning.c
        graphics/3d/backend/vgfx3d_backend_utils.c
        graphics/3d/backend/vgfx3d_backend_d3d11_shared.c
        graphics/3d/backend/vgfx3d_backend_metal_shared.c
        graphics/3d/backend/vgfx3d_backend_opengl_shared.c
)

set(RT_GRAPHICS_PLATFORM_SOURCES)

set(RT_GRAPHICS_DISABLED_SOURCES
        graphics/2d/rt_action.c
        graphics/2d/rt_action_presets.c
        graphics/2d/rt_action_io.c
        graphics/text/rt_font.c
        graphics/gui/rt_gui_app.c
        graphics/gui/rt_gui_app_font.c
        graphics/gui/rt_gui_accessibility.c
        graphics/gui/rt_gui_codeeditor.c
        graphics/gui/rt_gui_codeeditor_syntax.c
        graphics/gui/rt_gui_messagebox.c
        graphics/gui/rt_gui_filedialog.c
        graphics/gui/rt_gui_findbar.c
        graphics/gui/rt_gui_features.c
        graphics/gui/rt_gui_navaids.c
        graphics/gui/rt_gui_menus.c
        graphics/gui/rt_gui_bars.c
        graphics/gui/rt_gui_system.c
        graphics/gui/rt_gui_widgets.c
        graphics/gui/rt_gui_subhandle.c
        graphics/gui/rt_gui_layout.c
        graphics/gui/rt_gui_theme.c
        graphics/gui/rt_gui_widgets_complex.c
        graphics/gui/rt_gui_image.c
        graphics/gui/rt_gui_controls.c
        graphics/gui/rt_gui_color.c
        graphics/gui/rt_gui_constants.c
        graphics/gui/rt_gui_ide.cpp
        graphics/input/rt_input.c
        graphics/input/rt_input_pad.c
        graphics/input/rt_inputmgr.c
        graphics/input/rt_keychord.c
        graphics/math/rt_mat3.c
        graphics/math/rt_mat4.c
        graphics/math/rt_vec2.c
        graphics/math/rt_vec3.c
        graphics/math/rt_quat.c
        graphics/math/rt_spline.c
        graphics/2d/rt_physics2d.c
        graphics/2d/rt_physics2d_collision.c
        graphics/2d/rt_physics2d_joint.c
        graphics/2d/rt_pixels.c
        graphics/2d/rt_pixels_io.c
        graphics/2d/rt_pixels_png.c
        graphics/2d/rt_pixels_jpeg.c
        graphics/2d/rt_pixels_transform.c
        graphics/2d/rt_pixels_draw.c
        graphics/media/rt_gif.c
        graphics/3d/assets/rt_asset_error.c
        graphics/2d/rt_sprite.c
        graphics/2d/rt_spritesheet.c
        graphics/2d/rt_camera.c
        graphics/2d/rt_tilemap.c
        graphics/2d/rt_tilemap_io.c
        graphics/2d/rt_scene.c
        graphics/2d/rt_spritebatch.c
        graphics/2d/rt_texatlas.c
        graphics/text/rt_bitmapfont.c
        graphics/text/rt_ttf_font.c
        graphics/2d/rt_graphics2d.c
        graphics/2d/rt_graphics2d_tilemap.c
        graphics/2d/rt_graphics2d_render.c
        graphics/text/fonts/embedded_font.c
        graphics/3d/rt_g3d_commit_queue.c
        graphics/3d/rt_game3d.c
        graphics/3d/rt_game3d_enums.c
        graphics/3d/rt_game3d_controllers.c
        graphics/3d/rt_game3d_thirdperson.c
        graphics/3d/rt_game3d_targetlock.c
        graphics/3d/rt_game3d_combat.c
        graphics/3d/rt_game3d_railcamera.c
        graphics/3d/rt_game3d_timeline.c
        graphics/3d/rt_game3d_dialogue.c
        graphics/3d/rt_game3d_facial.c
        graphics/3d/rt_game3d_bodydef.c
        graphics/3d/rt_game3d_audio.c
        graphics/3d/rt_game3d_effects.c
        graphics/3d/rt_game3d_animator.c
        graphics/3d/rt_game3d_behavior.c
        graphics/3d/rt_game3d_entity.c
        graphics/3d/rt_game3d_input.c
        graphics/3d/rt_game3d_presets.c
        graphics/3d/rt_game3d_diagnostics.c
        graphics/3d/rt_game3d_ai.c
        graphics/3d/rt_game3d_interact.c
        graphics/3d/rt_game3d_minimap.c
        graphics/3d/rt_game3d_persistence.c
        graphics/3d/rt_game3d_footsteps.c
        graphics/3d/rt_game3d_surfaces.c
        graphics/media/rt_avi.c
        graphics/media/rt_theora.c
        graphics/media/rt_theora_recon.c
        graphics/media/rt_ycbcr.c
        graphics/3d/backend/vgfx3d_backend_utils.c
        graphics/common/rt_canvas_stubs.c
        graphics/common/rt_graphics_helper_stubs.c
        graphics/common/rt_canvas3d_stubs.c
        graphics/common/rt_embed_host.c
        graphics/common/rt_3d_asset_stubs.c
        graphics/common/rt_3d_textureasset_stubs.c
        graphics/common/rt_3d_scene_stubs.c
        graphics/common/rt_3d_physics_stubs.c
        graphics/common/rt_3d_world_stubs.c
        graphics/common/rt_graphics_media_stubs.c
        graphics/common/rt_disabled_runtime_stubs.c
)

# GPU backends (platform-conditional)
if (APPLE)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/3d/backend/vgfx3d_backend_metal.m)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_macos_menu.m)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_macos.m)
    set_source_files_properties(graphics/3d/backend/vgfx3d_backend_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc -U_POSIX_C_SOURCE")
    set_source_files_properties(graphics/gui/rt_gui_macos_menu.m PROPERTIES COMPILE_FLAGS "-fobjc-arc -U_POSIX_C_SOURCE")
    set_source_files_properties(graphics/gui/rt_gui_accessibility_macos.m PROPERTIES COMPILE_FLAGS "-fobjc-arc -U_POSIX_C_SOURCE")
endif ()
if (WIN32)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/3d/backend/vgfx3d_backend_d3d11.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_win32.c)
endif ()
if (UNIX AND NOT APPLE AND ZANNAGFX_BACKEND STREQUAL "X11")
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/gui/rt_gui_linux_portal.c
         graphics/gui/rt_gui_atspi_linux.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/3d/backend/vgfx3d_backend_opengl.c
         graphics/3d/backend/vgfx3d_egl_wayland.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_linux.c)
elseif (UNIX AND NOT APPLE AND ZANNAGFX_BACKEND STREQUAL "Wayland")
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/gui/rt_gui_linux_portal.c
         graphics/gui/rt_gui_atspi_linux.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/3d/backend/vgfx3d_backend_opengl.c
         graphics/3d/backend/vgfx3d_egl_wayland.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_wayland.c)
elseif (UNIX AND NOT APPLE AND ZANNAGFX_BACKEND STREQUAL "LinuxAuto")
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/gui/rt_gui_linux_portal.c
         graphics/gui/rt_gui_atspi_linux.c)
    list(APPEND RT_GRAPHICS_PLATFORM_SOURCES
         graphics/3d/backend/vgfx3d_backend_opengl.c
         graphics/3d/backend/vgfx3d_egl_wayland.c)
    if (ZANNAGFX_AUTO_HAS_X11)
        list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_linux.c)
    else ()
        list(APPEND RT_GRAPHICS_PLATFORM_SOURCES graphics/gui/rt_gui_accessibility_wayland.c)
    endif ()
endif ()

set(RT_AUDIO_SOURCES
        audio/rt_audio.c
        audio/rt_audio_decode.c
        audio/rt_audio_diagnostics.c
        audio/rt_audio_fx.c
        audio/rt_playlist.c
        audio/rt_sound3d.c
        audio/rt_sound3d_objects.c
        audio/rt_soundbank.c
        audio/rt_synth.c
        audio/rt_musicgen.c
        audio/rt_ogg.c
        audio/rt_vorbis.c
        audio/rt_mp3.c
)

set(RT_THREADS_SOURCES
        threads/rt_async.c
        threads/rt_channel.c
        threads/rt_concmap.c
        threads/rt_future.c
        threads/rt_parallel.c
        threads/rt_parallel_ops.c
        threads/rt_safe_i64.c
        threads/rt_threads_common.c
        threads/rt_threadpool.c
        threads/rt_threads_primitives.cpp
        threads/rt_concqueue.c
        threads/rt_cancellation.c
        threads/rt_debounce.c
        threads/rt_scheduler.c
)

# Monitor and thread runtimes are split by platform: SRWLOCK/CONDITION_VARIABLE
# and Win32 threads on Windows, pthread mutex/condvar/threads elsewhere. The
# platform-neutral SafeThread layer (rt_threads_common.c) is compiled everywhere.
if (WIN32)
    list(APPEND RT_THREADS_SOURCES threads/rt_monitor_win.c threads/rt_threads_win.c)
else ()
    list(APPEND RT_THREADS_SOURCES threads/rt_monitor_posix.c threads/rt_threads_posix.c)
endif ()

set(RT_LOCALIZATION_SOURCES
        localization/rt_locale.c
        localization/rt_locale_data_en_us.c
        localization/rt_locale_info.c
        localization/rt_locale_manager.c
        localization/rt_locale_plural.c
        localization/rt_numformat.c
        localization/rt_plural_rules.c
        localization/rt_dateformat.c
        localization/rt_dateformat_patterns.c
        localization/rt_timezone.c
        localization/rt_reltime_format.c
        localization/rt_message_bundle.c
        localization/rt_list_format.c
        localization/rt_text_direction.c
        localization/rt_collator.c
        localization/rt_collator_table.c
)

if (WIN32)
    list(APPEND RT_LOCALIZATION_SOURCES localization/rt_locale_platform_windows.c)
elseif (APPLE)
    list(APPEND RT_LOCALIZATION_SOURCES localization/rt_locale_platform_macos.c)
else ()
    list(APPEND RT_LOCALIZATION_SOURCES localization/rt_locale_platform_posix.c)
endif ()

set(RT_NETWORK_SOURCES
        network/rt_network.c
        network/rt_network_udp.c
        network/rt_network_dns.c
        network/rt_network_http.c
        network/rt_network_http_api.c
        network/rt_http2.c
        network/rt_hpack.c
        network/rt_http_url.c
        network/rt_restclient.c
        network/rt_retry.c
        network/rt_ratelimit.c
        network/rt_websocket.c
        network/rt_ws_crypto.c
        network/rt_crypto.c
        network/rt_x25519.c
        network/rt_crypto_module.c
        network/rt_tls.c
        network/rt_tls_api.c
        network/rt_tls_certs.c
        network/rt_tls_verify_common.c
        network/rt_ecdsa_p256.c
        network/rt_rsa.c
        network/rt_http_router.c
        network/rt_http_server.c
        network/rt_https_server.c
        network/rt_connpool.c
        network/rt_multipart.c
        network/rt_netutils.c
        network/rt_ws_server.c
        network/rt_wss_server.c
        network/rt_sse.c
        network/rt_http_client.c
        network/rt_smtp.c
        network/rt_async_socket.c
)

if (WIN32)
    list(APPEND RT_NETWORK_SOURCES
            network/rt_entropy_platform_win.c
            network/rt_socket_platform_win.c)
else ()
    list(APPEND RT_NETWORK_SOURCES
            network/rt_entropy_platform_posix.c
            network/rt_socket_platform_posix.c)
endif ()

# TLS certificate verification is split by platform: CryptoAPI trust store on
# Windows and the in-tree PEM-bundle verifier on macOS/Linux. The common
# DER/SAN/hostname parsing (rt_tls_verify_common.c) is compiled everywhere.
if (WIN32)
    list(APPEND RT_NETWORK_SOURCES network/rt_tls_verify_win.c)
else ()
    list(APPEND RT_NETWORK_SOURCES network/rt_tls_verify_posix.c)
endif ()

set(RT_PUBLIC_HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_action.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_context.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_error.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_trap.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_stack_safety.h
        ${CMAKE_CURRENT_SOURCE_DIR}/rt.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_fp.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_math.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_numeric.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_random.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_string.h
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays/rt_array.h
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays/rt_array_i64.h
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays/rt_array_f64.h
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays/rt_array_str.h
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays/rt_array_obj.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_heap.h
        ${CMAKE_CURRENT_SOURCE_DIR}/collections/rt_list.h
        ${CMAKE_CURRENT_SOURCE_DIR}/oop/rt_box.h
        ${CMAKE_CURRENT_SOURCE_DIR}/oop/rt_object.h
        ${CMAKE_CURRENT_SOURCE_DIR}/oop/rt_oop.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_debug.h
        ${CMAKE_CURRENT_SOURCE_DIR}/io/rt_file.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_fmt.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_format.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_int_format.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_log.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_printf_compat.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_string_builder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_output.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_modvar.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_args.h
        ${CMAKE_CURRENT_SOURCE_DIR}/collections/rt_numbuf.h
        ${CMAKE_CURRENT_SOURCE_DIR}/system/rt_shutdown.h
        ${CMAKE_CURRENT_SOURCE_DIR}/oop/rt_sb_bridge.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/text/rt_font.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/common/rt_graphics.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/gui/rt_gui.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/gui/rt_gui_ide.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/input/rt_input.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/input/rt_inputmgr.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_grid2d.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_timer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_statemachine.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_animation_events.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_animstate.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_animtimeline.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_tween.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_buttongroup.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_smoothvalue.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_particle.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_spriteanim.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_collision.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_world_projection.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_objpool.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_screenfx.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_pathfollow.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_quadtree.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_debugoverlay.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_gameui.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_scene_editor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_pathfinder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_dialogue.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_lighting2d.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_platformer_ctrl.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_achievement.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_quests.h
        ${CMAKE_CURRENT_SOURCE_DIR}/game/rt_typewriter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/core/rt_datetime.h
        ${CMAKE_CURRENT_SOURCE_DIR}/localization/rt_timezone.h
        ${CMAKE_CURRENT_SOURCE_DIR}/collections/rt_map.h
        ${CMAKE_CURRENT_SOURCE_DIR}/collections/rt_binbuf.h
        ${CMAKE_CURRENT_SOURCE_DIR}/text/rt_parse.h
        ${CMAKE_CURRENT_SOURCE_DIR}/text/rt_fuzzy_match.h
        ${CMAKE_CURRENT_SOURCE_DIR}/io/rt_ide_primitives.h
        ${CMAKE_CURRENT_SOURCE_DIR}/io/rt_path.h
        ${CMAKE_CURRENT_SOURCE_DIR}/io/rt_dir.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_pixels.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_sprite.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_camera.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_tilemap.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_scene.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_spritebatch.h
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d/rt_texatlas.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_audio.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_sound3d.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_soundlistener3d.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_soundsource3d.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_soundbank.h
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/rt_synth.h
        ${CMAKE_CURRENT_SOURCE_DIR}/threads/rt_threads.h
        ${CMAKE_CURRENT_SOURCE_DIR}/network/rt_network.h
)

find_package(Threads REQUIRED)

add_library(zanna_rt_base_obj OBJECT ${RT_BASE_SOURCES})
add_library(zanna_rt_arrays_obj OBJECT ${RT_ARRAY_SOURCES})
add_library(zanna_rt_oop_obj OBJECT ${RT_OOP_SOURCES})
add_library(zanna_rt_collections_obj OBJECT ${RT_COLLECTIONS_SOURCES})
add_library(zanna_rt_game_obj OBJECT ${RT_GAME_SOURCES})
add_library(zanna_rt_text_obj OBJECT ${RT_TEXT_SOURCES})
add_library(zanna_rt_io_fs_obj OBJECT ${RT_IO_FS_SOURCES})
add_library(zanna_rt_exec_obj OBJECT ${RT_EXEC_SOURCES})
add_library(zanna_rt_graphics_obj OBJECT ${RT_GRAPHICS_SOURCES})
add_library(zanna_rt_audio_obj OBJECT ${RT_AUDIO_SOURCES})
add_library(zanna_rt_threads_obj OBJECT ${RT_THREADS_SOURCES})
add_library(zanna_rt_network_obj OBJECT ${RT_NETWORK_SOURCES})
add_library(zanna_rt_localization_obj OBJECT ${RT_LOCALIZATION_SOURCES})

add_library(zanna_rt_base STATIC $<TARGET_OBJECTS:zanna_rt_base_obj>)
add_library(zanna_rt_arrays STATIC $<TARGET_OBJECTS:zanna_rt_arrays_obj>)
add_library(zanna_rt_oop STATIC $<TARGET_OBJECTS:zanna_rt_oop_obj>)
add_library(zanna_rt_collections STATIC $<TARGET_OBJECTS:zanna_rt_collections_obj>)
add_library(zanna_rt_game STATIC $<TARGET_OBJECTS:zanna_rt_game_obj>)
add_library(zanna_rt_text STATIC $<TARGET_OBJECTS:zanna_rt_text_obj>)
add_library(zanna_rt_io_fs STATIC $<TARGET_OBJECTS:zanna_rt_io_fs_obj>)
add_library(zanna_rt_exec STATIC $<TARGET_OBJECTS:zanna_rt_exec_obj>)
add_library(zanna_rt_graphics STATIC $<TARGET_OBJECTS:zanna_rt_graphics_obj>)
add_library(zanna_rt_audio STATIC $<TARGET_OBJECTS:zanna_rt_audio_obj>)
add_library(zanna_rt_threads STATIC $<TARGET_OBJECTS:zanna_rt_threads_obj>)
add_library(zanna_rt_network STATIC $<TARGET_OBJECTS:zanna_rt_network_obj>)
add_library(zanna_rt_localization STATIC $<TARGET_OBJECTS:zanna_rt_localization_obj>)

add_library(zanna_runtime STATIC
        $<TARGET_OBJECTS:zanna_rt_base_obj>
        $<TARGET_OBJECTS:zanna_rt_arrays_obj>
        $<TARGET_OBJECTS:zanna_rt_oop_obj>
        $<TARGET_OBJECTS:zanna_rt_collections_obj>
        $<TARGET_OBJECTS:zanna_rt_game_obj>
        $<TARGET_OBJECTS:zanna_rt_text_obj>
        $<TARGET_OBJECTS:zanna_rt_io_fs_obj>
        $<TARGET_OBJECTS:zanna_rt_exec_obj>
        $<TARGET_OBJECTS:zanna_rt_graphics_obj>
        $<TARGET_OBJECTS:zanna_rt_audio_obj>
        $<TARGET_OBJECTS:zanna_rt_threads_obj>
        $<TARGET_OBJECTS:zanna_rt_network_obj>
        $<TARGET_OBJECTS:zanna_rt_localization_obj>
)

set(ZANNA_RUNTIME_COMPONENT_OBJECT_LIBS
        zanna_rt_base_obj
        zanna_rt_arrays_obj
        zanna_rt_oop_obj
        zanna_rt_collections_obj
        zanna_rt_game_obj
        zanna_rt_text_obj
        zanna_rt_io_fs_obj
        zanna_rt_exec_obj
        zanna_rt_graphics_obj
        zanna_rt_audio_obj
        zanna_rt_threads_obj
        zanna_rt_network_obj
        zanna_rt_localization_obj
)

set(ZANNA_RUNTIME_COMPONENT_LIBS
        zanna_rt_base
        zanna_rt_arrays
        zanna_rt_oop
        zanna_rt_collections
        zanna_rt_game
        zanna_rt_text
        zanna_rt_io_fs
        zanna_rt_exec
        zanna_rt_graphics
        zanna_rt_audio
        zanna_rt_threads
        zanna_rt_network
        zanna_rt_localization
        zanna_runtime
)

add_custom_target(zanna_runtime_components DEPENDS ${ZANNA_RUNTIME_COMPONENT_LIBS})

# Subdirectory list for include-path extension (all consumers get flat #include "rt_*.h" lookup)
set(_RT_SUBDIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/core
        ${CMAKE_CURRENT_SOURCE_DIR}/arrays
        ${CMAKE_CURRENT_SOURCE_DIR}/oop
        ${CMAKE_CURRENT_SOURCE_DIR}/collections
        ${CMAKE_CURRENT_SOURCE_DIR}/game
        ${CMAKE_CURRENT_SOURCE_DIR}/text
        ${CMAKE_CURRENT_SOURCE_DIR}/io
        ${CMAKE_CURRENT_SOURCE_DIR}/system
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/common
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/math
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/render
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/scene
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/physics
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/anim
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/nav
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/world
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/assets
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/backend
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/gui
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/input
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/media
        ${CMAKE_CURRENT_SOURCE_DIR}/graphics/text
        ${CMAKE_CURRENT_SOURCE_DIR}/audio
        ${CMAKE_CURRENT_SOURCE_DIR}/threads
        ${CMAKE_CURRENT_SOURCE_DIR}/network
        ${CMAKE_CURRENT_SOURCE_DIR}/localization
)

foreach (_rtobj IN LISTS ZANNA_RUNTIME_COMPONENT_OBJECT_LIBS)
    target_include_directories(${_rtobj} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}    # root: rt_internal.h, rt_platform.h
            ${_RT_SUBDIRS}
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src
    )
    if (MSVC)
        # Function-level linking (enables dead code elimination when supported by the linker)
        target_compile_options(${_rtobj} PRIVATE /Gy /W4 /wd4100 /wd4201)
    else ()
        target_compile_options(${_rtobj} PRIVATE
                -ffunction-sections -fdata-sections
                -Wall -Wextra -Wpedantic
                -Wshadow -Wnull-dereference -Wcast-qual
                -Wimplicit-fallthrough -Wformat=2
                -Wno-unused-parameter)
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
                CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            target_compile_options(${_rtobj} PRIVATE -mno-outline-atomics)
        endif ()
    endif ()
    if (IL_WARN_AS_ERROR AND NOT MSVC)
        target_compile_options(${_rtobj} PRIVATE -Werror)
    endif ()
    if (NOT WIN32)
        target_compile_definitions(${_rtobj} PRIVATE _POSIX_C_SOURCE=200809L)
        if (NOT APPLE)
            # _DEFAULT_SOURCE is needed on Linux (glibc) to expose SIGSTKSZ,
            # sigaltstack, SA_ONSTACK, etc. when _POSIX_C_SOURCE is set.
            target_compile_definitions(${_rtobj} PRIVATE _DEFAULT_SOURCE)
        endif ()
    endif ()
endforeach ()

foreach (_rtlib IN LISTS ZANNA_RUNTIME_COMPONENT_LIBS)
    target_include_directories(${_rtlib}
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/arrays>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/oop>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/collections>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/game>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/text>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/io>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/system>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/common>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/math>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/2d>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/render>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/scene>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/physics>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/anim>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/nav>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/world>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/assets>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/3d/backend>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/gui>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/input>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/media>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/graphics/text>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/audio>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/threads>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/network>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/localization>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna_runtime>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna/runtime>
    )
endforeach ()

target_link_libraries(zanna_rt_arrays PUBLIC zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_oop PUBLIC zanna_rt_base)
target_link_libraries(zanna_rt_collections PUBLIC zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_game PUBLIC zanna_rt_collections zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_text PUBLIC zanna_rt_collections zanna_rt_base)
target_link_libraries(zanna_rt_io_fs PUBLIC zanna_rt_collections zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_exec PUBLIC zanna_rt_collections zanna_rt_base)
target_link_libraries(zanna_rt_graphics PUBLIC zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_threads PUBLIC zanna_rt_oop zanna_rt_base)
target_link_libraries(zanna_rt_network PUBLIC zanna_rt_collections zanna_rt_base)
target_link_libraries(zanna_rt_localization PUBLIC zanna_rt_collections zanna_rt_oop zanna_rt_threads zanna_rt_base)

# Windows requires ws2_32 for sockets
if (WIN32)
    target_link_libraries(zanna_rt_base PUBLIC shell32 advapi32)
    target_link_libraries(zanna_runtime PUBLIC shell32 advapi32)
    target_link_libraries(zanna_rt_network PUBLIC ws2_32)
    target_link_libraries(zanna_runtime PUBLIC ws2_32)
    target_link_libraries(zanna_rt_graphics PUBLIC Xinput9_1_0)
    target_link_libraries(zanna_runtime PUBLIC Xinput9_1_0)
    # UI Automation bridge: BSTR/VARIANT/SAFEARRAY helpers live in oleaut32;
    # uiautomationcore.dll itself is resolved dynamically at runtime.
    target_link_libraries(zanna_rt_graphics PUBLIC oleaut32)
    target_link_libraries(zanna_runtime PUBLIC oleaut32)
endif ()

if (NOT WIN32)
    # -lm is an implementation detail of the runtime; PRIVATE prevents it from
    # leaking into consumers' link lines (MED-7).
    target_link_libraries(zanna_runtime PRIVATE m)
    target_link_libraries(zanna_rt_base PRIVATE m)
endif ()

# MSVC release linker flags: /OPT:REF removes unreferenced functions/data,
# /OPT:ICF merges identical COMDAT sections — equivalent to GCC/Clang
# --gc-sections + --icf=all (MED-6).
if (MSVC)
    target_link_options(zanna_runtime PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
endif ()

if (APPLE)
    target_link_libraries(zanna_rt_graphics PUBLIC "-framework IOKit" "-framework CoreFoundation")
    target_link_libraries(zanna_runtime PUBLIC "-framework IOKit" "-framework CoreFoundation")
    target_link_libraries(zanna_rt_graphics PUBLIC "-framework ApplicationServices")
    target_link_libraries(zanna_runtime PUBLIC "-framework ApplicationServices")
    # Metal frameworks for 3D GPU backend
    target_link_libraries(zanna_rt_graphics PUBLIC "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
    target_link_libraries(zanna_runtime PUBLIC "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
endif ()

target_link_libraries(zanna_rt_threads PUBLIC Threads::Threads)
target_link_libraries(zanna_runtime PUBLIC Threads::Threads)

# ZANNA_RC_DEBUG can be manually enabled for refcount debugging:
# cmake -DCMAKE_CXX_FLAGS="-DZANNA_RC_DEBUG=1" ...

install(FILES ${RT_PUBLIC_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna_runtime
)

install(FILES ${RT_PUBLIC_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/runtime
)

install(FILES ${PROJECT_SOURCE_DIR}/include/zanna/runtime/rt.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/runtime
)

set(ZANNA_RUNTIME_COMPONENT_ARCHIVES
        zanna_rt_base
        zanna_rt_arrays
        zanna_rt_oop
        zanna_rt_collections
        zanna_rt_game
        zanna_rt_text
        zanna_rt_io_fs
        zanna_rt_exec
        zanna_rt_threads
        zanna_rt_graphics
        zanna_rt_audio
        zanna_rt_network
        zanna_rt_localization
        PARENT_SCOPE)

if (NOT DEFINED ZANNA_GRAPHICS_MODE)
    message(FATAL_ERROR "ZANNA_GRAPHICS_MODE must be defined before configuring src/runtime")
endif ()
if (NOT DEFINED ZANNA_AUDIO_MODE)
    message(FATAL_ERROR "ZANNA_AUDIO_MODE must be defined before configuring src/runtime")
endif ()

if (ZANNA_GRAPHICS_MODE STREQUAL "OFF")
    set(ZANNA_ENABLE_GRAPHICS OFF)
else ()
    set(ZANNA_ENABLE_GRAPHICS ON)
endif ()

if (ZANNA_AUDIO_MODE STREQUAL "OFF")
    set(ZANNA_ENABLE_AUDIO OFF)
else ()
    set(ZANNA_ENABLE_AUDIO ON)
endif ()

if (ZANNA_ENABLE_GRAPHICS AND TARGET zannagfx)
    message(STATUS "zanna_runtime: Graphics support enabled")
    target_sources(zanna_rt_graphics_obj PRIVATE ${RT_GRAPHICS_PLATFORM_SOURCES})
    target_compile_definitions(zanna_rt_graphics_obj PRIVATE ZANNA_ENABLE_GRAPHICS)
    target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_ENABLE_GRAPHICS)
    if (ZANNAGFX_BACKEND STREQUAL "Headless")
        target_compile_definitions(zanna_rt_graphics_obj PRIVATE ZANNA_GRAPHICS_HEADLESS)
        target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_GRAPHICS_HEADLESS)
    elseif (ZANNAGFX_BACKEND STREQUAL "Wayland")
        target_compile_definitions(zanna_rt_graphics_obj PRIVATE ZANNA_GRAPHICS_WAYLAND)
        target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_GRAPHICS_WAYLAND)
    elseif (ZANNAGFX_BACKEND STREQUAL "LinuxAuto")
        if (ZANNAGFX_AUTO_HAS_X11)
            target_compile_definitions(zanna_rt_graphics_obj PRIVATE ZANNA_GRAPHICS_LINUX_AUTO)
            target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_GRAPHICS_LINUX_AUTO)
        else ()
            # A Wayland-only AUTO build must not require X11 headers in the OpenGL adapter.
            target_compile_definitions(zanna_rt_graphics_obj PRIVATE ZANNA_GRAPHICS_WAYLAND)
            target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_GRAPHICS_WAYLAND)
        endif ()
    endif ()
    if (ZANNA_BUILD_TESTING)
        target_compile_definitions(zanna_rt_graphics_obj PRIVATE RT_G3D_ALLOW_STACK_FIXTURES=1)
    endif ()
    target_include_directories(zanna_rt_graphics_obj PRIVATE
            ${PROJECT_SOURCE_DIR}/src/lib/graphics/include
            ${PROJECT_SOURCE_DIR}/src/lib/gui/include
    )
    target_link_libraries(zanna_rt_graphics PUBLIC zannagfx)
    target_link_libraries(zanna_runtime PUBLIC zannagfx)
    # Link GUI library if available
    if (TARGET zannagui)
        target_link_libraries(zanna_rt_graphics PRIVATE zannagui)
        target_link_libraries(zanna_runtime PRIVATE zannagui)
    endif ()
else ()
    message(STATUS "zanna_runtime: Graphics support disabled (zannagfx not available)")
    # Replace backend-dependent sources with the no-backend runtime surface.
    set_property(TARGET zanna_rt_graphics_obj PROPERTY SOURCES ${RT_GRAPHICS_DISABLED_SOURCES})
endif ()

if (ZANNA_ENABLE_AUDIO AND TARGET zannaaud)
    message(STATUS "zanna_runtime: Audio support enabled")
    target_compile_definitions(zanna_rt_audio_obj PRIVATE ZANNA_ENABLE_AUDIO)
    target_include_directories(zanna_rt_audio_obj PRIVATE
            ${PROJECT_SOURCE_DIR}/src/lib/audio/include
    )
    target_link_libraries(zanna_rt_audio PUBLIC zannaaud)
    target_link_libraries(zanna_runtime PUBLIC zannaaud)
else ()
    message(STATUS "zanna_runtime: Audio support disabled (zannaaud not available)")
endif ()
