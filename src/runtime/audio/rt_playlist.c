//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_playlist.c
// Purpose: Implements a music playlist for the Zanna.Sound.Playlist class.
//          Supports sequential and shuffle playback, repeat modes (none/all/one),
//          volume control, and track navigation (Next, Prev, Seek). Delegates
//          actual audio decoding and playback to rt_audio.
//
// Key invariants:
//   - Track indices are zero-based; current == -1 means no track is loaded.
//   - Shuffle mode generates a random permutation of indices at play-start.
//   - Repeat=none stops at end; repeat=all wraps to track 0; repeat=one loops.
//   - Poll must be called periodically to advance to the next track when done.
//   - Volume range is 0-100; values outside this range are clamped.
//   - The shuffle_order sequence is regenerated each time shuffle is toggled on.
//
// Ownership/Lifetime:
//   - The playlist retains a reference to the current Music object while playing.
//   - Track path strings in the tracks sequence are retained by the sequence.
//   - The playlist object is heap-allocated and managed by the runtime GC.
//
// Links: src/runtime/audio/rt_playlist.h (public API),
//        src/runtime/audio/rt_audio.h (music load/play/stop primitives)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime playlist object and crossfade-aware navigation.
/// @details Playlists own a sequence of retained track-path strings, optionally
///          maintain a Fisher-Yates permutation, and retain at most one current
///          Music wrapper outside crossfade state. Track mutations preserve the
///          logical current item where possible, while play/advance paths skip
///          unloadable entries within a bounded number of attempts.

#include "rt_playlist.h"
#include "rt_audio.h"
#include "rt_internal.h"
#include "rt_mixgroup.h"
#include "rt_object.h"
#include "rt_random.h"
#include "rt_seq.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

//=============================================================================
// Internal Structure
//=============================================================================

#define RT_PLAYLIST_CLASS_ID INT64_C(-0x730102)

typedef struct {
    void *tracks;         // Seq of path strings
    int64_t current;      // Current track index (-1 if none)
    void *music;          // Currently loaded Music object
    int64_t volume;       // Playback volume (0-100)
    int8_t shuffle;       // Shuffle mode
    int64_t repeat;       // RT_REPEAT_NONE, RT_REPEAT_ALL, or RT_REPEAT_ONE
    int8_t playing;       // Currently playing
    int8_t paused;        // Paused state
    void *shuffle_order;  // Shuffled index sequence
    int64_t crossfade_ms; // Crossfade duration for track changes (0 = disabled)
} playlist_impl;

//=============================================================================
// Helper Functions
//=============================================================================

/// @brief Validate and cast an opaque runtime object to playlist storage.
/// @param obj Opaque object to examine.
/// @return Mutable playlist implementation, or NULL when the class identifier
///         or allocation size does not match.
static playlist_impl *as_playlist(void *obj) {
    if (!rt_obj_is_instance(obj, RT_PLAYLIST_CLASS_ID, sizeof(playlist_impl)))
        return NULL;
    return (playlist_impl *)obj;
}

