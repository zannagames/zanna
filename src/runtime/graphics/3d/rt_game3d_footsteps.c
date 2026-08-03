//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_footsteps.c
// Purpose: Surface-driven footsteps — SurfaceTable3D (per-surface clip sets +
//   loudness keyed by Game3D.Surfaces ids) and Footsteps3D (per-entity binding
//   that consumes animator "footstep" events, raycasts the ground, resolves
//   the surface row, and plays a deterministically-selected clip variant).
// Key invariants:
//   - Variant selection uses a per-component LCG seeded at bind: replays are
//     byte-identical. Row 0 is the untyped-surface fallback; a fully unset
//     table is a silent no-op.
//   - Surface rows retain only Sound handles and fixed-array counts are always
//     clamped before indexing or finalization.
// Ownership/Lifetime:
//   - Tables retain their clips; Footsteps3D retains its table and holds a
//     zeroing weak reference to the owning entity.
// Links: ADR 0092, rt_game3d_surfaces.c.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements surface-specific deterministic footstep playback for Game3D.
/// @details SurfaceTable3D stores up to eight retained clip variants and a
///          loudness scalar for each surface identifier, with row zero as the
///          fallback. Footsteps3D attaches to an entity, scans matching animator
///          events, raycasts beneath the entity, and chooses a clip through a
///          fixed-seed linear congruential generator. A cooldown bounds duplicate
///          events without introducing nondeterministic timing or allocation.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_audio.h"
#include "rt_collider3d.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FOOTSTEPS3D_MAX_SURFACE 255
#define FOOTSTEPS3D_MAX_CLIPS 8
#define FOOTSTEPS3D_MIN_INTERVAL 0.12

/// @brief One surface identifier's retained clip variants and stimulus metadata.
typedef struct rt_game3d_surface_row {
    void *clips[FOOTSTEPS3D_MAX_CLIPS]; /* retained audio clips */
    int32_t clip_count;
    double loudness; /* hearing-stimulus scale (default 1) */
} rt_game3d_surface_row;

/// @brief Fixed-index table of all supported surface rows.
typedef struct rt_game3d_surface_table {
    void *vptr;
    rt_game3d_surface_row rows[FOOTSTEPS3D_MAX_SURFACE + 1]; /* index = surface id; 0 = default */
} rt_game3d_surface_table;

/// @brief Per-entity footstep event consumer and deterministic selector state.
typedef struct rt_game3d_footsteps {
    void *vptr;
    void *entity; /* zeroing weak owner Entity3D slot */
    void *table;  /* retained SurfaceTable3D */
    rt_string event_prefix;
    int64_t ground_mask;
    double volume_scale;
    double cooldown; /* seconds until the next step may fire */
    uint32_t rng;    /* deterministic variant selector */
    int64_t step_count;
    int64_t last_surface; /* surface id of the most recent step (tests/telemetry) */
} rt_game3d_footsteps;

/// @brief Clamp a surface row's logical clip count to its fixed backing array.
/// @param row Surface row whose private count is inspected.
/// @return Safe count in `[0, FOOTSTEPS3D_MAX_CLIPS]`.
static int32_t game3d_surface_row_clip_count(const rt_game3d_surface_row *row) {
    if (!row || row->clip_count <= 0)
        return 0;
    return row->clip_count > FOOTSTEPS3D_MAX_CLIPS ? FOOTSTEPS3D_MAX_CLIPS : row->clip_count;
}

/*==========================================================================
 * SurfaceTable3D
 *=========================================================================*/

/// @brief Release every retained clip in a SurfaceTable3D.
/// @param obj SurfaceTable3D storage being finalized; NULL is ignored.
static void game3d_surface_table_finalize(void *obj) {
    rt_game3d_surface_table *table = (rt_game3d_surface_table *)obj;
    if (!table)
        return;
    for (int32_t r = 0; r <= FOOTSTEPS3D_MAX_SURFACE; ++r) {
        for (int32_t c = 0; c < FOOTSTEPS3D_MAX_CLIPS; ++c)
            game3d_release_ref(&table->rows[r].clips[c]);
        table->rows[r].clip_count = 0;
    }
}

