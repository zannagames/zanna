//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// RTAudioIntegrationTests.cpp - Integration tests for audio + playlist APIs
//
// Tests the audio system and playlist management APIs in a headless
// environment. Audio hardware is optional — the runtime gracefully
// degrades when no device is available. These tests exercise the API
// surface, verify null-safety, and confirm playlist manipulation semantics.
//===----------------------------------------------------------------------===//

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_audio_internal.h"
#include "rt_internal.h"
#include "rt_mixgroup.h"
#include "rt_object.h"
#include "rt_playlist.h"
#include "rt_random.h"
#include "rt_string.h"
#include "rt_time.h"

/// @brief Vm_trap.
void vm_trap(const char *msg) {
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}
}

#include "ZpakWriter.hpp"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

static rt_string make_str(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

//=============================================================================
// Audio system tests (headless-safe)
//=============================================================================

static void test_audio_init() {
    // Init may succeed or fail depending on hardware — both are valid
    int64_t result = rt_audio_init();
    ASSERT(result == 0 || result == 1, "audio init returns 0 or 1");
}

static void test_audio_default_volume_before_init() {
    if (!rt_audio_is_available()) {
        ASSERT(1, "audio default volume skipped when audio is unavailable");
        return;
    }
    rt_audio_shutdown();
    ASSERT(rt_audio_get_master_volume() == 100, "master volume defaults to 100 before init");
}

static void test_audio_volume() {
    // Volume functions are no-ops without hardware but shouldn't crash
    rt_audio_set_master_volume(75);
    int64_t vol = rt_audio_get_master_volume();
    // If audio initialized, vol should be 75; if not, default 100 or 0
    ASSERT(vol >= 0 && vol <= 100, "master volume in valid range");
}

static void test_audio_pause_resume() {
    // These are safe no-ops without hardware
    rt_audio_pause_all();
    rt_audio_resume_all();
    rt_audio_stop_all_sounds();
    ASSERT(1, "pause/resume/stop don't crash");
}

static void test_sound_null_safety() {
    // NULL sound handle operations
    rt_sound_destroy(NULL);
    int64_t voice = rt_sound_play(NULL);
    ASSERT(voice == -1, "play null sound returns -1");

    int64_t voice2 = rt_sound_play_ex(NULL, 50, 0);
    ASSERT(voice2 == -1, "play_ex null sound returns -1");

    int64_t voice3 = rt_sound_play_loop(NULL, 50, 0);
    ASSERT(voice3 == -1, "play_loop null sound returns -1");
}

static void test_embedded_nul_paths_rejected_before_backend_open() {
    const char path_bytes[] = {'/', 't', 'm', 'p', '/', 'v', 'i', 'p', 'e', 'r', '\0', 'x'};
    rt_string path = rt_string_from_bytes(path_bytes, sizeof(path_bytes));
    ASSERT(rt_sound_load(path) == NULL, "Sound.Load rejects embedded-NUL paths");
    ASSERT(rt_music_load(path) == NULL, "Music.Load rejects embedded-NUL paths");
    rt_string_unref(path);
}

static void test_voice_null_safety() {
    // Voice operations on invalid IDs
    rt_voice_stop(0);
    rt_voice_stop(999999);
    rt_voice_set_volume(0, 50);
    rt_voice_set_pan(0, 0);
    ASSERT(rt_voice_is_playing(0) == 0, "invalid voice not playing");
    ASSERT(rt_voice_is_playing(999999) == 0, "invalid voice not playing");
}

static void test_music_null_safety() {
    rt_music_destroy(NULL);
    rt_music_play(NULL, 0);
    rt_music_stop(NULL);
    rt_music_pause(NULL);
    rt_music_resume(NULL);
    rt_music_set_volume(NULL, 50);
    ASSERT(rt_music_get_volume(NULL) == 0, "null music volume = 0");
    ASSERT(rt_music_is_playing(NULL) == 0, "null music not playing");
    ASSERT(rt_music_get_position(NULL) == 0, "null music position = 0");
    ASSERT(rt_music_get_duration(NULL) == 0, "null music duration = 0");
    rt_music_seek(NULL, 0);
    ASSERT(1, "music null operations don't crash");
}

//=============================================================================
// Playlist management tests (pure data structure, no audio needed)
//=============================================================================

static void test_playlist_new() {
    void *pl = rt_playlist_new();
    ASSERT(pl != NULL, "playlist created");
    ASSERT(rt_playlist_len(pl) == 0, "new playlist is empty");
    ASSERT(rt_playlist_get_current(pl) == -1, "no current track");
    ASSERT(rt_playlist_is_playing(pl) == 0, "not playing");
    ASSERT(rt_playlist_is_paused(pl) == 0, "not paused");
}

static void test_playlist_add_remove() {
    void *pl = rt_playlist_new();
    rt_string track1 = make_str("track1.wav");
    rt_string track2 = make_str("track2.wav");
    rt_string track3 = make_str("track3.wav");

    rt_playlist_add(pl, track1);
    ASSERT(rt_playlist_len(pl) == 1, "added 1 track");

    rt_playlist_add(pl, track2);
    rt_playlist_add(pl, track3);
    ASSERT(rt_playlist_len(pl) == 3, "added 3 tracks");

    // Verify track at index
    rt_string got = rt_playlist_get(pl, 0);
    if (got) {
        ASSERT(strcmp(rt_string_cstr(got), "track1.wav") == 0, "track 0 = track1.wav");
        rt_str_release_maybe(got);
    }

    got = rt_playlist_get(pl, 2);
    if (got) {
        ASSERT(strcmp(rt_string_cstr(got), "track3.wav") == 0, "track 2 = track3.wav");
        rt_str_release_maybe(got);
    }

    // Remove middle track
    rt_playlist_remove(pl, 1);
    ASSERT(rt_playlist_len(pl) == 2, "removed 1 track");

    got = rt_playlist_get(pl, 1);
    if (got) {
        ASSERT(strcmp(rt_string_cstr(got), "track3.wav") == 0,
               "after remove: track 1 = track3.wav");
        rt_str_release_maybe(got);
    }
}

static void test_playlist_insert() {
    void *pl = rt_playlist_new();
    rt_string a = make_str("a.wav");
    rt_string b = make_str("b.wav");
    rt_string c = make_str("c.wav");

    rt_playlist_add(pl, a);
    rt_playlist_add(pl, c);
    ASSERT(rt_playlist_len(pl) == 2, "2 tracks");

    // Insert b at position 1 (between a and c)
    rt_playlist_insert(pl, 1, b);
    ASSERT(rt_playlist_len(pl) == 3, "3 tracks after insert");

    rt_string got = rt_playlist_get(pl, 1);
    if (got) {
        ASSERT(strcmp(rt_string_cstr(got), "b.wav") == 0, "inserted at position 1");
        rt_str_release_maybe(got);
    }
}

static void test_playlist_insert_clamps_indices() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("b.wav"));
    rt_playlist_insert(pl, -10, make_str("a.wav"));
    rt_playlist_insert(pl, 99, make_str("c.wav"));

    ASSERT(rt_playlist_len(pl) == 3, "clamped inserts still add tracks");
    rt_string got0 = rt_playlist_get(pl, 0);
    ASSERT(strcmp(rt_string_cstr(got0), "a.wav") == 0, "negative insert clamps to front");
    rt_str_release_maybe(got0);
    rt_string got2 = rt_playlist_get(pl, 2);
    ASSERT(strcmp(rt_string_cstr(got2), "c.wav") == 0, "oversized insert clamps to end");
    rt_str_release_maybe(got2);
}

