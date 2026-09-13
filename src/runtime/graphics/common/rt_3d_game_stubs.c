//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/common/rt_3d_game_stubs.c
// Purpose: Graphics-disabled Game3D entry points whose implementations compile
//          only with graphics: AI (BehaviorTree3D, Perception3D), interaction,
//          footsteps and surfaces, minimaps, entity persistence, world
//          streaming events, and the internal world-step hooks they install.
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds; the matching
//     rt_game3d_*.c sources contribute nothing there.
//   - Constructors trap through rt_graphics_unavailable_(). World3D.New already
//     traps without graphics, so component handles can never exist and every
//     other member returns a documented fallback (ZANNA_GRAPHICS_STUBS_STRICT
//     makes those trap too).
//   - Fluent members return their receiver unchanged.
//   - The internal world-step hooks are unreachable no-ops that only satisfy
//     references from the always-compiled rt_game3d.c.
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles;
//     string fallbacks are new empty runtime strings owned by the caller.
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//        src/runtime/graphics/3d/rt_game3d.h,
//        src/runtime/graphics/3d/rt_game3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Graphics-disabled Game3D AI, interaction, minimap, persistence, and streaming stubs.
/// @details The owning Game3D sources are wrapped in `ZANNA_ENABLE_GRAPHICS`, so these
///          definitions keep the registered runtime surface linkable without graphics.

#include "rt_graphics_stubs_internal.h"

#include "rt_game3d.h"
#include "rt_game3d_internal.h"

/* Surfaces stubs */

