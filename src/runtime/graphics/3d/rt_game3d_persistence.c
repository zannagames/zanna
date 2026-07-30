//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_persistence.c
// Purpose: Streamed-world entity-state persistence (plan 17) — per-entity
//   opt-in records (alive/pose/state tag), WorldStream3D cell flags and
//   loaded-cell events, and the VW3DSAV1 binary snapshot under the per-user
//   SaveData directory.
// Key invariants:
//   - Records key on explicit game-stable string keys; duplicate live keys
//     trap at SetPersistent time.
//   - The snapshot reader validates every count/length and recovers false on
//     corrupt input — it never traps (SaveData missing-file convention).
// Ownership/Lifetime:
//   - Records/flags are world- and stream-owned heap arrays freed by the
//     owning finalizers; keys are retained rt_strings.
// Links: misc/plans/thirdpersonupgrade/17-world-persistence.md, ADR 0097
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements persistent entity state, stream flags, and Game3D save snapshots.
/// @details The persistence layer captures explicitly keyed entity poses and state
/// tags, tracks per-cell stream flags and loaded-cell events, and encodes the
/// bounded little-endian `VW3DSAV1` format beneath the platform SaveData directory.
/// Snapshot validation is fail-closed and does not mutate live state.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_file_stdio.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_quat.h"
#include "rt_savedata.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERSIST3D_MAGIC "VW3DSAV1"
#define PERSIST3D_VERSION 1u
#define PERSIST3D_MAX_RECORDS 100000u
#define PERSIST3D_MAX_KEY 255u
#define PERSIST3D_MAX_BYTES (64u * 1024u * 1024u)

/// @brief Validate a non-empty runtime string for a bounded C-string-backed format field.
/// @details Comparing the runtime byte length with `strlen` rejects embedded NUL bytes instead of
/// silently truncating the stored identity.
/// @param value Borrowed runtime string.
/// @param max_bytes Maximum permitted byte length.
/// @param out_text Receives the borrowed NUL-terminated bytes on success.
/// @param out_length Optional output receiving the validated byte length.
/// @return Nonzero only for a valid handle containing 1..@p max_bytes bytes and no embedded NUL.
static int persist3d_bounded_cstr(rt_string value,
                                  size_t max_bytes,
                                  const char **out_text,
                                  size_t *out_length) {
    int64_t length;
    const char *text;
    if (!value || !out_text || !rt_string_is_handle(value))
        return 0;
    length = rt_str_len(value);
    if (length <= 0 || (uint64_t)length > (uint64_t)max_bytes)
        return 0;
    text = rt_string_cstr(value);
    if (!text || strlen(text) != (size_t)length)
        return 0;
    *out_text = text;
    if (out_length)
        *out_length = (size_t)length;
    return 1;
}

/*==========================================================================
 * Record store
 *=========================================================================*/

/// @brief Entity-array count clamped to capacity (same guard the step loops use).
/// @param world Borrowed world payload whose dense entity array is inspected.
/// @return Safe number of readable entity slots, or zero when storage is absent.
static int32_t persist3d_entity_count(const rt_game3d_world *world) {
    if (!world || world->entity_count <= 0 || world->entity_capacity <= 0 || !world->entities ||
        world->entity_count > world->entity_capacity)
        return 0;
    return world->entity_count;
}

/// @brief Find a record slot by key, or -1.
/// @param world Borrowed world containing the persistence store.
/// @param key Non-empty null-terminated key to match.
/// @return Zero-based record index, or `-1` when no key matches.
static int32_t game3d_persist_find(rt_game3d_world *world, const char *key) {
    if (!world || !key || world->persist_count < 0 || world->persist_capacity < 0 ||
        world->persist_count > world->persist_capacity ||
        (world->persist_capacity > 0 && !world->persist_records))
        return -1;
    for (int32_t i = 0; i < world->persist_count; ++i) {
        const char *have =
            world->persist_records[i].key ? rt_string_cstr(world->persist_records[i].key) : NULL;
        if (have && strcmp(have, key) == 0)
            return i;
    }
    return -1;
}

/// @brief Find-or-append a record for @p key (retains the key on append).
/// @param world Borrowed world whose record store may grow.
/// @param key Borrowed non-empty runtime string; retained when a record is appended.
/// @return Borrowed record slot, or `NULL` for an invalid key or allocation failure.
static rt_game3d_persist_record *game3d_persist_upsert(rt_game3d_world *world, rt_string key) {
    const char *ckey = NULL;
    int32_t new_capacity;
    if (!world || !persist3d_bounded_cstr(key, PERSIST3D_MAX_KEY, &ckey, NULL) ||
        world->persist_count < 0 || world->persist_capacity < 0 ||
        world->persist_count > world->persist_capacity ||
        (world->persist_capacity > 0 && !world->persist_records))
        return NULL;
    int32_t index = game3d_persist_find(world, ckey);
    if (index >= 0)
        return &world->persist_records[index];
    if (world->persist_count >= world->persist_capacity) {
        if (world->persist_count == INT32_MAX ||
            !game3d_checked_capacity_i32(world->persist_capacity,
                                         world->persist_count + 1,
                                         16,
                                         sizeof(*world->persist_records),
                                         &new_capacity))
            return NULL;
        rt_game3d_persist_record *grown = (rt_game3d_persist_record *)realloc(
            world->persist_records, (size_t)new_capacity * sizeof(*grown));
        if (!grown)
            return NULL;
        world->persist_records = grown;
        world->persist_capacity = new_capacity;
    }
    rt_game3d_persist_record *record = &world->persist_records[world->persist_count++];
    memset(record, 0, sizeof(*record));
    record->key = rt_string_ref(key);
    record->alive = 1;
    record->rotation[3] = 1.0;
    record->scale[0] = record->scale[1] = record->scale[2] = 1.0;
    return record;
}