static void test_playlist_clear() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("x.wav"));
    rt_playlist_add(pl, make_str("y.wav"));
    ASSERT(rt_playlist_len(pl) == 2, "2 tracks before clear");

    rt_playlist_clear(pl);
    ASSERT(rt_playlist_len(pl) == 0, "empty after clear");
    ASSERT(rt_playlist_get_current(pl) == -1, "no current after clear");
}

static void test_playlist_volume() {
    void *pl = rt_playlist_new();

    rt_playlist_set_volume(pl, 80);
    ASSERT(rt_playlist_get_volume(pl) == 80, "volume = 80");

    rt_playlist_set_volume(pl, 0);
    ASSERT(rt_playlist_get_volume(pl) == 0, "volume = 0");

    rt_playlist_set_volume(pl, 100);
    ASSERT(rt_playlist_get_volume(pl) == 100, "volume = 100");
}

static void test_playlist_shuffle_repeat() {
    void *pl = rt_playlist_new();

    // Shuffle
    ASSERT(rt_playlist_get_shuffle(pl) == 0, "shuffle off by default");
    rt_playlist_set_shuffle(pl, 1);
    ASSERT(rt_playlist_get_shuffle(pl) == 1, "shuffle on");
    rt_playlist_set_shuffle(pl, 0);
    ASSERT(rt_playlist_get_shuffle(pl) == 0, "shuffle off");

    // Repeat modes
    ASSERT(rt_playlist_get_repeat(pl) == 0, "no repeat by default");
    rt_playlist_set_repeat(pl, 1);
    ASSERT(rt_playlist_get_repeat(pl) == 1, "repeat all");
    rt_playlist_set_repeat(pl, 2);
    ASSERT(rt_playlist_get_repeat(pl) == 2, "repeat one");
    rt_playlist_set_repeat(pl, 0);
    ASSERT(rt_playlist_get_repeat(pl) == 0, "no repeat");
}

static void test_playlist_navigation() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));

    // Jump to track
    rt_playlist_jump(pl, 1);
    ASSERT(rt_playlist_get_current(pl) == 1, "jumped to track 1");

    // Next
    rt_playlist_next(pl);
    ASSERT(rt_playlist_get_current(pl) == 2, "next -> track 2");

    // Prev
    rt_playlist_prev(pl);
    ASSERT(rt_playlist_get_current(pl) == 1, "prev -> track 1");

    // Jump to beginning
    rt_playlist_jump(pl, 0);
    ASSERT(rt_playlist_get_current(pl) == 0, "jumped to track 0");
}

static void test_playlist_skip_on_error_terminates() {
    // VDOC-119: Play() and auto-advance skip unloadable entries with a
    // bounded scan; a playlist whose every entry fails stops cleanly instead
    // of stalling on the first bad track or looping forever under repeat-all.
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("missing_a.wav"));
    rt_playlist_add(pl, make_str("missing_b.wav"));
    rt_playlist_add(pl, make_str("missing_c.wav"));
    rt_playlist_set_repeat(pl, RT_REPEAT_ALL);

    rt_playlist_play(pl); // must terminate despite repeat-all
    ASSERT(rt_playlist_is_playing(pl) == 0, "all-unloadable playlist is not playing");

    rt_playlist_update(pl); // no-op; must not stall or crash
    ASSERT(rt_playlist_is_playing(pl) == 0, "update stays stopped");
}

static void test_playlist_shuffle_preserves_current_track() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));

    rt_playlist_jump(pl, 1);
    ASSERT(rt_playlist_get_current(pl) == 1, "current starts on actual track 1");

    rt_playlist_set_shuffle(pl, 1);
    ASSERT(rt_playlist_get_current(pl) == 1, "enabling shuffle preserves current actual track");

    rt_playlist_set_shuffle(pl, 0);
    ASSERT(rt_playlist_get_current(pl) == 1, "disabling shuffle preserves current actual track");
}

static void test_playlist_jump_uses_actual_index_in_shuffle_mode() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));

    rt_playlist_set_shuffle(pl, 1);
    rt_playlist_jump(pl, 2);
    ASSERT(rt_playlist_get_current(pl) == 2, "jump uses actual track index while shuffled");
}

static void collect_seeded_shuffle_order(int64_t out[4]);

static void test_playlist_shuffle_remove_current_preserves_successor() {
    int64_t order[4] = {};
    collect_seeded_shuffle_order(order);

    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));
    rt_playlist_add(pl, make_str("four.wav"));
    rt_randomize_i64(12345);
    rt_playlist_set_shuffle(pl, 1);
    rt_playlist_jump(pl, order[1]);

    int64_t expected = order[2] > order[1] ? order[2] - 1 : order[2];
    rt_playlist_remove(pl, order[1]);
    ASSERT(rt_playlist_get_current(pl) == expected,
           "removing shuffled current track preserves its logical successor");
}

static void test_playlist_crossfade_duration_is_bounded() {
    void *pl = rt_playlist_new();
    rt_playlist_set_crossfade(pl, -1);
    ASSERT(rt_playlist_get_crossfade(pl) == 0, "negative playlist crossfade clamps to zero");
    rt_playlist_set_crossfade(pl, INT64_MAX);
    ASSERT(rt_playlist_get_crossfade(pl) == 3600000,
           "extreme playlist crossfade clamps to one hour");
}

