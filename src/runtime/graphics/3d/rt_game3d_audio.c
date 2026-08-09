//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_audio.c
// Purpose: Sound3D audio subsystem for the Zanna.Game3D layer — listener/source
//   management, distance attenuation, and camera-follow listener. Split out of
//   rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Ownership/Lifetime:
//   - GC-managed Sound3D handle; finalizer releases tracked sources + listener.
// Links: rt_game3d_internal.h, rt_sound3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Game3D spatial audio, listener control, and immersion effects.
/// @details Sound3D owns tracked positional sources and coordinates camera-follow listening,
///   attenuation, reverb zones, occlusion probes, dialogue routing, and ambient-bed crossfades.
///   All world-space effect selection runs deterministically on the game thread.

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_mixgroup.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Validate the dense retained reverb-zone array.
static int game3d_audio_reverb_storage_valid(const rt_game3d_audio *audio) {
    return audio && audio->reverb_zone_count >= 0 && audio->reverb_zone_capacity >= 0 &&
           audio->reverb_zone_count <= audio->reverb_zone_capacity &&
           (audio->reverb_zone_capacity == 0 || audio->reverb_zones);
}

/// @brief Validate the dense retained positional-source array.
static int game3d_audio_source_storage_valid(const rt_game3d_audio *audio) {
    return audio && audio->source_count >= 0 && audio->source_capacity >= 0 &&
           audio->source_count <= audio->source_capacity &&
           (audio->source_capacity == 0 || audio->sources);
}

/// @brief GC finalizer for the audio subsystem: release every tracked source plus the
///   camera and listener, then free the source array.
/// @param obj Sound3D allocation being finalized.
static void game3d_audio_finalize(void *obj) {
    rt_game3d_audio *audio = (rt_game3d_audio *)obj;
    if (!audio)
        return;
    game3d_audio_repair_sources(audio);
    for (int32_t i = 0; i < audio->source_count; ++i)
        game3d_release_typed_ref(&audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
    free(audio->sources);
    audio->sources = NULL;
    audio->source_count = 0;
    audio->source_capacity = 0;
    game3d_release_typed_ref(&audio->camera, RT_G3D_CAMERA3D_CLASS_ID);
    game3d_release_typed_ref(&audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID);
    int32_t reverb_zone_count =
        game3d_audio_reverb_storage_valid(audio) ? audio->reverb_zone_count : 0;
    for (int32_t z = 0; z < reverb_zone_count; ++z)
        game3d_release_typed_ref(&audio->reverb_zones[z], RT_G3D_GAME3D_REVERBZONE_CLASS_ID);
    free(audio->reverb_zones);
    audio->reverb_zones = NULL;
    audio->reverb_zone_count = 0;
    audio->reverb_zone_capacity = 0;
    game3d_release_typed_ref(&audio->ambient_bed, RT_G3D_GAME3D_AMBIENTBED_CLASS_ID);
}

/// @brief Allocate the audio subsystem with default attenuation/volume and a new
///   listener; if `camera` is given, bind and activate the listener to follow it.
/// @param camera Optional Camera3D to retain as the listener-follow target.
/// @return A new Sound3D subsystem, or NULL when allocation fails.
void *game3d_audio_new(void *camera) {
    rt_game3d_audio *audio =
        (rt_game3d_audio *)rt_obj_new_i64(RT_G3D_GAME3D_SOUND_CLASS_ID, (int64_t)sizeof(*audio));
    if (!audio) {
        rt_trap("Game3D.Sound3D.New: allocation failed");
        return NULL;
    }
    memset(audio, 0, sizeof(*audio));
    rt_obj_set_finalizer(audio, game3d_audio_finalize);
    audio->ref_distance = RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE;
    audio->max_distance = RT_GAME3D_DEFAULT_AUDIO_MAX_DISTANCE;
    audio->volume = RT_GAME3D_DEFAULT_AUDIO_VOLUME;
    audio->reverb_group = -1;
    audio->reverb_fx = -1;
    audio->reverb_blend = 0.5;
    audio->reverb_room = 0.5;
    audio->reverb_damp = 0.5;
    audio->reverb_wet = 0.0;
    audio->reverb_routing = 1;
    audio->occlusion_mask = -1;
    audio->occlusion_amount = 1.0;
    audio->occlusion_budget = 8;
    audio->dialogue_group = -1;
    audio->listener = rt_soundlistener3d_new();
    if (rt_g3d_has_class(camera, RT_G3D_CAMERA3D_CLASS_ID))
        game3d_assign_typed_ref(&audio->camera, camera, RT_G3D_CAMERA3D_CLASS_ID);
    if (rt_g3d_has_class(audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID) && audio->camera) {
        audio->listener_follow_camera = 1;
        rt_soundlistener3d_bind_camera(audio->listener, audio->camera);
        rt_soundlistener3d_set_is_active(audio->listener, 1);
    }
    return audio;
}

/// @brief Get the listener (ears) object (NULL if invalid).
/// @param obj Sound3D subsystem to query.
/// @return The borrowed validated SoundListener3D handle, or NULL when unavailable.
void *rt_game3d_audio_get_listener(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_Listener: invalid audio");
    return audio ? rt_g3d_checked_or_null(audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID) : NULL;
}

/// @brief True if the listener auto-follows the camera.
/// @param obj Sound3D subsystem to query.
/// @return 1 when camera-follow is enabled, or 0 when disabled or invalid.
int8_t rt_game3d_audio_get_listener_follows_camera(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_listenerFollowsCamera: invalid audio");
    return audio && audio->listener_follow_camera ? 1 : 0;
}

/// @brief Get the attenuation reference (full-volume) distance.
/// @param obj Sound3D subsystem to query.
/// @return The sanitized full-volume reference distance, or 0 for an invalid subsystem.
double rt_game3d_audio_get_ref_distance(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_refDistance: invalid audio");
    return audio ? game3d_positive_clamped_or(audio->ref_distance,
                                              RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE,
                                              RT_GAME3D_AUDIO_DISTANCE_MAX)
                 : 0.0;
}

/// @brief Get the attenuation maximum (silence) distance.
/// @param obj Sound3D subsystem to query.
/// @return The sanitized maximum distance, never below the reference distance, or 0 if invalid.
double rt_game3d_audio_get_max_distance(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_maxDistance: invalid audio");
    if (!audio)
        return 0.0;
    double ref_distance = game3d_positive_clamped_or(
        audio->ref_distance, RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE, RT_GAME3D_AUDIO_DISTANCE_MAX);
    double max_distance = game3d_positive_clamped_or(
        audio->max_distance, RT_GAME3D_DEFAULT_AUDIO_MAX_DISTANCE, RT_GAME3D_AUDIO_DISTANCE_MAX);
    return max_distance < ref_distance ? ref_distance : max_distance;
}

/// @brief Get the master output volume (0–100).
/// @param obj Sound3D subsystem to query.
/// @return The clamped master volume, or 0 for an invalid subsystem.
int64_t rt_game3d_audio_get_volume(void *obj) {
    rt_game3d_audio *audio = game3d_audio_checked(obj, "Game3D.Sound3D.get_volume: invalid audio");
    return audio ? game3d_clamp_i64(audio->volume, 0, 100) : 0;
}

/// @brief Count currently active 3D sound sources.
/// @param obj Sound3D subsystem whose source slots are repaired and counted.
/// @return The number of valid tracked SoundSource3D handles.
int64_t rt_game3d_audio_get_source_count(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_sourceCount: invalid audio");
    if (audio)
        game3d_audio_prune_sources(audio);
    return audio ? audio->source_count : 0;
}

/// @brief Enable/disable the listener following the camera; rebinds or unbinds the
///   camera accordingly and reactivates the listener.
/// @param obj Sound3D subsystem to configure.
/// @param enabled Non-zero to bind the listener to the retained camera; zero to unbind it.
void rt_game3d_audio_listener_follow_camera(void *obj, int8_t enabled) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.listenerFollowCamera: invalid audio");
    if (!audio)
        return;
    void *listener = rt_g3d_checked_or_null(audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID);
    void *camera = rt_g3d_checked_or_null(audio->camera, RT_G3D_CAMERA3D_CLASS_ID);
    if (!listener)
        return;
    audio->listener_follow_camera = enabled ? 1 : 0;
    if (audio->listener_follow_camera && camera)
        rt_soundlistener3d_bind_camera(listener, camera);
    else
        rt_soundlistener3d_clear_camera_binding(listener);
    rt_soundlistener3d_set_is_active(listener, 1);
}