/// @brief Build a fresh Fisher-Yates shuffle order for the current track list.
/// @details Reuses the global runtime RNG so callers can pre-seed for
///          deterministic playback. Releases any previously-built order
///          and swaps it in only after the replacement is complete. Traps on
///          OOM (allocating the temporary indices array or Seq failing). The
///          shuffle sequence stores `actual_index` values that
///          @ref get_track_index translates into track positions.
/// @param pl Playlist instance (not NULL).
static void generate_shuffle_order(playlist_impl *pl) {
    void *new_order = NULL;
    int64_t *indices = NULL;
    int64_t count = rt_seq_len(pl->tracks);
    if (count <= 0) {
        if (pl->shuffle_order) {
            if (rt_obj_release_check0(pl->shuffle_order))
                rt_obj_free(pl->shuffle_order);
            pl->shuffle_order = NULL;
        }
        return;
    }

    new_order = rt_seq_with_capacity(count);
    if (!new_order) {
        rt_trap("rt_playlist: memory allocation failed");
        return;
    }

    // Create sequential order first
    if ((uint64_t)count > SIZE_MAX / sizeof(int64_t)) {
        if (rt_obj_release_check0(new_order))
            rt_obj_free(new_order);
        rt_trap("rt_playlist: shuffle order allocation overflow");
        return;
    }
    indices = (int64_t *)malloc(count * sizeof(int64_t));
    if (!indices) {
        if (rt_obj_release_check0(new_order))
            rt_obj_free(new_order);
        rt_trap("rt_playlist: memory allocation failed");
        return;
    }
    for (int64_t i = 0; i < count; i++) {
        indices[i] = i;
    }

    // Fisher-Yates shuffle using the runtime RNG; callers can seed it for determinism.
    for (int64_t i = count - 1; i > 0; i--) {
        int64_t j = (int64_t)rt_rand_int((long long)(i + 1));
        int64_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    // Store in shuffle_order seq
    for (int64_t i = 0; i < count; i++) {
        rt_seq_push(new_order, (void *)indices[i]);
    }

    free(indices);
    if (pl->shuffle_order) {
        if (rt_obj_release_check0(pl->shuffle_order))
            rt_obj_free(pl->shuffle_order);
    }
    pl->shuffle_order = new_order;
}

/// @brief Translate a playback position into an "actual" track index.
/// @details When shuffle is on, @p position is an index into the
///          shuffle sequence which holds the randomised track order;
///          when off, @p position is the track index directly. Used
///          by every load/skip/jump path so the caller never needs to
///          branch on shuffle state.
/// @param pl       Playlist instance.
/// @param position Playback position (current or skip-target).
/// @return Track-list index, or @p position when shuffle is inactive
///         or @p position is out of range.
static int64_t get_track_index(playlist_impl *pl, int64_t position) {
    if (pl->shuffle && pl->shuffle_order) {
        int64_t count = rt_seq_len(pl->shuffle_order);
        if (position >= 0 && position < count) {
            return (int64_t)(intptr_t)rt_seq_get(pl->shuffle_order, position);
        }
    }
    return position;
}

/// @brief Stop the currently-loaded music handle and release the wrapper.
/// @details Combines @ref rt_music_stop_related (which also clears any
///          related crossfade slot) with @ref rt_music_destroy. Used on
///          every track transition, clear, finalize, and stop.
/// @param pl Playlist implementation whose current music reference is cleared.
static void playlist_release_music(playlist_impl *pl) {
    if (pl->music) {
        rt_music_stop_related(pl->music);
        rt_music_destroy(pl->music);
        pl->music = NULL;
    }
}

/// @brief Resolve the current playback position to its track-list index.
/// @details Returns -1 for empty playlists or when the current cursor is
///          unset; used to compare the active track across add/remove
///          operations so position can be preserved when its underlying
///          entry shifts.
/// @param pl Playlist implementation whose playback cursor is resolved.
/// @return Underlying track-list index, or `-1` when no current track exists.
static int64_t playlist_current_actual_index(playlist_impl *pl) {
    if (!pl || pl->current < 0)
        return -1;
    return get_track_index(pl, pl->current);
}

/// @brief Find which shuffle slot currently holds @p actual_index.
/// @details Linear scan over the shuffle sequence. Used when an
///          operation alters the underlying track list (insert/remove)
///          so the playback cursor can stay anchored to the same song
///          even after shuffle indices shift.
/// @param pl Playlist implementation whose shuffle order is searched.
/// @param actual_index Underlying track-list index to locate.
/// @return Shuffle position on success, -1 when not found.
static int64_t playlist_find_shuffle_position(playlist_impl *pl, int64_t actual_index) {
    if (!pl || !pl->shuffle_order || actual_index < 0)
        return -1;
    int64_t count = rt_seq_len(pl->shuffle_order);
    for (int64_t i = 0; i < count; i++) {
        if ((int64_t)(intptr_t)rt_seq_get(pl->shuffle_order, i) == actual_index)
            return i;
    }
    return -1;
}

/// @brief Set the playback cursor so it points at @p actual_index.
/// @details Bridges between the user-visible track index and the
///          shuffle-aware `current` cursor. In shuffle mode, looks up
///          where @p actual_index lives in the shuffle order (falling
///          back to 0 if missing); otherwise the cursor *is* the track
///          index. Out-of-range inputs clear the cursor to -1.
/// @param pl Playlist implementation whose cursor is updated.
/// @param actual_index Underlying track-list index, or an invalid value to clear the cursor.
static void playlist_set_current_from_actual(playlist_impl *pl, int64_t actual_index) {
    if (!pl) {
        return;
    }
    if (actual_index < 0 || actual_index >= rt_seq_len(pl->tracks)) {
        pl->current = -1;
        return;
    }
    if (pl->shuffle) {
        int64_t pos = playlist_find_shuffle_position(pl, actual_index);
        pl->current = pos >= 0 ? pos : 0;
        return;
    }
    pl->current = actual_index;
}

/// @brief Load the music track at the current cursor and apply the playlist volume.
/// @details Returns NULL when the cursor is out of range, the path slot
///          is empty, or `rt_music_load` rejects the file. The caller
///          assumes ownership of the new music wrapper.
/// @param pl Playlist implementation supplying current track and volume state.
/// @return New owned music wrapper, or NULL when the current track cannot be loaded.
static void *playlist_load_current_music(playlist_impl *pl) {
    if (!pl)
        return NULL;

    if (pl->current < 0 || pl->current >= rt_seq_len(pl->tracks))
        return NULL;

    int64_t actual_index = get_track_index(pl, pl->current);
    if (actual_index < 0 || actual_index >= rt_seq_len(pl->tracks))
        return NULL;

    rt_string path = (rt_string)rt_seq_get(pl->tracks, actual_index);
    if (!path)
        return NULL;

    void *music = rt_music_load(path);
    if (music)
        rt_music_set_volume(music, pl->volume);
    return music;
}

/// @brief Swap in a freshly-loaded music wrapper, with optional crossfade.
/// @details When @p play_now is true and a crossfade window is
///          configured (`pl->crossfade_ms > 0`) plus the playlist is
///          already playing, the old and new tracks share a
///          @ref rt_music_crossfade_to slot. Otherwise the old track
///          is stopped immediately and the new one starts at the front
///          of the foreground. Updates `playing`/`paused` flags to
///          reflect ZannaAUD's actual state.
/// @param pl        Playlist instance.
/// @param new_music Newly-loaded music wrapper (NULL means "track ended,
///                  no replacement yet").
/// @param play_now  Non-zero to start the new track; zero to swap but
///                  remain paused/stopped.
static void playlist_replace_music(playlist_impl *pl, void *new_music, int8_t play_now) {
    void *old_music = pl->music;
    int loop = (pl->repeat == RT_REPEAT_ONE) ? 1 : 0;

    pl->music = new_music;
    if (new_music)
        rt_music_set_volume(new_music, pl->volume);

    if (play_now && new_music) {
        if (pl->crossfade_ms > 0 && old_music && pl->playing) {
            rt_music_set_loop(new_music, loop);
            rt_music_crossfade_to(old_music, new_music, pl->crossfade_ms);
        } else {
            if (old_music)
                rt_music_stop_related(old_music);
            rt_music_play(new_music, loop);
        }
        pl->playing = rt_music_is_playing(new_music) ? 1 : 0;
        pl->paused = 0;
    } else {
        if (old_music)
            rt_music_stop_related(old_music);
    }

    if (old_music && old_music != new_music)
        rt_music_destroy(old_music);
}

/// @brief Move the playback cursor to @p new_position with the desired play state.
/// @details Shared by next/prev/jump/play paths. The combination of
///          @p resume_playing and @p preserve_paused selects between
///          three behaviours: (1) "stopped" — release the music, clear
///          flags; (2) "play now" — load the new track and start it,
///          honouring crossfade; (3) "load but stay paused" — load and
///          mark paused so a later resume starts at the right track.
/// @param pl              Playlist instance.
/// @param new_position    Target cursor (shuffle position or actual index).
/// @param resume_playing  Non-zero to start playback after the swap.
/// @param preserve_paused Non-zero to load and keep paused state.
static void playlist_select_position(playlist_impl *pl,
                                     int64_t new_position,
                                     int8_t resume_playing,
                                     int8_t preserve_paused) {
    if (!pl)
        return;

    pl->current = new_position;
    if (!resume_playing && !preserve_paused) {
        playlist_release_music(pl);
        pl->playing = 0;
        pl->paused = 0;
        return;
    }

    void *new_music = playlist_load_current_music(pl);
    if (!new_music && resume_playing) {
        // Skip-on-error auto-advance (VDOC-119): one unloadable entry must
        // not stall the queue. Try subsequent positions with the same wrap
        // semantics as Next, bounded by the track count so a playlist whose
        // every entry fails stops cleanly instead of looping.
        int64_t count = rt_seq_len(pl->tracks);
        for (int64_t attempts = 1; attempts < count && !new_music; ++attempts) {
            int64_t candidate = pl->current + 1;
            if (candidate >= count) {
                if (pl->repeat != RT_REPEAT_ALL)
                    break;
                candidate = 0;
                if (pl->shuffle)
                    generate_shuffle_order(pl);
            }
            pl->current = candidate;
            new_music = playlist_load_current_music(pl);
        }
    }
    playlist_replace_music(pl, new_music, resume_playing);

    if (resume_playing && pl->music) {
        pl->playing = rt_music_is_playing(pl->music) ? 1 : 0;
        pl->paused = 0;
    } else if (!pl->music) {
        pl->playing = 0;
        pl->paused = 0;
    } else {
        pl->playing = 0;
        pl->paused = (preserve_paused && pl->music) ? 1 : 0;
    }
}

//=============================================================================
// Creation
//=============================================================================

/// @brief GC finalizer for a Playlist object.
/// @details Releases the active music wrapper, drops a reference on the
///          shuffle-order sequence and the track-list sequence, and
///          NULLs the slots. Called by the runtime once the playlist's
///          refcount hits zero (C-1 incident-marker in the source).
/// @param obj Runtime-managed Playlist object.
static void playlist_finalize(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    playlist_release_music(pl);

    if (pl->shuffle_order) {
        if (rt_obj_release_check0(pl->shuffle_order))
            rt_obj_free(pl->shuffle_order);
        pl->shuffle_order = NULL;
    }

    if (pl->tracks) {
        if (rt_obj_release_check0(pl->tracks))
            rt_obj_free(pl->tracks);
        pl->tracks = NULL;
    }
}

/// @brief Create a new playlist with shuffle, repeat, crossfade, and auto-advance support.
/// @details Initializes an owning track sequence, neutral volume, no repeat,
///          sequential ordering, and no selected track. Allocation failure
///          raises the runtime trap and returns NULL.
/// @return Caller-owned runtime Playlist object, or NULL on allocation failure.
void *rt_playlist_new(void) {
    playlist_impl *pl =
        (playlist_impl *)rt_obj_new_i64(RT_PLAYLIST_CLASS_ID, (int64_t)sizeof(playlist_impl));
    if (!pl) {
        rt_trap("rt_playlist: memory allocation failed");
        return NULL;
    }
    memset(pl, 0, sizeof(playlist_impl));

    pl->tracks = rt_seq_new();
    if (!pl->tracks) {
        if (rt_obj_release_check0(pl))
            rt_obj_free(pl);
        rt_trap("rt_playlist: memory allocation failed");
        return NULL;
    }
    rt_seq_set_owns_elements(pl->tracks, 1);
    pl->current = -1;
    pl->music = NULL;
    pl->volume = 100;
    pl->shuffle = 0;
    pl->repeat = RT_REPEAT_NONE;
    pl->playing = 0;
    pl->paused = 0;
    pl->shuffle_order = NULL;

    rt_obj_set_finalizer(pl, playlist_finalize);

    return pl;
}

//=============================================================================
// Track Management
//=============================================================================

/// @brief Append a music track to the end of the playlist.
/// @details Retains a reference to the path string and appends it to the
///          tracks sequence. If shuffle mode is active, the shuffle order is
///          regenerated to include the new track.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param path Path to the music file to add.
void rt_playlist_add(void *obj, rt_string path) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;
    int64_t current_actual = playlist_current_actual_index(pl);

    rt_string empty_path = NULL;
    if (!path) {
        empty_path = rt_const_cstr("");
        path = empty_path;
    }
    rt_seq_push(pl->tracks, (void *)path);
    if (empty_path)
        rt_string_unref(empty_path);

    if (pl->shuffle) {
        generate_shuffle_order(pl);
        if (current_actual >= 0)
            playlist_set_current_from_actual(pl, current_actual);
    }
}