/// @brief Copy the entity's current world pose into its record.
/// @param entity Borrowed live entity whose node pose and state tag are captured.
/// @param[out] record Borrowed persistence record to overwrite.
static void game3d_persist_capture_pose(rt_game3d_entity *entity,
                                        rt_game3d_persist_record *record) {
    void *node = game3d_entity_node_ref(entity);
    if (!node)
        return;
    double pos[3];
    if (game3d_entity_world_position_components(entity, pos))
        memcpy(record->position, pos, sizeof(pos));
    double qx, qy, qz, qw;
    if (rt_scene_node3d_get_world_rotation_components(node, &qx, &qy, &qz, &qw)) {
        record->rotation[0] = qx;
        record->rotation[1] = qy;
        record->rotation[2] = qz;
        record->rotation[3] = qw;
    }
    record->state_tag = entity->state_tag;
}

/// @brief Restore an alive record's saved pose (position + rotation) and state tag.
/// @details Rotation is applied to the entity node before the position write so the
///          subsequent body sync (game3d_sync_body_from_entity_node, invoked by
///          set_position) mirrors the full transform. The capture stores the world
///          rotation; persistent entities are top-level, so node-local == world here,
///          matching the position round-trip convention.
/// @param entity Borrowed live entity to restore.
/// @param record Borrowed alive record containing the saved pose and state tag.
static void game3d_persist_apply_alive_record(rt_game3d_entity *entity,
                                              const rt_game3d_persist_record *record) {
    void *node = game3d_entity_node_ref(entity);
    if (node) {
        void *quat = rt_quat_new(
            record->rotation[0], record->rotation[1], record->rotation[2], record->rotation[3]);
        if (quat)
            rt_scene_node3d_set_rotation(node, quat);
        game3d_release_ref(&quat);
    }
    rt_game3d_entity_set_position(
        entity, record->position[0], record->position[1], record->position[2]);
    entity->state_tag = record->state_tag;
}

/// @brief Fluent: opt the entity into persistence under a game-stable key.
/// @details Traps when another live spawned entity already holds the key.
///   When a record for the key already exists (from LoadState or an earlier
///   session of this world), an alive record is applied to the entity
///   immediately; a dead record is left for the game to check via
///   World3D.GetPersistentAlive before spawning.
/// @param obj Borrowed live Entity3D handle.
/// @param key Borrowed non-empty game-stable key no longer than 255 bytes; retained on success.
/// @return The original borrowed entity handle for fluent chaining, including on failure.
void *rt_game3d_entity_set_persistent(void *obj, rt_string key) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.SetPersistent: invalid entity");
    if (!entity)
        return obj;
    const char *ckey = NULL;
    if (!persist3d_bounded_cstr(key, PERSIST3D_MAX_KEY, &ckey, NULL)) {
        rt_trap("Game3D.Entity3D.SetPersistent: key must be 1..255 bytes without NUL");
        return obj;
    }
    rt_game3d_world *world =
        (rt_game3d_world *)rt_g3d_checked_or_null(entity->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    if (world) {
        int32_t count = persist3d_entity_count(world);
        for (int32_t i = 0; i < count; ++i) {
            rt_game3d_entity *other = world->entities[i];
            if (!other || other == entity || !other->alive || !other->persistent_key)
                continue;
            const char *other_key = rt_string_cstr(other->persistent_key);
            if (other_key && strcmp(other_key, ckey) == 0) {
                rt_trap("Game3D.Entity3D.SetPersistent: duplicate persistence key");
                return obj;
            }
        }
    }
    game3d_assign_ref((void **)&entity->persistent_key, key);
    if (world) {
        rt_game3d_persist_record *record = game3d_persist_upsert(world, key);
        if (record && record->applied_pending) {
            /* A loaded snapshot recorded this key: apply the stored state. */
            record->applied_pending = 0;
            if (record->alive) {
                game3d_persist_apply_alive_record(entity, record);
            }
        } else if (record) {
            game3d_persist_capture_pose(entity, record);
        }
    }
    return obj;
}

/// @brief The entity's persistence key ("" when not persistent).
/// @param obj Borrowed live Entity3D handle.
/// @return New reference to the retained key, or the shared empty string when unset or invalid.
rt_string rt_game3d_entity_get_persistent_key(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_PersistentKey: invalid entity");
    if (!entity || !entity->persistent_key)
        return rt_str_empty();
    return rt_string_ref(entity->persistent_key);
}

/// @brief Set the free-form persisted state tag.
/// @param obj Borrowed live Entity3D handle.
/// @param tag Caller-defined signed state value captured with the entity.
void rt_game3d_entity_set_state_tag(void *obj, int64_t tag) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.SetStateTag: invalid entity");
    if (entity)
        entity->state_tag = tag;
}

/// @brief Get the free-form persisted state tag.
/// @param obj Borrowed live Entity3D handle.
/// @return Stored state tag, or zero for an invalid or stale entity.
int64_t rt_game3d_entity_get_state_tag(void *obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(obj, "Game3D.Entity3D.get_StateTag: invalid entity");
    return entity ? entity->state_tag : 0;
}

/// @brief True when the persistence record for @p key is alive (or absent —
///   an unseen key is not yet dead).
/// @param obj Borrowed live World3D handle.
/// @param key Borrowed non-empty persistence key to query.
/// @return Nonzero for an absent or alive record; zero for a dead record or invalid input.
int8_t rt_game3d_world_get_persistent_alive(void *obj, rt_string key) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.GetPersistentAlive: invalid world");
    const char *ckey = key ? rt_string_cstr(key) : NULL;
    if (!world || !ckey || !*ckey)
        return 0;
    int32_t index = game3d_persist_find(world, ckey);
    return index < 0 ? 1 : (world->persist_records[index].alive ? 1 : 0);
}

