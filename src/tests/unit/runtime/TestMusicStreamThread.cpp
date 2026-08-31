//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/TestMusicStreamThread.cpp
// Purpose: Regression tests for the background music streaming thread: a
//          playing stream must keep advancing past its ~0.5 s ring prefill
//          with ZERO vaud_update() calls from the app thread (the
//          loading-screen stall scenario), and stop/free must remain safe
//          while the streamer is live.
// Key invariants:
//   - No test thread ever calls vaud_update(); refills come only from the
//     context's background streamer thread.
//   - Device-dependent: vaud_create() may fail headless; tests skip then.
// Ownership/Lifetime:
//   - Each test owns its context and music handles and destroys them.
// Links: src/lib/audio/src/vaud.c,
//        docs/adr/0307-vaud-background-music-streaming-thread.md
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
#include "vaud.h"
}

#include "../../common/Mp3Fixtures.hpp"

namespace {

const char *kFixturePath = "/tmp/zanna_test_stream_thread_tone.mp3";

bool write_fixture() {
    FILE *f = fopen(kFixturePath, "wb");
    if (!f)
        return false;
    size_t written = fwrite(
        zanna_test_mp3::kMp3ToneMpeg1Stereo48k, 1, zanna_test_mp3::kMp3ToneMpeg1Stereo48kSize, f);
    fclose(f);
    return written == zanna_test_mp3::kMp3ToneMpeg1Stereo48kSize;
}

void set_silent_output_env() {
#if defined(VAUD_PLATFORM_WINDOWS)
    _putenv_s("ZANNA_AUDIO_SILENT", "1");
#else
    setenv("ZANNA_AUDIO_SILENT", "1", 1);
#endif
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

// The loading-stall regression: the app thread never pumps, yet playback must
// advance well past the ~557 ms ring prefill. Before the streamer thread
// existed, position froze at the prefill ceiling and the track went silent
// while vaud_music_is_playing() kept reporting 1.
TEST(MusicStreamThread, PlaybackSurvivesAppThreadStall) {
    set_silent_output_env();
    ASSERT_TRUE(write_fixture());

    vaud_context_t ctx = vaud_create();
    if (!ctx) {
        remove(kFixturePath);
        ZANNA_TEST_SKIP("no audio device available");
    }

    vaud_music_t music = vaud_load_music_mp3(ctx, kFixturePath);
    ASSERT_TRUE(music != nullptr);

    vaud_music_set_volume(music, 0.25f);
    vaud_music_play(music, /*loop=*/1);

    // Poll the position; NEVER call vaud_update() here. The fixture is
    // ~1.056 s and looping wraps position back to zero, so track the maximum
    // observed position rather than the final one.
    float max_pos = 0.0f;
    for (int i = 0; i < 60; i++) {
        sleep_ms(25);
        float pos = vaud_music_get_position(music);
        if (pos > max_pos)
            max_pos = pos;
        if (max_pos > 0.8f)
            break;
    }

    EXPECT_GT(max_pos, 0.75f);
    EXPECT_EQ(vaud_music_is_playing(music), 1);

    vaud_music_stop(music);
    vaud_free_music(music);
    vaud_destroy(ctx);
    remove(kFixturePath);
}

// Stop/free racing the live streamer: repeated immediate teardown must never
// deadlock, crash, or leave a stale refill claim behind (exercises the
// exclusive lock-no-refill ownership path in play/stop/free).
TEST(MusicStreamThread, ImmediateFreeUnderStreamerIsSafe) {
    set_silent_output_env();
    ASSERT_TRUE(write_fixture());

    vaud_context_t ctx = vaud_create();
    if (!ctx) {
        remove(kFixturePath);
        ZANNA_TEST_SKIP("no audio device available");
    }

    for (int i = 0; i < 12; i++) {
        vaud_music_t music = vaud_load_music_mp3(ctx, kFixturePath);
        ASSERT_TRUE(music != nullptr);
        vaud_music_play(music, /*loop=*/1);
        if (i % 3 == 0)
            sleep_ms(30); // let the streamer claim some refills first
        vaud_music_stop(music);
        vaud_free_music(music);
    }

    vaud_destroy(ctx);
    remove(kFixturePath);
}

int main() {
    return zanna_test::run_all_tests();
}