/// @brief Insert a music track at a specific position in the playlist.
/// @details Shifts existing tracks at and after @p index to make room. If the
///          insertion point is before the current track, the current index is
///          adjusted to keep it pointing at the same track.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param index Zero-based insertion position.
/// @param path Path to the music file to insert.
void rt_playlist_insert(void *obj, int64_t index, rt_string path) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;
    int64_t current_actual = playlist_current_actual_index(pl);
    int64_t count = rt_seq_len(pl->tracks);

    if (index < 0)
        index = 0;
    if (index > count)
        index = count;

    rt_string empty_path = NULL;
    if (!path) {
        empty_path = rt_const_cstr("");
        path = empty_path;
    }
    rt_seq_insert(pl->tracks, index, (void *)path);
    if (empty_path)
        rt_string_unref(empty_path);

    if (pl->shuffle) {
        generate_shuffle_order(pl);
        if (current_actual >= 0 && index <= current_actual)
            current_actual++;
        if (current_actual >= 0)
            playlist_set_current_from_actual(pl, current_actual);
    } else if (pl->current >= index) {
        pl->current++;
    }
}

/// @brief Remove a track from the playlist by index.
/// @details If the removed track is before the current track, the current index
///          is decremented. If the current track itself is removed, playback is
///          stopped and the next available track becomes current.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param index Zero-based index of the track to remove.
void rt_playlist_remove(void *obj, int64_t index) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    int64_t count = rt_seq_len(pl->tracks);
    if (index < 0 || index >= count)
        return;
    int64_t current_actual = playlist_current_actual_index(pl);
    int64_t current_position = pl->current;
    int8_t was_playing = pl->playing;
    int8_t was_paused = pl->paused;
    int8_t removed_current = (current_actual == index);

    if (current_actual > index)
        current_actual--;

    void *removed = rt_seq_remove(pl->tracks, index);
    if (removed && rt_obj_release_check0(removed))
        rt_obj_free(removed);

    count = rt_seq_len(pl->tracks);
    if (pl->shuffle)
        generate_shuffle_order(pl);

    if (count == 0) {
        playlist_release_music(pl);
        pl->current = -1;
        pl->playing = 0;
        pl->paused = 0;
        return;
    }

    if (removed_current) {
        int64_t next_position = current_position;
        if (next_position < 0)
            next_position = 0;
        if (next_position >= count)
            next_position = 0;
        pl->current = next_position;

        void *new_music = playlist_load_current_music(pl);
        playlist_replace_music(pl, new_music, was_playing);
        if (!pl->music) {
            pl->playing = 0;
            pl->paused = 0;
        } else if (!was_playing) {
            pl->playing = 0;
            pl->paused = was_paused;
        }
        return;
    }

    playlist_set_current_from_actual(pl, current_actual);
}

