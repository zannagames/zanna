//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTAudioSurfaceLinkTests.cpp
// Purpose: Link-smoke coverage for the audio/runtime surface that must remain
//          exported in both full and audio-disabled builds.
//
//===----------------------------------------------------------------------===//

#include "rt_audio.h"
#include "rt_mixgroup.h"
#include "rt_musicgen.h"
#include "rt_playlist.h"
#include "rt_sound3d.h"
#include "rt_soundbank.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_synth.h"

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

} // namespace

int main() {
    volatile std::uintptr_t surface[] = {
        fn_bits(&rt_audio_is_available),
        fn_bits(&rt_audio_init),
        fn_bits(&rt_audio_shutdown),
        fn_bits(&rt_audio_set_master_volume),
        fn_bits(&rt_audio_get_master_volume),
        fn_bits(&rt_audio_pause_all),
        fn_bits(&rt_audio_resume_all),
        fn_bits(&rt_audio_update),
        fn_bits(&rt_audio_stop_all_sounds),
        fn_bits(&rt_sound_load),
        fn_bits(&rt_sound_load_asset),
        fn_bits(&rt_sound_load_mem),
        fn_bits(&rt_sound_destroy),
        fn_bits(&rt_sound_play),
        fn_bits(&rt_sound_play_ex),
        fn_bits(&rt_sound_play_loop),
        fn_bits(&rt_voice_stop),
        fn_bits(&rt_voice_set_volume),
        fn_bits(&rt_voice_set_pan),
        fn_bits(&rt_voice_is_playing),
        fn_bits(&rt_music_load),
        fn_bits(&rt_music_load_asset),
        fn_bits(&rt_music_destroy),
        fn_bits(&rt_music_play),
        fn_bits(&rt_music_stop),
        fn_bits(&rt_music_pause),
        fn_bits(&rt_music_resume),
        fn_bits(&rt_music_set_volume),
        fn_bits(&rt_music_get_volume),
        fn_bits(&rt_music_is_playing),
        fn_bits(&rt_music_seek),
        fn_bits(&rt_music_get_position),
        fn_bits(&rt_music_get_duration),
        fn_bits(&rt_audio_set_group_volume),
        fn_bits(&rt_audio_get_group_volume),
        fn_bits(&rt_snd_group_add_lowpass),
        fn_bits(&rt_snd_group_add_highpass),
        fn_bits(&rt_snd_group_add_peaking),
        fn_bits(&rt_snd_group_add_delay),
        fn_bits(&rt_snd_group_add_reverb),
        fn_bits(&rt_snd_group_fx_bypass),
        fn_bits(&rt_snd_group_remove_fx),
        fn_bits(&rt_snd_group_clear_fx),
        fn_bits(&rt_music_crossfade_to),
        fn_bits(&rt_music_is_crossfading),
        fn_bits(&rt_music_crossfade_update),
        fn_bits(&rt_sound_play_in_group),
        fn_bits(&rt_sound_play_ex_in_group),
        fn_bits(&rt_sound_play_loop_in_group),
        fn_bits(&rt_playlist_new),
        fn_bits(&rt_playlist_add),
        fn_bits(&rt_playlist_insert),
        fn_bits(&rt_playlist_remove),
        fn_bits(&rt_playlist_clear),
        fn_bits(&rt_playlist_len),
        fn_bits(&rt_playlist_get),
        fn_bits(&rt_playlist_play),
        fn_bits(&rt_playlist_pause),
        fn_bits(&rt_playlist_stop),
        fn_bits(&rt_playlist_next),
        fn_bits(&rt_playlist_prev),
        fn_bits(&rt_playlist_jump),
        fn_bits(&rt_playlist_get_current),
        fn_bits(&rt_playlist_is_playing),
        fn_bits(&rt_playlist_is_paused),
        fn_bits(&rt_playlist_get_volume),
        fn_bits(&rt_playlist_set_volume),
        fn_bits(&rt_playlist_set_shuffle),
        fn_bits(&rt_playlist_get_shuffle),
        fn_bits(&rt_playlist_set_repeat),
        fn_bits(&rt_playlist_get_repeat),
        fn_bits(&rt_playlist_set_crossfade),
        fn_bits(&rt_playlist_get_crossfade),
        fn_bits(&rt_playlist_update),
        fn_bits(&rt_soundbank_new),
        fn_bits(&rt_soundbank_register),
        fn_bits(&rt_soundbank_register_sound),
        fn_bits(&rt_soundbank_play),
        fn_bits(&rt_soundbank_play_ex),
        fn_bits(&rt_soundbank_has),
        fn_bits(&rt_soundbank_get),
        fn_bits(&rt_soundbank_remove),
        fn_bits(&rt_soundbank_clear),
        fn_bits(&rt_soundbank_count),
        fn_bits(&rt_synth_tone),
        fn_bits(&rt_synth_sweep),
        fn_bits(&rt_synth_noise),
        fn_bits(&rt_synth_sfx),
        fn_bits(&rt_musicgen_new),
        fn_bits(&rt_musicgen_add_channel),
        fn_bits(&rt_musicgen_set_envelope),
        fn_bits(&rt_musicgen_set_channel_vol),
        fn_bits(&rt_musicgen_set_duty),
        fn_bits(&rt_musicgen_set_pan),
        fn_bits(&rt_musicgen_set_detune),
        fn_bits(&rt_musicgen_set_vibrato),
        fn_bits(&rt_musicgen_set_tremolo),
        fn_bits(&rt_musicgen_set_arpeggio),
        fn_bits(&rt_musicgen_set_portamento),
        fn_bits(&rt_musicgen_add_note),
        fn_bits(&rt_musicgen_add_note_vel),
        fn_bits(&rt_musicgen_set_length),
        fn_bits(&rt_musicgen_set_swing),
        fn_bits(&rt_musicgen_set_loopable),
        fn_bits(&rt_musicgen_get_bpm),
        fn_bits(&rt_musicgen_get_length),
        fn_bits(&rt_musicgen_get_channel_count),
        fn_bits(&rt_musicgen_build),
        fn_bits(&rt_sound3d_listener_state_identity),
        fn_bits(&rt_sound3d_listener_state_set),
        fn_bits(&rt_sound3d_get_effective_listener_state),
        fn_bits(&rt_sound3d_set_active_listener_state),
        fn_bits(&rt_sound3d_clear_active_listener_state),
        fn_bits(&rt_sound3d_compute_voice_params),
        fn_bits(&rt_sound3d_set_listener),
        fn_bits(&rt_sound3d_play_at),
        fn_bits(&rt_sound3d_update_voice),
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
        fn_bits(&rt_soundsource3d_set_position_vec),
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

    assert(surface[0] != 0);
    return 0;
}