/// @brief Set the listener pose explicitly from Vec3 position/forward/up;
///   disables camera-follow. Traps on non-Vec3 args.
/// @param obj Sound3D subsystem to configure.
/// @param position Vec3 containing the listener's world-space position.
/// @param forward Vec3 containing the listener's forward direction.
/// @param up Vec3 containing the listener's up direction.
void rt_game3d_audio_set_listener_pose(void *obj, void *position, void *forward, void *up) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.setListenerPose: invalid audio");
    if (!audio)
        return;
    void *listener = rt_g3d_checked_or_null(audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID);
    if (!listener)
        return;
    if (!rt_g3d_is_vec3(position) || !rt_g3d_is_vec3(forward) || !rt_g3d_is_vec3(up)) {
        rt_trap("Game3D.Sound3D.setListenerPose: expected Vec3 position, forward, and up");
        return;
    }
    audio->listener_follow_camera = 0;
    rt_soundlistener3d_clear_camera_binding(listener);
    rt_soundlistener3d_set_position(listener, position);
    rt_soundlistener3d_set_forward(listener, forward);
    rt_soundlistener3d_set_up(listener, up);
    rt_soundlistener3d_set_is_active(listener, 1);
}

/// @brief Set the distance-attenuation reference and max radii; non-positive/non-finite
///   values fall back to the library defaults.
/// @param obj Sound3D subsystem and tracked sources to configure.
/// @param ref_distance Full-volume distance in world units.
/// @param max_distance Silence distance in world units, raised to @p ref_distance when necessary.
void rt_game3d_audio_set_attenuation(void *obj, double ref_distance, double max_distance) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.setAttenuation: invalid audio");
    if (!audio)
        return;
    audio->ref_distance = game3d_positive_clamped_or(
        ref_distance, RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE, RT_GAME3D_AUDIO_DISTANCE_MAX);
    audio->max_distance = game3d_positive_clamped_or(
        max_distance, RT_GAME3D_DEFAULT_AUDIO_MAX_DISTANCE, RT_GAME3D_AUDIO_DISTANCE_MAX);
    if (audio->max_distance < audio->ref_distance)
        audio->max_distance = audio->ref_distance;
    game3d_audio_prune_sources(audio);
    for (int32_t i = 0; i < audio->source_count; ++i) {
        void *source = rt_g3d_checked_or_null(audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
        if (!source)
            continue;
        rt_soundsource3d_set_ref_distance(source, audio->ref_distance);
        rt_soundsource3d_set_max_distance(source, audio->max_distance);
    }
}

