//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/audio/rt_playlist.h
// Purpose: Queue-based music playlist manager providing sequential track playback with shuffle,
// repeat, and queue manipulation (add, remove, skip) built on top of the audio bridge.
//
// Key invariants:
//   - Track queue is FIFO by default; shuffle randomizes playback order.
//   - Repeat modes: none (stop at end), one (replay current track), all (loop queue).
//   - Skip and previous navigate relative to the current playback position.
//   - Track storage grows dynamically through the runtime sequence container.
//
// Ownership/Lifetime:
//   - Playlist objects are GC-managed runtime objects; they are freed when their refcount drops to
//   0.
//   - Track strings are retained by the underlying sequence; callers keep ownership of their inputs
//   too.
//   - rt_playlist_get returns a retained string reference; C callers must release it when done.
//
// Links: src/runtime/audio/rt_playlist.c (implementation), src/runtime/audio/rt_audio.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime Playlist object for ordered/shuffled music queues.
/// @details A playlist retains track-path strings, lazily loads a Music wrapper
///          for the selected entry, and coordinates repeat, shuffle,
///          crossfade-aware navigation, volume, pause/resume, and per-frame
///          auto-advance. Invalid object pointers are treated as no-ops by
///          mutators and yield documented fallback values from queries.

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief End-of-track behavior used by a playlist.
typedef enum {
    RT_REPEAT_NONE = 0, ///< Stop at end of playlist.
    RT_REPEAT_ALL = 1,  ///< Loop back to first track after last.
    RT_REPEAT_ONE = 2,  ///< Replay the current track indefinitely.
} rt_playlist_repeat_t;

//=============================================================================
// Zanna.Sound.Playlist
//=============================================================================

/// @brief Create a new empty playlist.
/// @return Caller-owned Playlist object, or NULL after an allocation trap.
void *rt_playlist_new(void);

/// @brief Add a music file to the end of the playlist.
/// @details The playlist retains the path; shuffle order is regenerated when enabled.
/// @param playlist Playlist object.
/// @param path Path to music file; NULL is stored as an empty path.
void rt_playlist_add(void *playlist, rt_string path);

/// @brief Insert a music file at a specific position.
/// @details Preserves the logically selected track when indices shift and
///          regenerates an active shuffle order.
/// @param playlist Playlist object.
/// @param index Position to insert (clamped into the valid range, 0 = first).
/// @param path Path to music file; NULL is stored as an empty path.
void rt_playlist_insert(void *playlist, int64_t index, rt_string path);

/// @brief Remove a track from the playlist by index.
/// @details Removing the current track transitions to the next available entry
///          while preserving active/paused state where possible.
/// @param playlist Playlist object.
/// @param index Zero-based position; invalid values are ignored.
void rt_playlist_remove(void *playlist, int64_t index);

/// @brief Clear all tracks from the playlist.
/// @details Stops/releases current music and resets cursor and shuffle state.
/// @param playlist Playlist object.
void rt_playlist_clear(void *playlist);

/// @brief Get the number of tracks in the playlist.
/// @param playlist Playlist object.
/// @return Number of tracks.
int64_t rt_playlist_len(void *playlist);

/// @brief Get the path of a track at a given index.
/// @param playlist Playlist object.
/// @param index Track index.
/// @return Retained path string, or retained empty string if index out of bounds.
/// @note C callers must release the returned reference.
rt_string rt_playlist_get(void *playlist, int64_t index);

//=============================================================================
// Playback Control
//=============================================================================

/// @brief Start the selected track or resume paused playback.
/// @details Defaults an unset cursor to the first playback position and skips
///          unloadable entries within the queue.
/// @param playlist Playlist object.
void rt_playlist_play(void *playlist);

/// @brief Pause playback.
/// @details Preserves position and pauses any related crossfade companion.
/// @param playlist Playlist object.
void rt_playlist_pause(void *playlist);

/// @brief Stop playback and reset the current stream to its beginning.
/// @details The selected track index remains unchanged.
/// @param playlist Playlist object.
void rt_playlist_stop(void *playlist);

/// @brief Skip to the next track.
/// @details Honors repeat-all wrapping and preserves playing/paused state.
/// @param playlist Playlist object.
void rt_playlist_next(void *playlist);

/// @brief Go back to the previous track.
/// @details Wraps only in repeat-all mode; otherwise clamps to the first entry.
/// @param playlist Playlist object.
void rt_playlist_prev(void *playlist);

/// @brief Jump to a specific track by index.
/// @details Maps the actual track index through shuffle order and preserves the
///          playlist's current playing/paused state.
/// @param playlist Playlist object.
/// @param index Zero-based actual track index; invalid values are ignored.
void rt_playlist_jump(void *playlist, int64_t index);

//=============================================================================
// Properties
//=============================================================================

/// @brief Get the current track index.
/// @details Returns the actual track-list index even while shuffle is enabled.
/// @param playlist Playlist object.
/// @return Current track index, or `-1` if none is selected/invalid.
int64_t rt_playlist_get_current(void *playlist);

/// @brief Check if the playlist is currently playing.
/// @param playlist Playlist object.
/// @return 1 if playing, 0 otherwise.
int8_t rt_playlist_is_playing(void *playlist);

/// @brief Check if the playlist is paused.
/// @param playlist Playlist object.
/// @return 1 if paused, 0 otherwise.
int8_t rt_playlist_is_paused(void *playlist);

/// @brief Get the playback volume.
/// @param playlist Playlist object.
/// @return Volume (0-100).
int64_t rt_playlist_get_volume(void *playlist);

/// @brief Set the playback volume.
/// @details Clamps the value and immediately applies it across an active
///          crossfade pair when present.
/// @param playlist Playlist object.
/// @param volume Volume (0-100).
void rt_playlist_set_volume(void *playlist, int64_t volume);

//=============================================================================
// Playback Modes
//=============================================================================

/// @brief Enable or disable shuffle mode.
/// @details Enabling generates a Fisher-Yates permutation using the runtime RNG
///          while keeping the current actual track selected.
/// @param playlist Playlist object.
/// @param shuffle 1 to enable shuffle, 0 to disable.
void rt_playlist_set_shuffle(void *playlist, int8_t shuffle);

/// @brief Check if shuffle mode is enabled.
/// @param playlist Playlist object.
/// @return 1 if shuffle is enabled, 0 otherwise.
int8_t rt_playlist_get_shuffle(void *playlist);

/// @brief Set the repeat mode.
/// @details Out-of-range values are clamped, and repeat-one immediately updates
///          the current backend stream's loop flag.
/// @param playlist Playlist object.
/// @param mode Repeat mode (RT_REPEAT_NONE, RT_REPEAT_ALL, or RT_REPEAT_ONE).
void rt_playlist_set_repeat(void *playlist, int64_t mode);

/// @brief Get the current repeat mode.
/// @param playlist Playlist object.
/// @return Repeat mode (RT_REPEAT_NONE, RT_REPEAT_ALL, or RT_REPEAT_ONE).
int64_t rt_playlist_get_repeat(void *playlist);

//=============================================================================
// Update (for auto-advance)
//=============================================================================

/// @brief Update the playlist state (call each frame).
/// @details Advances audio/crossfade timing and handles repeat or auto-advance
///          when the current track ends.
/// @param playlist Playlist object.
void rt_playlist_update(void *playlist);

#ifdef __cplusplus
}
#endif