/// @brief Last recorded world position for @p key (zero Vec3 when unknown).
/// @param obj Borrowed live World3D handle.
/// @param key Borrowed persistence key to query.
/// @return New GC-managed Vec3 containing the saved position or zero vector.
void *rt_game3d_world_get_persistent_position(void *obj, rt_string key) {
    rt_game3d_world *world =
        game3d_world_checked(obj, "Game3D.World3D.GetPersistentPosition: invalid world");
    const char *ckey = key ? rt_string_cstr(key) : NULL;
    if (world && ckey && *ckey) {
        int32_t index = game3d_persist_find(world, ckey);
        if (index >= 0)
            return rt_vec3_new(world->persist_records[index].position[0],
                               world->persist_records[index].position[1],
                               world->persist_records[index].position[2]);
    }
    return rt_vec3_new(0.0, 0.0, 0.0);
}

/// @brief Despawn hook: mark the entity's record dead with its final pose.
/// @param world Borrowed world owning the persistence record.
/// @param entity Borrowed entity being despawned; entities without keys are ignored.
void game3d_persistence_on_despawn(rt_game3d_world *world, rt_game3d_entity *entity) {
    if (!world || !entity || !entity->persistent_key)
        return;
    rt_game3d_persist_record *record = game3d_persist_upsert(world, entity->persistent_key);
    if (!record)
        return;
    game3d_persist_capture_pose(entity, record);
    record->alive = 0;
}

/// @brief Per-step capture: refresh resident persistent entities' records.
/// @param world Borrowed live world whose keyed resident entities are sampled.
void game3d_persistence_tick(rt_game3d_world *world) {
    if (!world || world->persist_count <= 0)
        return;
    int32_t count = persist3d_entity_count(world);
    for (int32_t i = 0; i < count; ++i) {
        rt_game3d_entity *entity = world->entities[i];
        if (!entity || !entity->alive || !entity->persistent_key)
            continue;
        rt_game3d_persist_record *record = game3d_persist_upsert(world, entity->persistent_key);
        if (!record)
            continue;
        record->alive = 1;
        game3d_persist_capture_pose(entity, record);
    }
}

/// @brief Release the world's persistence store (world finalizer helper).
/// @param world Borrowed world payload to clear; `NULL` is ignored.
void game3d_persistence_release(rt_game3d_world *world) {
    int32_t count;
    if (!world)
        return;
    count = world->persist_records && world->persist_count > 0 && world->persist_capacity > 0
                ? world->persist_count
                : 0;
    if (count > world->persist_capacity)
        count = world->persist_capacity;
    for (int32_t i = 0; i < count; ++i)
        game3d_release_ref((void **)&world->persist_records[i].key);
    free(world->persist_records);
    world->persist_records = NULL;
    world->persist_count = 0;
    world->persist_capacity = 0;
}

/*==========================================================================
 * WorldStream3D cell flags + loaded-cell events
 *=========================================================================*/

/// @brief Set a per-cell integer flag (door-opened / chest-looted state).
/// @param obj Borrowed WorldStream3D handle.
/// @param cell Borrowed non-empty cell name, retained when a flag is appended.
/// @param key Borrowed non-empty flag name, retained when a flag is appended.
/// @param value Signed game-defined value to store.
void rt_game3d_world_stream_set_cell_flag(void *obj, rt_string cell, rt_string key, int64_t value) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    const char *ccell = NULL;
    const char *ckey = NULL;
    int32_t new_capacity;
    if (!stream) {
        rt_trap("Game3D.WorldStream3D.SetCellFlag: invalid stream");
        return;
    }
    if (!persist3d_bounded_cstr(cell, PERSIST3D_MAX_KEY, &ccell, NULL) ||
        !persist3d_bounded_cstr(key, PERSIST3D_MAX_KEY, &ckey, NULL)) {
        rt_trap("Game3D.WorldStream3D.SetCellFlag: cell and key must be 1..255 bytes without NUL");
        return;
    }
    if (stream->cell_flag_count < 0 || stream->cell_flag_capacity < 0 ||
        stream->cell_flag_count > stream->cell_flag_capacity ||
        (stream->cell_flag_capacity > 0 && !stream->cell_flags)) {
        rt_trap("Game3D.WorldStream3D.SetCellFlag: corrupt flag storage");
        return;
    }
    for (int32_t i = 0; i < stream->cell_flag_count; ++i) {
        rt_game3d_cell_flag *flag = &stream->cell_flags[i];
        const char *stored_cell = flag->cell ? rt_string_cstr(flag->cell) : NULL;
        const char *stored_key = flag->key ? rt_string_cstr(flag->key) : NULL;
        if (stored_cell && stored_key && strcmp(stored_cell, ccell) == 0 &&
            strcmp(stored_key, ckey) == 0) {
            flag->value = value;
            return;
        }
    }
    if (stream->cell_flag_count >= stream->cell_flag_capacity) {
        if (stream->cell_flag_count == INT32_MAX ||
            !game3d_checked_capacity_i32(stream->cell_flag_capacity,
                                         stream->cell_flag_count + 1,
                                         16,
                                         sizeof(*stream->cell_flags),
                                         &new_capacity)) {
            rt_trap("Game3D.WorldStream3D.SetCellFlag: capacity overflow");
            return;
        }
        rt_game3d_cell_flag *grown = (rt_game3d_cell_flag *)realloc(
            stream->cell_flags, (size_t)new_capacity * sizeof(*grown));
        if (!grown) {
            rt_trap("Game3D.WorldStream3D.SetCellFlag: allocation failed");
            return;
        }
        stream->cell_flags = grown;
        stream->cell_flag_capacity = new_capacity;
    }
    rt_game3d_cell_flag *flag = &stream->cell_flags[stream->cell_flag_count++];
    flag->cell = rt_string_ref(cell);
    flag->key = rt_string_ref(key);
    flag->value = value;
}