/// @brief Set the master output volume, clamped to [0, 100].
/// @param obj Sound3D subsystem to configure.
/// @param volume Requested integer volume.
void rt_game3d_audio_set_volume(void *obj, int64_t volume) {
    rt_game3d_audio *audio = game3d_audio_checked(obj, "Game3D.Sound3D.set_volume: invalid audio");
    if (!audio)
        return;
    audio->volume = game3d_clamp_i64(volume, 0, 100);
    game3d_audio_repair_sources(audio);
    for (int32_t i = 0; i < audio->source_count; ++i) {
        void *source = rt_g3d_checked_or_null(audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
        if (source)
            rt_soundsource3d_set_volume(source, audio->volume);
    }
}

/// @brief Load a sound clip from a filesystem path.
/// @param obj Sound3D subsystem validating the operation.
/// @param path Runtime string containing the filesystem path.
/// @return A loaded Sound handle, or NULL on failure.
void *rt_game3d_audio_load(void *obj, rt_string path) {
    (void)game3d_audio_checked(obj, "Game3D.Sound3D.load: invalid audio");
    return rt_sound_load(path);
}

/// @brief Load a sound clip from a packed asset path.
/// @param obj Sound3D subsystem validating the operation.
/// @param asset_path Runtime string containing the packed-asset path.
/// @return A loaded Sound handle, or NULL on failure.
void *rt_game3d_audio_load_asset(void *obj, rt_string asset_path) {
    (void)game3d_audio_checked(obj, "Game3D.Sound3D.loadAsset: invalid audio");
    return rt_sound_load_asset(asset_path);
}

/// @brief Play a clip as a one-shot at a fixed world position, applying the subsystem's
///   attenuation/volume; tracks and returns the new source. Traps on bad clip/position.
/// @param obj Sound3D subsystem that owns and configures the source.
/// @param clip Sound handle to play.
/// @param position Vec3 containing the fixed world-space source position.
/// @return A new tracked SoundSource3D handle, or NULL on validation or allocation failure.
void *rt_game3d_audio_play_at(void *obj, void *clip, void *position) {
    rt_game3d_audio *audio = game3d_audio_checked(obj, "Game3D.Sound3D.playAt: invalid audio");
    if (!audio || !clip)
        return NULL;
    if (!rt_sound_is_handle(clip)) {
        rt_trap("Game3D.Sound3D.playAt: expected Sound clip");
        return NULL;
    }
    if (!rt_g3d_is_vec3(position)) {
        rt_trap("Game3D.Sound3D.playAt: expected Vec3 position");
        return NULL;
    }
    void *source = rt_soundsource3d_new(clip);
    if (!source)
        return NULL;
    rt_soundsource3d_set_position(source, position);
    rt_soundsource3d_set_ref_distance(source, rt_game3d_audio_get_ref_distance(audio));
    rt_soundsource3d_set_max_distance(source, rt_game3d_audio_get_max_distance(audio));
    rt_soundsource3d_set_volume(source, rt_game3d_audio_get_volume(audio));
    if (audio->reverb_routing && audio->reverb_group >= 0)
        rt_soundsource3d_set_mix_group(source, audio->reverb_group);
    (void)rt_soundsource3d_play(source);
    game3d_audio_track_source(audio, source);
    return source;
}

/// @brief Play a clip attached to an entity so it tracks the entity's node (or its
///   position if nodeless); tracks and returns the source. Traps on bad clip/entity.
/// @param obj Sound3D subsystem that owns and configures the source.
/// @param clip Sound handle to play.
/// @param entity_obj Entity3D whose node or world position drives the source.
/// @return A new tracked SoundSource3D handle, or NULL on validation or allocation failure.
void *rt_game3d_audio_play_attached(void *obj, void *clip, void *entity_obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.playAttached: invalid audio");
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Sound3D.playAttached: invalid entity");
    if (!audio || !entity || !clip)
        return NULL;
    if (!rt_sound_is_handle(clip)) {
        rt_trap("Game3D.Sound3D.playAttached: expected Sound clip");
        return NULL;
    }
    void *source = rt_soundsource3d_new(clip);
    if (!source)
        return NULL;
    rt_soundsource3d_set_ref_distance(source, rt_game3d_audio_get_ref_distance(audio));
    rt_soundsource3d_set_max_distance(source, rt_game3d_audio_get_max_distance(audio));
    rt_soundsource3d_set_volume(source, rt_game3d_audio_get_volume(audio));
    void *node = game3d_entity_node_ref(entity);
    if (node)
        rt_soundsource3d_bind_node(source, node);
    else {
        void *pos = rt_game3d_entity_position(entity);
        rt_soundsource3d_set_position(source, pos);
        game3d_release_ref(&pos);
    }
    if (audio->reverb_routing && audio->reverb_group >= 0)
        rt_soundsource3d_set_mix_group(source, audio->reverb_group);
    (void)rt_soundsource3d_play(source);
    game3d_audio_track_source(audio, source);
    return source;
}

/// @brief Play a clip as non-spatial 2D audio at the master volume; returns a positive
///   voice id, or 0 on failure. Traps on a bad clip.
/// @param obj Sound3D subsystem supplying the master volume.
/// @param clip Sound handle to play.
/// @return A positive mixer voice identifier, or 0 on failure.
int64_t rt_game3d_audio_play2d(void *obj, void *clip) {
    rt_game3d_audio *audio = game3d_audio_checked(obj, "Game3D.Sound3D.play2D: invalid audio");
    if (!audio || !clip)
        return 0;
    if (!rt_sound_is_handle(clip)) {
        rt_trap("Game3D.Sound3D.play2D: expected Sound clip");
        return 0;
    }
    int64_t voice = rt_sound_play_ex(clip, rt_game3d_audio_get_volume(audio), 0);
    return voice > 0 ? voice : 0;
}

/// @brief Stop and release every tracked source, resetting the active count to 0.
/// @param obj Sound3D subsystem whose positional sources are cleared.
void rt_game3d_audio_clear_sources(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.clearSources: invalid audio");
    if (!audio)
        return;
    game3d_audio_repair_sources(audio);
    for (int32_t i = 0; i < audio->source_count; ++i) {
        void *source = rt_g3d_checked_or_null(audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
        if (source)
            rt_soundsource3d_stop(source);
        game3d_release_typed_ref(&audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
    }
    audio->source_count = 0;
}

/*==========================================================================
 * Audio immersion (plan 24) — reverb zones, occlusion raycasts, ambient
 * beds, and dialogue routing. All evaluation happens in the world step on
 * the game thread; the mixer only ever sees parameter writes.
 *=========================================================================*/

#define GAME3D_AMBIENTBED_MAX_ZONES 16

/// @brief AABB reverb-zone payload (Game3D.ReverbZone3D).
typedef struct rt_game3d_reverbzone {
    double min[3];    ///< AABB minimum corner.
    double max[3];    ///< AABB maximum corner.
    double room;      ///< Reverb room size 0..1.
    double damping;   ///< Reverb damping 0..1.
    double wet;       ///< Reverb wet mix 0..1.
    int64_t priority; ///< Higher wins when zones overlap.
} rt_game3d_reverbzone;

/// @brief One ambient-bed zone: an AABB with a looping clip.
typedef struct rt_game3d_ambientbed_zone {
    double min[3];  ///< AABB minimum corner.
    double max[3];  ///< AABB maximum corner.
    void *clip;     ///< Retained audio clip (looping bed).
    int64_t volume; ///< Playback volume 0..100.
} rt_game3d_ambientbed_zone;

/// @brief Zone-driven ambient loop crossfader (Game3D.AmbientBed3D).
typedef struct rt_game3d_ambientbed {
    rt_game3d_ambientbed_zone zones[GAME3D_AMBIENTBED_MAX_ZONES]; ///< Zone table.
    int32_t zone_count;                                           ///< Registered zones.
    void *default_clip;     ///< Retained outside-all-zones clip (may be NULL).
    int64_t default_volume; ///< Default-bed volume 0..100.
    double crossfade;       ///< Crossfade seconds between beds.
    int64_t group;          ///< Lazily-registered "g3d_ambience" group (-1 unset).
    int32_t active;         ///< Active zone index; -1 default bed; -2 unset.
    int64_t cur_voice;      ///< Voice id of the active bed loop (0 = silence).
    int64_t prev_voice;     ///< Voice id fading out (0 = none).
    int64_t cur_volume;     ///< Target volume of the active bed.
    int64_t prev_volume;    ///< Full volume of the fading bed.
    double fade_t;          ///< Crossfade progress 0..1 (1 = settled).
} rt_game3d_ambientbed;

/// @brief Read a defensively bounded ambient-bed zone count.
static int32_t game3d_ambientbed_safe_zone_count(const rt_game3d_ambientbed *bed) {
    if (!bed || bed->zone_count <= 0)
        return 0;
    return bed->zone_count < GAME3D_AMBIENTBED_MAX_ZONES ? bed->zone_count
                                                         : GAME3D_AMBIENTBED_MAX_ZONES;
}

/// @brief Clamp @p value to [0, 1]; non-finite values keep @p fallback.
/// @param value Candidate unit-range value.
/// @param fallback Value returned when @p value is not finite.
/// @return @p value clamped to the unit interval, or @p fallback.
static double game3d_unit_clamped_or(double value, double fallback) {
    if (!isfinite(value))
        return fallback;
    if (value < 0.0)
        return 0.0;
    return value > 1.0 ? 1.0 : value;
}

/// @brief Persist a canonical reverb-zone AABB and unit-range mixer parameters.
/// @param zone Mutable retained zone state; NULL is ignored.
static void game3d_reverbzone_repair_state(rt_game3d_reverbzone *zone) {
    if (!zone)
        return;
    for (int lane = 0; lane < 3; ++lane) {
        double lo = game3d_clamp_coord_or(zone->min[lane], 0.0);
        double hi = game3d_clamp_coord_or(zone->max[lane], 0.0);
        zone->min[lane] = lo < hi ? lo : hi;
        zone->max[lane] = lo < hi ? hi : lo;
    }
    zone->room = game3d_unit_clamped_or(zone->room, 0.5);
    zone->damping = game3d_unit_clamped_or(zone->damping, 0.5);
    zone->wet = game3d_unit_clamped_or(zone->wet, 0.35);
}

/// @brief Persist bounded AmbientBed3D counters, controls, voices, and zone AABBs.
/// @param bed Mutable retained ambient-bed state; NULL is ignored.
static void game3d_ambientbed_repair_state(rt_game3d_ambientbed *bed) {
    if (!bed)
        return;
    if (bed->zone_count < 0)
        bed->zone_count = 0;
    else if (bed->zone_count > GAME3D_AMBIENTBED_MAX_ZONES)
        bed->zone_count = GAME3D_AMBIENTBED_MAX_ZONES;
    for (int32_t z = 0; z < bed->zone_count; ++z) {
        for (int lane = 0; lane < 3; ++lane) {
            double lo = game3d_clamp_coord_or(bed->zones[z].min[lane], 0.0);
            double hi = game3d_clamp_coord_or(bed->zones[z].max[lane], 0.0);
            bed->zones[z].min[lane] = lo < hi ? lo : hi;
            bed->zones[z].max[lane] = lo < hi ? hi : lo;
        }
        bed->zones[z].volume = game3d_clamp_i64(bed->zones[z].volume, 0, 100);
    }
    bed->default_volume = game3d_clamp_i64(bed->default_volume, 0, 100);
    if (!isfinite(bed->crossfade) || bed->crossfade < 0.0)
        bed->crossfade = 2.0;
    if (bed->group < -1)
        bed->group = -1;
    if (bed->active < -2 || bed->active >= bed->zone_count)
        bed->active = -2;
    if (bed->cur_voice < 0)
        bed->cur_voice = 0;
    if (bed->prev_voice < 0)
        bed->prev_voice = 0;
    bed->cur_volume = game3d_clamp_i64(bed->cur_volume, 0, 100);
    bed->prev_volume = game3d_clamp_i64(bed->prev_volume, 0, 100);
    bed->fade_t = game3d_unit_clamped_or(bed->fade_t, 1.0);
}

/// @brief GC finalizer for AmbientBed3D: stop live voices, release clips.
/// @param obj AmbientBed3D allocation being finalized.
static void game3d_ambientbed_finalize(void *obj) {
    rt_game3d_ambientbed *bed = (rt_game3d_ambientbed *)obj;
    if (!bed)
        return;
    game3d_ambientbed_repair_state(bed);
    if (bed->cur_voice > 0)
        rt_voice_stop(bed->cur_voice);
    if (bed->prev_voice > 0)
        rt_voice_stop(bed->prev_voice);
    int32_t zone_count = game3d_ambientbed_safe_zone_count(bed);
    for (int32_t z = 0; z < zone_count; ++z)
        game3d_release_ref(&bed->zones[z].clip);
    bed->zone_count = 0;
    game3d_release_ref(&bed->default_clip);
}

/// @brief Validate and cast an opaque handle to a reverb-zone object.
/// @param obj Candidate ReverbZone3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated zone, or NULL after reporting an invalid handle.
static rt_game3d_reverbzone *game3d_reverbzone_checked(void *obj, const char *method) {
    rt_game3d_reverbzone *zone =
        (rt_game3d_reverbzone *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_REVERBZONE_CLASS_ID);
    if (!zone)
        rt_trap(method);
    else
        game3d_reverbzone_repair_state(zone);
    return zone;
}

/// @brief Validate and cast an opaque handle to an ambient-bed object.
/// @param obj Candidate AmbientBed3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated bed, or NULL after reporting an invalid handle.
static rt_game3d_ambientbed *game3d_ambientbed_checked(void *obj, const char *method) {
    rt_game3d_ambientbed *bed =
        (rt_game3d_ambientbed *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_AMBIENTBED_CLASS_ID);
    if (!bed)
        rt_trap(method);
    else
        game3d_ambientbed_repair_state(bed);
    return bed;
}

/// @brief True when @p p lies inside the axis-aligned box [min, max].
/// @param mn Three-component inclusive minimum corner.
/// @param mx Three-component inclusive maximum corner.
/// @param p Three-component point to test.
/// @return 1 when every point component lies within the corresponding interval.
static int game3d_aabb_contains(const double mn[3], const double mx[3], const double p[3]) {
    return p[0] >= mn[0] && p[0] <= mx[0] && p[1] >= mn[1] && p[1] <= mx[1] && p[2] >= mn[2] &&
           p[2] <= mx[2];
}

/// @brief Create a reverb zone from Vec3 min/max corners (auto-sorted per axis).
/// @param min_obj Vec3 containing one input corner.
/// @param max_obj Vec3 containing the opposite input corner.
/// @return A new ReverbZone3D with balanced defaults, or NULL on validation or allocation failure.
void *rt_game3d_reverbzone_new(void *min_obj, void *max_obj) {
    double mn[3], mx[3];
    if (!game3d_read_vec3(min_obj, mn, "Game3D.ReverbZone3D.New: min must be Vec3") ||
        !game3d_read_vec3(max_obj, mx, "Game3D.ReverbZone3D.New: max must be Vec3"))
        return NULL;
    rt_game3d_reverbzone *zone = (rt_game3d_reverbzone *)rt_obj_new_i64(
        RT_G3D_GAME3D_REVERBZONE_CLASS_ID, (int64_t)sizeof(*zone));
    if (!zone) {
        rt_trap("Game3D.ReverbZone3D.New: allocation failed");
        return NULL;
    }
    memset(zone, 0, sizeof(*zone));
    for (int i = 0; i < 3; ++i) {
        zone->min[i] = mn[i] < mx[i] ? mn[i] : mx[i];
        zone->max[i] = mn[i] < mx[i] ? mx[i] : mn[i];
    }
    zone->room = 0.5;
    zone->damping = 0.5;
    zone->wet = 0.35;
    return zone;
}

/// @brief Fluent: set the zone's reverb character (room/damping/wet, 0..1 each).
/// @param obj ReverbZone3D to configure.
/// @param room Room-size control clamped to the unit interval.
/// @param damping High-frequency damping control clamped to the unit interval.
/// @param wet Wet-signal mix clamped to the unit interval.
/// @return @p obj for fluent chaining.
void *rt_game3d_reverbzone_set_reverb(void *obj, double room, double damping, double wet) {
    rt_game3d_reverbzone *zone =
        game3d_reverbzone_checked(obj, "Game3D.ReverbZone3D.WithReverb: invalid zone");
    if (zone) {
        zone->room = game3d_unit_clamped_or(room, zone->room);
        zone->damping = game3d_unit_clamped_or(damping, zone->damping);
        zone->wet = game3d_unit_clamped_or(wet, zone->wet);
    }
    return obj;
}

/// @brief Set the zone's overlap priority (higher wins).
/// @param obj ReverbZone3D to configure.
/// @param priority Priority used when multiple zones contain the listener.
void rt_game3d_reverbzone_set_priority(void *obj, int64_t priority) {
    rt_game3d_reverbzone *zone =
        game3d_reverbzone_checked(obj, "Game3D.ReverbZone3D.set_Priority: invalid zone");
    if (zone)
        zone->priority = priority;
}

/// @brief Get the zone's overlap priority.
/// @param obj ReverbZone3D to query.
/// @return The configured priority, or 0 for an invalid zone.
int64_t rt_game3d_reverbzone_get_priority(void *obj) {
    rt_game3d_reverbzone *zone =
        game3d_reverbzone_checked(obj, "Game3D.ReverbZone3D.get_Priority: invalid zone");
    return zone ? zone->priority : 0;
}

/// @brief Register a reverb zone; lazily creates the "g3d_reverb" group + insert.
/// @details From this point positional playback routes to the reverb group
///   (unless SetReverbRouting(false)), so zone wet sweeps affect it.
/// @param obj Sound3D subsystem that will retain the zone.
/// @param zone_obj ReverbZone3D to register.
void rt_game3d_audio_add_reverb_zone(void *obj, void *zone_obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.AddReverbZone: invalid audio");
    rt_game3d_reverbzone *zone =
        game3d_reverbzone_checked(zone_obj, "Game3D.Sound3D.AddReverbZone: invalid zone");
    int32_t new_capacity;
    if (!audio || !zone)
        return;
    if (!game3d_audio_reverb_storage_valid(audio)) {
        rt_trap("Game3D.Sound3D.AddReverbZone: corrupt zone storage");
        return;
    }
    if (audio->reverb_zone_count >= audio->reverb_zone_capacity) {
        if (audio->reverb_zone_count == INT32_MAX ||
            !game3d_checked_capacity_i32(audio->reverb_zone_capacity,
                                         audio->reverb_zone_count + 1,
                                         8,
                                         sizeof(*audio->reverb_zones),
                                         &new_capacity)) {
            rt_trap("Game3D.Sound3D.AddReverbZone: capacity overflow");
            return;
        }
        void **grown = (void **)realloc(audio->reverb_zones, (size_t)new_capacity * sizeof(void *));
        if (!grown) {
            rt_trap("Game3D.Sound3D.AddReverbZone: allocation failed");
            return;
        }
        audio->reverb_zones = grown;
        audio->reverb_zone_capacity = new_capacity;
    }
    audio->reverb_zones[audio->reverb_zone_count] = NULL;
    game3d_assign_typed_ref(&audio->reverb_zones[audio->reverb_zone_count],
                            zone_obj,
                            RT_G3D_GAME3D_REVERBZONE_CLASS_ID);
    audio->reverb_zone_count += 1;
    if (audio->reverb_group < 0) {
        rt_string group_name = rt_const_cstr("g3d_reverb");
        audio->reverb_group = rt_audio_register_group(group_name);
        rt_string_unref(group_name);
        if (audio->reverb_group >= 0)
            audio->reverb_fx = rt_snd_group_add_reverb(
                audio->reverb_group, audio->reverb_room, audio->reverb_damp, 0.0);
    }
}

/// @brief Set the reverb-zone parameter blend time in seconds (default 0.5).
/// @param obj Sound3D subsystem to configure.
/// @param seconds Non-negative finite blend duration.
void rt_game3d_audio_set_reverb_blend(void *obj, double seconds) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.SetReverbBlendSeconds: invalid audio");
    if (audio && isfinite(seconds) && seconds >= 0.0)
        audio->reverb_blend = seconds;
}

/// @brief Current eased reverb wet mix (telemetry; 0 when outside all zones).
/// @param obj Sound3D subsystem to query.
/// @return The current wet mix, or 0 outside every zone or for an invalid subsystem.
double rt_game3d_audio_get_reverb_wet(void *obj) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.get_ReverbWet: invalid audio");
    return audio ? audio->reverb_wet : 0.0;
}

/// @brief Route future positional playback to the zone-reverb group (default on).
/// @param obj Sound3D subsystem to configure.
/// @param enabled Non-zero to route new positional sources through reverb; zero to bypass it.
void rt_game3d_audio_set_reverb_routing(void *obj, int8_t enabled) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.SetReverbRouting: invalid audio");
    if (audio)
        audio->reverb_routing = enabled ? 1 : 0;
}