/// @brief Trapping stub for `Surfaces.Register` (graphics-disabled build).
/// @param name Name (ignored before trapping).
/// @return `0` after raising the graphics-unavailable trap.
int64_t rt_game3d_surfaces_register(rt_string name) {
    (void)name;
    RT_GRAPHICS_TRAP_RET("Surfaces.Register: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Surfaces.NameOf` (graphics-disabled build).
/// @param id Identifier (ignored).
/// @return An empty runtime string.
rt_string rt_game3d_surfaces_name_of(int64_t id) {
    (void)id;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Surfaces.NameOf: graphics support not compiled in",
                                  rt_const_cstr(""));
}

/// @brief Silent fallback stub for `Surfaces.IdOf` (graphics-disabled build).
/// @param name Name (ignored).
/// @return `0`.
int64_t rt_game3d_surfaces_id_of(rt_string name) {
    (void)name;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Surfaces.IdOf: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Surfaces.get_Count` (graphics-disabled build).
/// @return `0`.
int64_t rt_game3d_surfaces_count(void) {
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Surfaces.get_Count: graphics support not compiled in", 0);
}

/* SurfaceTable3D stubs */

/// @brief Trapping stub for `SurfaceTable3D.New` (graphics-disabled build).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_surface_table_new(void) {
    RT_GRAPHICS_TRAP_RET("SurfaceTable3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `SurfaceTable3D.AddClip` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param table SurfaceTable3D handle (ignored).
/// @param surface_id Surface identifier (ignored).
/// @param clip Clip (ignored).
/// @return @p table unchanged.
void *rt_game3d_surface_table_add_clip(void *table, int64_t surface_id, void *clip) {
    (void)surface_id;
    (void)clip;
    return table;
}

/// @brief Silent no-op stub for `SurfaceTable3D.SetLoudness` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param table SurfaceTable3D handle (ignored).
/// @param surface_id Surface identifier (ignored).
/// @param loudness Loudness (ignored).
/// @return @p table unchanged.
void *rt_game3d_surface_table_set_loudness(void *table, int64_t surface_id, double loudness) {
    (void)surface_id;
    (void)loudness;
    return table;
}

/// @brief Silent fallback stub for `SurfaceTable3D.ClipCount` (graphics-disabled build).
/// @param table SurfaceTable3D handle (ignored).
/// @param surface_id Surface identifier (ignored).
/// @return `0`.
int64_t rt_game3d_surface_table_clip_count(void *table, int64_t surface_id) {
    (void)table;
    (void)surface_id;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("SurfaceTable3D.ClipCount: graphics support not compiled in", 0);
}

/* Footsteps3D stubs */

/// @brief Trapping stub for `Footsteps3D.New` (graphics-disabled build).
/// @param entity Entity3D instance used by the operation (ignored before trapping).
/// @param table Table (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_footsteps_new(void *entity, void *table) {
    (void)entity;
    (void)table;
    RT_GRAPHICS_TRAP_RET("Footsteps3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Footsteps3D.SetEventPrefix` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param steps Footsteps3D handle (ignored).
/// @param prefix Prefix (ignored).
/// @return @p steps unchanged.
void *rt_game3d_footsteps_set_event_prefix(void *steps, rt_string prefix) {
    (void)prefix;
    return steps;
}

/// @brief Silent no-op stub for `Footsteps3D.SetGroundMask` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param steps Footsteps3D handle (ignored).
/// @param mask Mask (ignored).
/// @return @p steps unchanged.
void *rt_game3d_footsteps_set_ground_mask(void *steps, int64_t mask) {
    (void)mask;
    return steps;
}

/// @brief Silent no-op stub for `Footsteps3D.SetVolumeScale` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param steps Footsteps3D handle (ignored).
/// @param scale Scale (ignored).
/// @return @p steps unchanged.
void *rt_game3d_footsteps_set_volume_scale(void *steps, double scale) {
    (void)scale;
    return steps;
}

/// @brief Silent fallback stub for `Footsteps3D.get_StepCount` (graphics-disabled build).
/// @param steps Footsteps3D handle (ignored).
/// @return `0`.
int64_t rt_game3d_footsteps_get_step_count(void *steps) {
    (void)steps;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Footsteps3D.get_StepCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Footsteps3D.get_LastSurface` (graphics-disabled build).
/// @param steps Footsteps3D handle (ignored).
/// @return `0`.
int64_t rt_game3d_footsteps_get_last_surface(void *steps) {
    (void)steps;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Footsteps3D.get_LastSurface: graphics support not compiled in",
                                  0);
}

/* Interactable3D stubs */

/// @brief Trapping stub for `Interactable3D.New` (graphics-disabled build).
/// @param entity Entity3D that will own the component (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_interactable_new(void *entity) {
    (void)entity;
    RT_GRAPHICS_TRAP_RET("Interactable3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Interactable3D.WithPrompt` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param item Interactable3D component to configure (ignored).
/// @param prompt Runtime string containing the new prompt (ignored).
/// @return @p item unchanged.
void *rt_game3d_interactable_with_prompt(void *item, rt_string prompt) {
    (void)prompt;
    return item;
}

/// @brief Silent fallback stub for `Interactable3D.get_Prompt` (graphics-disabled build).
/// @param item Interactable3D component to query (ignored).
/// @return An empty runtime string.
rt_string rt_game3d_interactable_get_prompt(void *item) {
    (void)item;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactable3D.get_Prompt: graphics support not compiled in",
                                  rt_const_cstr(""));
}

/// @brief Silent no-op stub for `Interactable3D.WithKind` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param item Interactable3D component to configure (ignored).
/// @param kind Application-defined kind value reported with the interaction (ignored).
/// @return @p item unchanged.
void *rt_game3d_interactable_with_kind(void *item, int64_t kind) {
    (void)kind;
    return item;
}

/// @brief Silent fallback stub for `Interactable3D.get_Kind` (graphics-disabled build).
/// @param item Interactable3D component to query (ignored).
/// @return `0`.
int64_t rt_game3d_interactable_get_kind(void *item) {
    (void)item;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactable3D.get_Kind: graphics support not compiled in", 0);
}

/// @brief Silent no-op stub for `Interactable3D.WithRadius` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param item Interactable3D component to configure (ignored).
/// @param radius Focus radius in world units (ignored).
/// @return @p item unchanged.
void *rt_game3d_interactable_with_radius(void *item, double radius) {
    (void)radius;
    return item;
}

/// @brief Silent fallback stub for `Interactable3D.get_Radius` (graphics-disabled build).
/// @param item Interactable3D component to query (ignored).
/// @return `0.0`.
double rt_game3d_interactable_get_radius(void *item) {
    (void)item;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactable3D.get_Radius: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent no-op stub for `Interactable3D.set_IsEnabled` (graphics-disabled build).
/// @param item Interactable3D component to configure (ignored).
/// @param enabled Non-zero to enable interaction (ignored).
void rt_game3d_interactable_set_enabled(void *item, int8_t enabled) {
    (void)item;
    (void)enabled;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Interactable3D.set_IsEnabled: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Interactable3D.get_IsEnabled` (graphics-disabled build).
/// @param item Interactable3D component to query (ignored).
/// @return `0`.
int8_t rt_game3d_interactable_get_enabled(void *item) {
    (void)item;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactable3D.get_IsEnabled: graphics support not compiled in",
                                  0);
}

/// @brief Silent no-op stub for `Interactable3D.set_FocusPriority` (graphics-disabled build).
/// @param item Interactable3D component to configure (ignored).
/// @param priority Finite focus-priority value, bounded to the runtime coordinate limit (ignored).
void rt_game3d_interactable_set_focus_priority(void *item, double priority) {
    (void)item;
    (void)priority;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Interactable3D.set_FocusPriority: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Interactable3D.get_FocusPriority` (graphics-disabled build).
/// @param item Interactable3D component to query (ignored).
/// @return `0.0`.
double rt_game3d_interactable_get_focus_priority(void *item) {
    (void)item;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Interactable3D.get_FocusPriority: graphics support not compiled in", 0.0);
}

/* Interactor3D stubs */

/// @brief Trapping stub for `Interactor3D.New` (graphics-disabled build).
/// @param entity Entity3D that will own the scanner (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_interactor_new(void *entity) {
    (void)entity;
    RT_GRAPHICS_TRAP_RET("Interactor3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Interactor3D.set_ConeDegrees` (graphics-disabled build).
/// @param scanner Interactor3D component to configure (ignored).
/// @param degrees Full cone angle in degrees (ignored).
void rt_game3d_interactor_set_cone_degrees(void *scanner, double degrees) {
    (void)scanner;
    (void)degrees;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Interactor3D.set_ConeDegrees: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Interactor3D.get_ConeDegrees` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `0.0`.
double rt_game3d_interactor_get_cone_degrees(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactor3D.get_ConeDegrees: graphics support not compiled in",
                                  0.0);
}

/// @brief Silent no-op stub for `Interactor3D.set_RequireLineOfSight` (graphics-disabled build).
/// @param scanner Interactor3D component to configure (ignored).
/// @param required Non-zero to require a line-of-sight test (ignored).
void rt_game3d_interactor_set_require_los(void *scanner, int8_t required) {
    (void)scanner;
    (void)required;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Interactor3D.set_RequireLineOfSight: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Interactor3D.get_RequireLineOfSight` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `0`.
int8_t rt_game3d_interactor_get_require_los(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Interactor3D.get_RequireLineOfSight: graphics support not compiled in", 0);
}

/// @brief Silent no-op stub for `Interactor3D.set_LosMask` (graphics-disabled build).
/// @param scanner Interactor3D component to configure (ignored).
/// @param mask Bit mask of layers that can obstruct interaction focus (ignored).
void rt_game3d_interactor_set_los_mask(void *scanner, int64_t mask) {
    (void)scanner;
    (void)mask;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Interactor3D.set_LosMask: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Interactor3D.get_LosMask` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `0`.
int64_t rt_game3d_interactor_get_los_mask(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactor3D.get_LosMask: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Interactor3D.get_Focused` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `NULL`.
void *rt_game3d_interactor_get_focused(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactor3D.get_Focused: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Interactor3D.FocusChanged` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `0`.
int8_t rt_game3d_interactor_focus_changed(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactor3D.FocusChanged: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Interactor3D.Interact` (graphics-disabled build).
/// @param scanner Interactor3D component issuing the interaction (ignored).
/// @return `0`.
int8_t rt_game3d_interactor_interact(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Interactor3D.Interact: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Interactor3D.get_InteractCount` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `0`.
int64_t rt_game3d_interactor_get_interact_count(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Interactor3D.get_InteractCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Interactor3D.get_LastInteracted` (graphics-disabled build).
/// @param scanner Interactor3D component to query (ignored).
/// @return `NULL`.
void *rt_game3d_interactor_get_last_interacted(void *scanner) {
    (void)scanner;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Interactor3D.get_LastInteracted: graphics support not compiled in", NULL);
}

/* Perception3D stubs */

/// @brief Trapping stub for `Perception3D.New` (graphics-disabled build).
/// @param entity Entity3D that will own the component (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_perception_new(void *entity) {
    (void)entity;
    RT_GRAPHICS_TRAP_RET("Perception3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Perception3D.SetSight` (graphics-disabled build).
/// @param sense Perception3D component to configure (ignored).
/// @param range Maximum sight distance in world units (ignored).
/// @param fov_degrees Full field-of-view angle (ignored).
/// @param eye_height Finite vertical sight-origin offset, bounded to the coordinate limit
/// (ignored).
void rt_game3d_perception_set_sight(void *sense,
                                    double range,
                                    double fov_degrees,
                                    double eye_height) {
    (void)sense;
    (void)range;
    (void)fov_degrees;
    (void)eye_height;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Perception3D.SetSight: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Perception3D.SetHearing` (graphics-disabled build).
/// @param sense Perception3D component to configure (ignored).
/// @param range_at_loudness1 Non-negative base range, clamped to 512 world units (ignored).
void rt_game3d_perception_set_hearing(void *sense, double range_at_loudness1) {
    (void)sense;
    (void)range_at_loudness1;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Perception3D.SetHearing: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Perception3D.SetTargetMask` (graphics-disabled build).
/// @param sense Perception3D component to configure (ignored).
/// @param mask Bit mask of target entity layers (ignored).
void rt_game3d_perception_set_target_mask(void *sense, int64_t mask) {
    (void)sense;
    (void)mask;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Perception3D.SetTargetMask: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Perception3D.SetLosMask` (graphics-disabled build).
/// @param sense Perception3D component to configure (ignored).
/// @param mask Bit mask of line-of-sight obstruction layers (ignored).
void rt_game3d_perception_set_los_mask(void *sense, int64_t mask) {
    (void)sense;
    (void)mask;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Perception3D.SetLosMask: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Perception3D.SeenCount` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @return `0`.
int64_t rt_game3d_perception_seen_count(void *sense) {
    (void)sense;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.SeenCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Perception3D.SeenTarget` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @param index Zero-based index in the currently visible target list (ignored).
/// @return `NULL`.
void *rt_game3d_perception_seen_target(void *sense, int64_t index) {
    (void)sense;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.SeenTarget: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Perception3D.LastKnownPosition` (graphics-disabled build).
/// @param sense Perception3D component holding the target track (ignored).
/// @param target Entity3D whose track should be queried (ignored).
/// @return `NULL`.
void *rt_game3d_perception_last_known_position(void *sense, void *target) {
    (void)sense;
    (void)target;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "Perception3D.LastKnownPosition: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `Perception3D.SeenChanged` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @return `0`.
int8_t rt_game3d_perception_seen_changed(void *sense) {
    (void)sense;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.SeenChanged: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Perception3D.HeardCount` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @return `0`.
int64_t rt_game3d_perception_heard_count(void *sense) {
    (void)sense;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.HeardCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Perception3D.HeardPosition` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @param index Zero-based heard-event index (ignored).
/// @return `NULL`.
void *rt_game3d_perception_heard_position(void *sense, int64_t index) {
    (void)sense;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.HeardPosition: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `Perception3D.HeardTag` (graphics-disabled build).
/// @param sense Perception3D component to query (ignored).
/// @param index Zero-based heard-event index (ignored).
/// @return `0`.
int64_t rt_game3d_perception_heard_tag(void *sense, int64_t index) {
    (void)sense;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Perception3D.HeardTag: graphics support not compiled in", 0);
}

/* World3D stubs */

/// @brief Silent no-op stub for `World3D.ReportSound` (graphics-disabled build).
/// @param world World3D whose active perceivers receive the stimulus (ignored).
/// @param position Vec3 world-space origin of the sound (ignored).
/// @param loudness Positive finite loudness multiplier (ignored).
/// @param tag Application-defined value stored with each delivered event (ignored).
void rt_game3d_world_report_sound(void *world, void *position, double loudness, int64_t tag) {
    (void)world;
    (void)position;
    (void)loudness;
    (void)tag;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("World3D.ReportSound: graphics support not compiled in");
}

/// @brief Silent fallback stub for `World3D.GetPersistentAlive` (graphics-disabled build).
/// @param world World3D containing the persistence store (ignored).
/// @param key Stable persistence key to query (ignored).
/// @return `0`.
int8_t rt_game3d_world_get_persistent_alive(void *world, rt_string key) {
    (void)world;
    (void)key;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("World3D.GetPersistentAlive: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `World3D.GetPersistentPosition` (graphics-disabled build).
/// @param world World3D containing the persistence store (ignored).
/// @param key Stable persistence key to query (ignored).
/// @return `NULL`.
void *rt_game3d_world_get_persistent_position(void *world, rt_string key) {
    (void)world;
    (void)key;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("World3D.GetPersistentPosition: graphics support not compiled in",
                                  NULL);
}

/// @brief Silent fallback stub for `World3D.SaveState` (graphics-disabled build).
/// @param world World3D whose persistence store should be saved (ignored).
/// @param app_name Application name used to locate the platform data directory (ignored).
/// @param slot Save-slot name used as the `.vw3dsav` file stem (ignored).
/// @return `0`.
int8_t rt_game3d_world_save_state(void *world, rt_string app_name, rt_string slot) {
    (void)world;
    (void)app_name;
    (void)slot;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("World3D.SaveState: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `World3D.LoadState` (graphics-disabled build).
/// @param world World3D that will receive the loaded state (ignored).
/// @param app_name Application name used to locate the platform data directory (ignored).
/// @param slot Save-slot name used as the `.vw3dsav` file stem (ignored).
/// @return `0`.
int8_t rt_game3d_world_load_state(void *world, rt_string app_name, rt_string slot) {
    (void)world;
    (void)app_name;
    (void)slot;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("World3D.LoadState: graphics support not compiled in", 0);
}

/* BehaviorTree3D stubs */

/// @brief Trapping stub for `BehaviorTree3D.New` (graphics-disabled build).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_btree_new(void) {
    RT_GRAPHICS_TRAP_RET("BehaviorTree3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent fallback stub for `BehaviorTree3D.Sequence` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @return `0`.
int64_t rt_game3d_btree_sequence(void *tree) {
    (void)tree;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.Sequence: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.Selector` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @return `0`.
int64_t rt_game3d_btree_selector(void *tree) {
    (void)tree;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.Selector: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.Inverter` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @return `0`.
int64_t rt_game3d_btree_inverter(void *tree) {
    (void)tree;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.Inverter: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.TargetVisible` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @return `0`.
int64_t rt_game3d_btree_can_see(void *tree) {
    (void)tree;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.TargetVisible: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.Wait` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @param seconds Non-negative duration before the leaf succeeds (ignored).
/// @return `0`.
int64_t rt_game3d_btree_wait(void *tree, double seconds) {
    (void)tree;
    (void)seconds;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.Wait: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.MoveToTarget` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @param speed Movement speed in world units per second (ignored).
/// @param arrive_distance Distance at which the leaf succeeds (ignored).
/// @return `0`.
int64_t rt_game3d_btree_move_to_target(void *tree, double speed, double arrive_distance) {
    (void)tree;
    (void)speed;
    (void)arrive_distance;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.MoveToTarget: graphics support not compiled in",
                                  0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.MoveToLastKnown` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @param speed Movement speed in world units per second (ignored).
/// @param arrive_distance Distance at which the leaf succeeds (ignored).
/// @return `0`.
int64_t rt_game3d_btree_move_to_last_known(void *tree, double speed, double arrive_distance) {
    (void)tree;
    (void)speed;
    (void)arrive_distance;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "BehaviorTree3D.MoveToLastKnown: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `BehaviorTree3D.Custom` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to extend (ignored).
/// @param id Nonzero application-defined identifier exposed while the leaf is pending (ignored).
/// @return `0`.
int64_t rt_game3d_btree_custom(void *tree, int64_t id) {
    (void)tree;
    (void)id;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("BehaviorTree3D.Custom: graphics support not compiled in", 0);
}

/// @brief Silent no-op stub for `BehaviorTree3D.AddChild` (graphics-disabled build).
/// @param tree BehaviorTree3D containing both nodes (ignored).
/// @param parent Index of the node that will own the child (ignored).
/// @param child Index of the node to append (ignored).
void rt_game3d_btree_add_child(void *tree, int64_t parent, int64_t child) {
    (void)tree;
    (void)parent;
    (void)child;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("BehaviorTree3D.AddChild: graphics support not compiled in");
}

/// @brief Silent no-op stub for `BehaviorTree3D.SetRoot` (graphics-disabled build).
/// @param tree BehaviorTree3D definition to configure (ignored).
/// @param node Valid node index to use as the root (ignored).
void rt_game3d_btree_set_root(void *tree, int64_t node) {
    (void)tree;
    (void)node;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("BehaviorTree3D.SetRoot: graphics support not compiled in");
}

/* BehaviorTreeInstance3D stubs */

/// @brief Trapping stub for `BehaviorTreeInstance3D.New` (graphics-disabled build).
/// @param entity Entity3D that will own and tick the instance (ignored before trapping).
/// @param tree Rooted BehaviorTree3D definition retained by the instance (ignored before trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_bt_instance_new(void *entity, void *tree) {
    (void)entity;
    (void)tree;
    RT_GRAPHICS_TRAP_RET("BehaviorTreeInstance3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `BehaviorTreeInstance3D.SetTarget` (graphics-disabled build).
/// @param instance BehaviorTreeInstance3D to configure (ignored).
/// @param target_entity Entity3D to target, or NULL to clear the current target (ignored).
void rt_game3d_bt_instance_set_target(void *instance, void *target_entity) {
    (void)instance;
    (void)target_entity;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "BehaviorTreeInstance3D.SetTarget: graphics support not compiled in");
}

/// @brief Silent fallback stub for `BehaviorTreeInstance3D.get_PendingCustom` (graphics-disabled
/// build).
/// @param instance BehaviorTreeInstance3D to query (ignored).
/// @return `0`.
int64_t rt_game3d_bt_instance_pending_custom(void *instance) {
    (void)instance;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "BehaviorTreeInstance3D.get_PendingCustom: graphics support not compiled in", 0);
}

/// @brief Silent no-op stub for `BehaviorTreeInstance3D.Resolve` (graphics-disabled build).
/// @param instance BehaviorTreeInstance3D with a pending custom leaf (ignored).
/// @param success Non-zero to resolve the leaf as success (ignored).
void rt_game3d_bt_instance_resolve(void *instance, int8_t success) {
    (void)instance;
    (void)success;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "BehaviorTreeInstance3D.Resolve: graphics support not compiled in");
}

/* Entity3D stubs */

/// @brief Silent no-op stub for `Entity3D.SetPersistent` (graphics-disabled build).
/// @details Returns the receiver unchanged so fluent chains type-check.
/// @param entity Entity3D whose pose, alive state, and state tag should persist (ignored).
/// @param key Non-empty key of at most 255 bytes with no embedded NUL, stable across sessions
/// (ignored).
/// @return @p entity unchanged.
void *rt_game3d_entity_set_persistent(void *entity, rt_string key) {
    (void)key;
    return entity;
}

/// @brief Silent fallback stub for `Entity3D.get_PersistentKey` (graphics-disabled build).
/// @param entity Entity3D to query (ignored).
/// @return An empty runtime string.
rt_string rt_game3d_entity_get_persistent_key(void *entity) {
    (void)entity;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Entity3D.get_PersistentKey: graphics support not compiled in",
                                  rt_const_cstr(""));
}

/// @brief Silent no-op stub for `Entity3D.set_StateTag` (graphics-disabled build).
/// @param entity Entity3D whose persisted record should carry the tag (ignored).
/// @param tag Free-form application state value (ignored).
void rt_game3d_entity_set_state_tag(void *entity, int64_t tag) {
    (void)entity;
    (void)tag;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Entity3D.set_StateTag: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Entity3D.get_StateTag` (graphics-disabled build).
/// @param entity Entity3D to query (ignored).
/// @return `0`.
int64_t rt_game3d_entity_get_state_tag(void *entity) {
    (void)entity;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Entity3D.get_StateTag: graphics support not compiled in", 0);
}

/* WorldStream3D stubs */

/// @brief Silent no-op stub for `WorldStream3D.SetCellFlag` (graphics-disabled build).
/// @param stream WorldStream3D whose persistent cell flags are updated (ignored).
/// @param cell Non-empty stable cell name no longer than 255 bytes and containing no embedded NUL
/// (ignored).
/// @param key Non-empty flag name within the cell no longer than 255 bytes and containing no NUL
/// (ignored).
/// @param value Integer value to store (ignored).
void rt_game3d_world_stream_set_cell_flag(void *stream,
                                          rt_string cell,
                                          rt_string key,
                                          int64_t value) {
    (void)stream;
    (void)cell;
    (void)key;
    (void)value;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("WorldStream3D.SetCellFlag: graphics support not compiled in");
}

/// @brief Silent fallback stub for `WorldStream3D.GetCellFlag` (graphics-disabled build).
/// @param stream WorldStream3D containing the persistent cell flags (ignored).
/// @param cell Stable cell name (ignored).
/// @param key Flag name within the cell (ignored).
/// @return `0`.
int64_t rt_game3d_world_stream_get_cell_flag(void *stream, rt_string cell, rt_string key) {
    (void)stream;
    (void)cell;
    (void)key;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("WorldStream3D.GetCellFlag: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `WorldStream3D.LoadedCellEventCount` (graphics-disabled build).
/// @param stream WorldStream3D to query (ignored).
/// @return `0`.
int64_t rt_game3d_world_stream_loaded_event_count(void *stream) {
    (void)stream;
    RT_GRAPHICS_OPTIONAL_TRAP_RET(
        "WorldStream3D.LoadedCellEventCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `WorldStream3D.LoadedCellEvent` (graphics-disabled build).
/// @param stream WorldStream3D to query (ignored).
/// @param index Zero-based buffered-event index (ignored).
/// @return An empty runtime string.
rt_string rt_game3d_world_stream_loaded_event(void *stream, int64_t index) {
    (void)stream;
    (void)index;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("WorldStream3D.LoadedCellEvent: graphics support not compiled in",
                                  rt_const_cstr(""));
}

/// @brief Silent no-op stub for `WorldStream3D.ClearLoadedCellEvents` (graphics-disabled build).
/// @param stream WorldStream3D whose notification buffer should be emptied (ignored).
void rt_game3d_world_stream_clear_loaded_events(void *stream) {
    (void)stream;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "WorldStream3D.ClearLoadedCellEvents: graphics support not compiled in");
}

/* Minimap3D stubs */

/// @brief Trapping stub for `Minimap3D.New` (graphics-disabled build).
/// @param world World3D providing the canvas, camera, and entity state (ignored before trapping).
/// @param size_px Initial width and height in pixels, from 32 through 2048 (ignored before
/// trapping).
/// @return `NULL` after raising the graphics-unavailable trap.
void *rt_game3d_minimap_new(void *world, int64_t size_px) {
    (void)world;
    (void)size_px;
    RT_GRAPHICS_TRAP_RET("Minimap3D.New: graphics support not compiled in", NULL);
}

/// @brief Silent no-op stub for `Minimap3D.SetMapImage` (graphics-disabled build).
/// @param minimap Minimap3D to configure (ignored).
/// @param pixels Pixels resource drawn as the map backdrop, or NULL for the fallback panel
/// (ignored).
/// @param min_x Minimum world X coordinate represented by the image (ignored).
/// @param min_z Minimum world Z coordinate represented by the image (ignored).
/// @param max_x Maximum world X coordinate represented by the image (ignored).
/// @param max_z Maximum world Z coordinate represented by the image (ignored).
void rt_game3d_minimap_set_map_image(
    void *minimap, void *pixels, double min_x, double min_z, double max_x, double max_z) {
    (void)minimap;
    (void)pixels;
    (void)min_x;
    (void)min_z;
    (void)max_x;
    (void)max_z;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.SetMapImage: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetTrackedEntity` (graphics-disabled build).
/// @param minimap Minimap3D to configure (ignored).
/// @param entity Entity3D to track, or NULL to clear the tracked entity (ignored).
void rt_game3d_minimap_set_tracked_entity(void *minimap, void *entity) {
    (void)minimap;
    (void)entity;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.SetTrackedEntity: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetViewport` (graphics-disabled build).
/// @param minimap Minimap3D to configure (ignored).
/// @param x Finite bounded left edge in canvas pixels (ignored).
/// @param y Finite bounded top edge in canvas pixels (ignored).
/// @param w Positive finite bounded viewport width in pixels (ignored).
/// @param h Positive finite bounded viewport height in pixels (ignored).
void rt_game3d_minimap_set_viewport(void *minimap, double x, double y, double w, double h) {
    (void)minimap;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.SetViewport: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetCompass` (graphics-disabled build).
/// @param minimap Minimap3D to configure (ignored).
/// @param enabled Non-zero to draw the compass (ignored).
/// @param width_px Compass width in pixels (ignored).
void rt_game3d_minimap_set_compass(void *minimap, int8_t enabled, double width_px) {
    (void)minimap;
    (void)enabled;
    (void)width_px;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.SetCompass: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Minimap3D.AddMarker` (graphics-disabled build).
/// @param minimap Minimap3D that will own the marker (ignored).
/// @param entity Entity3D whose world position drives the marker (ignored).
/// @param icon Optional Pixels resource (ignored).
/// @param color Packed runtime color used to tint or draw the marker (ignored).
/// @return `0`.
int64_t rt_game3d_minimap_add_marker(void *minimap, void *entity, void *icon, int64_t color) {
    (void)minimap;
    (void)entity;
    (void)icon;
    (void)color;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Minimap3D.AddMarker: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Minimap3D.AddMarkerAt` (graphics-disabled build).
/// @param minimap Minimap3D that will own the marker (ignored).
/// @param point Vec3 containing the fixed marker position (ignored).
/// @param icon Optional Pixels resource (ignored).
/// @param color Packed runtime color used to tint or draw the marker (ignored).
/// @return `0`.
int64_t rt_game3d_minimap_add_marker_at(void *minimap, void *point, void *icon, int64_t color) {
    (void)minimap;
    (void)point;
    (void)icon;
    (void)color;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Minimap3D.AddMarkerAt: graphics support not compiled in", 0);
}

/// @brief Silent no-op stub for `Minimap3D.RemoveMarker` (graphics-disabled build).
/// @param minimap Minimap3D containing the marker (ignored).
/// @param id Marker identifier returned by an add operation (ignored).
void rt_game3d_minimap_remove_marker(void *minimap, int64_t id) {
    (void)minimap;
    (void)id;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.RemoveMarker: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetMarkerEdgeClamp` (graphics-disabled build).
/// @param minimap Minimap3D containing the marker (ignored).
/// @param id Marker identifier to configure (ignored).
/// @param clamp Non-zero to clamp to the rim (ignored).
void rt_game3d_minimap_set_marker_edge_clamp(void *minimap, int64_t id, int8_t clamp) {
    (void)minimap;
    (void)id;
    (void)clamp;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Minimap3D.SetMarkerEdgeClamp: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetMarkerScale` (graphics-disabled build).
/// @param minimap Minimap3D containing the marker (ignored).
/// @param id Marker identifier to configure (ignored).
/// @param scale Positive finite multiplier, clamped to a maximum of 8 (ignored).
void rt_game3d_minimap_set_marker_scale(void *minimap, int64_t id, double scale) {
    (void)minimap;
    (void)id;
    (void)scale;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.SetMarkerScale: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetMarkerOnCompass` (graphics-disabled build).
/// @param minimap Minimap3D containing the marker (ignored).
/// @param id Marker identifier to configure (ignored).
/// @param enabled Non-zero to include the marker on the compass (ignored).
void rt_game3d_minimap_set_marker_on_compass(void *minimap, int64_t id, int8_t enabled) {
    (void)minimap;
    (void)id;
    (void)enabled;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Minimap3D.SetMarkerOnCompass: graphics support not compiled in");
}

/// @brief Silent no-op stub for `Minimap3D.SetObjectiveIndicator` (graphics-disabled build).
/// @param minimap Minimap3D containing the marker (ignored).
/// @param id Marker identifier to configure (ignored).
/// @param enabled Non-zero to draw the objective indicator (ignored).
void rt_game3d_minimap_set_objective_indicator(void *minimap, int64_t id, int8_t enabled) {
    (void)minimap;
    (void)id;
    (void)enabled;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID(
        "Minimap3D.SetObjectiveIndicator: graphics support not compiled in");
}

/// @brief Silent fallback stub for `Minimap3D.get_MarkerCount` (graphics-disabled build).
/// @param minimap Minimap3D to query (ignored).
/// @return `0`.
int64_t rt_game3d_minimap_get_marker_count(void *minimap) {
    (void)minimap;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Minimap3D.get_MarkerCount: graphics support not compiled in", 0);
}

/// @brief Silent fallback stub for `Minimap3D.MapX` (graphics-disabled build).
/// @param minimap Minimap3D supplying the world bounds and viewport (ignored).
/// @param world_x World-space X coordinate (ignored).
/// @param world_z World-space Z coordinate (ignored).
/// @return `0.0`.
double rt_game3d_minimap_map_x(void *minimap, double world_x, double world_z) {
    (void)minimap;
    (void)world_x;
    (void)world_z;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Minimap3D.MapX: graphics support not compiled in", 0.0);
}

/// @brief Silent fallback stub for `Minimap3D.MapY` (graphics-disabled build).
/// @param minimap Minimap3D supplying the world bounds and viewport (ignored).
/// @param world_x World-space X coordinate (ignored).
/// @param world_z World-space Z coordinate (ignored).
/// @return `0.0`.
double rt_game3d_minimap_map_y(void *minimap, double world_x, double world_z) {
    (void)minimap;
    (void)world_x;
    (void)world_z;
    RT_GRAPHICS_OPTIONAL_TRAP_RET("Minimap3D.MapY: graphics support not compiled in", 0.0);
}

/// @brief Silent no-op stub for `Minimap3D.Draw` (graphics-disabled build).
/// @param minimap Minimap3D to render during the application's HUD pass (ignored).
void rt_game3d_minimap_draw(void *minimap) {
    (void)minimap;
    RT_GRAPHICS_OPTIONAL_TRAP_VOID("Minimap3D.Draw: graphics support not compiled in");
}

/* Game3D internal world-step hook stubs */

/// @brief No-op stub for the internal Game3D hook `game3d_footsteps_tick` (graphics-disabled
/// build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed world providing physics and audio services (ignored).
/// @param entity Borrowed entity whose Footsteps3D component is advanced (ignored).
/// @param dt Sanitized simulation delta in seconds (ignored).
void game3d_footsteps_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt) {
    (void)world;
    (void)entity;
    (void)dt;
}

/// @brief No-op stub for the internal Game3D hook `game3d_interactor_tick` (graphics-disabled
/// build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed world providing the entity and physics queries (ignored).
/// @param owner Borrowed entity whose Interactor3D component is advanced (ignored).
/// @param dt Sanitized simulation delta in seconds (ignored).
void game3d_interactor_tick(rt_game3d_world *world, rt_game3d_entity *owner, double dt) {
    (void)world;
    (void)owner;
    (void)dt;
}

/// @brief No-op stub for the internal Game3D hook `game3d_ai_tick` (graphics-disabled build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed world providing query context (ignored).
/// @param entity Borrowed entity whose AI components are advanced (ignored).
/// @param dt Sanitized simulation delta in seconds (ignored).
void game3d_ai_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt) {
    (void)world;
    (void)entity;
    (void)dt;
}

/// @brief No-op stub for the internal Game3D hook `game3d_cloth_tick` (graphics-disabled build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed live world payload (ignored).
/// @param dt Sanitized simulation delta in seconds (ignored).
void game3d_cloth_tick(struct rt_game3d_world *world, double dt) {
    (void)world;
    (void)dt;
}

/// @brief No-op stub for the internal Game3D hook `game3d_persistence_tick` (graphics-disabled
/// build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed live world payload (ignored).
void game3d_persistence_tick(struct rt_game3d_world *world) {
    (void)world;
}

/// @brief No-op stub for the internal Game3D hook `game3d_persistence_on_despawn`
/// (graphics-disabled build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed world owning the persistence records (ignored).
/// @param entity Borrowed entity about to leave the world (ignored).
void game3d_persistence_on_despawn(struct rt_game3d_world *world, struct rt_game3d_entity *entity) {
    (void)world;
    (void)entity;
}

/// @brief No-op stub for the internal Game3D hook `game3d_persistence_release` (graphics-disabled
/// build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param world Borrowed world payload being reset or destroyed (ignored).
void game3d_persistence_release(struct rt_game3d_world *world) {
    (void)world;
}

/// @brief No-op stub for the internal Game3D hook `game3d_stream_push_loaded_event`
/// (graphics-disabled build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param stream Borrowed WorldStream3D payload receiving the event (ignored).
/// @param cell_name Borrowed runtime string naming the loaded cell (ignored).
void game3d_stream_push_loaded_event(struct rt_game3d_world_stream *stream, rt_string cell_name) {
    (void)stream;
    (void)cell_name;
}

/// @brief No-op stub for the internal Game3D hook `game3d_stream_persistence_release`
/// (graphics-disabled build).
/// @details World3D cannot be constructed without graphics, so the world step never
///          reaches this hook; the definition only satisfies the link.
/// @param stream Borrowed WorldStream3D payload being reset or destroyed (ignored).
void game3d_stream_persistence_release(struct rt_game3d_world_stream *stream) {
    (void)stream;
}
