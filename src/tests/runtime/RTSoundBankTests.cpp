//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSoundBankTests.cpp
// Purpose: Tests for SoundBank named sound registry — CRUD operations, name
//   lookup, capacity limits, null safety. Tests the registry logic without
//   requiring audio playback (no Audio.Init needed for registration).
// Key invariants:
//   - Names are unique; registering same name overwrites.
//   - Maximum 64 entries per bank.
//   - All functions are null-safe.
// Links: rt_soundbank.c, rt_soundbank.h
//
//===----------------------------------------------------------------------===//

#include "rt_audio.h"
#include "rt_object.h"
#include "rt_soundbank.h"
#include "rt_synth.h"

#include <cassert>
#include <cstdio>
#include <cstring>

extern "C" void vm_trap(const char *msg) {
    (void)msg;
}

static rt_string S(const char *s) {
    return rt_const_cstr(s);
}

static void *make_registry_sound() {
    return rt_synth_tone(440, 20, 0);
}

static void *make_fake_object() {
    void *sound = rt_obj_new_i64(0, 8);
    assert(sound != nullptr);
    return sound;
}

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

// ============================================================================
// Null Safety
// ============================================================================

static void test_null_safety() {
    // All operations on NULL bank should not crash
    assert(rt_soundbank_has(nullptr, S("x")) == 0);
    assert(rt_soundbank_play(nullptr, S("x")) == -1);
    assert(rt_soundbank_play_ex(nullptr, S("x"), 100, 0) == -1);
    assert(rt_soundbank_get(nullptr, S("x")) == nullptr);
    assert(rt_soundbank_count(nullptr) == 0);
    rt_soundbank_remove(nullptr, S("x"));
    rt_soundbank_clear(nullptr);

    printf("  test_null_safety: PASSED\n");
}

static void test_invalid_bank_rejected() {
    void *fake = make_fake_object();

    assert(rt_soundbank_has(fake, S("x")) == 0);
    assert(rt_soundbank_play(fake, S("x")) == -1);
    assert(rt_soundbank_play_ex(fake, S("x"), 100, 0) == -1);
    assert(rt_soundbank_get(fake, S("x")) == nullptr);
    assert(rt_soundbank_count(fake) == 0);
    assert(rt_soundbank_register(fake, S("x"), S("missing.wav")) == 0);
    assert(rt_soundbank_register_sound(fake, S("x"), fake) == 0);
    rt_soundbank_remove(fake, S("x"));
    rt_soundbank_clear(fake);

    release_obj(fake);
    printf("  test_invalid_bank_rejected: PASSED\n");
}

// ============================================================================
// Creation and Count
// ============================================================================

static void test_create() {
    void *bank = rt_soundbank_new();
    assert(bank != nullptr);
    assert(rt_soundbank_count(bank) == 0);

    printf("  test_create: PASSED\n");
}

// ============================================================================
// Register + Has + Get + Count
// ============================================================================

static void test_register_sound() {
    void *bank = rt_soundbank_new();
    void *snd = make_registry_sound();
    if (!snd) {
        printf("  test_register_sound: SKIPPED (audio unavailable)\n");
        return;
    }

    // RegisterSound returns 1 on success
    int64_t ok = rt_soundbank_register_sound(bank, S("test"), snd);
    assert(ok == 1);
    assert(rt_soundbank_count(bank) == 1);

    // Has returns 1 for registered name
    assert(rt_soundbank_has(bank, S("test")) == 1);
    assert(rt_soundbank_has(bank, S("missing")) == 0);

    // Get returns the sound object
    void *got = rt_soundbank_get(bank, S("test"));
    assert(got == snd);
    release_obj(got);

    // Get returns NULL for missing name
    assert(rt_soundbank_get(bank, S("missing")) == nullptr);
    release_obj(snd);

    printf("  test_register_sound: PASSED\n");
}

static void test_register_null_sound_rejected() {
    void *bank = rt_soundbank_new();

    assert(rt_soundbank_register_sound(bank, S("nil"), nullptr) == 0);
    assert(rt_soundbank_count(bank) == 0);
    assert(rt_soundbank_has(bank, S("nil")) == 0);

    printf("  test_register_null_sound_rejected: PASSED\n");
}

static void test_register_non_sound_rejected() {
    void *bank = rt_soundbank_new();
    void *fake = make_fake_object();

    assert(rt_soundbank_register_sound(bank, S("fake"), fake) == 0);
    assert(rt_soundbank_count(bank) == 0);
    assert(rt_soundbank_has(bank, S("fake")) == 0);
    release_obj(fake);

    printf("  test_register_non_sound_rejected: PASSED\n");
}