/// @brief Configure listener->source occlusion raycasts for tracked sources.
/// @param obj Sound3D subsystem to configure.
/// @param enabled Non-zero to perform occlusion raycasts; zero to disable them.
/// @param mask Collision-layer mask used by the occlusion rays.
/// @param amount Occlusion applied to a blocked source (0..1; mixer smooths).
void rt_game3d_audio_set_occlusion(void *obj, int8_t enabled, int64_t mask, double amount) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.SetOcclusion: invalid audio");
    if (!audio)
        return;
    audio->occlusion_enabled = enabled ? 1 : 0;
    audio->occlusion_mask = mask;
    audio->occlusion_amount = game3d_unit_clamped_or(amount, 1.0);
    if (!audio->occlusion_enabled && game3d_audio_source_storage_valid(audio)) {
        for (int32_t i = 0; i < audio->source_count; ++i) {
            void *source = rt_g3d_checked_or_null(audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
            if (source)
                rt_soundsource3d_set_occlusion(source, 0.0);
        }
    }
}

/// @brief Cap occlusion raycasts per world step (default 8, round-robin).
/// @param obj Sound3D subsystem to configure.
/// @param budget Positive per-step raycast count, clamped to 256.
void rt_game3d_audio_set_occlusion_budget(void *obj, int64_t budget) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.SetOcclusionBudget: invalid audio");
    if (audio && budget > 0)
        audio->occlusion_budget = budget > 256 ? 256 : (int32_t)budget;
}