/// @brief Get a per-cell integer flag (0 when unset).
/// @param obj Borrowed WorldStream3D handle.
/// @param cell Borrowed cell name.
/// @param key Borrowed flag name.
/// @return Stored flag value, or zero when absent or invalid.
int64_t rt_game3d_world_stream_get_cell_flag(void *obj, rt_string cell, rt_string key) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    const char *ccell = NULL;
    const char *ckey = NULL;
    if (!stream || !persist3d_bounded_cstr(cell, PERSIST3D_MAX_KEY, &ccell, NULL) ||
        !persist3d_bounded_cstr(key, PERSIST3D_MAX_KEY, &ckey, NULL) ||
        stream->cell_flag_count < 0 || stream->cell_flag_capacity < 0 ||
        stream->cell_flag_count > stream->cell_flag_capacity ||
        (stream->cell_flag_capacity > 0 && !stream->cell_flags))
        return 0;
    for (int32_t i = 0; i < stream->cell_flag_count; ++i) {
        rt_game3d_cell_flag *flag = &stream->cell_flags[i];
        const char *stored_cell = flag->cell ? rt_string_cstr(flag->cell) : NULL;
        const char *stored_key = flag->key ? rt_string_cstr(flag->key) : NULL;
        if (stored_cell && stored_key && strcmp(stored_cell, ccell) == 0 &&
            strcmp(stored_key, ckey) == 0)
            return flag->value;
    }
    return 0;
}

/// @brief Streaming hook: record a just-loaded cell name for game polling.
/// @details When the fixed event queue is full, releases and drops the oldest event.
/// @param stream Borrowed WorldStream3D payload receiving the event.
/// @param cell_name Borrowed cell name retained by the queue.
void game3d_stream_push_loaded_event(rt_game3d_world_stream *stream, rt_string cell_name) {
    if (!stream || !cell_name)
        return;
    if (stream->loaded_event_count < 0 ||
        stream->loaded_event_count > RT_GAME3D_STREAM_MAX_LOADED_EVENTS)
        stream->loaded_event_count = 0;
    if (stream->loaded_event_count >= RT_GAME3D_STREAM_MAX_LOADED_EVENTS) {
        /* Drop the oldest: shift down (ring would also work; N is tiny). */
        game3d_release_ref((void **)&stream->loaded_events[0]);
        memmove(&stream->loaded_events[0],
                &stream->loaded_events[1],
                (RT_GAME3D_STREAM_MAX_LOADED_EVENTS - 1) * sizeof(rt_string));
        stream->loaded_event_count = RT_GAME3D_STREAM_MAX_LOADED_EVENTS - 1;
    }
    stream->loaded_events[stream->loaded_event_count++] = rt_string_ref(cell_name);
}

/// @brief Number of buffered loaded-cell events.
/// @param obj Borrowed WorldStream3D handle.
/// @return Current event count, or zero for an invalid handle.
int64_t rt_game3d_world_stream_loaded_event_count(void *obj) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    if (!stream || stream->loaded_event_count < 0 ||
        stream->loaded_event_count > RT_GAME3D_STREAM_MAX_LOADED_EVENTS)
        return 0;
    return stream->loaded_event_count;
}

/// @brief Cell name of buffered loaded-cell event @p index ("" out of range).
/// @param obj Borrowed WorldStream3D handle.
/// @param index Zero-based event index.
/// @return New reference to the cell name, or the shared empty string when out of range.
rt_string rt_game3d_world_stream_loaded_event(void *obj, int64_t index) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    if (!stream || stream->loaded_event_count < 0 ||
        stream->loaded_event_count > RT_GAME3D_STREAM_MAX_LOADED_EVENTS || index < 0 ||
        index >= stream->loaded_event_count || !stream->loaded_events[index])
        return rt_str_empty();
    return rt_string_ref(stream->loaded_events[index]);
}

/// @brief Clear the buffered loaded-cell events (games poll then clear).
/// @param obj Borrowed WorldStream3D handle; invalid handles are ignored.
void rt_game3d_world_stream_clear_loaded_events(void *obj) {
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        obj, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    if (!stream)
        return;
    int32_t count = stream->loaded_event_count >= 0 &&
                            stream->loaded_event_count <= RT_GAME3D_STREAM_MAX_LOADED_EVENTS
                        ? stream->loaded_event_count
                        : 0;
    for (int32_t i = 0; i < count; ++i)
        game3d_release_ref((void **)&stream->loaded_events[i]);
    stream->loaded_event_count = 0;
}

/// @brief Release stream persistence state (stream finalizer helper).
/// @param stream Borrowed WorldStream3D payload whose flags and events are cleared.
void game3d_stream_persistence_release(rt_game3d_world_stream *stream) {
    int32_t flag_count;
    int32_t event_count;
    if (!stream)
        return;
    flag_count = stream->cell_flags && stream->cell_flag_count > 0 && stream->cell_flag_capacity > 0
                     ? stream->cell_flag_count
                     : 0;
    if (flag_count > stream->cell_flag_capacity)
        flag_count = stream->cell_flag_capacity;
    for (int32_t i = 0; i < flag_count; ++i) {
        game3d_release_ref((void **)&stream->cell_flags[i].cell);
        game3d_release_ref((void **)&stream->cell_flags[i].key);
    }
    free(stream->cell_flags);
    stream->cell_flags = NULL;
    stream->cell_flag_count = 0;
    stream->cell_flag_capacity = 0;
    event_count = stream->loaded_event_count >= 0 &&
                          stream->loaded_event_count <= RT_GAME3D_STREAM_MAX_LOADED_EVENTS
                      ? stream->loaded_event_count
                      : 0;
    for (int32_t i = 0; i < event_count; ++i)
        game3d_release_ref((void **)&stream->loaded_events[i]);
    stream->loaded_event_count = 0;
}

/*==========================================================================
 * VW3DSAV1 snapshot
 *=========================================================================*/

/// @brief Growable in-memory encoder with a sticky allocation-failure flag.
typedef struct persist3d_writer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} persist3d_writer;