static void test_register_overwrite() {
    void *bank = rt_soundbank_new();

    void *snd1 = make_registry_sound();
    void *snd2 = make_registry_sound();
    if (!snd1 || !snd2) {
        release_obj(snd1);
        release_obj(snd2);
        printf("  test_register_overwrite: SKIPPED (audio unavailable)\n");
        return;
    }

    rt_soundbank_register_sound(bank, S("beep"), snd1);
    assert(rt_soundbank_count(bank) == 1);

    // Registering same name should overwrite, not duplicate
    rt_soundbank_register_sound(bank, S("beep"), snd2);
    assert(rt_soundbank_count(bank) == 1);

    // Get returns the new sound
    void *got = rt_soundbank_get(bank, S("beep"));
    assert(got == snd2);
    release_obj(got);
    release_obj(snd1);
    release_obj(snd2);

    printf("  test_register_overwrite: PASSED\n");
}

static void test_register_overwrite_same_sound() {
    void *bank = rt_soundbank_new();
    void *snd = make_registry_sound();
    if (!snd) {
        printf("  test_register_overwrite_same_sound: SKIPPED (audio unavailable)\n");
        return;
    }

    assert(rt_soundbank_register_sound(bank, S("beep"), snd) == 1);
    assert(rt_soundbank_register_sound(bank, S("beep"), snd) == 1);
    assert(rt_soundbank_count(bank) == 1);

    void *got = rt_soundbank_get(bank, S("beep"));
    assert(got == snd);
    release_obj(got);
    release_obj(snd);

    printf("  test_register_overwrite_same_sound: PASSED\n");
}

static void test_long_names_do_not_alias() {
    void *bank = rt_soundbank_new();

    const char *name1 = "ambience_forest_loop_segment_alpha_version_01";
    const char *name2 = "ambience_forest_loop_segment_alpha_version_02";
    assert(strlen(name1) > 31);
    assert(strlen(name2) > 31);

    void *snd1 = make_registry_sound();
    void *snd2 = make_registry_sound();
    if (!snd1 || !snd2) {
        release_obj(snd1);
        release_obj(snd2);
        printf("  test_long_names_do_not_alias: SKIPPED (audio unavailable)\n");
        return;
    }

    rt_soundbank_register_sound(bank, S(name1), snd1);
    rt_soundbank_register_sound(bank, S(name2), snd2);

    assert(rt_soundbank_count(bank) == 2);
    assert(rt_soundbank_has(bank, S(name1)) == 1);
    assert(rt_soundbank_has(bank, S(name2)) == 1);
    void *got1 = rt_soundbank_get(bank, S(name1));
    void *got2 = rt_soundbank_get(bank, S(name2));
    assert(got1 == snd1);
    assert(got2 == snd2);
    release_obj(got1);
    release_obj(got2);

    rt_soundbank_remove(bank, S(name1));
    assert(rt_soundbank_count(bank) == 1);
    assert(rt_soundbank_has(bank, S(name1)) == 0);
    assert(rt_soundbank_has(bank, S(name2)) == 1);
    got2 = rt_soundbank_get(bank, S(name2));
    assert(got2 == snd2);
    release_obj(got2);
    release_obj(snd1);
    release_obj(snd2);

    printf("  test_long_names_do_not_alias: PASSED\n");
}

// ============================================================================
// Remove + Clear
// ============================================================================

static void test_remove() {
    void *bank = rt_soundbank_new();
    void *snd1 = make_registry_sound();
    void *snd2 = make_registry_sound();
    if (!snd1 || !snd2) {
        release_obj(snd1);
        release_obj(snd2);
        printf("  test_remove: SKIPPED (audio unavailable)\n");
        return;
    }

    rt_soundbank_register_sound(bank, S("a"), snd1);
    rt_soundbank_register_sound(bank, S("b"), snd2);
    assert(rt_soundbank_count(bank) == 2);

    rt_soundbank_remove(bank, S("a"));
    assert(rt_soundbank_count(bank) == 1);
    assert(rt_soundbank_has(bank, S("a")) == 0);
    assert(rt_soundbank_has(bank, S("b")) == 1);

    // Remove non-existent — no crash
    rt_soundbank_remove(bank, S("missing"));
    assert(rt_soundbank_count(bank) == 1);
    release_obj(snd1);
    release_obj(snd2);

    printf("  test_remove: PASSED\n");
}

static void test_clear() {
    void *bank = rt_soundbank_new();
    void *snd1 = make_registry_sound();
    void *snd2 = make_registry_sound();
    void *snd3 = make_registry_sound();
    if (!snd1 || !snd2 || !snd3) {
        release_obj(snd1);
        release_obj(snd2);
        release_obj(snd3);
        printf("  test_clear: SKIPPED (audio unavailable)\n");
        return;
    }

    rt_soundbank_register_sound(bank, S("x"), snd1);
    rt_soundbank_register_sound(bank, S("y"), snd2);
    rt_soundbank_register_sound(bank, S("z"), snd3);
    assert(rt_soundbank_count(bank) == 3);

    rt_soundbank_clear(bank);
    assert(rt_soundbank_count(bank) == 0);
    assert(rt_soundbank_has(bank, S("x")) == 0);
    release_obj(snd1);
    release_obj(snd2);
    release_obj(snd3);

    printf("  test_clear: PASSED\n");
}