static void collect_seeded_shuffle_order(int64_t out[4]) {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));
    rt_playlist_add(pl, make_str("four.wav"));

    rt_randomize_i64(12345);
    rt_playlist_set_shuffle(pl, 1);
    rt_playlist_set_repeat(pl, 1);

    for (int i = 0; i < 4; i++) {
        rt_playlist_next(pl);
        out[i] = rt_playlist_get_current(pl);
    }
}

static void test_playlist_shuffle_uses_runtime_seed() {
    int64_t first[4] = {};
    int64_t second[4] = {};
    collect_seeded_shuffle_order(first);
    collect_seeded_shuffle_order(second);

    for (int i = 0; i < 4; i++)
        ASSERT(first[i] == second[i], "seeded playlist shuffle is reproducible");
}

static void test_playlist_stop_preserves_selected_track() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("one.wav"));
    rt_playlist_add(pl, make_str("two.wav"));
    rt_playlist_add(pl, make_str("three.wav"));

    rt_playlist_jump(pl, 2);
    ASSERT(rt_playlist_get_current(pl) == 2, "selected track 2 before stop");

    rt_playlist_stop(pl);
    ASSERT(rt_playlist_get_current(pl) == 2, "stop preserves selected track");
}

static void test_playlist_update_no_crash() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("song.wav"));

    // Update should not crash even without audio
    rt_playlist_update(pl);
    rt_playlist_update(pl);
    ASSERT(1, "playlist update doesn't crash");
}

static void test_playlist_null_safety() {
    // All operations on NULL should be safe
    rt_playlist_add(NULL, make_str("x.wav"));
    rt_playlist_insert(NULL, 0, make_str("x.wav"));
    rt_playlist_remove(NULL, 0);
    rt_playlist_clear(NULL);
    ASSERT(rt_playlist_len(NULL) == 0, "null len = 0");
    // rt_playlist_get returns empty string for null/invalid, not NULL
    {
        rt_string got = rt_playlist_get(NULL, 0);
        ASSERT(got != NULL, "null playlist get returns non-null (empty string)");
        rt_str_release_maybe(got);
    }
    rt_playlist_play(NULL);
    rt_playlist_pause(NULL);
    rt_playlist_stop(NULL);
    rt_playlist_next(NULL);
    rt_playlist_prev(NULL);
    rt_playlist_jump(NULL, 0);
    ASSERT(rt_playlist_get_current(NULL) == -1, "null current = -1");
    ASSERT(rt_playlist_is_playing(NULL) == 0, "null not playing");
    ASSERT(rt_playlist_is_paused(NULL) == 0, "null not paused");
    ASSERT(rt_playlist_get_volume(NULL) == 0, "null volume = 0");
    rt_playlist_set_volume(NULL, 50);
    rt_playlist_set_shuffle(NULL, 1);
    ASSERT(rt_playlist_get_shuffle(NULL) == 0, "null shuffle = 0");
    rt_playlist_set_repeat(NULL, 1);
    ASSERT(rt_playlist_get_repeat(NULL) == 0, "null repeat = 0");
    rt_playlist_update(NULL);
    ASSERT(1, "all null operations safe");
}

static void test_playlist_rejects_non_playlist_handle() {
    void *fake = rt_obj_new_i64(0, 8);
    ASSERT(fake != nullptr, "fake object allocated");

    rt_playlist_add(fake, rt_const_cstr("x.wav"));
    rt_playlist_insert(fake, 0, rt_const_cstr("x.wav"));
    rt_playlist_remove(fake, 0);
    rt_playlist_clear(fake);
    ASSERT(rt_playlist_len(fake) == 0, "fake playlist len = 0");

    rt_string got = rt_playlist_get(fake, 0);
    ASSERT(got != nullptr && rt_str_len(got) == 0, "fake playlist get returns empty string");
    rt_str_release_maybe(got);

    rt_playlist_play(fake);
    rt_playlist_pause(fake);
    rt_playlist_stop(fake);
    rt_playlist_next(fake);
    rt_playlist_prev(fake);
    rt_playlist_jump(fake, 0);
    ASSERT(rt_playlist_get_current(fake) == -1, "fake playlist current = -1");
    ASSERT(rt_playlist_is_playing(fake) == 0, "fake playlist not playing");
    ASSERT(rt_playlist_is_paused(fake) == 0, "fake playlist not paused");
    ASSERT(rt_playlist_get_volume(fake) == 0, "fake playlist volume = 0");
    rt_playlist_set_volume(fake, 50);
    rt_playlist_set_shuffle(fake, 1);
    ASSERT(rt_playlist_get_shuffle(fake) == 0, "fake playlist shuffle = 0");
    rt_playlist_set_repeat(fake, 1);
    ASSERT(rt_playlist_get_repeat(fake) == 0, "fake playlist repeat = 0");
    rt_playlist_set_crossfade(fake, 500);
    ASSERT(rt_playlist_get_crossfade(fake) == 0, "fake playlist crossfade = 0");
    rt_playlist_update(fake);

    release_obj(fake);
}

static void test_playlist_bounds() {
    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str("a.wav"));

    // Out-of-bounds operations return empty string
    {
        rt_string got1 = rt_playlist_get(pl, -1);
        ASSERT(got1 != NULL && rt_str_len(got1) == 0, "negative index = empty string");
        rt_str_release_maybe(got1);
        rt_string got2 = rt_playlist_get(pl, 100);
        ASSERT(got2 != NULL && rt_str_len(got2) == 0, "out of bounds = empty string");
        rt_str_release_maybe(got2);
    }

    /// @brief Rt_playlist_remove.
    rt_playlist_remove(pl, 99); // should not crash
                                /// @brief Rt_playlist_jump.
    rt_playlist_jump(pl, 99);   // should not crash
    ASSERT(1, "out-of-bounds operations safe");
}

//=============================================================================
// Bug-fix regression tests
//=============================================================================