/// @brief Append raw bytes to a snapshot writer.
/// @param[in,out] writer Writer whose storage grows as needed and whose failure flag is sticky.
/// @param bytes Source byte range to copy.
/// @param count Number of bytes to append.
static void persist3d_write(persist3d_writer *writer, const void *bytes, size_t count) {
    size_t needed;
    size_t new_capacity;
    if (!writer || writer->failed)
        return;
    if ((count > 0 && !bytes) || writer->size > writer->capacity ||
        writer->capacity > PERSIST3D_MAX_BYTES || (writer->capacity > 0 && !writer->data) ||
        writer->size > PERSIST3D_MAX_BYTES || count > PERSIST3D_MAX_BYTES - writer->size) {
        writer->failed = 1;
        return;
    }
    needed = writer->size + count;
    if (needed > writer->capacity) {
        new_capacity = writer->capacity > 0 ? writer->capacity : 1024u;
        while (new_capacity < needed) {
            if (new_capacity > PERSIST3D_MAX_BYTES / 2u) {
                new_capacity = PERSIST3D_MAX_BYTES;
                break;
            }
            new_capacity *= 2u;
        }
        if (new_capacity < needed) {
            writer->failed = 1;
            return;
        }
        uint8_t *grown = (uint8_t *)realloc(writer->data, new_capacity);
        if (!grown) {
            writer->failed = 1;
            return;
        }
        writer->data = grown;
        writer->capacity = new_capacity;
    }
    if (count > 0)
        memcpy(writer->data + writer->size, bytes, count);
    writer->size = needed;
}

/// @brief Encode an unsigned 32-bit integer in little-endian order.
/// @param[in,out] writer Destination snapshot writer.
/// @param value Integer value to append.
static void persist3d_write_u32(persist3d_writer *writer, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)(value & 0xFF),
                        (uint8_t)((value >> 8) & 0xFF),
                        (uint8_t)((value >> 16) & 0xFF),
                        (uint8_t)((value >> 24) & 0xFF)};
    persist3d_write(writer, bytes, 4);
}

/// @brief Encode an unsigned 64-bit integer in little-endian order.
/// @param[in,out] writer Destination snapshot writer.
/// @param value Integer value to append.
static void persist3d_write_u64(persist3d_writer *writer, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i)
        bytes[i] = (uint8_t)((value >> (8 * i)) & 0xFF);
    persist3d_write(writer, bytes, 8);
}

/// @brief Encode a signed 64-bit integer in little-endian two's-complement order.
/// @param[in,out] writer Destination snapshot writer.
/// @param value Integer value to append.
static void persist3d_write_i64(persist3d_writer *writer, int64_t value) {
    persist3d_write_u64(writer, (uint64_t)value);
}

/// @brief Encode an IEEE-754 double through its little-endian 64-bit representation.
/// @param[in,out] writer Destination snapshot writer.
/// @param value Floating-point value to append.
static void persist3d_write_f64(persist3d_writer *writer, double value) {
    uint64_t raw;
    if (!writer || !isfinite(value)) {
        if (writer)
            writer->failed = 1;
        return;
    }
    memcpy(&raw, &value, sizeof(raw));
    persist3d_write_u64(writer, raw);
}

/// @brief Encode a runtime string as a bounded length-prefixed byte sequence.
/// @param[in,out] writer Destination snapshot writer.
/// @param value Borrowed non-empty runtime string no longer than the format key limit.
static void persist3d_write_str(persist3d_writer *writer, rt_string value) {
    const char *text = NULL;
    size_t len = 0;
    if (!writer || !persist3d_bounded_cstr(value, PERSIST3D_MAX_KEY, &text, &len)) {
        if (writer)
            writer->failed = 1;
        return;
    }
    persist3d_write_u32(writer, (uint32_t)len);
    persist3d_write(writer, text, len);
}

/// @brief Bounds-checked in-memory decoder with a sticky parse-failure flag.
typedef struct persist3d_reader {
    const uint8_t *data;
    size_t size;
    size_t cursor;
    int failed;
} persist3d_reader;

/// @brief Copy a fixed byte count from the current decoder cursor.
/// @param[in,out] reader Reader whose cursor advances on success and whose failure flag is sticky.
/// @param[out] out Destination byte range.
/// @param count Number of bytes required.
/// @return Nonzero on success; zero after truncation or an earlier parse failure.
static int persist3d_read(persist3d_reader *reader, void *out, size_t count) {
    if (!reader)
        return 0;
    if (reader->failed || (count > 0 && (!reader->data || !out)) || reader->cursor > reader->size ||
        count > reader->size - reader->cursor) {
        reader->failed = 1;
        return 0;
    }
    if (count > 0)
        memcpy(out, reader->data + reader->cursor, count);
    reader->cursor += count;
    return 1;
}