/// @brief Play a dialogue clip on the ducking-trigger "g3d_dialogue" group.
/// @details Pair with Audio.SetGroupDucking("g3d_dialogue", "music", ...) so
///   music dips under speech. Returns a positive voice id, or 0 on failure.
/// @param obj Sound3D subsystem supplying master volume and dialogue routing.
/// @param clip Sound handle to play.
/// @return A positive mixer voice identifier, or 0 on failure.
int64_t rt_game3d_audio_play_dialogue(void *obj, void *clip) {
    rt_game3d_audio *audio =
        game3d_audio_checked(obj, "Game3D.Sound3D.PlayDialogue: invalid audio");
    if (!audio || !clip)
        return 0;
    if (!rt_sound_is_handle(clip)) {
        rt_trap("Game3D.Sound3D.PlayDialogue: expected Sound clip");
        return 0;
    }
    if (audio->dialogue_group < 0) {
        rt_string group_name = rt_const_cstr("g3d_dialogue");
        audio->dialogue_group = rt_audio_register_group(group_name);
        rt_string_unref(group_name);
    }
    if (audio->dialogue_group < 0)
        return 0;
    int64_t voice = rt_sound_play_ex_in_group(
        clip, rt_game3d_audio_get_volume(audio), 0, audio->dialogue_group);
    return voice > 0 ? voice : 0;
}