// C-1 / C-2 / C-3: Playlist shuffle_order memory lifecycle.
//
// Before the fix:
//   C-2: generate_shuffle_order() abandoned the old shuffle_order seq on every
//        reshuffle without releasing it.
//   C-3: rt_playlist_clear() set shuffle_order = NULL without releasing it.
//   C-1: rt_playlist_new() never registered a finalizer, so all of the above
//        also leaked if the playlist was GC'd without an explicit call to clear.
//
// After the fix, all three release paths correctly call rt_obj_release_check0
// + rt_obj_free. These tests verify no crash occurs through all lifecycle
// transitions that previously triggered the leaks.
static void test_playlist_shuffle_lifecycle() {
    void *pl = rt_playlist_new();
    ASSERT(pl != NULL, "playlist created");

    // Enable shuffle before adding tracks (empty → no shuffle_order generated yet)
    rt_playlist_set_shuffle(pl, 1);
    ASSERT(rt_playlist_get_shuffle(pl) == 1, "shuffle enabled");

    // Adding the first track generates shuffle_order for the first time (nothing to release)
    rt_playlist_add(pl, make_str("a.wav"));
    ASSERT(rt_playlist_len(pl) == 1, "1 track");

    // Each subsequent add reshuffles — C-2: old shuffle_order must be released, not leaked
    rt_playlist_add(pl, make_str("b.wav"));
    rt_playlist_add(pl, make_str("c.wav"));
    ASSERT(rt_playlist_len(pl) == 3, "3 tracks after adds");

    // Toggling shuffle off then on re-generates shuffle_order (C-2 path again)
    rt_playlist_set_shuffle(pl, 0);
    rt_playlist_set_shuffle(pl, 1);
    ASSERT(rt_playlist_get_shuffle(pl) == 1, "shuffle re-enabled");

    // Clear — C-3: shuffle_order must be released, not leaked
    rt_playlist_clear(pl);
    ASSERT(rt_playlist_len(pl) == 0, "empty after clear");
    ASSERT(rt_playlist_get_current(pl) == -1, "no current after clear");

    // Post-clear add — shuffle_order must regenerate cleanly from NULL
    rt_playlist_add(pl, make_str("d.wav"));
    ASSERT(rt_playlist_len(pl) == 1, "can add after clear+shuffle");

    ASSERT(1, "shuffle lifecycle: no crash");
}

// C-2: Stress the reshuffle path with many cycles.
// If the old shuffle_order is leaked each time, ASAN / valgrind will catch it.
static void test_playlist_shuffle_many_reshuffles() {
    void *pl = rt_playlist_new();
    rt_playlist_set_shuffle(pl, 1);

    // 20 adds with shuffle on = 20 shuffle_order generations; each must release the previous
    for (int i = 0; i < 20; i++)
        rt_playlist_add(pl, make_str("track.wav"));
    ASSERT(rt_playlist_len(pl) == 20, "20 tracks");

    // Toggle shuffle 10 times = 10 more release/reallocate cycles
    for (int i = 0; i < 10; i++) {
        rt_playlist_set_shuffle(pl, 0);
        rt_playlist_set_shuffle(pl, 1);
    }
    ASSERT(rt_playlist_get_shuffle(pl) == 1, "shuffle on after toggles");

    /// @brief Rt_playlist_clear.
    rt_playlist_clear(pl); // C-3: final release
    ASSERT(rt_playlist_len(pl) == 0, "cleared");

    ASSERT(1, "many reshuffles: no crash");
}

// C-3: Clear with shuffle enabled — shuffle_order must be released.
static void test_playlist_clear_releases_shuffle_order() {
    void *pl = rt_playlist_new();
    rt_playlist_set_shuffle(pl, 1);
    rt_playlist_add(pl, make_str("x.wav"));
    rt_playlist_add(pl, make_str("y.wav"));

    // Multiple clears must each handle NULL / non-NULL shuffle_order safely
    rt_playlist_clear(pl);
    ASSERT(rt_playlist_len(pl) == 0, "cleared once");

    /// @brief Rt_playlist_clear.
    rt_playlist_clear(pl); // Second clear: shuffle_order is NULL — must not double-free
    ASSERT(rt_playlist_len(pl) == 0, "cleared twice (idempotent)");

    ASSERT(1, "clear releases shuffle_order safely");
}

//=============================================================================
// H-7: WAV sample_rate validation
//
// A WAV file with sample_rate=0 in its header previously caused a division-
// by-zero crash inside the resampler. After the fix, parse_wav_header rejects
// invalid sample rates and vaud_load_sound returns NULL gracefully.
//=============================================================================

// Write a minimal PCM WAV file with the given sample_rate to `path`.
// Returns 1 on success, 0 if the temp file could not be written.
static int write_test_wav_frames(const char *path, uint32_t sample_rate, uint32_t frame_count) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;

    // RIFF/WAVE header (12 bytes)
    fwrite("RIFF", 1, 4, f);
    uint32_t data_sz = frame_count * 2; // 16-bit mono PCM
    uint32_t riff_sz = 36 + data_sz;
    fwrite(&riff_sz, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk (24 bytes)
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_sz = 16;
    fwrite(&fmt_sz, 4, 1, f);
    uint16_t audio_fmt = 1; // PCM
    fwrite(&audio_fmt, 2, 1, f);
    uint16_t channels = 1;
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    uint32_t byte_rate = sample_rate * 2; // sr * ch * bytes_per_sample
    fwrite(&byte_rate, 4, 1, f);
    uint16_t block_align = 2; // ch * bytes_per_sample
    fwrite(&block_align, 2, 1, f);
    uint16_t bits = 16;
    fwrite(&bits, 2, 1, f);

    // data chunk (10 bytes)
    fwrite("data", 1, 4, f);
    fwrite(&data_sz, 4, 1, f);
    uint16_t sample = 0;
    for (uint32_t i = 0; i < frame_count; i++)
        fwrite(&sample, 2, 1, f);

    fclose(f);
    return 1;
}

static int write_test_wav(const char *path, uint32_t sample_rate) {
    return write_test_wav_frames(path, sample_rate, 1);
}

