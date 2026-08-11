//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_mixgroup.h
// Purpose: Audio mix groups (MUSIC / SFX) with independent volume control,
//   music crossfading, and group-aware sound playback.
//
// Key invariants:
//   - Two built-in groups: RT_MIXGROUP_MUSIC (0) and RT_MIXGROUP_SFX (1).
//   - Group volumes default to 100 (full) and are clamped to [0, 100].
//   - Effective volume = voice_volume × group_volume × master_volume / 10000.
//   - Crossfades are tracked per active music pair; unrelated playlist transitions do not collide.
//
// Ownership/Lifetime:
//   - Mix group state is global (module-level); no allocation needed.
//   - Crossfade holds references to two music objects during the fade.
//
// Links: src/runtime/audio/rt_audio.c (implementation),
//        src/runtime/audio/rt_playlist.h (playlist crossfade integration)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares audio mix-group registration, volume, effects, crossfades,
///        and group-aware sound playback.
/// @details The runtime provides built-in music and sound-effect groups plus a
///          bounded registry of canonicalized named groups. Group volume scales
///          each voice's logical gain before master volume, and ordered effect
///          chains process a group's mixed samples. Music transitions retain
///          both wrappers while a crossfade is active.

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Built-in group used by streamed music.
#define RT_MIXGROUP_MUSIC 0
/// @brief Built-in group used by transient sound effects.
#define RT_MIXGROUP_SFX 1
/// @brief Number of reserved built-in mix groups.
#define RT_MIXGROUP_COUNT 2
/// @brief First identifier available to dynamically registered named groups.
#define RT_MIXGROUP_NAMED_BASE 100
/// @brief Fixed capacity of the process-wide mix-group registry.
#define RT_MIXGROUP_MAX_GROUPS 256

//=========================================================================
// Mix Group Volume
//=========================================================================

/// @brief Set the volume for a mix group (0-100, clamped).
/// @details Active voices or music assigned to the group are updated
///          immediately. Invalid or unused group identifiers are ignored.
/// @param group Registered mix-group identifier.
/// @param volume Requested group volume percentage.
void rt_audio_set_group_volume(int64_t group, int64_t volume);

/// @brief Get the volume for a mix group.
/// @param group Registered mix-group identifier.
/// @return Volume in `[0, 100]`, or the neutral value `100` if invalid/unused.
int64_t rt_audio_get_group_volume(int64_t group);

/// @brief Register a named mix group. Built-ins "music" and "sfx" return 0 and 1.
/// @details Leading/trailing spaces and tabs are trimmed. Registering an
///          existing canonical name is idempotent; new groups begin at volume
///          `100`.
/// @param group_name Runtime string containing the requested group name.
/// @return Group identifier, or `-1` for an invalid/overlong name or a full
///         registry.
int64_t rt_audio_register_group(rt_string group_name);

/// @brief Find a registered named mix group, or -1 when missing.
/// @param group_name Runtime string containing the canonicalizable name.
/// @return Registered identifier, or `-1` for an invalid or absent name.
int64_t rt_audio_find_group(rt_string group_name);

/// @brief Find a registered named mix group as an Option id.
/// @details Returns `SomeI64(id)` for built-in or registered groups and `None`
///          when the name is missing, avoiding the legacy `-1` sentinel.
/// @param group_name Group name.
/// @return Opaque Zanna.Option containing the group id, or None.
void *rt_audio_find_group_option(rt_string group_name);

/// @brief Set a named group's volume (0-100, clamped). Missing groups are registered.
/// @param group_name Runtime string naming the target group.
/// @param volume Requested group volume percentage.
void rt_audio_set_group_volume_named(rt_string group_name, int64_t volume);

/// @brief Get a named group's volume, or 100 when missing.
/// @param group_name Runtime string naming a built-in or registered group.
/// @return Stored group volume, or the neutral value `100` if absent/invalid.
int64_t rt_audio_get_group_volume_named(rt_string group_name);

/// @brief Return a group's registered name, or an empty string when invalid.
/// @param group_id Registered mix-group identifier.
/// @return Runtime-owned copy of the registered name, or the shared empty
///         string for an invalid/unused identifier.
rt_string rt_audio_group_name(int64_t group_id);

//=========================================================================
// Mix Group Effects
//=========================================================================

/// @brief Add a low-pass biquad to a mix group.
/// @param group Registered mix-group identifier.
/// @param cutoff_hz Requested cutoff frequency in hertz.
/// @param q Requested resonance/quality factor.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_snd_group_add_lowpass(int64_t group, double cutoff_hz, double q);

/// @brief Add a high-pass biquad to a mix group.
/// @param group Registered mix-group identifier.
/// @param cutoff_hz Requested cutoff frequency in hertz.
/// @param q Requested resonance/quality factor.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_snd_group_add_highpass(int64_t group, double cutoff_hz, double q);