/// @brief Create an ambient-bed crossfader bound to the world's audio subsystem.
/// @param world_obj World3D whose Sound3D subsystem will own the bed.
/// @return A new AmbientBed3D, or NULL when the world lacks audio or allocation fails.
void *rt_game3d_ambientbed_new(void *world_obj) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.AmbientBed3D.New: invalid world");
    rt_game3d_audio *audio =
        world
            ? (rt_game3d_audio *)rt_g3d_checked_or_null(world->audio, RT_G3D_GAME3D_SOUND_CLASS_ID)
            : NULL;
    if (!audio) {
        rt_trap("Game3D.AmbientBed3D.New: world has no audio subsystem");
        return NULL;
    }
    rt_game3d_ambientbed *bed = (rt_game3d_ambientbed *)rt_obj_new_i64(
        RT_G3D_GAME3D_AMBIENTBED_CLASS_ID, (int64_t)sizeof(*bed));
    if (!bed) {
        rt_trap("Game3D.AmbientBed3D.New: allocation failed");
        return NULL;
    }
    memset(bed, 0, sizeof(*bed));
    rt_obj_set_finalizer(bed, game3d_ambientbed_finalize);
    bed->default_volume = 60;
    bed->crossfade = 2.0;
    bed->group = -1;
    bed->active = -2;
    bed->fade_t = 1.0;
    game3d_assign_typed_ref(&audio->ambient_bed, bed, RT_G3D_GAME3D_AMBIENTBED_CLASS_ID);
    return bed;
}

/// @brief Fluent: add a zone (Vec3 min/max) whose bed loops @p clip at @p volume.
/// @param obj AmbientBed3D to extend.
/// @param min_obj Vec3 containing one input corner.
/// @param max_obj Vec3 containing the opposite input corner.
/// @param clip Sound handle to loop while the listener is inside.
/// @param volume Playback volume clamped to the range 0 through 100.
/// @return @p obj for fluent chaining.
void *rt_game3d_ambientbed_add_zone(
    void *obj, void *min_obj, void *max_obj, void *clip, int64_t volume) {
    rt_game3d_ambientbed *bed =
        game3d_ambientbed_checked(obj, "Game3D.AmbientBed3D.AddZone: invalid bed");
    double mn[3], mx[3];
    if (!bed)
        return obj;
    if (!game3d_read_vec3(min_obj, mn, "Game3D.AmbientBed3D.AddZone: min must be Vec3") ||
        !game3d_read_vec3(max_obj, mx, "Game3D.AmbientBed3D.AddZone: max must be Vec3"))
        return obj;
    if (!clip || !rt_sound_is_handle(clip)) {
        rt_trap("Game3D.AmbientBed3D.AddZone: expected Sound clip");
        return obj;
    }
    if (bed->zone_count < 0 || bed->zone_count >= GAME3D_AMBIENTBED_MAX_ZONES) {
        rt_trap("Game3D.AmbientBed3D.AddZone: zone budget (16) exceeded");
        return obj;
    }
    rt_game3d_ambientbed_zone *zone = &bed->zones[bed->zone_count];
    for (int i = 0; i < 3; ++i) {
        zone->min[i] = mn[i] < mx[i] ? mn[i] : mx[i];
        zone->max[i] = mn[i] < mx[i] ? mx[i] : mn[i];
    }
    rt_obj_retain_maybe(clip);
    zone->clip = clip;
    zone->volume = game3d_clamp_i64(volume, 0, 100);
    bed->zone_count += 1;
    return obj;
}

/// @brief Fluent: set the outside-all-zones bed (NULL clip = silence).
/// @param obj AmbientBed3D to configure.
/// @param clip Sound handle to loop outside every zone, or NULL for silence.
/// @param volume Playback volume clamped to the range 0 through 100.
/// @return @p obj for fluent chaining.
void *rt_game3d_ambientbed_set_default(void *obj, void *clip, int64_t volume) {
    rt_game3d_ambientbed *bed =
        game3d_ambientbed_checked(obj, "Game3D.AmbientBed3D.SetDefault: invalid bed");
    if (!bed)
        return obj;
    if (clip && !rt_sound_is_handle(clip)) {
        rt_trap("Game3D.AmbientBed3D.SetDefault: expected Sound clip");
        return obj;
    }
    if (clip)
        rt_obj_retain_maybe(clip);
    game3d_release_ref(&bed->default_clip);
    bed->default_clip = clip;
    bed->default_volume = game3d_clamp_i64(volume, 0, 100);
    return obj;
}

/// @brief Set the bed crossfade duration in seconds (default 2).
/// @param obj AmbientBed3D to configure.
/// @param seconds Non-negative finite crossfade duration.
void rt_game3d_ambientbed_set_crossfade(void *obj, double seconds) {
    rt_game3d_ambientbed *bed =
        game3d_ambientbed_checked(obj, "Game3D.AmbientBed3D.set_CrossfadeSeconds: invalid bed");
    if (bed && isfinite(seconds) && seconds >= 0.0)
        bed->crossfade = seconds;
}

/// @brief Get the bed crossfade duration in seconds.
/// @param obj AmbientBed3D to query.
/// @return The crossfade duration, or 0 for an invalid bed.
double rt_game3d_ambientbed_get_crossfade(void *obj) {
    rt_game3d_ambientbed *bed =
        game3d_ambientbed_checked(obj, "Game3D.AmbientBed3D.get_CrossfadeSeconds: invalid bed");
    return bed ? bed->crossfade : 0.0;
}