/// @brief Remove all tracks from the playlist and stop playback.
/// @details Stops the current music, releases all track path strings from the
///          sequence, resets the current index to -1, and frees the shuffle
///          order if one exists.
/// @param obj Playlist object pointer; no-op if NULL.
void rt_playlist_clear(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    playlist_release_music(pl);

    rt_seq_clear(pl->tracks);

    pl->current = -1;
    pl->playing = 0;
    pl->paused = 0;

    // C-3: Release shuffle_order instead of leaking it (C-3)
    if (pl->shuffle_order) {
        if (rt_obj_release_check0(pl->shuffle_order))
            rt_obj_free(pl->shuffle_order);
        pl->shuffle_order = NULL;
    }
}

/// @brief Return the number of tracks in the playlist.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return Track count.
int64_t rt_playlist_len(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return rt_seq_len(pl->tracks);
}

/// @brief Return the file path of the track at the given index.
/// @param obj Playlist object pointer; returns empty string if NULL.
/// @param index Zero-based track index.
/// @return Track path string, or empty string if index is out of bounds.
rt_string rt_playlist_get(void *obj, int64_t index) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return rt_const_cstr("");

    if (index < 0 || index >= rt_seq_len(pl->tracks))
        return rt_const_cstr("");

    rt_string track = (rt_string)rt_seq_get(pl->tracks, index);
    rt_str_retain_maybe(track);
    return track;
}