/// @brief Decode a little-endian unsigned 32-bit integer.
/// @param[in,out] reader Source snapshot reader.
/// @return Decoded value, or zero after a parse failure.
static uint32_t persist3d_read_u32(persist3d_reader *reader) {
    uint8_t bytes[4] = {0};
    if (!persist3d_read(reader, bytes, 4))
        return 0;
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/// @brief Decode a little-endian unsigned 64-bit integer.
/// @param[in,out] reader Source snapshot reader.
/// @return Decoded value, or zero after a parse failure.
static uint64_t persist3d_read_u64(persist3d_reader *reader) {
    uint8_t bytes[8] = {0};
    if (!persist3d_read(reader, bytes, 8))
        return 0;
    uint64_t raw = 0;
    for (int i = 0; i < 8; ++i)
        raw |= (uint64_t)bytes[i] << (8 * i);
    return raw;
}

/// @brief Decode a little-endian signed 64-bit integer without implementation-defined narrowing.
/// @param[in,out] reader Source snapshot reader.
/// @return Decoded two's-complement bit pattern, or zero after a parse failure.
static int64_t persist3d_read_i64(persist3d_reader *reader) {
    uint64_t raw = persist3d_read_u64(reader);
    if (raw <= (uint64_t)INT64_MAX)
        return (int64_t)raw;
    return -1 - (int64_t)(UINT64_MAX - raw);
}

/// @brief Decode an IEEE-754 double from its little-endian 64-bit representation.
/// @param[in,out] reader Source snapshot reader.
/// @return Decoded floating-point value; callers inspect the reader or validate finiteness.
static double persist3d_read_f64(persist3d_reader *reader) {
    double value;
    uint64_t bits = persist3d_read_u64(reader);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/// @brief Read a length-prefixed string into a bounded stack buffer.
/// @param[in,out] reader Source snapshot reader.
/// @param[out] out Destination buffer receiving a null-terminated key.
/// @param out_size Available destination bytes including the terminator.
/// @return Nonzero on success; zero when the length or remaining input is invalid.
static int persist3d_read_key(persist3d_reader *reader, char *out, size_t out_size) {
    if (!reader || !out || out_size == 0) {
        if (reader)
            reader->failed = 1;
        return 0;
    }
    uint32_t len = persist3d_read_u32(reader);
    if (reader->failed || len == 0 || len > PERSIST3D_MAX_KEY || len + 1 > out_size) {
        reader->failed = 1;
        return 0;
    }
    if (!persist3d_read(reader, out, len))
        return 0;
    if (memchr(out, '\0', len)) {
        reader->failed = 1;
        return 0;
    }
    out[len] = '\0';
    return 1;
}

/// @brief Validate a VW3DSAV1 buffer without applying it (fuzz surface).
/// @param data Borrowed snapshot byte range.
/// @param size Number of bytes in @p data; inputs larger than 64 MiB are rejected.
/// @return 1 when the buffer parses cleanly, 0 otherwise. Never traps.
int8_t rt_game3d_persistence_validate(const void *data, int64_t size) {
    if (!data || size < 16 || size > (int64_t)PERSIST3D_MAX_BYTES)
        return 0;
    persist3d_reader reader = {(const uint8_t *)data, (size_t)size, 0, 0};
    char magic[8];
    if (!persist3d_read(&reader, magic, 8) || memcmp(magic, PERSIST3D_MAGIC, 8) != 0)
        return 0;
    uint32_t version = persist3d_read_u32(&reader);
    if (reader.failed || version != PERSIST3D_VERSION)
        return 0;
    uint32_t record_count = persist3d_read_u32(&reader);
    uint32_t flag_count = persist3d_read_u32(&reader);
    if (reader.failed || record_count > PERSIST3D_MAX_RECORDS || flag_count > PERSIST3D_MAX_RECORDS)
        return 0;
    for (int i = 0; i < 4; ++i) {
        /* origin xyz + elapsed: reject non-finite so NaN/Inf from a corrupt but
         * structurally-valid file cannot poison streaming distance / time-of-day. */
        if (!isfinite(persist3d_read_f64(&reader)))
            return 0;
    }
    char key[PERSIST3D_MAX_KEY + 1];
    for (uint32_t r = 0; r < record_count && !reader.failed; ++r) {
        if (!persist3d_read_key(&reader, key, sizeof(key)))
            return 0;
        uint8_t alive;
        if (!persist3d_read(&reader, &alive, 1) || alive > 1)
            return 0;
        for (int i = 0; i < 10; ++i) {
            if (!isfinite(persist3d_read_f64(&reader))) /* pos3 + quat4 + scale3 */
                return 0;
        }
        (void)persist3d_read_i64(&reader); /* tag */
    }
    for (uint32_t f = 0; f < flag_count && !reader.failed; ++f) {
        if (!persist3d_read_key(&reader, key, sizeof(key)))
            return 0;
        if (!persist3d_read_key(&reader, key, sizeof(key)))
            return 0;
        (void)persist3d_read_i64(&reader);
    }
    return reader.failed || reader.cursor != reader.size ? 0 : 1;
}

/// @brief Release a staging array of decoded persistence records.
/// @param records Owned array, or `NULL`.
/// @param count Number of initialized records in @p records.
static void persist3d_release_record_array(rt_game3d_persist_record *records, int32_t count) {
    if (!records)
        return;
    for (int32_t i = 0; i < count; ++i)
        game3d_release_ref((void **)&records[i].key);
    free(records);
}

/// @brief Release a staging array of decoded stream flags.
/// @param flags Owned array, or `NULL`.
/// @param count Number of initialized flags in @p flags.
static void persist3d_release_flag_array(rt_game3d_cell_flag *flags, int32_t count) {
    if (!flags)
        return;
    for (int32_t i = 0; i < count; ++i) {
        game3d_release_ref((void **)&flags[i].cell);
        game3d_release_ref((void **)&flags[i].key);
    }
    free(flags);
}

/// @brief Sort decoded entity records by key for deterministic duplicate detection.
static int persist3d_compare_records(const void *lhs, const void *rhs) {
    const rt_game3d_persist_record *a = (const rt_game3d_persist_record *)lhs;
    const rt_game3d_persist_record *b = (const rt_game3d_persist_record *)rhs;
    return strcmp(rt_string_cstr(a->key), rt_string_cstr(b->key));
}

/// @brief Sort decoded stream flags by `(cell,key)` for deterministic duplicate detection.
static int persist3d_compare_flags(const void *lhs, const void *rhs) {
    const rt_game3d_cell_flag *a = (const rt_game3d_cell_flag *)lhs;
    const rt_game3d_cell_flag *b = (const rt_game3d_cell_flag *)rhs;
    int cell_order = strcmp(rt_string_cstr(a->cell), rt_string_cstr(b->cell));
    return cell_order != 0 ? cell_order : strcmp(rt_string_cstr(a->key), rt_string_cstr(b->key));
}

/// @brief Compose "<data dir>/<slot>.vw3dsav"; NULL on invalid names (trapped
///   by rt_path_data_dir for the app component).
/// @param app_name Borrowed application name used to resolve the platform data directory.
/// @param slot Borrowed 1-to-64-character alphanumeric, hyphen, or underscore slot name.
/// @return Heap path owned by the caller, or `NULL` for invalid input or allocation failure.
static char *persist3d_slot_path(rt_string app_name, rt_string slot) {
    const char *cslot = NULL;
    size_t dir_len;
    size_t slot_len;
    size_t needed;
    if (!persist3d_bounded_cstr(slot, 64u, &cslot, &slot_len))
        return NULL;
    for (const char *p = cslot; *p; ++p) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok)
            return NULL;
    }
    rt_string dir = rt_path_data_dir(app_name);
    if (!dir)
        return NULL;
    const char *cdir = rt_string_cstr(dir);
    dir_len = strlen(cdir);
    if (dir_len > SIZE_MAX - slot_len - sizeof(".vw3dsav") - 1u) {
        game3d_release_ref((void **)&dir);
        return NULL;
    }
    needed = dir_len + 1u + slot_len + sizeof(".vw3dsav");
    char *path = (char *)malloc(needed);
    if (path)
        snprintf(path, needed, "%s/%s.vw3dsav", cdir, cslot);
    game3d_release_ref((void **)&dir);
    return path;
}