static int read_file_bytes(const char *path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    out.resize((size_t)size);
    int ok = size == 0 || fread(out.data(), 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static void test_wav_zero_sample_rate() {
    const char *path = "/tmp/zanna_test_wav_zero_sr.wav";
    if (!write_test_wav(path, 0)) {
        ASSERT(1, "could not write temp WAV file (skip H-7 test)");
        return;
    }

    void *snd = rt_sound_load(make_str(path));
    ASSERT(snd == NULL, "H-7: WAV with sample_rate=0 returns NULL (no crash)");

    remove(path);
}

static void test_wav_extreme_sample_rate() {
    // sample_rate > 384000 is also rejected (H-7 upper bound)
    const char *path = "/tmp/zanna_test_wav_extreme_sr.wav";
    if (!write_test_wav(path, 999999999u)) {
        ASSERT(1, "could not write temp WAV file (skip)");
        return;
    }

    void *snd = rt_sound_load(make_str(path));
    ASSERT(snd == NULL, "H-7: WAV with sample_rate=999999999 returns NULL");

    remove(path);
}

static void test_wav_valid_sample_rate() {
    // Positive control: a well-formed single-sample WAV at 44100 Hz
    const char *path = "/tmp/zanna_test_wav_valid_sr.wav";
    if (!write_test_wav(path, 44100)) {
        ASSERT(1, "could not write temp WAV file (skip)");
        return;
    }

    // The load may succeed (returns non-NULL) or fail due to headless environment
    // (no audio context). What must NOT happen is a crash.
    rt_sound_load(make_str(path)); // return value intentionally ignored
    ASSERT(1, "H-7: valid WAV at 44100 Hz does not crash");

    remove(path);
}

static void test_sound_load_asset_from_mounted_pack() {
    const char *wav_path = "/tmp/zanna_test_sound_load_asset.wav";
    const char *pack_path = "/tmp/zanna_test_sound_load_asset.zpak";
    if (!write_test_wav_frames(wav_path, 44100, 128)) {
        ASSERT(1, "could not write temp WAV file (skip Sound.LoadAsset pack test)");
        return;
    }

    std::vector<uint8_t> wav_bytes;
    if (!read_file_bytes(wav_path, wav_bytes) || wav_bytes.empty()) {
        ASSERT(1, "could not read temp WAV file (skip Sound.LoadAsset pack test)");
        remove(wav_path);
        return;
    }

    zanna::asset::ZpakWriter writer;
    writer.addEntry("audio/pack.wav", wav_bytes.data(), wav_bytes.size(), false);
    std::string err;
    if (!writer.writeToFile(pack_path, err)) {
        ASSERT(1, "could not write ZPAK file (skip Sound.LoadAsset pack test)");
        remove(wav_path);
        return;
    }
    if (rt_asset_mount(make_str(pack_path)) != 1) {
        ASSERT(1, "could not mount ZPAK file (skip Sound.LoadAsset pack test)");
        remove(pack_path);
        remove(wav_path);
        return;
    }

    void *sound = rt_sound_load_asset(make_str("asset://audio/pack.wav"));
    if (sound)
        rt_sound_destroy(sound);
    ASSERT(1, "Sound.LoadAsset reads WAV bytes from a mounted pack without crashing");

    rt_asset_unmount(make_str(pack_path));
    remove(pack_path);
    remove(wav_path);
}

static void test_destroy_loaded_handles_after_shutdown() {
    const char *path = "/tmp/zanna_test_destroy_after_shutdown.wav";
    if (!write_test_wav_frames(path, 44100, 128)) {
        ASSERT(1, "could not write temp WAV file (skip shutdown finalizer test)");
        return;
    }

    void *sound = rt_sound_load(make_str(path));
    void *music = rt_music_load(make_str(path));
    if (!sound && !music) {
        ASSERT(1, "audio load unavailable in environment (skip shutdown finalizer test)");
        remove(path);
        return;
    }

    rt_audio_shutdown();

    if (sound) {
        ASSERT(rt_sound_play(sound) == -1, "detached sound cannot play after shutdown");
        rt_sound_destroy(sound);
    }
    if (music) {
        ASSERT(rt_music_is_playing(music) == 0, "detached music is not playing after shutdown");
        rt_music_crossfade_to(NULL, music, 100);
        ASSERT(rt_music_is_crossfading() == 0, "detached music cannot start a crossfade");
        rt_music_destroy(music);
    }

    remove(path);
}

static void test_default_sound_play_survives_sfx_group_changes() {
    const char *path = "/tmp/zanna_test_default_sound_sfx_group.wav";
    if (!write_test_wav_frames(path, 44100, 256)) {
        ASSERT(1, "could not write temp WAV file (skip SFX group playback test)");
        return;
    }

    void *sound = rt_sound_load(make_str(path));
    if (!sound) {
        ASSERT(1, "sound load unavailable in environment (skip SFX group playback test)");
        remove(path);
        return;
    }

    rt_audio_set_group_volume(RT_MIXGROUP_SFX, 0);
    int64_t voice = rt_sound_play(sound);
    if (voice >= 0) {
        rt_voice_set_volume(voice, 80);
        rt_audio_set_group_volume(RT_MIXGROUP_SFX, 40);
        rt_voice_stop(voice);
        ASSERT(1, "default Sound.Play voice is tracked through SFX group volume changes");
    } else {
        ASSERT(1, "sound playback unavailable in environment (skip SFX group playback test)");
    }

    rt_audio_set_group_volume(RT_MIXGROUP_SFX, 100);
    rt_sound_destroy(sound);
    remove(path);
}

static void test_music_seek_resampled_wav() {
    const char *path = "/tmp/zanna_test_music_seek_22050.wav";
    if (!write_test_wav_frames(path, 22050, 22050)) {
        ASSERT(1, "could not write temp WAV file (skip music seek test)");
        return;
    }

    void *music = rt_music_load(make_str(path));
    if (!music) {
        ASSERT(1, "music load unavailable in headless environment (skip)");
        remove(path);
        return;
    }

    int64_t duration_ms = rt_music_get_duration(music);
    ASSERT(duration_ms >= 950 && duration_ms <= 1050, "resampled WAV duration stays near 1000ms");

    rt_music_seek(music, 500);
    int64_t pos_ms = rt_music_get_position(music);
    ASSERT(pos_ms >= 450 && pos_ms <= 550,
           "resampled WAV seek reports stable millisecond position");

    rt_music_stop(music);
    ASSERT(rt_music_get_position(music) <= 50, "music stop rewinds to the beginning");

    rt_music_destroy(music);
    remove(path);
}

static void test_music_seek_to_duration_reaches_eof() {
    const char *path = "/tmp/zanna_test_music_seek_exact_eof.wav";
    if (!write_test_wav_frames(path, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV file (skip exact EOF seek test)");
        return;
    }

    void *music = rt_music_load(make_str(path));
    if (!music) {
        ASSERT(1, "music load unavailable in environment (skip exact EOF seek test)");
        remove(path);
        return;
    }

    int64_t duration_ms = rt_music_get_duration(music);
    ASSERT(duration_ms >= 950 && duration_ms <= 1050, "exact EOF seek test duration is valid");

    rt_music_seek(music, duration_ms);
    int64_t pos_ms = rt_music_get_position(music);
    ASSERT(pos_ms >= duration_ms - 5 && pos_ms <= duration_ms + 5,
           "seeking to Music.Duration lands exactly at EOF instead of clamping before it");

    rt_music_destroy(music);
    remove(path);
}

static void test_music_seek_huge_position_clamps_to_duration() {
    const char *path = "/tmp/zanna_test_music_seek_huge.wav";
    if (!write_test_wav_frames(path, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV file (skip huge seek test)");
        return;
    }

    void *music = rt_music_load(make_str(path));
    if (!music) {
        ASSERT(1, "music load unavailable in environment (skip huge seek test)");
        remove(path);
        return;
    }

    int64_t duration_ms = rt_music_get_duration(music);
    rt_music_seek(music, INT64_MAX);
    int64_t pos_ms = rt_music_get_position(music);
    ASSERT(pos_ms >= duration_ms - 5 && pos_ms <= duration_ms + 5,
           "huge Music.Seek position clamps to duration");

    rt_music_destroy(music);
    remove(path);
}

static void test_playlist_stopped_jump_releases_old_music_before_play() {
    const char *valid_path = "/tmp/zanna_test_playlist_stopped_jump_valid.wav";
    const char *missing_path = "/tmp/zanna_test_playlist_stopped_jump_missing.wav";
    if (!write_test_wav_frames(valid_path, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV file (skip stopped-jump test)");
        return;
    }
    remove(missing_path);

    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str(valid_path));
    rt_playlist_add(pl, make_str(missing_path));

    rt_playlist_play(pl);
    if (!rt_playlist_is_playing(pl)) {
        ASSERT(1, "playlist playback unavailable in environment (skip stopped-jump test)");
        remove(valid_path);
        return;
    }

    rt_playlist_stop(pl);
    rt_playlist_jump(pl, 1);
    ASSERT(rt_playlist_get_current(pl) == 1, "stopped jump selects missing second track");

    rt_playlist_play(pl);
    ASSERT(rt_playlist_is_playing(pl) == 0,
           "stopped jump does not keep playing the old loaded track");

    remove(valid_path);
}

static void test_playlist_play_after_paused_jump_starts_new_track() {
    const char *path1 = "/tmp/zanna_test_playlist_paused_jump_1.wav";
    const char *path2 = "/tmp/zanna_test_playlist_paused_jump_2.wav";
    const char *path3 = "/tmp/zanna_test_playlist_paused_jump_3.wav";
    if (!write_test_wav_frames(path1, 44100, 4410) || !write_test_wav_frames(path2, 44100, 4410) ||
        !write_test_wav_frames(path3, 44100, 4410)) {
        ASSERT(1, "could not write temp WAV files (skip paused-jump test)");
        return;
    }

    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str(path1));
    rt_playlist_add(pl, make_str(path2));
    rt_playlist_add(pl, make_str(path3));

    rt_playlist_play(pl);
    if (!rt_playlist_is_playing(pl)) {
        ASSERT(1, "playlist playback unavailable in environment (skip paused-jump test)");
        remove(path1);
        remove(path2);
        remove(path3);
        return;
    }

    rt_playlist_pause(pl);
    ASSERT(rt_playlist_is_paused(pl) == 1, "playlist paused before track jump");

    rt_playlist_jump(pl, 1);
    ASSERT(rt_playlist_get_current(pl) == 1, "paused jump selects second track");
    ASSERT(rt_playlist_is_paused(pl) == 1, "paused jump keeps paused state");

    rt_playlist_play(pl);
    ASSERT(rt_playlist_is_playing(pl) == 1, "play resumes playlist after paused jump");

    rt_playlist_update(pl);
    ASSERT(rt_playlist_get_current(pl) == 1,
           "paused jump track remains current after immediate update");

    rt_playlist_stop(pl);
    remove(path1);
    remove(path2);
    remove(path3);
}

static void test_playlist_remove_current_failed_replacement_clears_state() {
    const char *valid_path = "/tmp/zanna_test_playlist_remove_current.wav";
    const char *missing_path = "/tmp/zanna_test_playlist_missing_replacement.wav";
    if (!write_test_wav_frames(valid_path, 44100, 4410)) {
        ASSERT(1, "could not write temp WAV file (skip remove-current test)");
        return;
    }
    remove(missing_path);

    void *pl = rt_playlist_new();
    rt_playlist_add(pl, make_str(valid_path));
    rt_playlist_add(pl, make_str(missing_path));

    rt_playlist_play(pl);
    if (!rt_playlist_is_playing(pl)) {
        ASSERT(1, "playlist playback unavailable in environment (skip remove-current test)");
        remove(valid_path);
        return;
    }

    ASSERT(rt_playlist_get_current(pl) == 0, "first track selected before remove");
    rt_playlist_remove(pl, 0);

    ASSERT(rt_playlist_get_current(pl) == 0, "replacement track becomes current");
    ASSERT(rt_playlist_is_playing(pl) == 0, "failed replacement clears playing state");
    ASSERT(rt_playlist_is_paused(pl) == 0, "failed replacement clears paused state");

    remove(valid_path);
}

static void test_music_seek_does_not_stop_other_music() {
    const char *path1 = "/tmp/zanna_test_music_seek_other_1.wav";
    const char *path2 = "/tmp/zanna_test_music_seek_other_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip seek-other test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip seek-other test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip seek-other test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_seek(music_b, 500);
    ASSERT(rt_music_is_playing(music_a) == 1,
           "seeking an idle track does not stop foreground music");

    rt_music_stop(music_a);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_music_resume_reclaims_foreground() {
    const char *path1 = "/tmp/zanna_test_music_resume_fg_1.wav";
    const char *path2 = "/tmp/zanna_test_music_resume_fg_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip resume-foreground test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip resume-foreground test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip resume-foreground test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_pause(music_a);
    rt_music_play(music_b, 1);
    ASSERT(rt_music_is_playing(music_b) == 1, "second track is foreground before resume");

    rt_music_resume(music_a);
    ASSERT(rt_music_is_playing(music_a) == 1, "resume restarts the paused foreground track");
    ASSERT(rt_music_is_playing(music_b) == 0, "resume stops newer unrelated foreground music");

    rt_music_stop(music_a);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_pause_resume_holds_progress() {
    const char *path1 = "/tmp/zanna_test_crossfade_pause_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_pause_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip crossfade-pause test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip crossfade-pause test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip crossfade-pause test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 200);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade starts");

    rt_music_pause_related(music_a);
    rt_music_crossfade_update(250);
    ASSERT(rt_music_is_crossfading() == 1, "paused crossfade does not finish during manual update");

    rt_music_resume_related(music_a);
    rt_music_crossfade_update(250);
    ASSERT(rt_music_is_crossfading() == 0, "resumed crossfade can complete");

    rt_music_stop(music_b);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_preserves_destination_loop() {
    const char *path1 = "/tmp/zanna_test_crossfade_loop_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_loop_2.wav";
    if (!write_test_wav_frames(path1, 44100, 4410) || !write_test_wav_frames(path2, 44100, 4410)) {
        ASSERT(1, "could not write temp WAV files (skip crossfade-loop test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip crossfade-loop test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip crossfade-loop test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_set_loop(music_b, 1);
    rt_music_crossfade_to(music_a, music_b, 20);
    rt_music_crossfade_update(25);
    ASSERT(rt_music_is_crossfading() == 0, "crossfade into looped destination completes");

    rt_sleep_ms(250);
    ASSERT(rt_music_is_playing(music_b) == 1, "crossfade preserves destination loop flag");

    rt_music_stop(music_b);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_query_is_pure() {
    const char *path1 = "/tmp/zanna_test_crossfade_query_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_query_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip crossfade-query test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip crossfade-query test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip crossfade-query test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 20);
    rt_sleep_ms(50);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade query does not advance elapsed time");
    rt_audio_update();
    ASSERT(rt_music_is_crossfading() == 0, "Audio.Update advances elapsed crossfade time");

    rt_music_stop(music_b);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_completion_after_external_destroy() {
    const char *path1 = "/tmp/zanna_test_crossfade_release_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_release_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip crossfade-release test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip crossfade-release test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip crossfade-release test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 100);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade starts before destroying caller refs");

    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    rt_music_crossfade_update(150);
    ASSERT(rt_music_is_crossfading() == 0, "crossfade completes after external refs are released");

    remove(path1);
    remove(path2);
}

static void test_crossfade_stop_fade_out_keeps_destination() {
    const char *path1 = "/tmp/zanna_test_crossfade_stop_out_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_stop_out_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip stop-fade-out test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip stop-fade-out test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip stop-fade-out test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 500);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade starts before stopping fade-out side");

    rt_music_stop(music_a);
    ASSERT(rt_music_is_crossfading() == 0, "stopping fade-out side cancels only that crossfade");
    ASSERT(rt_music_is_playing(music_a) == 0, "fade-out track stops");
    ASSERT(rt_music_is_playing(music_b) == 1, "fade-in destination keeps playing");

    rt_music_stop(music_b);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_stop_fade_in_restores_source_loop() {
    const char *path1 = "/tmp/zanna_test_crossfade_stop_in_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_stop_in_2.wav";
    if (!write_test_wav_frames(path1, 44100, 4410) || !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip stop-fade-in test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip stop-fade-in test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip stop-fade-in test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 500);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade starts before stopping fade-in side");

    rt_music_stop(music_b);
    ASSERT(rt_music_is_crossfading() == 0, "stopping fade-in side cancels only that crossfade");
    ASSERT(rt_music_is_playing(music_b) == 0, "fade-in track stops");
    ASSERT(rt_music_is_playing(music_a) == 1, "fade-out source keeps playing after cancel");

    rt_sleep_ms(250);
    ASSERT(rt_music_is_playing(music_a) == 1,
           "cancelled crossfade restores the source track's loop flag");

    rt_music_stop(music_a);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_crossfade_set_loop_on_fade_out_still_completes() {
    const char *path1 = "/tmp/zanna_test_crossfade_loop_out_1.wav";
    const char *path2 = "/tmp/zanna_test_crossfade_loop_out_2.wav";
    if (!write_test_wav_frames(path1, 44100, 44100) ||
        !write_test_wav_frames(path2, 44100, 44100)) {
        ASSERT(1, "could not write temp WAV files (skip fade-out-loop test)");
        return;
    }

    void *music_a = rt_music_load(make_str(path1));
    void *music_b = rt_music_load(make_str(path2));
    if (!music_a || !music_b) {
        ASSERT(1, "music load unavailable in environment (skip fade-out-loop test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_play(music_a, 1);
    if (!rt_music_is_playing(music_a)) {
        ASSERT(1, "music playback unavailable in environment (skip fade-out-loop test)");
        rt_music_destroy(music_a);
        rt_music_destroy(music_b);
        remove(path1);
        remove(path2);
        return;
    }

    rt_music_crossfade_to(music_a, music_b, 50);
    ASSERT(rt_music_is_crossfading() == 1, "crossfade starts before fade-out loop change");

    rt_music_set_loop(music_a, 1);
    rt_music_crossfade_update(60);
    ASSERT(rt_music_is_crossfading() == 0, "fade-out loop setter does not prevent completion");
    ASSERT(rt_music_is_playing(music_a) == 0, "fade-out source stops on completion");
    ASSERT(rt_music_is_playing(music_b) == 1, "fade-in destination remains playing");

    rt_music_stop(music_b);
    rt_music_destroy(music_a);
    rt_music_destroy(music_b);
    remove(path1);
    remove(path2);
}

static void test_audio_format_detection_rejects_partial_signatures() {
    const uint8_t riff_only[12] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'N', 'O', 'P', 'E'};
    const uint8_t wav[12] = {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const uint8_t old_ogg[5] = {'O', 'g', 'g', 'S', 1};
    const uint8_t ogg[5] = {'O', 'g', 'g', 'S', 0};
    const uint8_t short_id3[4] = {'I', 'D', '3', 4};
    const uint8_t id3[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};

    ASSERT(detect_audio_format_mem(riff_only, sizeof(riff_only)) == 0,
           "RIFF without WAVE form is rejected");
    ASSERT(detect_audio_format_mem(wav, sizeof(wav)) == 1, "RIFF/WAVE form is detected");
    ASSERT(detect_audio_format_mem(old_ogg, sizeof(old_ogg)) == 0,
           "unsupported Ogg stream version is rejected");
    ASSERT(detect_audio_format_mem(ogg, sizeof(ogg)) == 2, "Ogg version zero is detected");
    ASSERT(detect_audio_format_mem(short_id3, sizeof(short_id3)) == 0,
           "truncated ID3 header is rejected");
    ASSERT(detect_audio_format_mem(id3, sizeof(id3)) == 3, "valid ID3v2 header is detected");
}

static void test_audio_format_detection_validates_mp3_frame_header() {
    const uint8_t valid[4] = {0xFF, 0xFB, 0x90, 0x00};
    const uint8_t reserved_version[4] = {0xFF, 0xEB, 0x90, 0x00};
    const uint8_t reserved_layer[4] = {0xFF, 0xF9, 0x90, 0x00};
    const uint8_t bad_bitrate[4] = {0xFF, 0xFB, 0xF0, 0x00};
    const uint8_t bad_rate[4] = {0xFF, 0xFB, 0x9C, 0x00};
    const uint8_t reserved_emphasis[4] = {0xFF, 0xFB, 0x90, 0x02};

    ASSERT(detect_audio_format_mem(valid, sizeof(valid)) == 3, "valid MP3 frame is detected");
    ASSERT(detect_audio_format_mem(reserved_version, sizeof(reserved_version)) == 0,
           "reserved MPEG version is rejected");
    ASSERT(detect_audio_format_mem(reserved_layer, sizeof(reserved_layer)) == 0,
           "reserved MPEG layer is rejected");
    ASSERT(detect_audio_format_mem(bad_bitrate, sizeof(bad_bitrate)) == 0,
           "reserved MP3 bitrate is rejected");
    ASSERT(detect_audio_format_mem(bad_rate, sizeof(bad_rate)) == 0,
           "reserved MP3 sample rate is rejected");
    ASSERT(detect_audio_format_mem(reserved_emphasis, sizeof(reserved_emphasis)) == 0,
           "reserved MP3 emphasis is rejected");
}

static void test_audio_decode_failures_clear_outputs() {
    const uint8_t malformed[4] = {0, 1, 2, 3};
    uint8_t *out = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(1));
    size_t out_len = SIZE_MAX;

    ASSERT(ogg_mem_to_wav(malformed, sizeof(malformed), &out, &out_len) == -1,
           "malformed memory Ogg decode fails");
    ASSERT(out == nullptr && out_len == 0, "failed memory Ogg decode clears outputs");

    out = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(1));
    out_len = SIZE_MAX;
    ASSERT(mp3_data_to_wav(malformed, sizeof(malformed), &out, &out_len) == -1,
           "malformed memory MP3 decode fails");
    ASSERT(out == nullptr && out_len == 0, "failed memory MP3 decode clears outputs");

    out = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(1));
    out_len = SIZE_MAX;
    ASSERT(ogg_file_to_wav(nullptr, &out, &out_len) == -1, "null Ogg path fails");
    ASSERT(out == nullptr && out_len == 0, "failed file Ogg decode clears outputs");

    out = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(1));
    out_len = SIZE_MAX;
    ASSERT(mp3_file_to_wav(nullptr, &out, &out_len) == -1, "null MP3 path fails");
    ASSERT(out == nullptr && out_len == 0, "failed file MP3 decode clears outputs");
    ASSERT(detect_audio_format(nullptr) == 0, "null format-detection path is rejected");
}

/// @brief Main.
int main() {
    // Audio system (headless-safe)
    test_audio_default_volume_before_init();
    test_audio_init();
    test_audio_volume();
    test_audio_pause_resume();
    test_sound_null_safety();
    test_embedded_nul_paths_rejected_before_backend_open();
    test_voice_null_safety();
    test_music_null_safety();
    test_audio_format_detection_rejects_partial_signatures();
    test_audio_format_detection_validates_mp3_frame_header();
    test_audio_decode_failures_clear_outputs();

    // Playlist management (pure data structure)
    test_playlist_new();
    test_playlist_add_remove();
    test_playlist_insert();
    test_playlist_insert_clamps_indices();
    test_playlist_clear();
    test_playlist_volume();
    test_playlist_shuffle_repeat();
    test_playlist_navigation();
    test_playlist_skip_on_error_terminates();
    test_playlist_shuffle_preserves_current_track();
    test_playlist_jump_uses_actual_index_in_shuffle_mode();
    test_playlist_shuffle_remove_current_preserves_successor();
    test_playlist_crossfade_duration_is_bounded();
    test_playlist_shuffle_uses_runtime_seed();
    test_playlist_stop_preserves_selected_track();
    test_playlist_update_no_crash();
    test_playlist_null_safety();
    test_playlist_rejects_non_playlist_handle();
    test_playlist_bounds();

    // Bug-fix regressions (C-1/C-2/C-3: playlist shuffle_order lifecycle)
    test_playlist_shuffle_lifecycle();
    test_playlist_shuffle_many_reshuffles();
    test_playlist_clear_releases_shuffle_order();

    // Bug-fix regressions (H-7: WAV sample_rate validation)
    test_wav_zero_sample_rate();
    test_wav_extreme_sample_rate();
    test_wav_valid_sample_rate();
    test_sound_load_asset_from_mounted_pack();
    test_destroy_loaded_handles_after_shutdown();
    test_default_sound_play_survives_sfx_group_changes();
    test_music_seek_resampled_wav();
    test_music_seek_to_duration_reaches_eof();
    test_music_seek_huge_position_clamps_to_duration();
    test_music_seek_does_not_stop_other_music();
    test_music_resume_reclaims_foreground();
    test_crossfade_pause_resume_holds_progress();
    test_crossfade_preserves_destination_loop();
    test_crossfade_query_is_pure();
    test_crossfade_completion_after_external_destroy();
    test_crossfade_stop_fade_out_keeps_destination();
    test_crossfade_stop_fade_in_restores_source_loop();
    test_crossfade_set_loop_on_fade_out_still_completes();
    test_playlist_play_after_paused_jump_starts_new_track();
    test_playlist_stopped_jump_releases_old_music_before_play();
    test_playlist_remove_current_failed_replacement_clears_state();

    printf("Audio integration tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