//=============================================================================
// Playback Control
//=============================================================================

/// @brief Start or resume playlist playback.
/// @details If paused, resumes the current track. If stopped, loads the
///          current track (defaulting to index 0) and begins playback.
///          Repeat-one mode passes the loop flag to the music backend.
/// @param obj Playlist object pointer; no-op if NULL or empty.
void rt_playlist_play(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    if (rt_seq_len(pl->tracks) == 0)
        return;

    if (pl->paused && pl->music) {
        int loop = (pl->repeat == RT_REPEAT_ONE) ? 1 : 0;
        rt_music_resume_related(pl->music);
        if (!rt_music_is_playing(pl->music))
            rt_music_play(pl->music, loop);
        pl->paused = 0;
        pl->playing = rt_music_is_playing(pl->music) ? 1 : 0;
        return;
    }

    if (pl->current < 0 || pl->current >= rt_seq_len(pl->tracks)) {
        pl->current = 0;
    }

    if (!pl->music) {
        pl->music = playlist_load_current_music(pl);
        // Skip-on-error (VDOC-119): Play() also advances past unloadable
        // entries so a single bad track cannot defeat playback.
        int64_t count = rt_seq_len(pl->tracks);
        for (int64_t attempts = 1; attempts < count && !pl->music; ++attempts) {
            int64_t candidate = pl->current + 1;
            if (candidate >= count) {
                if (pl->repeat != RT_REPEAT_ALL)
                    break;
                candidate = 0;
                if (pl->shuffle)
                    generate_shuffle_order(pl);
            }
            pl->current = candidate;
            pl->music = playlist_load_current_music(pl);
        }
    }

    if (pl->music) {
        int loop = (pl->repeat == RT_REPEAT_ONE) ? 1 : 0;
        rt_music_play(pl->music, loop);
        pl->playing = rt_music_is_playing(pl->music) ? 1 : 0;
        pl->paused = 0;
    }
}