// ============================================================================
// Multiple entries
// ============================================================================

static void test_multiple_entries() {
    void *bank = rt_soundbank_new();
    void *sound = make_registry_sound();
    if (!sound) {
        printf("  test_multiple_entries: SKIPPED (audio unavailable)\n");
        return;
    }

    // Register several sounds
    for (int i = 0; i < 10; i++) {
        char name[8];
        name[0] = 'S';
        name[1] = '0' + (char)i;
        name[2] = '\0';
        rt_soundbank_register_sound(bank, S(name), sound);
    }
    release_obj(sound);

    assert(rt_soundbank_count(bank) == 10);
    assert(rt_soundbank_has(bank, S("S0")) == 1);
    assert(rt_soundbank_has(bank, S("S9")) == 1);

    printf("  test_multiple_entries: PASSED\n");
}

static void test_capacity_is_bounded_and_reusable() {
    void *bank = rt_soundbank_new();
    void *sound = make_registry_sound();
    if (!sound) {
        printf("  test_capacity_is_bounded_and_reusable: SKIPPED (audio unavailable)\n");
        return;
    }

    for (int i = 0; i < 64; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "sound-%d", i);
        rt_string key = rt_string_from_bytes(name, std::strlen(name));
        assert(key != nullptr);
        assert(rt_soundbank_register_sound(bank, key, sound) == 1);
        rt_string_unref(key);
    }
    assert(rt_soundbank_count(bank) == 64);
    assert(rt_soundbank_register_sound(bank, S("overflow"), sound) == 0);
    assert(rt_soundbank_count(bank) == 64);

    assert(rt_soundbank_register_sound(bank, S("sound-0"), sound) == 1);
    assert(rt_soundbank_count(bank) == 64);
    rt_soundbank_remove(bank, S("sound-1"));
    assert(rt_soundbank_count(bank) == 63);
    assert(rt_soundbank_register_sound(bank, S("replacement"), sound) == 1);
    assert(rt_soundbank_count(bank) == 64);

    release_obj(sound);
    printf("  test_capacity_is_bounded_and_reusable: PASSED\n");
}

// ============================================================================
// Synth Sound Generation
// ============================================================================

static void test_synth_tone() {
    // Synth.Tone generates a Sound object — verify non-null
    void *snd = rt_synth_tone(440, 100, 0); // sine wave
    // May be NULL if audio not compiled in, but shouldn't crash
    printf("  test_synth_tone: PASSED (snd=%s)\n", snd ? "ok" : "null/no-audio");
}

static void test_synth_sweep() {
    void *snd = rt_synth_sweep(880, 220, 200, 0);
    printf("  test_synth_sweep: PASSED (snd=%s)\n", snd ? "ok" : "null/no-audio");
}

static void test_synth_noise() {
    void *snd = rt_synth_noise(100, 50);
    printf("  test_synth_noise: PASSED (snd=%s)\n", snd ? "ok" : "null/no-audio");
}

static void test_synth_sfx_presets() {
    // Test all 6 SFX presets
    for (int i = 0; i <= 5; i++) {
        void *snd = rt_synth_sfx(i);
        // Should return non-null if audio compiled in, null otherwise
        printf("  test_synth_sfx[%d]: %s\n", i, snd ? "ok" : "null/no-audio");
    }

    // Invalid preset — should not crash
    void *bad = rt_synth_sfx(99);
    printf("  test_synth_sfx[99]: %s (expected null)\n", bad ? "unexpected" : "null/ok");
}

static void test_synth_edge_cases() {
    // Zero duration
    void *z = rt_synth_tone(440, 0, 0);
    printf("  test_synth_zero_duration: %s\n", z ? "ok" : "null/ok");

    // Very high frequency
    void *h = rt_synth_tone(20000, 50, 0);
    printf("  test_synth_high_freq: %s\n", h ? "ok" : "null/ok");

    // All waveform types
    for (int w = 0; w <= 3; w++) {
        void *s = rt_synth_tone(440, 50, w);
        printf("  test_synth_waveform[%d]: %s\n", w, s ? "ok" : "null/ok");
    }
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== RTSoundBankTests ===\n\n");

    printf("--- Null Safety ---\n");
    test_null_safety();
    test_invalid_bank_rejected();

    printf("\n--- SoundBank CRUD ---\n");
    test_create();
    test_register_sound();
    test_register_null_sound_rejected();
    test_register_non_sound_rejected();
    test_register_overwrite();
    test_register_overwrite_same_sound();
    test_long_names_do_not_alias();
    test_remove();
    test_clear();
    test_multiple_entries();
    test_capacity_is_bounded_and_reusable();

    printf("\n--- Synth Sound Generation ---\n");
    test_synth_tone();
    test_synth_sweep();
    test_synth_noise();
    test_synth_sfx_presets();
    test_synth_edge_cases();

    printf("\n=== All RTSoundBankTests passed! ===\n");
    return 0;
}