/// @brief Serialize the delta store to "<data dir>/<slot>.vw3dsav" atomically.
/// @param obj Borrowed live World3D handle.
/// @param app_name Borrowed application name used for the platform data directory.
/// @param slot Borrowed validated save-slot name.
/// @return 1 on success, 0 on any IO/encoding failure (no partial files).
int8_t rt_game3d_world_save_state(void *obj, rt_string app_name, rt_string slot) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.World3D.SaveState: invalid world");
    int32_t persist_count;
    int32_t cell_flag_count = 0;
    if (!world || world->persist_count < 0 || world->persist_capacity < 0 ||
        world->persist_count > world->persist_capacity ||
        (world->persist_capacity > 0 && !world->persist_records) ||
        world->persist_count > (int32_t)PERSIST3D_MAX_RECORDS)
        return 0;
    persist_count = world->persist_count;
    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        world->stream, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    if (stream) {
        if (stream->cell_flag_count < 0 || stream->cell_flag_capacity < 0 ||
            stream->cell_flag_count > stream->cell_flag_capacity ||
            (stream->cell_flag_capacity > 0 && !stream->cell_flags) ||
            stream->cell_flag_count > (int32_t)PERSIST3D_MAX_RECORDS)
            return 0;
        cell_flag_count = stream->cell_flag_count;
    }

    persist3d_writer writer = {NULL, 0, 0, 0};
    persist3d_write(&writer, PERSIST3D_MAGIC, 8);
    persist3d_write_u32(&writer, PERSIST3D_VERSION);
    persist3d_write_u32(&writer, (uint32_t)persist_count);
    persist3d_write_u32(&writer, (uint32_t)cell_flag_count);
    persist3d_write_f64(&writer, world->world_origin[0]);
    persist3d_write_f64(&writer, world->world_origin[1]);
    persist3d_write_f64(&writer, world->world_origin[2]);
    persist3d_write_f64(&writer, world->elapsed);
    for (int32_t i = 0; i < persist_count; ++i) {
        rt_game3d_persist_record *record = &world->persist_records[i];
        persist3d_write_str(&writer, record->key);
        uint8_t alive = record->alive ? 1 : 0;
        persist3d_write(&writer, &alive, 1);
        for (int k = 0; k < 3; ++k)
            persist3d_write_f64(&writer, record->position[k]);
        for (int k = 0; k < 4; ++k)
            persist3d_write_f64(&writer, record->rotation[k]);
        for (int k = 0; k < 3; ++k)
            persist3d_write_f64(&writer, record->scale[k]);
        persist3d_write_i64(&writer, record->state_tag);
    }
    if (stream) {
        for (int32_t i = 0; i < cell_flag_count; ++i) {
            persist3d_write_str(&writer, stream->cell_flags[i].cell);
            persist3d_write_str(&writer, stream->cell_flags[i].key);
            persist3d_write_i64(&writer, stream->cell_flags[i].value);
        }
    }
    if (writer.failed) {
        free(writer.data);
        return 0;
    }

    char *path = persist3d_slot_path(app_name, slot);
    if (!path) {
        free(writer.data);
        return 0;
    }
    char *tmp_path = NULL;
    int8_t ok = 0;
    FILE *file = rt_file_stdio_open_temp_for_replace_utf8(path, &tmp_path);
    if (file) {
        size_t written = writer.size ? fwrite(writer.data, 1, writer.size, file) : 0;
        int closed = rt_file_stdio_flush_sync_close(file);
        if (written == writer.size && closed)
            ok = rt_file_stdio_replace_utf8(tmp_path, path) ? 1 : 0;
        if (!ok)
            (void)rt_file_stdio_unlink_utf8(tmp_path);
    }
    free(tmp_path);
    free(path);
    free(writer.data);
    return ok;
}