/// @brief Pause the currently playing track without resetting position.
/// @param obj Playlist object pointer; no-op if NULL or not playing.
void rt_playlist_pause(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    if (pl->music && pl->playing) {
        rt_music_pause_related(pl->music);
        pl->paused = 1;
        pl->playing = 0;
    }
}

/// @brief Stop playback and reset the current position to the beginning.
/// @param obj Playlist object pointer; no-op if NULL.
void rt_playlist_stop(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    if (pl->music) {
        rt_music_stop_related(pl->music);
    }

    pl->playing = 0;
    pl->paused = 0;
}

/// @brief Advance to the next track in the playlist.
/// @details Handles repeat-all (wraps to track 0 and re-shuffles if needed)
///          and repeat-none (stops at the last track). If playback was active,
///          the new track is loaded and started automatically.
/// @param obj Playlist object pointer; no-op if NULL or empty.
void rt_playlist_next(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    int64_t count = rt_seq_len(pl->tracks);
    if (count == 0)
        return;

    int8_t was_playing = pl->playing;
    int8_t was_paused = pl->paused;
    int64_t next_position = (pl->current < 0) ? 0 : pl->current + 1;

    if (next_position >= count) {
        if (pl->repeat == RT_REPEAT_ALL) {
            next_position = 0;
            if (pl->shuffle)
                generate_shuffle_order(pl);
        } else {
            pl->current = count - 1;
            rt_playlist_stop(obj);
            return;
        }
    }

    playlist_select_position(pl, next_position, was_playing, was_paused);
}