/// @brief Allocate an empty surface table with unit loudness on every row.
/// @return A newly allocated SurfaceTable3D, or NULL after allocation failure.
void *rt_game3d_surface_table_new(void) {
    rt_game3d_surface_table *table = (rt_game3d_surface_table *)rt_obj_new_i64(
        RT_G3D_GAME3D_SURFACETABLE_CLASS_ID, (int64_t)sizeof(rt_game3d_surface_table));
    if (!table) {
        rt_trap("Game3D.SurfaceTable3D.New: allocation failed");
        return NULL;
    }
    memset(table, 0, sizeof(*table));
    rt_obj_set_finalizer(table, game3d_surface_table_finalize);
    for (int32_t r = 0; r <= FOOTSTEPS3D_MAX_SURFACE; ++r)
        table->rows[r].loudness = 1.0;
    return table;
}

/// @brief Validate a runtime handle as SurfaceTable3D.
/// @param obj Candidate runtime handle.
/// @param method Trap message used when validation fails.
/// @return The typed table pointer, or NULL after trapping.
static rt_game3d_surface_table *game3d_surface_table_checked(void *obj, const char *method) {
    rt_game3d_surface_table *table =
        (rt_game3d_surface_table *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_SURFACETABLE_CLASS_ID);
    if (!table)
        rt_trap(method);
    return table;
}

/// @brief Append a clip variant for @p surface_id (row 0 = the untyped default).
/// @param obj SurfaceTable3D runtime handle.
/// @param surface_id Row identifier in [0, 255].
/// @param clip Audio clip object to retain; NULL is ignored.
/// @return @p obj for fluent chaining. Invalid rows and full eight-clip rows
///         remain unchanged without trapping.
void *rt_game3d_surface_table_add_clip(void *obj, int64_t surface_id, void *clip) {
    rt_game3d_surface_table *table =
        game3d_surface_table_checked(obj, "Game3D.SurfaceTable3D.addClip: invalid table");
    if (!table || !clip)
        return obj;
    if (surface_id < 0 || surface_id > FOOTSTEPS3D_MAX_SURFACE)
        return obj;
    if (!rt_sound_is_handle(clip)) {
        rt_trap("Game3D.SurfaceTable3D.addClip: clip must be Sound");
        return obj;
    }
    rt_game3d_surface_row *row = &table->rows[surface_id];
    row->clip_count = game3d_surface_row_clip_count(row);
    if (row->clip_count >= FOOTSTEPS3D_MAX_CLIPS)
        return obj;
    game3d_release_ref(&row->clips[row->clip_count]);
    rt_obj_retain_maybe(clip);
    row->clips[row->clip_count++] = clip;
    return obj;
}

/// @brief Hearing-stimulus scale for @p surface_id (plan 22 consumer; default 1).
/// @param obj SurfaceTable3D runtime handle.
/// @param surface_id Row identifier in [0, 255].
/// @param loudness Requested non-negative scale; finite values are capped at
///                 four and invalid/negative values reset the row to one.
/// @return @p obj for fluent chaining.
void *rt_game3d_surface_table_set_loudness(void *obj, int64_t surface_id, double loudness) {
    rt_game3d_surface_table *table =
        game3d_surface_table_checked(obj, "Game3D.SurfaceTable3D.setLoudness: invalid table");
    if (!table || surface_id < 0 || surface_id > FOOTSTEPS3D_MAX_SURFACE)
        return obj;
    table->rows[surface_id].loudness =
        isfinite(loudness) && loudness >= 0.0 ? (loudness > 4.0 ? 4.0 : loudness) : 1.0;
    return obj;
}