/// @brief Active zone index, or -1 for the default bed / outside all zones.
/// @param obj AmbientBed3D to query.
/// @return The zero-based active zone index, or -1 while the default bed is selected.
int64_t rt_game3d_ambientbed_get_active_zone(void *obj) {
    rt_game3d_ambientbed *bed =
        game3d_ambientbed_checked(obj, "Game3D.AmbientBed3D.get_ActiveZone: invalid bed");
    return bed && bed->active >= 0 ? bed->active : -1;
}

/// @brief Ease the group reverb toward the highest-priority zone containing the listener.
/// @param audio Sound3D subsystem whose current mixer parameters are advanced.
/// @param listener Three-component listener position in world space.
/// @param dt Simulation interval controlling the maximum blend step.
static void game3d_audio_reverb_tick(rt_game3d_audio *audio, const double listener[3], double dt) {
    if (!game3d_audio_reverb_storage_valid(audio) || !listener || audio->reverb_group < 0 ||
        audio->reverb_fx < 0)
        return;
    rt_game3d_reverbzone *best = NULL;
    for (int32_t z = 0; z < audio->reverb_zone_count; ++z) {
        rt_game3d_reverbzone *zone = (rt_game3d_reverbzone *)rt_g3d_checked_or_null(
            audio->reverb_zones[z], RT_G3D_GAME3D_REVERBZONE_CLASS_ID);
        if (!zone || !game3d_aabb_contains(zone->min, zone->max, listener))
            continue;
        if (!best || zone->priority > best->priority)
            best = zone;
    }
    double target_room = best ? best->room : audio->reverb_room;
    double target_damp = best ? best->damping : audio->reverb_damp;
    double target_wet = best ? best->wet : 0.0;
    double max_step = audio->reverb_blend > 1e-3 ? dt / audio->reverb_blend : 1.0;
    double deltas[3] = {target_room - audio->reverb_room,
                        target_damp - audio->reverb_damp,
                        target_wet - audio->reverb_wet};
    double *params[3] = {&audio->reverb_room, &audio->reverb_damp, &audio->reverb_wet};
    double moved = 0.0;
    for (int i = 0; i < 3; ++i) {
        double step = deltas[i];
        if (step > max_step)
            step = max_step;
        else if (step < -max_step)
            step = -max_step;
        *params[i] += step;
        moved += step < 0.0 ? -step : step;
    }
    if (moved > 1e-5)
        rt_snd_group_set_reverb(audio->reverb_group,
                                audio->reverb_fx,
                                audio->reverb_room,
                                audio->reverb_damp,
                                audio->reverb_wet);
}