/// @brief Go back to the previous track in the playlist.
/// @details With repeat-all, wrapping past the first track goes to the last.
///          Without repeat, the position clamps at track 0.
/// @param obj Playlist object pointer; no-op if NULL or empty.
void rt_playlist_prev(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    int64_t count = rt_seq_len(pl->tracks);
    if (count == 0)
        return;

    int8_t was_playing = pl->playing;
    int8_t was_paused = pl->paused;
    int64_t prev_position = (pl->current < 0) ? 0 : pl->current - 1;

    if (prev_position < 0) {
        if (pl->repeat == RT_REPEAT_ALL) {
            prev_position = count - 1;
        } else {
            prev_position = 0;
        }
    }

    playlist_select_position(pl, prev_position, was_playing, was_paused);
}

/// @brief Jump to a specific track by index and begin playback if active.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param index Zero-based track index to jump to; out-of-range is ignored.
void rt_playlist_jump(void *obj, int64_t index) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    int64_t count = rt_seq_len(pl->tracks);
    if (count == 0 || index < 0 || index >= count)
        return;

    int64_t target_position = index;
    if (pl->shuffle) {
        if (!pl->shuffle_order)
            generate_shuffle_order(pl);
        target_position = playlist_find_shuffle_position(pl, index);
        if (target_position < 0)
            return;
    }

    playlist_select_position(pl, target_position, pl->playing, pl->paused);
}

//=============================================================================
// Properties
//=============================================================================

/// @brief Return the real track index of the currently playing/loaded track.
/// @details In shuffle mode, maps the internal position through the shuffle
///          order to return the actual track index in the original sequence.
/// @param obj Playlist object pointer; returns -1 if NULL.
/// @return Zero-based track index, or -1 if no track is loaded.
int64_t rt_playlist_get_current(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return -1;

    if (pl->shuffle && pl->current >= 0) {
        return get_track_index(pl, pl->current);
    }
    return pl->current;
}

/// @brief Check whether the playlist is currently playing a track.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return 1 if playing, 0 if stopped or paused.
int8_t rt_playlist_is_playing(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->playing;
}

/// @brief Check whether the playlist is paused.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return 1 if paused, 0 otherwise.
int8_t rt_playlist_is_paused(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->paused;
}

/// @brief Return the current playback volume of the playlist.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return Volume in range [0, 100].
int64_t rt_playlist_get_volume(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->volume;
}

/// @brief Set the playback volume, clamped to [0, 100].
/// @details Also applies the new volume to the currently loaded music track
///          so the change takes effect immediately.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param volume Desired volume (0-100).
void rt_playlist_set_volume(void *obj, int64_t volume) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    pl->volume = volume;

    if (pl->music) {
        rt_music_set_crossfade_pair_volume(pl->music, volume);
    }
}

//=============================================================================
// Playback Modes
//=============================================================================