/// @brief Clip count configured for @p surface_id (tests/tooling).
/// @param obj SurfaceTable3D runtime handle.
/// @param surface_id Row identifier in [0, 255].
/// @return The number of retained variants, or zero for an invalid table or row.
int64_t rt_game3d_surface_table_clip_count(void *obj, int64_t surface_id) {
    rt_game3d_surface_table *table =
        game3d_surface_table_checked(obj, "Game3D.SurfaceTable3D.clipCount: invalid table");
    if (!table || surface_id < 0 || surface_id > FOOTSTEPS3D_MAX_SURFACE)
        return 0;
    return game3d_surface_row_clip_count(&table->rows[surface_id]);
}

/// @brief Resolve the effective row for a surface id (row 0 fallback, NULL when empty).
/// @param table Surface table to inspect; may be NULL.
/// @param surface_id Raycast surface identifier.
/// @return The populated exact row, otherwise populated row zero, otherwise NULL.
static rt_game3d_surface_row *game3d_surface_table_resolve(rt_game3d_surface_table *table,
                                                           int64_t surface_id) {
    if (!table)
        return NULL;
    if (surface_id >= 1 && surface_id <= FOOTSTEPS3D_MAX_SURFACE &&
        game3d_surface_row_clip_count(&table->rows[surface_id]) > 0)
        return &table->rows[surface_id];
    if (game3d_surface_row_clip_count(&table->rows[0]) > 0)
        return &table->rows[0];
    return NULL;
}

/*==========================================================================
 * Footsteps3D
 *=========================================================================*/

/// @brief Clear the entity back-reference and release retained table/prefix values.
/// @param obj Footsteps3D storage being finalized; NULL is ignored.
static void game3d_footsteps_finalize(void *obj) {
    rt_game3d_footsteps *steps = (rt_game3d_footsteps *)obj;
    if (!steps)
        return;
    rt_weak_store(&steps->entity, NULL);
    game3d_release_ref(&steps->table);
    game3d_release_ref((void **)&steps->event_prefix);
}

/// @brief Create and install one footstep component on an entity.
/// @details The component retains @p table_obj and the entity retains the
///          component; any prior Footsteps3D has its weak entity back-reference
///          cleared. The returned creation reference remains owned by the caller.
/// @param entity_obj Entity3D that receives the component.
/// @param table_obj SurfaceTable3D used to resolve clip variants.
/// @return A newly allocated Footsteps3D, or NULL after validation or allocation failure.
void *rt_game3d_footsteps_new(void *entity_obj, void *table_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Footsteps3D.New: invalid entity");
    rt_game3d_surface_table *table = (rt_game3d_surface_table *)rt_g3d_checked_or_null(
        table_obj, RT_G3D_GAME3D_SURFACETABLE_CLASS_ID);
    if (!entity)
        return NULL;
    if (!table) {
        rt_trap("Game3D.Footsteps3D.New: SurfaceTable3D required");
        return NULL;
    }
    rt_game3d_footsteps *steps = (rt_game3d_footsteps *)rt_obj_new_i64(
        RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID, (int64_t)sizeof(rt_game3d_footsteps));
    if (!steps) {
        rt_trap("Game3D.Footsteps3D.New: allocation failed");
        return NULL;
    }
    memset(steps, 0, sizeof(*steps));
    rt_obj_set_finalizer(steps, game3d_footsteps_finalize);
    rt_weak_store(&steps->entity, entity);
    if (!steps->entity) {
        game3d_release_ref((void **)&steps);
        return NULL;
    }
    rt_obj_retain_maybe(table_obj);
    steps->table = table_obj;
    steps->event_prefix = rt_const_cstr("footstep");
    steps->ground_mask = -1;
    steps->volume_scale = 1.0;
    steps->rng = 0x9E3779B9u; /* fixed bind seed: deterministic replays */
    /* Install on the entity slot (previous component detaches, mirrors LipSync3D). */
    {
        rt_game3d_footsteps *previous = (rt_game3d_footsteps *)rt_g3d_checked_or_null(
            entity->footsteps, RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID);
        if (previous && previous != steps)
            rt_weak_store(&previous->entity, NULL);
        game3d_assign_typed_ref(&entity->footsteps, steps, RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID);
    }
    return steps;
}

