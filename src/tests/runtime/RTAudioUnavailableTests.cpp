//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTAudioUnavailableTests.cpp
// Purpose: Verify that audio stub builds fail loudly for load/play operations
//          instead of silently returning fake success values.
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_audio.h"
#include "rt_mixgroup.h"
#include "rt_musicgen.h"
#include "rt_soundbank.h"
#include "rt_synth.h"
#include "tests/common/PlatformSkip.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <setjmp.h>

#ifndef ZANNA_ENABLE_AUDIO
namespace {

using TrapFn = void (*)();

static void expect_invalid_operation(TrapFn fn, const char *snippet) {
    jmp_buf env;
    rt_trap_set_recovery(&env);
    if (setjmp(env) == 0) {
        fn();
        rt_trap_clear_recovery();
        assert(false && "expected trap");
    }

    const char *message = rt_trap_get_error();
    assert(rt_trap_get_kind() == RT_TRAP_KIND_INVALID_OPERATION);
    assert(rt_trap_get_code() == Err_InvalidOperation);
    assert(message != nullptr);
    assert(std::strstr(message, snippet) != nullptr);
    rt_trap_clear_recovery();
}

static void trap_sound_load() {
    (void)rt_sound_load(rt_const_cstr("missing.wav"));
}

static void trap_sound_load_asset() {
    (void)rt_sound_load_asset(rt_const_cstr("asset://missing.wav"));
}

static void trap_sound_play() {
    (void)rt_sound_play(reinterpret_cast<void *>(1));
}

static void trap_music_load() {
    (void)rt_music_load(rt_const_cstr("missing.ogg"));
}

static void trap_music_load_asset() {
    (void)rt_music_load_asset(rt_const_cstr("asset://missing.mp3"));
}

static void trap_music_play() {
    rt_music_play(reinterpret_cast<void *>(1), 0);
}

static void test_builder_apis_return_null_without_trapping() {
    assert(rt_synth_tone(440, 10, 0) == nullptr);
    assert(rt_synth_sweep(220, 440, 10, 1) == nullptr);
    assert(rt_synth_noise(10, 50) == nullptr);
    assert(rt_synth_sfx(0) == nullptr);

    void *song = rt_musicgen_new(120);
    assert(song != nullptr);
    int64_t ch = rt_musicgen_add_channel(song, 1);
    assert(ch == 0);
    rt_musicgen_set_length(song, 100);
    assert(rt_musicgen_add_note(song, ch, 0, 60, 50) == 1);
    assert(rt_musicgen_build(song) == nullptr);
}

static void test_soundbank_register_returns_failure_without_trapping() {
    void *bank = rt_soundbank_new();
    assert(bank != nullptr);
    assert(rt_soundbank_register(bank, rt_const_cstr("jump"), rt_const_cstr("jump.wav")) == 0);
    assert(rt_soundbank_count(bank) == 0);
}

static void test_null_handle_apis_do_not_trap() {
    assert(rt_sound_load(nullptr) == nullptr);
    assert(rt_sound_load_asset(nullptr) == nullptr);
    assert(rt_sound_load_mem(nullptr, 16) == nullptr);
    assert(rt_sound_load_mem("RIFF", 0) == nullptr);
    assert(rt_sound_play(nullptr) == -1);
    assert(rt_sound_play_ex(nullptr, 50, 0) == -1);
    assert(rt_sound_play_loop(nullptr, 50, 0) == -1);
    assert(rt_sound_play_in_group(nullptr, RT_MIXGROUP_SFX) == -1);
    assert(rt_sound_play_ex_in_group(nullptr, 50, 0, RT_MIXGROUP_SFX) == -1);
    assert(rt_sound_play_loop_in_group(nullptr, 50, 0, RT_MIXGROUP_SFX) == -1);

    assert(rt_music_load(nullptr) == nullptr);
    assert(rt_music_load_asset(nullptr) == nullptr);
    rt_music_play(nullptr, 0);
    rt_music_stop(nullptr);
    rt_music_pause(nullptr);
    rt_music_resume(nullptr);
    rt_music_set_loop(nullptr, 1);
    rt_music_set_volume(nullptr, 50);
    rt_music_seek(nullptr, 100);
    assert(rt_music_get_volume(nullptr) == 0);
    assert(rt_music_is_playing(nullptr) == 0);
    assert(rt_music_get_position(nullptr) == 0);
    assert(rt_music_get_duration(nullptr) == 0);
    rt_music_pause_related(nullptr);
    rt_music_resume_related(nullptr);
    rt_music_stop_related(nullptr);
    rt_music_set_crossfade_pair_volume(nullptr, 50);
    rt_music_crossfade_to(nullptr, nullptr, 250);
}

} // namespace
#endif

int main() {
#ifdef ZANNA_ENABLE_AUDIO
    assert(rt_audio_is_available() == 1);
    ZANNA_PLATFORM_SKIP("audio enabled in this build");
#else
    assert(rt_audio_is_available() == 0);
    expect_invalid_operation(trap_sound_load, "not compiled in");
    expect_invalid_operation(trap_sound_load_asset, "not compiled in");
    expect_invalid_operation(trap_sound_play, "not compiled in");
    expect_invalid_operation(trap_music_load, "not compiled in");
    expect_invalid_operation(trap_music_load_asset, "not compiled in");
    expect_invalid_operation(trap_music_play, "not compiled in");
    test_builder_apis_return_null_without_trapping();
    test_soundbank_register_returns_failure_without_trapping();
    test_null_handle_apis_do_not_trap();
    std::printf("All audio-unavailable tests passed.\n");
    return 0;
#endif
}