/// @brief Enable or disable shuffle mode.
/// @details When enabled, generates a random permutation of track indices
///          using the runtime RNG. `Zanna.Math.Random.Seed` can make the
///          permutation deterministic. The permutation is regenerated each
///          time shuffle is toggled on.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param shuffle 1 to enable shuffle, 0 to disable.
void rt_playlist_set_shuffle(void *obj, int8_t shuffle) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    int8_t new_shuffle = shuffle ? 1 : 0;
    if (pl->shuffle == new_shuffle)
        return;

    int64_t current_actual = playlist_current_actual_index(pl);
    pl->shuffle = new_shuffle;

    if (pl->shuffle) {
        generate_shuffle_order(pl);
        if (current_actual >= 0)
            playlist_set_current_from_actual(pl, current_actual);
    } else {
        if (pl->shuffle_order) {
            if (rt_obj_release_check0(pl->shuffle_order))
                rt_obj_free(pl->shuffle_order);
            pl->shuffle_order = NULL;
        }
        if (current_actual >= 0)
            pl->current = current_actual;
    }
}

/// @brief Check whether shuffle mode is currently enabled.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return 1 if shuffle is enabled, 0 otherwise.
int8_t rt_playlist_get_shuffle(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->shuffle;
}

/// @brief Set the repeat mode for the playlist.
/// @details Mode values: RT_REPEAT_NONE (stop at end), RT_REPEAT_ALL (loop),
///          RT_REPEAT_ONE (replay current track). Out-of-range values are clamped.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param mode Repeat mode constant.
void rt_playlist_set_repeat(void *obj, int64_t mode) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    if (mode < RT_REPEAT_NONE)
        mode = RT_REPEAT_NONE;
    if (mode > RT_REPEAT_ONE)
        mode = RT_REPEAT_ONE;
    pl->repeat = mode;
    if (pl->music)
        rt_music_set_loop(pl->music, pl->repeat == RT_REPEAT_ONE);
}

/// @brief Return the current repeat mode.
/// @param obj Playlist object pointer; returns 0 (REPEAT_NONE) if NULL.
/// @return Repeat mode constant (RT_REPEAT_NONE, RT_REPEAT_ALL, RT_REPEAT_ONE).
int64_t rt_playlist_get_repeat(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->repeat;
}

//=============================================================================
// Update
//=============================================================================

/// @brief Poll the playlist state and auto-advance when the current track ends.
/// @details Should be called once per frame or tick. When the current track
///          finishes, repeat-one restarts it; otherwise rt_playlist_next is
///          invoked to advance (which handles repeat-all and repeat-none).
/// @param obj Playlist object pointer; no-op if NULL or not playing.
void rt_playlist_update(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;

    rt_audio_update();

    if (!pl->playing || !pl->music)
        return;

    // Check if current track has ended
    if (!rt_music_is_playing(pl->music)) {
        // Track ended
        if (pl->repeat == RT_REPEAT_ONE) {
            // Repeat one: restart same track
            rt_music_seek(pl->music, 0);
            rt_music_play(pl->music, 1);
            pl->playing = rt_music_is_playing(pl->music) ? 1 : 0;
        } else {
            // Move to next
            rt_playlist_next(obj);
        }
    }
}

//=============================================================================
// Playlist Crossfade Settings
//=============================================================================

/// @brief Set the crossfade duration for track transitions.
/// @details A value of 0 disables crossfading (immediate switch). Positive
///          values enable a linear volume crossfade of the specified duration
///          when advancing between tracks.
/// @param obj Playlist object pointer; no-op if NULL.
/// @param duration_ms Crossfade duration in milliseconds (0 = disabled).
void rt_playlist_set_crossfade(void *obj, int64_t duration_ms) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return;
    pl->crossfade_ms = duration_ms < 0 ? 0 : duration_ms;
}

/// @brief Return the current crossfade duration in milliseconds.
/// @param obj Playlist object pointer; returns 0 if NULL.
/// @return Crossfade duration (0 = disabled).
int64_t rt_playlist_get_crossfade(void *obj) {
    playlist_impl *pl = as_playlist(obj);
    if (!pl)
        return 0;
    return pl->crossfade_ms;
}