/// @brief Validate a runtime handle as Footsteps3D.
/// @param obj Candidate runtime handle.
/// @param method Trap message used when validation fails.
/// @return The typed component pointer, or NULL after trapping.
static rt_game3d_footsteps *game3d_footsteps_checked(void *obj, const char *method) {
    rt_game3d_footsteps *steps =
        (rt_game3d_footsteps *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID);
    if (!steps)
        rt_trap(method);
    return steps;
}

/// @brief Set the non-empty animator-event prefix that triggers footsteps.
/// @param obj Footsteps3D runtime handle.
/// @param prefix Non-empty runtime string to retain; NULL or empty input leaves
///               the current prefix unchanged.
/// @return @p obj for fluent chaining.
void *rt_game3d_footsteps_set_event_prefix(void *obj, rt_string prefix) {
    rt_game3d_footsteps *steps =
        game3d_footsteps_checked(obj, "Game3D.Footsteps3D.setEventPrefix: invalid component");
    if (steps && prefix && rt_str_len(prefix) > 0)
        game3d_assign_ref((void **)&steps->event_prefix, prefix);
    return obj;
}

/// @brief Set the collision mask used by downward surface raycasts.
/// @param obj Footsteps3D runtime handle.
/// @param mask Physics collision-mask bits; -1 selects all layers by convention.
/// @return @p obj for fluent chaining.
void *rt_game3d_footsteps_set_ground_mask(void *obj, int64_t mask) {
    rt_game3d_footsteps *steps =
        game3d_footsteps_checked(obj, "Game3D.Footsteps3D.setGroundMask: invalid component");
    if (steps)
        steps->ground_mask = mask;
    return obj;
}

/// @brief Set the component's stored playback-volume scale.
/// @param obj Footsteps3D runtime handle.
/// @param scale Finite non-negative scale capped at four; invalid values leave
///              the existing setting unchanged.
/// @return @p obj for fluent chaining.
void *rt_game3d_footsteps_set_volume_scale(void *obj, double scale) {
    rt_game3d_footsteps *steps =
        game3d_footsteps_checked(obj, "Game3D.Footsteps3D.setVolumeScale: invalid component");
    if (steps && isfinite(scale) && scale >= 0.0)
        steps->volume_scale = scale > 4.0 ? 4.0 : scale;
    return obj;
}

/// @brief Return how many cooldown-admitted step events have fired.
/// @param obj Footsteps3D runtime handle.
/// @return The accumulated step count, including silent table/audio misses, or zero.
int64_t rt_game3d_footsteps_get_step_count(void *obj) {
    rt_game3d_footsteps *steps =
        game3d_footsteps_checked(obj, "Game3D.Footsteps3D.get_StepCount: invalid component");
    return steps ? steps->step_count : 0;
}

/// @brief Return the surface identifier observed by the most recent step raycast.
/// @param obj Footsteps3D runtime handle.
/// @return The last surface id, with zero representing no typed hit or invalid state.
int64_t rt_game3d_footsteps_get_last_surface(void *obj) {
    rt_game3d_footsteps *steps =
        game3d_footsteps_checked(obj, "Game3D.Footsteps3D.get_LastSurface: invalid component");
    return steps ? steps->last_surface : 0;
}