/// @brief Load "<data dir>/<slot>.vw3dsav": replaces the delta store and cell
///   flags, re-poses/kills resident persistent entities, restores extras.
/// @param obj Borrowed live World3D handle to restore.
/// @param app_name Borrowed application name used for the platform data directory.
/// @param slot Borrowed validated save-slot name.
/// @return 1 on success; 0 on missing/corrupt file (state unchanged).
int8_t rt_game3d_world_load_state(void *obj, rt_string app_name, rt_string slot) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.World3D.LoadState: invalid world");
    rt_game3d_persist_record *staged_records = NULL;
    rt_game3d_cell_flag *staged_flags = NULL;
    int32_t staged_record_count = 0;
    int32_t staged_flag_count = 0;
    if (!world)
        return 0;
    char *path = persist3d_slot_path(app_name, slot);
    if (!path)
        return 0;
    FILE *file = rt_file_stdio_open_utf8(path, "rb");
    free(path);
    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0 || file_size > (long)PERSIST3D_MAX_BYTES) {
        fclose(file);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)file_size);
    if (!data || fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    if (!rt_game3d_persistence_validate(data, file_size)) {
        free(data);
        return 0;
    }

    persist3d_reader reader = {data, (size_t)file_size, 0, 0};
    char magic[8];
    persist3d_read(&reader, magic, 8);
    (void)persist3d_read_u32(&reader); /* version (validated) */
    uint32_t record_count = persist3d_read_u32(&reader);
    uint32_t flag_count = persist3d_read_u32(&reader);
    double origin[3];
    origin[0] = persist3d_read_f64(&reader);
    origin[1] = persist3d_read_f64(&reader);
    origin[2] = persist3d_read_f64(&reader);
    double elapsed = persist3d_read_f64(&reader);

    if (record_count > (uint32_t)INT32_MAX ||
        (size_t)record_count > SIZE_MAX / sizeof(*staged_records) ||
        flag_count > (uint32_t)INT32_MAX || (size_t)flag_count > SIZE_MAX / sizeof(*staged_flags))
        goto load_failed;
    if (record_count > 0) {
        staged_records =
            (rt_game3d_persist_record *)calloc((size_t)record_count, sizeof(*staged_records));
        if (!staged_records)
            goto load_failed;
    }
    if (flag_count > 0) {
        staged_flags = (rt_game3d_cell_flag *)calloc((size_t)flag_count, sizeof(*staged_flags));
        if (!staged_flags)
            goto load_failed;
    }

    char key[PERSIST3D_MAX_KEY + 1];
    for (uint32_t r = 0; r < record_count; ++r) {
        rt_game3d_persist_record *record = &staged_records[r];
        uint8_t alive = 0;
        if (!persist3d_read_key(&reader, key, sizeof(key)) || !persist3d_read(&reader, &alive, 1))
            goto load_failed;
        record->key = rt_const_cstr(key);
        if (!record->key)
            goto load_failed;
        staged_record_count++;
        record->alive = alive ? 1 : 0;
        record->applied_pending = 1;
        for (int k = 0; k < 3; ++k)
            record->position[k] = persist3d_read_f64(&reader);
        for (int k = 0; k < 4; ++k)
            record->rotation[k] = persist3d_read_f64(&reader);
        for (int k = 0; k < 3; ++k)
            record->scale[k] = persist3d_read_f64(&reader);
        record->state_tag = persist3d_read_i64(&reader);
        if (reader.failed)
            goto load_failed;
    }
    for (uint32_t f = 0; f < flag_count; ++f) {
        rt_game3d_cell_flag *flag = &staged_flags[f];
        char cell[PERSIST3D_MAX_KEY + 1];
        if (!persist3d_read_key(&reader, cell, sizeof(cell)) ||
            !persist3d_read_key(&reader, key, sizeof(key)))
            goto load_failed;
        flag->cell = rt_const_cstr(cell);
        if (!flag->cell)
            goto load_failed;
        staged_flag_count++;
        flag->key = rt_const_cstr(key);
        if (!flag->key)
            goto load_failed;
        flag->value = persist3d_read_i64(&reader);
        if (reader.failed)
            goto load_failed;
    }
    if (reader.cursor != reader.size)
        goto load_failed;

    if (staged_record_count > 1) {
        qsort(staged_records,
              (size_t)staged_record_count,
              sizeof(*staged_records),
              persist3d_compare_records);
        for (int32_t i = 1; i < staged_record_count; ++i)
            if (persist3d_compare_records(&staged_records[i - 1], &staged_records[i]) == 0)
                goto load_failed;
    }
    if (staged_flag_count > 1) {
        qsort(staged_flags,
              (size_t)staged_flag_count,
              sizeof(*staged_flags),
              persist3d_compare_flags);
        for (int32_t i = 1; i < staged_flag_count; ++i)
            if (persist3d_compare_flags(&staged_flags[i - 1], &staged_flags[i]) == 0)
                goto load_failed;
    }

    rt_game3d_world_stream *stream = (rt_game3d_world_stream *)rt_g3d_checked_or_null(
        world->stream, RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID);
    game3d_persistence_release(world);
    world->persist_records = staged_records;
    world->persist_count = staged_record_count;
    world->persist_capacity = staged_record_count;
    staged_records = NULL;
    if (stream) {
        game3d_stream_persistence_release(stream);
        stream->cell_flags = staged_flags;
        stream->cell_flag_count = staged_flag_count;
        stream->cell_flag_capacity = staged_flag_count;
        staged_flags = NULL;
    }
    world->world_origin[0] = origin[0];
    world->world_origin[1] = origin[1];
    world->world_origin[2] = origin[2];
    world->elapsed = elapsed;

    /* Apply to resident persistent entities immediately. */
    int32_t count = persist3d_entity_count(world);
    for (int32_t i = count - 1; i >= 0; --i) {
        rt_game3d_entity *entity = world->entities[i];
        if (!entity || !entity->alive || !entity->persistent_key)
            continue;
        const char *ckey = rt_string_cstr(entity->persistent_key);
        int32_t index = ckey ? game3d_persist_find(world, ckey) : -1;
        if (index < 0)
            continue;
        rt_game3d_persist_record *record = &world->persist_records[index];
        record->applied_pending = 0;
        if (!record->alive) {
            rt_game3d_world_despawn(world, entity);
        } else {
            game3d_persist_apply_alive_record(entity, record);
        }
    }
    persist3d_release_flag_array(staged_flags, staged_flag_count);
    free(data);
    return 1;

load_failed:
    persist3d_release_record_array(staged_records, staged_record_count);
    persist3d_release_flag_array(staged_flags, staged_flag_count);
    free(data);
    return 0;
}

#else
typedef int rt_game3d_persistence_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