/// @brief Budgeted round-robin listener->source occlusion raycasts.
/// @param world World3D supplying the physics raycast interface.
/// @param audio Sound3D subsystem whose tracked sources are sampled.
/// @param listener Three-component listener position in world space.
static void game3d_audio_occlusion_tick(rt_game3d_world *world,
                                        rt_game3d_audio *audio,
                                        const double listener[3]) {
    if (!world || !audio || !listener || !world->physics ||
        !game3d_audio_source_storage_valid(audio) || audio->source_count <= 0)
        return;
    int32_t budget = audio->occlusion_budget;
    if (budget <= 0)
        return;
    if (budget > audio->source_count)
        budget = audio->source_count;
    int32_t cursor = audio->occlusion_cursor;
    if (cursor < 0 || cursor >= audio->source_count)
        cursor = 0;
    for (int32_t n = 0; n < budget; ++n) {
        int32_t index = (int32_t)(((int64_t)cursor + n) % audio->source_count);
        void *source = rt_g3d_checked_or_null(audio->sources[index], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
        if (!source)
            continue;
        void *pos_obj = rt_soundsource3d_get_position(source);
        if (!pos_obj)
            continue;
        double pos[3] = {rt_vec3_x(pos_obj), rt_vec3_y(pos_obj), rt_vec3_z(pos_obj)};
        game3d_release_ref(&pos_obj);
        double to[3] = {pos[0] - listener[0], pos[1] - listener[1], pos[2] - listener[2]};
        double dist = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
        double occluded = 0.0;
        if (isfinite(dist) && dist > 0.5) {
            void *origin = rt_vec3_new(listener[0], listener[1], listener[2]);
            void *dir = rt_vec3_new(to[0] / dist, to[1] / dist, to[2] / dist);
            if (origin && dir) {
                void *hit = rt_world3d_raycast(
                    world->physics, origin, dir, dist - 0.25, audio->occlusion_mask);
                if (hit) {
                    occluded = audio->occlusion_amount;
                    game3d_release_ref(&hit);
                }
            }
            game3d_release_ref(&origin);
            game3d_release_ref(&dir);
        }
        rt_soundsource3d_set_occlusion(source, occluded);
    }
    audio->occlusion_cursor = (int32_t)(((int64_t)cursor + (int64_t)budget) % audio->source_count);
}

/// @brief Crossfade the ambient bed toward the zone containing the listener.
/// @param bed AmbientBed3D whose active and previous voices are advanced.
/// @param listener Three-component listener position in world space.
/// @param dt Simulation interval used to advance the equal-power fade.
static void game3d_audio_ambientbed_tick(rt_game3d_ambientbed *bed,
                                         const double listener[3],
                                         double dt) {
    if (!bed || !listener)
        return;
    game3d_ambientbed_repair_state(bed);
    if (bed->cur_voice > 0 && !rt_voice_is_playing(bed->cur_voice))
        bed->cur_voice = 0;
    if (bed->prev_voice > 0 && !rt_voice_is_playing(bed->prev_voice))
        bed->prev_voice = 0;
    int32_t selected = -1;
    int32_t zone_count = game3d_ambientbed_safe_zone_count(bed);
    for (int32_t z = 0; z < zone_count; ++z) {
        if (game3d_aabb_contains(bed->zones[z].min, bed->zones[z].max, listener)) {
            selected = z;
            break;
        }
    }
    if (selected != bed->active) {
        if (bed->prev_voice > 0)
            rt_voice_stop(bed->prev_voice);
        bed->prev_voice = bed->cur_voice;
        bed->prev_volume = bed->cur_volume;
        void *clip = selected >= 0 ? bed->zones[selected].clip : bed->default_clip;
        int64_t volume = selected >= 0 ? bed->zones[selected].volume : bed->default_volume;
        if (bed->group < 0) {
            rt_string group_name = rt_const_cstr("g3d_ambience");
            bed->group = rt_audio_register_group(group_name);
            rt_string_unref(group_name);
        }
        bed->cur_voice =
            clip && bed->group >= 0 ? rt_sound_play_loop_in_group(clip, 0, 0, bed->group) : 0;
        if (bed->cur_voice < 0)
            bed->cur_voice = 0;
        bed->cur_volume = volume;
        bed->fade_t = bed->active == -2 ? 1.0 : 0.0; /* first selection snaps */
        bed->active = selected;
    }
    if (bed->cur_voice <= 0) {
        void *clip = bed->active >= 0 ? bed->zones[bed->active].clip : bed->default_clip;
        if (clip && bed->group < 0) {
            rt_string group_name = rt_const_cstr("g3d_ambience");
            bed->group = rt_audio_register_group(group_name);
            rt_string_unref(group_name);
        }
        if (clip && bed->group >= 0) {
            bed->cur_voice = rt_sound_play_loop_in_group(clip, 0, 0, bed->group);
            if (bed->cur_voice < 0)
                bed->cur_voice = 0;
        }
    }
    if (bed->fade_t < 1.0) {
        double advance = bed->crossfade > 1e-3 ? dt / bed->crossfade : 1.0;
        bed->fade_t += advance;
        if (bed->fade_t > 1.0)
            bed->fade_t = 1.0;
    }
    /* Equal-power overlap: rising sin for the new bed, falling cos for the old. */
    double angle = bed->fade_t * 1.57079632679489661923;
    if (bed->cur_voice > 0)
        rt_voice_set_volume(bed->cur_voice, (int64_t)((double)bed->cur_volume * sin(angle) + 0.5));
    if (bed->prev_voice > 0) {
        if (bed->fade_t >= 1.0) {
            rt_voice_stop(bed->prev_voice);
            bed->prev_voice = 0;
        } else {
            rt_voice_set_volume(bed->prev_voice,
                                (int64_t)((double)bed->prev_volume * cos(angle) + 0.5));
        }
    }
}

/// @brief Shift fixed-position audio by a floating-origin rebase delta.
/// @details The world rebase already shifts scene nodes (so node-bound sources
///          follow) and the listener; this moves the pieces the rebase otherwise
///          missed: playAt / nodeless sources (via rt_soundsource3d_rebase_origin,
///          which skips node-bound sources) and the reverb / ambient-bed zone AABBs,
///          which are stored in world space and would otherwise leave the listener
///          in the wrong zone after a recenter. Subtracts the delta to match the
///          scene/physics rebase convention.
/// @param audio Sound3D subsystem containing fixed sources and world-space effect zones.
/// @param delta Three-component world-origin shift to subtract.
void game3d_audio_rebase_origin(rt_game3d_audio *audio, const double delta[3]) {
    if (!audio || !delta || !isfinite(delta[0]) || !isfinite(delta[1]) || !isfinite(delta[2]))
        return;
    const double clean_delta[3] = {game3d_clamp_coord_or(delta[0], 0.0),
                                   game3d_clamp_coord_or(delta[1], 0.0),
                                   game3d_clamp_coord_or(delta[2], 0.0)};
    game3d_audio_repair_scalar_state(audio);
    int32_t source_count = game3d_audio_source_storage_valid(audio) ? audio->source_count : 0;
    for (int32_t i = 0; i < source_count; ++i) {
        void *source = rt_g3d_checked_or_null(audio->sources[i], RT_G3D_SOUNDSOURCE3D_CLASS_ID);
        if (source)
            rt_soundsource3d_rebase_origin(source, clean_delta[0], clean_delta[1], clean_delta[2]);
    }
    int32_t reverb_zone_count =
        game3d_audio_reverb_storage_valid(audio) ? audio->reverb_zone_count : 0;
    for (int32_t z = 0; z < reverb_zone_count; ++z) {
        rt_game3d_reverbzone *zone = (rt_game3d_reverbzone *)rt_g3d_checked_or_null(
            audio->reverb_zones[z], RT_G3D_GAME3D_REVERBZONE_CLASS_ID);
        if (!zone)
            continue;
        game3d_reverbzone_repair_state(zone);
        for (int k = 0; k < 3; ++k) {
            zone->min[k] = game3d_clamp_coord_or(zone->min[k] - clean_delta[k], zone->min[k]);
            zone->max[k] = game3d_clamp_coord_or(zone->max[k] - clean_delta[k], zone->max[k]);
        }
    }
    {
        rt_game3d_ambientbed *bed = (rt_game3d_ambientbed *)rt_g3d_checked_or_null(
            audio->ambient_bed, RT_G3D_GAME3D_AMBIENTBED_CLASS_ID);
        if (bed) {
            game3d_ambientbed_repair_state(bed);
            int32_t zone_count = game3d_ambientbed_safe_zone_count(bed);
            for (int32_t z = 0; z < zone_count; ++z) {
                for (int k = 0; k < 3; ++k) {
                    bed->zones[z].min[k] = game3d_clamp_coord_or(
                        bed->zones[z].min[k] - clean_delta[k], bed->zones[z].min[k]);
                    bed->zones[z].max[k] = game3d_clamp_coord_or(
                        bed->zones[z].max[k] - clean_delta[k], bed->zones[z].max[k]);
                }
            }
        }
    }
}

/// @brief Per-step audio-immersion pass: zone reverb, occlusion, ambient beds.
/// @param world World3D whose Sound3D subsystem and listener state are evaluated.
/// @param dt Deterministic simulation interval in seconds.
void game3d_audio_immersion_tick(struct rt_game3d_world *world, double dt) {
    if (!world)
        return;
    rt_game3d_audio *audio =
        (rt_game3d_audio *)rt_g3d_checked_or_null(world->audio, RT_G3D_GAME3D_SOUND_CLASS_ID);
    if (!audio)
        return;
    game3d_audio_repair_scalar_state(audio);
    if (!isfinite(dt) || dt < 0.0)
        dt = 0.0;
    else if (dt > RT_GAME3D_MAX_DT)
        dt = RT_GAME3D_MAX_DT;
    int wants_reverb = game3d_audio_reverb_storage_valid(audio) && audio->reverb_zone_count > 0;
    rt_game3d_ambientbed *bed = (rt_game3d_ambientbed *)rt_g3d_checked_or_null(
        audio->ambient_bed, RT_G3D_GAME3D_AMBIENTBED_CLASS_ID);
    int wants_occlusion = audio->occlusion_enabled && game3d_audio_source_storage_valid(audio) &&
                          audio->source_count > 0;
    if (!wants_reverb && !bed && !wants_occlusion)
        return;
    void *listener_obj = rt_g3d_checked_or_null(audio->listener, RT_G3D_SOUNDLISTENER3D_CLASS_ID);
    void *pos_obj = listener_obj ? rt_soundlistener3d_get_position(listener_obj) : NULL;
    if (!pos_obj)
        return;
    double listener[3] = {rt_vec3_x(pos_obj), rt_vec3_y(pos_obj), rt_vec3_z(pos_obj)};
    game3d_release_ref(&pos_obj);
    if (wants_reverb)
        game3d_audio_reverb_tick(audio, listener, dt);
    if (wants_occlusion)
        game3d_audio_occlusion_tick(world, audio, listener);
    if (bed)
        game3d_audio_ambientbed_tick(bed, listener, dt);
}