/// @brief Fire one footstep: ground raycast -> surface row -> deterministic clip.
/// @details The cooldown is armed before querying the ground. Each admitted event
///          updates telemetry even when no populated row or audio engine exists;
///          clip selection advances the RNG only when playback is possible.
/// @param world World3D supplying physics and audio services.
/// @param entity Entity3D whose world position seeds the downward ray.
/// @param steps Component containing mask, table, cooldown, and RNG state.
static void game3d_footsteps_fire(rt_game3d_world *world,
                                  rt_game3d_entity *entity,
                                  rt_game3d_footsteps *steps) {
    if (steps->cooldown > 0.0)
        return;
    steps->cooldown = FOOTSTEPS3D_MIN_INTERVAL;
    double origin[3];
    if (!game3d_entity_world_position_components(entity, origin)) {
        origin[0] = origin[1] = origin[2] = 0.0;
    }
    int64_t surface = 0;
    if (world->physics) {
        void *owner_body = game3d_entity_body_ref(entity);
        void *ground_body = rt_world3d_raycast_closest_body_raw(world->physics,
                                                                origin[0],
                                                                origin[1] + 0.25,
                                                                origin[2],
                                                                0.0,
                                                                -1.0,
                                                                0.0,
                                                                1.5,
                                                                steps->ground_mask,
                                                                owner_body,
                                                                NULL);
        void *collider = ground_body ? rt_body3d_get_collider(ground_body) : NULL;
        if (collider)
            surface = rt_collider3d_get_surface_type(collider);
    }
    steps->last_surface = surface;
    rt_game3d_surface_table *table = (rt_game3d_surface_table *)rt_g3d_checked_or_null(
        steps->table, RT_G3D_GAME3D_SURFACETABLE_CLASS_ID);
    rt_game3d_surface_row *row = game3d_surface_table_resolve(table, surface);
    if (steps->step_count < INT64_MAX)
        steps->step_count++;
    if (!row || !world->audio)
        return;
    int32_t clip_count = game3d_surface_row_clip_count(row);
    if (clip_count <= 0)
        return;
    steps->rng = steps->rng * 1664525u + 1013904223u;
    void *clip = row->clips[(steps->rng >> 8) % (uint32_t)clip_count];
    void *pos = rt_vec3_new(origin[0], origin[1], origin[2]);
    if (clip && pos) {
        void *voice = rt_game3d_audio_play_at(world->audio, clip, pos);
        if (voice) {
            double scaled = (double)rt_game3d_audio_get_volume(world->audio) * steps->volume_scale;
            int64_t volume = (int64_t)(scaled > 100.0 ? 100.0 : scaled + 0.5);
            rt_soundsource3d_set_volume(voice, volume);
        }
        game3d_release_ref(&voice);
    }
    game3d_release_ref(&pos);
}

/// @brief Per-step tick: consume this frame's matching animator events.
/// @param world World3D supplying physics and audio for admitted events.
/// @param entity Spawned Entity3D whose animator event list is scanned.
/// @param dt Simulation step in seconds used to reduce the duplicate-event cooldown.
void game3d_footsteps_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt) {
    rt_game3d_footsteps *steps = (rt_game3d_footsteps *)rt_g3d_checked_or_null(
        entity->footsteps, RT_G3D_GAME3D_FOOTSTEPS_CLASS_ID);
    if (!steps)
        return;
    if (steps->cooldown > 0.0) {
        steps->cooldown -= dt;
        if (steps->cooldown < 0.0)
            steps->cooldown = 0.0;
    }
    void *anim = entity->anim;
    if (!anim)
        return;
    const char *prefix = steps->event_prefix ? rt_string_cstr(steps->event_prefix) : "footstep";
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    int64_t event_count = rt_game3d_animator_event_count(anim);
    for (int64_t e = 0; e < event_count; ++e) {
        rt_string name = rt_game3d_animator_event_name(anim, e);
        const char *cname = name ? rt_string_cstr(name) : NULL;
        int matches = cname && prefix_len > 0 && strncmp(cname, prefix, prefix_len) == 0;
        if (name)
            rt_string_unref(name);
        if (matches)
            game3d_footsteps_fire(world, entity, steps);
    }
}

#else
typedef int rt_game3d_footsteps_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