/// @brief Add a peaking EQ biquad to a mix group.
/// @param group Registered mix-group identifier.
/// @param freq_hz Requested center frequency in hertz.
/// @param q Requested bandwidth/quality factor.
/// @param gain_db Requested center-frequency gain in decibels.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_snd_group_add_peaking(int64_t group, double freq_hz, double q, double gain_db);

/// @brief Add a feedback-delay insert to a mix group.
/// @param group Registered mix-group identifier.
/// @param delay_ms Requested delay time in milliseconds.
/// @param feedback Requested feedback fraction.
/// @param wet Requested wet-signal fraction.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_snd_group_add_delay(int64_t group, double delay_ms, double feedback, double wet);

/// @brief Add a small Freeverb-style reverb insert to a mix group.
/// @param group Registered mix-group identifier.
/// @param room_size Requested normalized room-size parameter.
/// @param damping Requested normalized damping parameter.
/// @param wet Requested wet-signal fraction.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_snd_group_add_reverb(int64_t group, double room_size, double damping, double wet);

/// @brief Update a group reverb insert's room/damping/wet in place.
/// @details Preserves the effect's delay-line state.
/// @param group Registered mix-group identifier.
/// @param fx_id Identifier returned by @ref rt_snd_group_add_reverb.
/// @param room_size Requested normalized room-size parameter.
/// @param damping Requested normalized damping parameter.
/// @param wet Requested wet-signal fraction.
void rt_snd_group_set_reverb(
    int64_t group, int64_t fx_id, double room_size, double damping, double wet);

/// @brief Bypass or enable one effect by id.
/// @param group Registered mix-group identifier.
/// @param fx_id Identifier returned by an effect-add operation.
/// @param bypass Non-zero to bypass processing; zero to enable it.
void rt_snd_group_fx_bypass(int64_t group, int64_t fx_id, int8_t bypass);

/// @brief Remove one effect from a group.
/// @param group Registered mix-group identifier.
/// @param fx_id Identifier returned by an effect-add operation.
void rt_snd_group_remove_fx(int64_t group, int64_t fx_id);

/// @brief Remove every effect from a group.
/// @param group Registered mix-group identifier.
void rt_snd_group_clear_fx(int64_t group);

//=========================================================================
// Music Crossfade
//=========================================================================

/// @brief Crossfade from current music to new music over duration_ms.
/// @details The runtime retains both valid wrappers for the transition and
///          releases those temporary references when it completes or is
///          cancelled.
/// @param current_music Currently playing music (will fade out). May be NULL.
/// @param new_music Music to fade in. If NULL, just fades out current.
/// @param duration_ms Crossfade duration. ≤ 0 means immediate switch.
void rt_music_crossfade_to(void *current_music, void *new_music, int64_t duration_ms);

/// @brief Check if a crossfade is in progress.
/// @return Non-zero while any crossfade slot is active.
int8_t rt_music_is_crossfading(void);

/// @brief Update crossfade state. Called internally per frame.
/// @param dt_ms Elapsed milliseconds; non-positive values do not advance.
void rt_music_crossfade_update(int64_t dt_ms);

//=========================================================================
// Playlist Crossfade
//=========================================================================

/// @brief Set the crossfade duration for playlist auto-advance (0 = disabled).
/// @param playlist Opaque playlist object.
/// @param duration_ms Requested transition duration in milliseconds; values are
///        clamped to `[0, 3600000]`.
void rt_playlist_set_crossfade(void *playlist, int64_t duration_ms);

/// @brief Get the crossfade duration.
/// @param playlist Opaque playlist object.
/// @return Stored duration in milliseconds, or `0` for an invalid object.
int64_t rt_playlist_get_crossfade(void *playlist);

//=========================================================================
// Group-Aware Sound Playback
//=========================================================================

/// @brief Play a sound in a specific mix group.
/// @param sound Opaque playable sound handle.
/// @param group Registered target group; invalid values fall back to SFX.
/// @return Voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_in_group(void *sound, int64_t group);

/// @brief Play a sound with volume/pan in a specific mix group.
/// @param sound Opaque playable sound handle.
/// @param volume Logical volume clamped to `[0, 100]`.
/// @param pan Stereo pan clamped to `[-100, 100]`.
/// @param group Registered target group; invalid values fall back to SFX.
/// @return Voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_ex_in_group(void *sound, int64_t volume, int64_t pan, int64_t group);

/// @brief Play a looping sound with volume/pan in a specific mix group.
/// @param sound Opaque playable sound handle.
/// @param volume Logical volume clamped to `[0, 100]`.
/// @param pan Stereo pan clamped to `[-100, 100]`.
/// @param group Registered target group; invalid values fall back to SFX.
/// @return Voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_loop_in_group(void *sound, int64_t volume, int64_t pan, int64_t group);

#ifdef __cplusplus
}
#endif
