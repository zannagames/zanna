//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio.c
// Purpose: Implements the runtime bridge between the Zanna audio API and the
//          ZannaAUD (vaud) library. Provides Init/Shutdown, LoadSound,
//          LoadMusic, Play/Stop/Pause/Resume for sounds and music, volume
//          control, and IsPlaying queries. When audio is disabled at compile
//          time, capability probes remain available and public playback/load
//          operations fail with deterministic InvalidOperation traps.
//
// Key invariants:
//   - All functions guard against NULL handles and return silently if passed one.
//   - The audio context is a module-level global; Init must be called once
//     before any other audio function.
//   - Shutdown releases all loaded sounds/music and destroys the context.
//   - Sounds use ref-counting; the caller owns the reference from LoadSound.
//   - Music is loaded as a single stream; only one music track plays at a time.
//   - The ZANNA_ENABLE_AUDIO compile flag controls whether real or stub impls
//     are compiled; stub playback/load entry points fail loudly.
//
// Ownership/Lifetime:
//   - Sound objects are ref-counted via the runtime heap; callers must release.
//   - The global audio context is owned by this module and freed on Shutdown.
//
// Links: src/runtime/audio/rt_audio.h (public API),
//        src/lib/audio/include/vaud.h (ZannaAUD low-level audio library)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the Zanna runtime's sound, music, mix-group, and audio
///        effect bridge.
/// @details The audio-enabled implementation owns the process-wide ZannaAUD
///          context, validates runtime wrapper handles, tracks active voices,
///          and coordinates music crossfades under the audio-state lock. The
///          audio-disabled branch preserves queryable logical volume/group
///          state while trapping operations that require a backend. Runtime
///          strings passed as file paths or group names are validated and
///          canonicalized before crossing into the backend or fixed-size
///          registry storage.

#include "rt_audio.h"
#include "rt_asset.h"
#include "rt_audio_fx.h"
#include "rt_error.h"
#include "rt_mixgroup.h"
#include "rt_mp3.h"
#include "rt_object.h"
#include "rt_ogg.h"
#include "rt_option.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_trap.h"
#include "rt_vorbis.h"

#include "rt_audio_internal.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Mix Group State (shared between real and stub implementations)
//===----------------------------------------------------------------------===//

/// @brief Per-group volume (0-100). Defaults to 100 for built-ins and named groups.
static int64_t g_group_volume[RT_MIXGROUP_MAX_GROUPS] = {100, 100};
static char g_group_names[RT_MIXGROUP_MAX_GROUPS][32] = {{0}};
static int8_t g_group_in_use[RT_MIXGROUP_MAX_GROUPS] = {0};
static int8_t g_group_names_initialized = 0;
/// @brief Logical master volume (0-100). Kept even before lazy backend init.
static int64_t g_master_volume = 100;

/// @brief Clamp @p volume into the public `[0, 100]` range.
/// @details Every public-API volume argument flows through this helper so
///          callers don't have to validate themselves and the runtime never
///          sees a negative or > 100 value internally.
/// @param volume Caller-supplied volume value.
/// @return Same value clipped to `[0, 100]`.
static int64_t clamp_volume_100(int64_t volume) {
    if (volume < 0)
        return 0;
    if (volume > 100)
        return 100;
    return volume;
}

/// @brief Lazily initialize the mix-group name/volume/in-use tables on first
///        use, seeding the built-in "music" and "sfx" groups.
/// @details Idempotent (guarded by g_group_names_initialized). The
///          `_unlocked` suffix means the caller must already hold the mix
///          group lock — this is not internally synchronized.
static void audio_groups_init_unlocked(void) {
    if (g_group_names_initialized)
        return;
    memset(g_group_names, 0, sizeof(g_group_names));
    memset(g_group_in_use, 0, sizeof(g_group_in_use));
    strncpy(g_group_names[RT_MIXGROUP_MUSIC], "music", sizeof(g_group_names[0]) - 1);
    strncpy(g_group_names[RT_MIXGROUP_SFX], "sfx", sizeof(g_group_names[0]) - 1);
    g_group_volume[RT_MIXGROUP_MUSIC] = clamp_volume_100(g_group_volume[RT_MIXGROUP_MUSIC]);
    g_group_volume[RT_MIXGROUP_SFX] = clamp_volume_100(g_group_volume[RT_MIXGROUP_SFX]);
    g_group_in_use[RT_MIXGROUP_MUSIC] = 1;
    g_group_in_use[RT_MIXGROUP_SFX] = 1;
    for (int64_t i = RT_MIXGROUP_COUNT; i < RT_MIXGROUP_MAX_GROUPS; i++) {
        if (g_group_volume[i] < 0 || g_group_volume[i] > 100)
            g_group_volume[i] = 100;
    }
    g_group_names_initialized = 1;
}

/// @brief Canonicalize a mix-group name into @p dst.
/// @details Trims space/tab edges and replaces embedded NUL bytes with '_'.
///          Names whose canonical form exceeds the storage capacity are
///          REJECTED (dst becomes empty, so registration/lookup return -1)
///          instead of silently truncated — truncation made distinct names
///          alias the same group and could split UTF-8 sequences (VDOC-118).
/// @param dst Destination buffer, which is cleared when the input is invalid.
/// @param cap Total destination capacity including the terminating NUL byte.
/// @param name Runtime string to canonicalize; NULL produces an empty result.
static void audio_group_copy_name(char *dst, size_t cap, rt_string name) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!name)
        return;
    const char *s = rt_string_cstr(name);
    if (!s)
        return;
    int64_t len = rt_str_len(name);
    if (len < 0)
        len = 0;
    while (len > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if ((size_t)len >= cap)
        return; // too long: reject rather than alias (VDOC-118)
    for (int64_t i = 0; i < len; i++)
        dst[i] = s[i] == '\0' ? '_' : s[i];
    dst[len] = '\0';
}

#ifdef ZANNA_ENABLE_AUDIO
/// @brief Validate a runtime string for use as a NUL-terminated backend path.
/// @details Rejects NULL strings, missing string storage, and embedded NUL
///          bytes within the runtime string's declared length so the backend
///          cannot observe a silently truncated path.
/// @param path Runtime string containing a filesystem or asset path.
/// @return Borrowed NUL-terminated character data, or NULL when invalid. The
///         returned pointer remains owned by @p path.
static const char *audio_path_cstr(rt_string path) {
    if (!path)
        return NULL;
    const char *path_str = rt_string_cstr(path);
    if (!path_str)
        return NULL;
    int64_t len = rt_str_len(path);
    if (len > 0 && memchr(path_str, '\0', (size_t)len))
        return NULL;
    return path_str;
}
#endif /* ZANNA_ENABLE_AUDIO */

/// @brief Find an in-use mix group by name. @return its group id, or -1 if
///        no such group (empty/NULL name yields -1). Caller holds the lock.
/// @param name Canonical NUL-terminated group name to find.
static int64_t audio_find_group_unlocked(const char *name) {
    audio_groups_init_unlocked();
    if (!name || name[0] == '\0')
        return -1;
    for (int64_t i = 0; i < RT_MIXGROUP_MAX_GROUPS; i++) {
        if (g_group_in_use[i] && strcmp(g_group_names[i], name) == 0)
            return i;
    }
    return -1;
}

/// @brief Get or create a named mix group. @details Returns the existing id
///        if @p name is already registered; otherwise claims the first free
///        slot at/after RT_MIXGROUP_NAMED_BASE (volume defaulted to 100).
/// @return The group id, or -1 if the name is empty or all slots are in use.
///         Caller holds the mix-group lock.
/// @param name Canonical NUL-terminated group name to find or register.
static int64_t audio_register_group_unlocked(const char *name) {
    audio_groups_init_unlocked();
    int64_t existing = audio_find_group_unlocked(name);
    if (existing >= 0)
        return existing;
    if (!name || name[0] == '\0')
        return -1;
    for (int64_t i = RT_MIXGROUP_NAMED_BASE; i < RT_MIXGROUP_MAX_GROUPS; i++) {
        if (g_group_in_use[i])
            continue;
        strncpy(g_group_names[i], name, sizeof(g_group_names[i]) - 1);
        g_group_names[i][sizeof(g_group_names[i]) - 1] = '\0';
        g_group_volume[i] = 100;
        g_group_in_use[i] = 1;
        return i;
    }
    return -1;
}

/// @brief True if @p group is a valid, currently in-use mix-group id.
///        Caller holds the mix-group lock.
/// @param group Candidate zero-based registry identifier.
/// @return Non-zero when the group slot is in range and active.
static int8_t audio_group_id_valid_unlocked(int64_t group) {
    audio_groups_init_unlocked();
    return group >= 0 && group < RT_MIXGROUP_MAX_GROUPS && g_group_in_use[group];
}

#ifdef ZANNA_ENABLE_AUDIO
/// @brief Convert a duration in seconds to rounded whole milliseconds.
/// @details Non-finite and non-positive durations map to zero. Positive
///          results that cannot be represented by `int64_t` saturate at
///          `INT64_MAX`.
/// @param seconds Duration expressed in seconds.
/// @return Rounded duration in milliseconds, within `[0, INT64_MAX]`.
static int64_t seconds_to_ms_i64(float seconds) {
    if (!isfinite(seconds) || seconds <= 0.0f)
        return 0;
    double ms = (double)seconds * 1000.0;
    if (!isfinite(ms) || ms >= (double)INT64_MAX)
        return INT64_MAX;
    return (int64_t)(ms + 0.5);
}
#endif /* ZANNA_ENABLE_AUDIO */

// The audio-state spinlock is available in BOTH configurations (VDOC-117):
// audio-disabled builds still mutate the shared mix-group tables and must
// serialise their lazy initialization and registration.
/// @brief Spinlock for thread-safe initialization (RACE-009 fix).
/// CONC-008: kept as spinlock (pthread_once doesn't support retry-on-failure);
/// yield hint added to reduce CPU waste under contention.
static volatile int g_audio_init_lock = 0;

#if !RT_PLATFORM_WINDOWS
#include <sched.h>
#endif

/// @brief Acquire the audio state's spinlock with acquire ordering.
/// @details Wraps `__atomic_test_and_set` (or MSVC's `_InterlockedExchange8`)
///          so concurrent callers serialise around the global audio state
///          (registries, crossfade table, voice tracker). Yields on
///          contention via `SwitchToThread`/`sched_yield` so a contended
///          lock doesn't burn an entire CPU core. The lock is retained as
///          a spinlock rather than promoted to a `pthread_mutex` because
///          `pthread_once` (CONC-008) can't retry on failure.
static void audio_state_lock(void) {
#if RT_COMPILER_MSVC
    while (_InterlockedExchange8((volatile char *)&g_audio_init_lock, 1))
#else
    while (__atomic_test_and_set(&g_audio_init_lock, __ATOMIC_ACQUIRE))
#endif
    {
#if RT_PLATFORM_WINDOWS
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}

/// @brief Release the audio state's spinlock with release ordering.
/// @details Pairs with @ref audio_state_lock. No-op on an unlocked lock
///          (the atomic clear simply writes zero again).
static void audio_state_unlock(void) {
#if RT_COMPILER_MSVC
    _InterlockedExchange8((volatile char *)&g_audio_init_lock, 0);
#else
    __atomic_clear(&g_audio_init_lock, __ATOMIC_RELEASE);
#endif
}

#ifdef ZANNA_ENABLE_AUDIO
#include "vaud.h"

#define RT_SOUND_MAGIC 0x56534E44u /* VSND */
#define RT_MUSIC_MAGIC 0x564D5553u /* VMUS */

typedef struct {
    void *fade_out;       ///< Music being faded out (NULL when not crossfading).
    void *fade_in;        ///< Music being faded in (NULL when not crossfading).
    int64_t elapsed;      ///< Milliseconds elapsed in crossfade.
    int64_t duration;     ///< Total crossfade duration in ms.
    int64_t vol_out;      ///< Target logical volume of the fade-out track.
    int64_t vol_in;       ///< Target logical volume of the fade-in track.
    int64_t last_tick_ms; ///< Last monotonic tick used to advance the fade.
    int8_t paused;        ///< 1 while the crossfade is locally paused.
    int8_t active;        ///< 1 if crossfade in progress.
} rt_music_crossfade_state;

static rt_music_crossfade_state g_crossfades[VAUD_MAX_MUSIC];

/// @brief Tell ZannaAUD whether a mix group currently has an effect chain.
/// @details Registered as the backend effect-query callback. The callback
///          delegates to the runtime effect registry; @p userdata is unused
///          because that registry is process-wide.
/// @param userdata Backend callback context; ignored.
/// @param group_id Mix-group identifier being considered by the mixer.
/// @return Non-zero when the group has at least one active effect.
static int rt_audio_fx_query_cb(void *userdata, int64_t group_id) {
    (void)userdata;
    return rt_audio_fx_group_has_effects(group_id);
}

/// @brief Process one interleaved sample block through a group's effect chain.
/// @details Registered as the ZannaAUD effect-processing callback and delegates
///          in-place processing to the runtime effect registry. No sample
///          storage is retained after the call.
/// @param userdata Backend callback context; ignored.
/// @param group_id Mix group whose effect chain should process the block.
/// @param samples Writable interleaved floating-point sample buffer.
/// @param frames Number of sample frames in @p samples.
/// @param channels Number of interleaved channels per frame.
/// @param sample_rate Sample rate in hertz.
static void rt_audio_fx_process_cb(void *userdata,
                                   int64_t group_id,
                                   float *samples,
                                   int32_t frames,
                                   int32_t channels,
                                   int32_t sample_rate) {
    (void)userdata;
    rt_audio_fx_process_group(group_id, samples, frames, channels, sample_rate);
}

//===----------------------------------------------------------------------===//
// Global Audio Context
//===----------------------------------------------------------------------===//

/// @brief Global audio context (singleton).
static vaud_context_t g_audio_ctx = NULL;

/// @brief Initialization state: 0 = not init, 1 = initialized, -1 = init failed.
static volatile int g_audio_initialized = 0;
static int8_t g_audio_paused = 0;


typedef struct {
    int64_t voice_id;
    int64_t group;
    int64_t base_volume;
} rt_tracked_voice;

static rt_tracked_voice g_tracked_voices[VAUD_MAX_VOICES];
static int32_t g_tracked_voice_count = 0;

typedef struct {
    void *objs[VAUD_MAX_MUSIC * 2];
    int32_t count;
} rt_deferred_release_list;

//===----------------------------------------------------------------------===//
// Sound Wrapper Structure
//===----------------------------------------------------------------------===//

/// @brief Internal sound wrapper structure.
/// @details Contains the vptr (for future OOP support) and the vaud sound handle.
typedef struct rt_sound {
    void *vptr;            ///< VTable pointer (reserved for future use)
    uint32_t magic;        ///< Runtime wrapper discriminator.
    vaud_sound_t sound;    ///< ZannaAUD sound handle
    struct rt_sound *prev; ///< Registry linkage
    struct rt_sound *next; ///< Registry linkage
} rt_sound;

static rt_sound *g_sound_wrappers = NULL;

/// @brief Insert @p snd at the head of the global sound registry list.
/// @details The registry tracks every live `rt_sound` wrapper so the audio
///          system can sweep them on `rt_audio_shutdown()` and validate
///          handles in `rt_sound_is_handle()`. Must be called under
///          @ref audio_state_lock.
/// @param snd Wrapper to register (NULL is a no-op).
static void sound_registry_add(rt_sound *snd) {
    if (!snd)
        return;
    snd->prev = NULL;
    snd->next = g_sound_wrappers;
    if (g_sound_wrappers)
        g_sound_wrappers->prev = snd;
    g_sound_wrappers = snd;
}

/// @brief Unlink @p snd from the global sound registry list.
/// @details Patches the previous/next links and clears @p snd's own links
///          so it can't be doubly-removed. Must be called under
///          @ref audio_state_lock.
/// @param snd Wrapper to unregister (NULL is a no-op).
static void sound_registry_remove(rt_sound *snd) {
    if (!snd)
        return;
    if (snd->prev)
        snd->prev->next = snd->next;
    else if (g_sound_wrappers == snd)
        g_sound_wrappers = snd->next;
    if (snd->next)
        snd->next->prev = snd->prev;
    snd->prev = NULL;
    snd->next = NULL;
}

/// @brief Look up an `rt_sound` wrapper by handle pointer, returning NULL when
///        the handle is not currently registered.
/// @details Validates both the pointer-equality (against entries in the
///          registry) and the magic discriminator so a stale handle from a
///          freed wrapper can't pass as a live one. Must be called under
///          @ref audio_state_lock.
/// @param sound Caller-supplied handle.
/// @return Matching wrapper pointer, or NULL when the handle is not live.
static rt_sound *rt_sound_from_handle_locked(void *sound) {
    if (!sound)
        return NULL;
    for (rt_sound *cur = g_sound_wrappers; cur; cur = cur->next) {
        if ((void *)cur == sound && cur->magic == RT_SOUND_MAGIC)
            return cur;
    }
    return NULL;
}

/// @brief Finalize a runtime sound wrapper and its backend resource.
/// @details Removes the wrapper from the protected live-handle registry,
///          invalidates its discriminator, and releases the detached ZannaAUD
///          sound after dropping the audio-state lock.
/// @param obj Runtime-managed `rt_sound` object; NULL is ignored.
static void rt_sound_finalize(void *obj) {
    if (!obj)
        return;

    rt_sound *snd = (rt_sound *)obj;
    audio_state_lock();
    sound_registry_remove(snd);
    vaud_sound_t raw = snd->sound;
    snd->sound = NULL;
    snd->magic = 0;
    audio_state_unlock();

    if (raw)
        vaud_free_sound(raw);
}

//===----------------------------------------------------------------------===//
// Music Wrapper Structure
//===----------------------------------------------------------------------===//

/// @brief Internal music wrapper structure.
/// @details Contains the vptr (for future OOP support) and the vaud music handle.
typedef struct rt_music {
    void *vptr;             ///< VTable pointer (reserved for future use)
    uint32_t magic;         ///< Runtime wrapper discriminator.
    vaud_music_t music;     ///< ZannaAUD music handle
    int64_t logical_volume; ///< User-facing 0-100 volume before mix-group scaling
    int8_t logical_loop;    ///< User-facing loop preference preserved across crossfades
    int8_t paused;          ///< Runtime pause state used by Resume() arbitration
    struct rt_music *prev;  ///< Registry linkage
    struct rt_music *next;  ///< Registry linkage
} rt_music;

static rt_music *g_music_wrappers = NULL;

/// @brief Insert @p mus at the head of the global music registry list.
/// @details Music counterpart of @ref sound_registry_add. Must be called under
///          @ref audio_state_lock.
/// @param mus Wrapper to register (NULL is a no-op).
static void music_registry_add(rt_music *mus) {
    if (!mus)
        return;
    mus->prev = NULL;
    mus->next = g_music_wrappers;
    if (g_music_wrappers)
        g_music_wrappers->prev = mus;
    g_music_wrappers = mus;
}

/// @brief Unlink @p mus from the global music registry list.
/// @details Music counterpart of @ref sound_registry_remove. Must be called
///          under @ref audio_state_lock.
/// @param mus Wrapper to unregister (NULL is a no-op).
static void music_registry_remove(rt_music *mus) {
    if (!mus)
        return;
    if (mus->prev)
        mus->prev->next = mus->next;
    else if (g_music_wrappers == mus)
        g_music_wrappers = mus->next;
    if (mus->next)
        mus->next->prev = mus->prev;
    mus->prev = NULL;
    mus->next = NULL;
}

/// @brief Look up an `rt_music` wrapper by handle pointer.
/// @details Music counterpart of @ref rt_sound_from_handle_locked. Validates
///          pointer-equality against the registry and the magic discriminator.
///          Must be called under @ref audio_state_lock.
/// @param music Caller-supplied handle.
/// @return Matching wrapper pointer, or NULL when the handle is not live.
static rt_music *rt_music_from_handle_locked(void *music) {
    if (!music)
        return NULL;
    for (rt_music *cur = g_music_wrappers; cur; cur = cur->next) {
        if ((void *)cur == music && cur->magic == RT_MUSIC_MAGIC)
            return cur;
    }
    return NULL;
}

/// @brief Finalize a runtime music wrapper and its streaming backend resource.
/// @details Removes the wrapper from the protected registry, invalidates its
///          discriminator, and frees the detached ZannaAUD stream after the
///          audio-state lock has been released.
/// @param obj Runtime-managed `rt_music` object; NULL is ignored.
static void rt_music_finalize(void *obj) {
    if (!obj)
        return;

    rt_music *mus = (rt_music *)obj;
    audio_state_lock();
    music_registry_remove(mus);
    vaud_music_t raw = mus->music;
    mus->music = NULL;
    mus->magic = 0;
    audio_state_unlock();

    if (raw)
        vaud_free_music(raw);
}

//===----------------------------------------------------------------------===//
// Audio System Management
//===----------------------------------------------------------------------===//

/// @brief Report whether the Audio runtime is usable in this build.
///
/// Mirror of the stub layer's `rt_audio_is_available` (in
/// `rt_audio_stubs.c`). The graphics-enabled implementation always
/// returns 1 — Audio doesn't depend on the windowing backend so it's
/// available wherever ZannaAUD is linked. Cheap; safe to call every frame.
///
/// @return Always `1` in the real implementation; the stubbed
///         `audio-disabled` build returns `0`.
int8_t rt_audio_is_available(void) {
    return 1;
}

/// @brief Ensure the audio system is initialized.
/// @details Uses double-checked locking with a spinlock to ensure thread-safe
///          initialization. Only one thread will perform the actual initialization;
///          other threads will wait and then use the result (RACE-009 fix).
/// @return 1 if initialized, 0 on failure.
static int ensure_audio_init(void) {
    // Fast path: already initialized (use acquire to sync with the release below)
#if RT_COMPILER_MSVC
    int state = rt_atomic_load_i32(&g_audio_initialized, __ATOMIC_ACQUIRE);
#else
    int state = __atomic_load_n(&g_audio_initialized, __ATOMIC_ACQUIRE);
#endif
    if (state > 0)
        return 1;

    // Slow path: acquire spinlock and double-check
#if RT_COMPILER_MSVC
    while (_InterlockedExchange8((volatile char *)&g_audio_init_lock, 1))
#else
    while (__atomic_test_and_set(&g_audio_init_lock, __ATOMIC_ACQUIRE))
#endif
    {
#if RT_PLATFORM_WINDOWS
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    // Double-check under lock
#if RT_COMPILER_MSVC
    state = rt_atomic_load_i32(&g_audio_initialized, __ATOMIC_RELAXED);
#else
    state = __atomic_load_n(&g_audio_initialized, __ATOMIC_RELAXED);
#endif
    if (state > 0) {
        // Another thread already initialized - release lock and return
#if RT_COMPILER_MSVC
        _InterlockedExchange8((volatile char *)&g_audio_init_lock, 0);
#else
        __atomic_clear(&g_audio_init_lock, __ATOMIC_RELEASE);
#endif
        return 1;
    }

    // We are the initializing thread
    g_audio_ctx = vaud_create();
    if (g_audio_ctx) {
        g_audio_paused = 0;
        vaud_set_master_volume(g_audio_ctx, (float)g_master_volume / 100.0f);
        vaud_set_group_effects_processor(
            g_audio_ctx, rt_audio_fx_query_cb, rt_audio_fx_process_cb, NULL);
    }

    // Set initialization state (use release to ensure context is visible)
#if RT_COMPILER_MSVC
    rt_atomic_store_i32(&g_audio_initialized, g_audio_ctx ? 1 : -1, __ATOMIC_RELEASE);
#else
    __atomic_store_n(&g_audio_initialized, g_audio_ctx ? 1 : -1, __ATOMIC_RELEASE);
#endif

#if RT_COMPILER_MSVC
    _InterlockedExchange8((volatile char *)&g_audio_init_lock, 0);
#else
    __atomic_clear(&g_audio_init_lock, __ATOMIC_RELEASE);
#endif
    return g_audio_ctx != NULL;
}

/// @brief Compose a per-voice volume with its mix-group volume into a single
///        effective gain in the `[0, 100]` range.
/// @details Both inputs are fixed-point `[0, 100]` scalars (not dB). The result
///          is `(voice_volume / 100) * (group_volume / 100) * 100` — a straight
///          multiplicative blend with integer math so the `(0, 100] → (0, 100]`
///          mapping stays monotonic and clamping-free. An out-of-range group
///          index silently maps to `SFX` so a caller-mistyped index can't index
///          out of `g_group_volume`.
/// @param volume Per-voice logical volume in `[0, 100]`.
/// @param group Candidate mix-group identifier.
/// @return Logical volume after multiplying by the selected group gain.
static int64_t apply_group_volume(int64_t volume, int64_t group) {
    if (!audio_group_id_valid_unlocked(group))
        group = RT_MIXGROUP_SFX;
    return clamp_volume_100(volume) * g_group_volume[group] / 100;
}

/// @brief Remove the tracked-voice entry at @p index by sliding later entries down.
/// @details The tracked-voice table is a flat array indexed by voice slot;
///          deletion preserves the slot ordering required by callers that
///          walk it linearly (e.g. `rt_audio_refresh_voice_group_volumes`).
///          Bounds-checked: out-of-range @p index is silently ignored.
/// @param index Position in `g_tracked_voices` to remove.
static void tracked_voice_remove_at(int32_t index) {
    if (index < 0 || index >= g_tracked_voice_count)
        return;
    for (int32_t i = index; i < g_tracked_voice_count - 1; i++)
        g_tracked_voices[i] = g_tracked_voices[i + 1];
    g_tracked_voice_count--;
}

/// @brief Linear-search the tracked-voice table for a given backend voice id.
/// @details The table is small (`VAUD_MAX_VOICES`) and accessed only on
///          play/stop transitions, so a linear scan is faster than the
///          bookkeeping a hash would require.
/// @param voice_id Backend voice id to look up.
/// @return Index in `g_tracked_voices`, or `-1` if not present.
static int32_t tracked_voice_find(int64_t voice_id) {
    for (int32_t i = 0; i < g_tracked_voice_count; i++) {
        if (g_tracked_voices[i].voice_id == voice_id)
            return i;
    }
    return -1;
}

/// @brief Insert or update the tracked-voice entry for @p voice_id.
/// @details If an entry already exists, overwrites the group/volume; if not,
///          appends. When the table is full, evicts the oldest entry first
///          (FIFO eviction policy). Negative @p voice_id is silently ignored;
///          out-of-range @p group is normalised to `SFX`.
/// @param voice_id    Backend voice id (must be non-negative).
/// @param group       Mix-group enum value; out-of-range → `SFX`.
/// @param base_volume Pre-group volume (`[0, 100]`).
static void tracked_voice_set(int64_t voice_id, int64_t group, int64_t base_volume) {
    if (voice_id < 0)
        return;
    if (!audio_group_id_valid_unlocked(group))
        group = RT_MIXGROUP_SFX;
    int32_t index = tracked_voice_find(voice_id);
    if (index < 0) {
        if (g_tracked_voice_count >= VAUD_MAX_VOICES)
            tracked_voice_remove_at(0);
        index = g_tracked_voice_count++;
    }
    g_tracked_voices[index].voice_id = voice_id;
    g_tracked_voices[index].group = group;
    g_tracked_voices[index].base_volume = clamp_volume_100(base_volume);
}

/// @brief Remove the tracking entry for @p voice_id if it exists.
/// @details Called when a voice stops or is explicitly released so the
///          mix-group volume refresher doesn't keep poking a dead voice.
/// @param voice_id Backend voice id to forget.
static void tracked_voice_remove(int64_t voice_id) {
    int32_t index = tracked_voice_find(voice_id);
    if (index >= 0)
        tracked_voice_remove_at(index);
}

/// @brief Remove any tracked-voice entries whose underlying backend voice has
///        already stopped playing.
/// @details Voices can stop asynchronously (natural end-of-sample, stolen by a
///          higher-priority voice, backend reset). This sweep catches those and
///          collapses the tracking array so lookups stay fast and the slot
///          budget `VAUD_MAX_VOICES` doesn't fill with dead entries that would
///          evict live ones on next `tracked_voice_set`. Walks the array back-
///          to-front because `tracked_voice_remove_at` is a shift-down — iterating
///          forward would skip the entry that shifts into the just-processed
///          slot. The `_locked` suffix means the caller must already hold the
///          audio-context mutex.
static void tracked_voice_prune_locked(void) {
    for (int32_t i = g_tracked_voice_count - 1; i >= 0; i--) {
        int64_t voice_id = g_tracked_voices[i].voice_id;
        if (!g_audio_ctx || !vaud_voice_is_playing(g_audio_ctx, (vaud_voice_id)voice_id))
            tracked_voice_remove_at(i);
    }
}

/// @brief Find the active crossfade slot containing @p music, if any.
/// @details Linear scan over the small fixed-size crossfade table.
///          Used by pause/resume/stop entry points to discover whether a
///          music handle is currently participating in a crossfade so they
///          can apply paired behaviour. Must be called under @ref audio_state_lock.
/// @param music Music handle to look for.
/// @return Index into `g_crossfades`, or `-1` if not currently crossfading.
static int32_t rt_audio_find_crossfade_by_music_locked(const void *music) {
    if (!music)
        return -1;
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (!g_crossfades[i].active)
            continue;
        if (g_crossfades[i].fade_out == music || g_crossfades[i].fade_in == music)
            return i;
    }
    return -1;
}

/// @brief Defer freeing @p obj until after the audio lock is dropped.
/// @details Calling `rt_obj_free` may run user-supplied finalizers, which
///          must not run with the audio spinlock held (they may try to
///          acquire it themselves or otherwise deadlock). Collect the
///          objects to free into a small fixed-size list that the caller
///          drains via @ref rt_audio_drain_releases after unlocking.
///          When @p releases is NULL, falls back to immediate free (caller
///          guarantees no lock is held). Drops releases on overflow rather
///          than reallocating — the small static cap is large enough for
///          any realistic single-operation batch.
/// @param releases Deferred-release list (may be NULL).
/// @param obj      Object to queue (NULL is a no-op).
static void rt_audio_queue_release(rt_deferred_release_list *releases, void *obj) {
    if (!obj)
        return;
    if (!releases) {
        rt_obj_free(obj);
        return;
    }
    if (releases->count < (int32_t)(sizeof(releases->objs) / sizeof(releases->objs[0])))
        releases->objs[releases->count++] = obj;
}

/// @brief Free every object queued by @ref rt_audio_queue_release.
/// @details Must be called outside the audio lock so finalizers can run
///          safely. Resets the count to 0 so the list is reusable.
/// @param releases Deferred-release list (may be NULL).
static void rt_audio_drain_releases(rt_deferred_release_list *releases) {
    if (!releases)
        return;
    for (int32_t i = 0; i < releases->count; i++) {
        if (releases->objs[i])
            rt_obj_free(releases->objs[i]);
    }
    releases->count = 0;
}

/// @brief Apply a logical volume to @p mus's backend voice, composing with mix-group.
/// @details The public Audio API uses a `[0, 100]` integer volume scale;
///          the backend expects a `[0.0, 1.0]` float. This helper composes
///          the user-facing logical volume with the music mix-group volume
///          (per @ref apply_group_volume) and converts to the backend scale.
/// @param mus            Music wrapper (may be NULL, treated as no-op).
/// @param logical_volume Logical `[0, 100]` volume to apply.
static void rt_audio_apply_music_volume_value(rt_music *mus, int64_t logical_volume) {
    if (!mus || !mus->music)
        return;
    int64_t effective = apply_group_volume(logical_volume, RT_MIXGROUP_MUSIC);
    vaud_music_set_volume(mus->music, (float)effective / 100.0f);
}

/// @brief Re-apply @p mus's stored logical volume to its backend voice.
/// @details Convenience wrapper that uses the wrapper's `logical_volume`
///          field, used after mix-group volume changes or crossfade
///          transitions that need to re-sync the backend gain.
/// @param mus Music wrapper.
static void rt_audio_apply_music_volume(rt_music *mus) {
    rt_audio_apply_music_volume_value(mus, mus ? mus->logical_volume : 0);
}

/// @brief Release the two music references stored in a crossfade slot.
/// @details Both the fade-out and fade-in tracks are retained while the
///          crossfade is active (so the runtime doesn't free a track that's
///          still being mixed). On crossfade completion or cancellation
///          this helper drops those retains, restores the `paused` flag to
///          0 on each track (the crossfade overrode it), and resets the
///          slot to inactive. The actual `rt_obj_free` is deferred via
///          @p releases since this function runs under the audio lock.
/// @param xf       Crossfade slot to clear.
/// @param releases Deferred-release list to queue freed objects into.
static void rt_audio_release_crossfade_refs_locked(rt_music_crossfade_state *xf,
                                                   rt_deferred_release_list *releases) {
    if (!xf)
        return;
    if (xf->fade_out) {
        ((rt_music *)xf->fade_out)->paused = 0;
        if (rt_obj_release_check0(xf->fade_out))
            rt_audio_queue_release(releases, xf->fade_out);
    }
    if (xf->fade_in) {
        ((rt_music *)xf->fade_in)->paused = 0;
        if (rt_obj_release_check0(xf->fade_in))
            rt_audio_queue_release(releases, xf->fade_in);
    }
    xf->fade_out = NULL;
    xf->fade_in = NULL;
    xf->elapsed = 0;
    xf->duration = 0;
    xf->vol_out = 100;
    xf->vol_in = 100;
    xf->last_tick_ms = 0;
    xf->paused = 0;
    xf->active = 0;
}

/// @brief Push the crossfade's current `(elapsed, duration)` state into both
///        participating music tracks' volumes, producing an equal-power-ish
///        linear blend.
/// @details Called after any state-affecting operation — advance, pause/resume,
///          external volume change on either track — so the mixer always sees
///          the correct instantaneous fade-out / fade-in volumes. Uses a
///          fixed-point `progress` in `[0, 1000]` (1000 == 1.0x complete):
///            - `elapsed <= 0` → progress 0 (fully fade-out, silent fade-in)
///            - `elapsed >= duration` → progress 1000 (silent fade-out, fully
///              fade-in, end-of-crossfade)
///            - otherwise `progress = round(elapsed * 1000 / duration)`, via
///              `long double` so very long durations (hours) don't lose
///              precision in the scale.
///          The output volumes are straight linear blends:
///            - fade-out: `vol_out * (1000 - progress) / 1000`
///            - fade-in: `vol_in * progress / 1000`
///          The sum tracks `vol_out + vol_in` at the endpoints but dips slightly
///          in the middle because the two linear envelopes cross at 500
///          without the sqrt(2) compensation an equal-power curve would use —
///          acceptable trade-off for the integer-math simplicity.
///          Zero-duration is the degenerate "jump cut" case: fade-out immediately
///          drops to 0 and fade-in jumps to `vol_in` so the crossfade completes
///          in a single tick. `_locked` suffix: caller must hold the audio mutex.
/// @param xf Active crossfade slot whose instantaneous pair volumes are recomputed.
static void rt_audio_reapply_crossfade_locked(rt_music_crossfade_state *xf) {
    if (!xf || !xf->active)
        return;

    if (xf->duration <= 0) {
        if (xf->fade_out)
            rt_audio_apply_music_volume_value((rt_music *)xf->fade_out, 0);
        if (xf->fade_in)
            rt_audio_apply_music_volume_value((rt_music *)xf->fade_in, xf->vol_in);
        return;
    }

    int64_t progress = 0;
    if (xf->elapsed <= 0) {
        progress = 0;
    } else if (xf->elapsed >= xf->duration) {
        progress = 1000;
    } else {
        long double scaled = ((long double)xf->elapsed * 1000.0L) / (long double)xf->duration;
        progress = (int64_t)scaled;
    }
    if (progress < 0)
        progress = 0;
    if (progress > 1000)
        progress = 1000;

    if (xf->fade_out) {
        int64_t vol = xf->vol_out * (1000 - progress) / 1000;
        rt_audio_apply_music_volume_value((rt_music *)xf->fade_out, vol);
    }
    if (xf->fade_in) {
        int64_t vol = xf->vol_in * progress / 1000;
        rt_audio_apply_music_volume_value((rt_music *)xf->fade_in, vol);
    }
}

/// @brief Re-apply the music mix-group volume to every live music wrapper.
/// @details Called when the music group volume changes (master, group, or
///          mute toggle). Skips wrappers currently participating in a
///          crossfade — those re-derive their volume from the crossfade's
///          fade curve via @ref rt_audio_reapply_crossfade_locked.
///          `_locked` suffix: caller must hold the audio mutex.
static void rt_audio_refresh_music_group_volumes_locked(void) {
    for (rt_music *mus = g_music_wrappers; mus; mus = mus->next) {
        if (mus->music && rt_audio_find_crossfade_by_music_locked(mus) < 0)
            rt_audio_apply_music_volume(mus);
    }
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (g_crossfades[i].active)
            rt_audio_reapply_crossfade_locked(&g_crossfades[i]);
    }
}

/// @brief Re-apply group volume scaling to every tracked voice in @p group.
/// @details Walks the tracked-voice array, prunes any dead entries, then
///          rewrites the backend voice volume for the survivors that match
///          the changed group. Used by the SFX-group volume entry point so
///          a running batch of sound effects responds to a volume change.
/// @param group Mix-group whose voices need refresh.
static void rt_audio_refresh_voice_group_volumes(int64_t group) {
    tracked_voice_prune_locked();
    for (int32_t i = g_tracked_voice_count - 1; i >= 0; i--) {
        int64_t voice_id = g_tracked_voices[i].voice_id;
        if (g_tracked_voices[i].group != group)
            continue;
        int64_t effective = apply_group_volume(g_tracked_voices[i].base_volume, group);
        vaud_set_voice_volume(g_audio_ctx, (vaud_voice_id)voice_id, (float)effective / 100.0f);
    }
}

/// @brief Detach every wrapper from its backend handle on audio-system shutdown.
/// @details Called during `rt_audio_shutdown()` so user code that still
///          holds Sound/Music handles after shutdown sees them go inert
///          (handles remain valid as objects, but playback fails because
///          the underlying ZannaAUD context is gone). The wrappers
///          themselves stay alive until the GC frees them; only the
///          backend pointers are nulled.
static void rt_audio_invalidate_wrappers_for_shutdown(void) {
    for (rt_sound *snd = g_sound_wrappers; snd; snd = snd->next) {
        if (snd->sound)
            vaud_detach_sound(snd->sound);
    }

    for (rt_music *mus = g_music_wrappers; mus; mus = mus->next) {
        if (mus->music)
            vaud_detach_music(mus->music);
    }

    g_tracked_voice_count = 0;
}

/// @brief Cancel a single crossfade slot with configurable side effects.
/// @details Used by every crossfade-cancellation path. Behaviour:
///            - `stop_fade_out`: when 1, hard-stops the fade-out track via
///              `vaud_music_stop`; otherwise the track keeps playing at its
///              current pre-fade volume.
///            - `stop_fade_in`: same, for the fade-in track.
///            - `restore_volumes`: when 1 and the corresponding `stop_*`
///              flag is 0, resets each non-stopped track to its stored
///              logical volume / loop, undoing the crossfade's overrides.
///          The crossfade slot is released via
///          @ref rt_audio_release_crossfade_refs_locked. `_locked` suffix:
///          caller holds the audio mutex.
/// @param xf              Crossfade slot to cancel.
/// @param stop_fade_out   Stop the fade-out track when non-zero.
/// @param stop_fade_in    Stop the fade-in track when non-zero.
/// @param restore_volumes Restore logical volumes on non-stopped tracks.
/// @param releases        Deferred-release list for the slot's retains.
static void rt_audio_crossfade_cancel_entry_locked(rt_music_crossfade_state *xf,
                                                   int stop_fade_out,
                                                   int stop_fade_in,
                                                   int restore_volumes,
                                                   rt_deferred_release_list *releases) {
    if (!xf || !xf->active)
        return;

    if (xf->fade_out) {
        rt_music *fade_out = (rt_music *)xf->fade_out;
        if (stop_fade_out) {
            if (fade_out->music)
                vaud_music_stop(fade_out->music);
        } else if (restore_volumes) {
            if (fade_out->music)
                vaud_music_set_loop(fade_out->music, fade_out->logical_loop);
            rt_audio_apply_music_volume_value(fade_out, fade_out->logical_volume);
        }
        fade_out->paused = 0;
    }

    if (xf->fade_in) {
        rt_music *fade_in = (rt_music *)xf->fade_in;
        if (stop_fade_in) {
            if (fade_in->music)
                vaud_music_stop(fade_in->music);
        } else if (restore_volumes) {
            if (fade_in->music)
                vaud_music_set_loop(fade_in->music, fade_in->logical_loop);
            rt_audio_apply_music_volume_value(fade_in, fade_in->logical_volume);
        }
        fade_in->paused = 0;
    }

    rt_audio_release_crossfade_refs_locked(xf, releases);
}

/// @brief Cancel every active crossfade slot.
/// @details Sweep helper used by `rt_audio_shutdown()` and by full-stop
///          entry points that need to wipe the crossfade state without
///          restoring per-track volumes (the tracks are being stopped or
///          torn down anyway). `_locked` suffix.
/// @param stop_fade_out Pass-through to @ref rt_audio_crossfade_cancel_entry_locked.
/// @param stop_fade_in  Pass-through to @ref rt_audio_crossfade_cancel_entry_locked.
/// @param releases      Deferred-release list.
static void rt_audio_crossfade_cancel_all_locked(int stop_fade_out,
                                                 int stop_fade_in,
                                                 rt_deferred_release_list *releases) {
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++)
        rt_audio_crossfade_cancel_entry_locked(
            &g_crossfades[i], stop_fade_out, stop_fade_in, 0, releases);
}

/// @brief Stop every crossfade that does not reference @p keep_a or @p keep_b.
/// @details Used when promoting a music track to the foreground: any
///          unrelated crossfade gets fully cancelled (both sides stopped)
///          so the new foreground track plays alone. Paused crossfades are
///          left alone — they aren't competing for foreground at the
///          moment and resuming them later should still work.
/// @param keep_a   Music handle to preserve (may be NULL).
/// @param keep_b   Music handle to preserve (may be NULL).
/// @param releases Deferred-release list.
static void rt_audio_stop_unrelated_music_locked(void *keep_a,
                                                 void *keep_b,
                                                 rt_deferred_release_list *releases) {
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        rt_music_crossfade_state *xf = &g_crossfades[i];
        if (!xf->active)
            continue;
        if (xf->fade_out == keep_a || xf->fade_out == keep_b || xf->fade_in == keep_a ||
            xf->fade_in == keep_b)
            continue;
        if (xf->paused)
            continue;
        rt_audio_crossfade_cancel_entry_locked(xf, 1, 1, 0, releases);
    }

    for (rt_music *mus = g_music_wrappers; mus; mus = mus->next) {
        if (!mus->music || mus == (rt_music *)keep_a || mus == (rt_music *)keep_b)
            continue;
        if (mus->paused)
            continue;
        mus->paused = 0;
        vaud_music_stop(mus->music);
    }
}

/// @brief Clear competing music state so @p music can become the foreground track.
/// @details Walks every crossfade slot and applies one of three actions:
///          (1) if the slot already involves @p music as one of its sides,
///          cancel just the *other* side and keep @p music's logical
///          volume/loop intact; (2) leave paused slots alone; (3) cancel
///          every other active slot in full. After the crossfade sweep,
///          stops every non-paused music wrapper that is not @p music so
///          the new foreground track plays alone. `_locked` suffix —
///          must hold the audio state lock.
/// @param music    Music wrapper that will be promoted to foreground.
/// @param releases Deferred-release list for retains dropped by cancellation.
static void rt_audio_prepare_music_for_foreground_locked(void *music,
                                                         rt_deferred_release_list *releases) {
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        rt_music_crossfade_state *xf = &g_crossfades[i];
        if (!xf->active)
            continue;

        if (xf->fade_out == music || xf->fade_in == music) {
            int stop_fade_out = (xf->fade_out != music);
            int stop_fade_in = (xf->fade_in != music);
            rt_audio_crossfade_cancel_entry_locked(
                xf, stop_fade_out, stop_fade_in, !stop_fade_out || !stop_fade_in, releases);
        } else if (xf->paused) {
            continue;
        } else {
            rt_audio_crossfade_cancel_entry_locked(xf, 1, 1, 0, releases);
        }
    }

    for (rt_music *mus = g_music_wrappers; mus; mus = mus->next) {
        if (!mus->music || mus == (rt_music *)music)
            continue;
        if (mus->paused)
            continue;
        mus->paused = 0;
        vaud_music_stop(mus->music);
    }
}

/// @brief Advance one crossfade slot by @p dt_ms milliseconds.
/// @details Adds @p dt_ms to the slot's `elapsed`, calls
///          @ref rt_audio_reapply_crossfade_locked to re-derive both
///          tracks' volumes, and finalises the slot when `elapsed`
///          reaches `duration` (stops the fade-out, leaves the fade-in
///          at its target volume, releases retains). `_locked` suffix.
/// @param xf       Slot to advance.
/// @param dt_ms    Elapsed time delta (milliseconds).
/// @param releases Deferred-release list.
static void rt_audio_update_crossfade_entry_locked(rt_music_crossfade_state *xf,
                                                   int64_t dt_ms,
                                                   rt_deferred_release_list *releases);

/// @brief Explicitly initialize the audio system. Returns 1 on success, 0 on failure.
/// @details Normally called lazily on first sound/music operation. This function
///          allows eager initialization to detect audio hardware issues early.
int64_t rt_audio_init(void) {
    return ensure_audio_init() ? 1 : 0;
}

/// @brief Shut down the audio system, releasing all device resources.
/// @details Thread-safe via spinlock. Resets the initialization state so the
///          system can be re-initialized later if needed.
void rt_audio_shutdown(void) {
    rt_deferred_release_list releases = {0};

    audio_state_lock();

    if (g_audio_ctx) {
        rt_audio_crossfade_cancel_all_locked(0, 0, &releases);
        rt_audio_invalidate_wrappers_for_shutdown();
        vaud_destroy(g_audio_ctx);
        g_audio_ctx = NULL;
        g_audio_paused = 0;
    }

    // Reset state to allow re-initialization
#if RT_COMPILER_MSVC
    rt_atomic_store_i32(&g_audio_initialized, 0, __ATOMIC_RELEASE);
#else
    __atomic_store_n(&g_audio_initialized, 0, __ATOMIC_RELEASE);
#endif

    audio_state_unlock();
    rt_audio_fx_clear_all();
    rt_audio_drain_releases(&releases);
}

/// @brief Set the process-wide logical master volume.
/// @details Clamps the requested percentage to `[0, 100]`, stores it even when
///          no backend context exists, and immediately applies it to a live
///          ZannaAUD context.
/// @param volume Requested master volume percentage.
void rt_audio_set_master_volume(int64_t volume) {
    volume = clamp_volume_100(volume);

    audio_state_lock();
    g_master_volume = volume;
    if (g_audio_ctx)
        vaud_set_master_volume(g_audio_ctx, (float)volume / 100.0f);
    audio_state_unlock();
}

/// @brief Get the current master volume as an integer percentage.
/// @details When a backend context exists, samples its floating-point master
///          gain and updates the module's logical copy; otherwise returns the
///          last stored value.
/// @return Master volume in `[0, 100]`.
int64_t rt_audio_get_master_volume(void) {
    audio_state_lock();
    int64_t result = g_master_volume;
    if (g_audio_ctx) {
        float vol = vaud_get_master_volume(g_audio_ctx);
        result = clamp_volume_100((int64_t)(vol * 100.0f + 0.5f));
        g_master_volume = result;
    }
    audio_state_unlock();
    return result;
}

/// @brief Pause all currently playing sounds and music.
/// @details Records the global paused state even without a backend and forwards
///          the request to ZannaAUD when a context is live. Crossfade clocks
///          observe this flag and do not advance while globally paused.
void rt_audio_pause_all(void) {
    audio_state_lock();
    g_audio_paused = 1;
    if (g_audio_ctx)
        vaud_pause_all(g_audio_ctx);
    audio_state_unlock();
}

/// @brief Resume all sounds and music paused by the global audio control.
/// @details Clears the global pause flag and forwards the request to a live
///          backend. Individual music/crossfade pause state remains governed by
///          the related-track APIs.
void rt_audio_resume_all(void) {
    audio_state_lock();
    g_audio_paused = 0;
    if (g_audio_ctx)
        vaud_resume_all(g_audio_ctx);
    audio_state_unlock();
}

/// @brief Advance backend maintenance and time-based crossfade state.
/// @details Updates the live ZannaAUD context, computes monotonic elapsed time
///          for each unpaused crossfade, and drains references completed by a
///          transition only after releasing the audio-state lock.
void rt_audio_update(void) {
    rt_deferred_release_list releases = {0};

    audio_state_lock();
    if (g_audio_ctx)
        vaud_update(g_audio_ctx);
    int8_t any_active = 0;
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (g_crossfades[i].active) {
            any_active = 1;
            break;
        }
    }
    if (any_active) {
        int64_t now_ms = rt_timer_ms();
        for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
            if (!g_crossfades[i].active)
                continue;
            if (g_audio_paused || g_crossfades[i].paused) {
                g_crossfades[i].last_tick_ms = now_ms;
                continue;
            }
            if (g_crossfades[i].last_tick_ms <= 0)
                g_crossfades[i].last_tick_ms = now_ms;
            int64_t dt_ms = now_ms - g_crossfades[i].last_tick_ms;
            if (dt_ms > 0) {
                g_crossfades[i].last_tick_ms = now_ms;
                rt_audio_update_crossfade_entry_locked(&g_crossfades[i], dt_ms, &releases);
            }
        }
    }
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Stop all currently playing sound effects.
/// @details Forwards the stop to a live backend and clears the runtime's
///          tracked-voice table. Music streams and crossfades are unaffected.
void rt_audio_stop_all_sounds(void) {
    audio_state_lock();
    if (g_audio_ctx)
        vaud_stop_all_sounds(g_audio_ctx);
    g_tracked_voice_count = 0;
    audio_state_unlock();
}

/// @brief Clamp a ZannaAUD unsigned diagnostic counter into the runtime signed range.
/// @param value Monotonic ZannaAUD counter.
/// @return Signed runtime value saturated at INT64_MAX.
static int64_t rt_audio_counter_to_i64(uint64_t value) {
    return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

/// @brief Copy the current audio context's ZannaAUD statistics without forcing initialization.
/// @details The debug counters should be readable before `Audio.Init()` and after failed
///          initialization. This helper therefore snapshots the existing context only when one is
///          live, otherwise returning zero-filled counters.
/// @return ZannaAUD statistics snapshot for the current context, or zeros.
static vaud_stats_t rt_audio_stats_snapshot(void) {
    vaud_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    audio_state_lock();
    if (g_audio_ctx)
        vaud_get_stats(g_audio_ctx, &stats);
    audio_state_unlock();
    return stats;
}

/// @brief Runtime accessor for ZannaAUD mixer render callbacks.
/// @return Monotonic render callback count for the live audio context, or zero.
int64_t rt_audio_get_render_calls(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.render_calls);
}

/// @brief Runtime accessor for mixer lock-contention callbacks.
/// @return Number of callbacks that emitted silence because state was locked.
int64_t rt_audio_get_mixer_lock_misses(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.mixer_lock_misses);
}

/// @brief Runtime accessor for platform backend write calls.
/// @return Monotonic platform write-call count, or zero when unsupported.
int64_t rt_audio_get_backend_write_calls(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_write_calls);
}

/// @brief Runtime accessor for partial platform writes.
/// @return Number of write calls that accepted fewer frames than requested.
int64_t rt_audio_get_backend_partial_writes(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_partial_writes);
}

/// @brief Runtime accessor for platform wait operations.
/// @return Number of backend waits used for transient write unavailability.
int64_t rt_audio_get_backend_waits(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_waits);
}

/// @brief Runtime accessor for backend underrun/suspend observations.
/// @return Number of xrun or suspend conditions seen by the platform backend.
int64_t rt_audio_get_backend_xruns(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_xruns);
}

/// @brief Runtime accessor for backend recovery attempts.
/// @return Number of platform recovery attempts, successful or not.
int64_t rt_audio_get_backend_recoveries(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_recoveries);
}

/// @brief Runtime accessor for unrecovered backend write failures.
/// @return Number of write failures remaining after recovery attempts.
int64_t rt_audio_get_backend_write_failures(void) {
    vaud_stats_t stats = rt_audio_stats_snapshot();
    return rt_audio_counter_to_i64(stats.backend_write_failures);
}

//===----------------------------------------------------------------------===//
// Sound Effects
//===----------------------------------------------------------------------===//

/// @brief Wrap a freshly-loaded ZannaAUD sound in an `rt_sound` heap object.
/// @details Allocates an `rt_sound` via `rt_obj_new_i64`, stamps the
///          discriminator magic (`RT_SOUND_MAGIC`), installs the finalizer
///          (@ref rt_sound_finalize) so the underlying `vaud_sound_t` is
///          released when the wrapper's refcount drops to zero, and
///          inserts the wrapper into the global sound registry. On
///          allocation failure the raw `vaud_sound_t` is freed so the
///          caller does not need to. `_locked` suffix — caller must hold
///          the audio state lock.
/// @param snd Ownership-transferring ZannaAUD sound handle (may be NULL).
/// @return New `rt_sound` wrapper, or NULL on allocation failure / NULL input.
static void *rt_sound_wrap_loaded_locked(vaud_sound_t snd) {
    if (!snd)
        return NULL;

    rt_sound *wrapper = (rt_sound *)rt_obj_new_i64(0, (int64_t)sizeof(rt_sound));
    if (!wrapper) {
        vaud_free_sound(snd);
        return NULL;
    }

    wrapper->vptr = NULL;
    wrapper->magic = RT_SOUND_MAGIC;
    wrapper->sound = snd;
    wrapper->prev = NULL;
    wrapper->next = NULL;
    rt_obj_set_finalizer(wrapper, rt_sound_finalize);
    sound_registry_add(wrapper);
    return wrapper;
}

/// @brief Load a sound effect from a file (WAV, OGG, or MP3 auto-detected from magic bytes).
/// @details OGG and MP3 files are decoded to WAV in memory before loading into the
///          audio engine. The returned handle can be played multiple times
///          concurrently and is registered for runtime handle validation.
/// @param path Runtime string containing a path without embedded NUL bytes.
/// @return New reference-counted sound wrapper, or NULL when the path is
///         invalid, initialization/decoding/loading fails, or allocation fails.
void *rt_sound_load(rt_string path) {
    if (!path)
        return NULL;

    const char *path_str = audio_path_cstr(path);
    if (!path_str)
        return NULL;

    if (!ensure_audio_init())
        return NULL;

    void *wrapper = NULL;

    // Detect format and dispatch
    int fmt = detect_audio_format(path_str);
    if (fmt == 2) {
        // OGG Vorbis
        uint8_t *wav_data = NULL;
        size_t wav_len = 0;
        if (ogg_file_to_wav(path_str, &wav_data, &wav_len) != 0)
            return NULL;
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound_mem(g_audio_ctx, wav_data, wav_len);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
        free(wav_data);
    } else if (fmt == 3) {
        // MP3
        uint8_t *wav_data = NULL;
        size_t wav_len = 0;
        if (mp3_file_to_wav(path_str, &wav_data, &wav_len) != 0)
            return NULL;
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound_mem(g_audio_ctx, wav_data, wav_len);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
        free(wav_data);
    } else {
        /* WAV path */
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound(g_audio_ctx, path_str);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
    }

    return wrapper;
}

/// @brief Load a sound effect through the runtime asset manager.
/// @details Loads the named asset into temporary raw storage, delegates format
///          detection and backend creation to @ref rt_sound_load_mem, and frees
///          the temporary bytes before returning.
/// @param name Runtime asset name.
/// @return New reference-counted sound wrapper, or NULL when the asset is
///         missing, empty, too large, invalid, or cannot be loaded.
void *rt_sound_load_asset(rt_string name) {
    if (!name)
        return NULL;

    size_t data_size = 0;
    uint8_t *data = rt_asset_load_raw(name, &data_size);
    if (!data || data_size == 0) {
        free(data);
        // A missing asset is a recoverable load failure, exactly like a
        // missing file in Sound.Load — return null, don't trap (VDOC-123).
        return NULL;
    }
    if (data_size > (size_t)INT64_MAX) {
        free(data);
        return NULL;
    }

    void *sound = rt_sound_load_mem(data, (int64_t)data_size);
    free(data);
    return sound;
}

/// @brief Load a sound effect from an in-memory WAV, OGG, or MP3 buffer.
/// @details OGG and MP3 input is decoded into temporary WAV storage before
///          backend loading. The function does not retain or modify the
///          caller's input buffer.
/// @param data Borrowed encoded audio bytes.
/// @param size Number of readable bytes in @p data.
/// @return New reference-counted sound wrapper, or NULL for invalid input or
///         any initialization, decoding, backend, or allocation failure.
void *rt_sound_load_mem(const void *data, int64_t size) {
    if (!data || size <= 0)
        return NULL;
    if ((uint64_t)size > (uint64_t)SIZE_MAX)
        return NULL;

    if (!ensure_audio_init())
        return NULL;

    size_t data_size = (size_t)size;
    void *wrapper = NULL;
    int fmt = detect_audio_format_mem(data, data_size);
    if (fmt == 2) {
        uint8_t *wav_data = NULL;
        size_t wav_len = 0;
        if (ogg_mem_to_wav(data, data_size, &wav_data, &wav_len) != 0)
            return NULL;
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound_mem(g_audio_ctx, wav_data, wav_len);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
        free(wav_data);
    } else if (fmt == 3) {
        uint8_t *wav_data = NULL;
        size_t wav_len = 0;
        if (mp3_data_to_wav((const uint8_t *)data, data_size, &wav_data, &wav_len) != 0)
            return NULL;
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound_mem(g_audio_ctx, wav_data, wav_len);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
        free(wav_data);
    } else {
        audio_state_lock();
        if (g_audio_ctx) {
            vaud_sound_t snd = vaud_load_sound_mem(g_audio_ctx, data, data_size);
            wrapper = rt_sound_wrap_loaded_locked(snd);
        }
        audio_state_unlock();
    }

    return wrapper;
}

/// @brief Release one caller-owned reference to a sound wrapper.
/// @details Invalid and NULL handles are ignored. When the reference count
///          reaches zero, the runtime finalizer unregisters the wrapper and
///          releases its underlying backend sound.
/// @param sound Opaque sound handle returned by a load operation.
void rt_sound_destroy(void *sound) {
    if (!sound)
        return;

    if (!rt_sound_is_handle(sound))
        return;

    if (rt_obj_release_check0(sound))
        rt_obj_free(sound);
}

/// @brief Shared implementation for every sound-effect playback entry point.
/// @details Clamps @p volume to 0..100 and @p pan to -100..100, normalises
///          out-of-range @p group to `RT_MIXGROUP_SFX`, then dispatches to
///          `vaud_play_loop` or `vaud_play_ex` against the wrapped
///          ZannaAUD sound. The voice id is stamped into the tracked-voice
///          table (after pruning stale slots) so subsequent group-volume
///          changes and `rt_voice_*` queries can address it. Returns -1
///          on NULL input or when the wrapper is no longer valid (e.g.
///          shut down during a play).
/// @param sound  `rt_sound` handle (must pass @ref rt_sound_is_handle).
/// @param volume Logical volume 0–100 *before* mix-group scaling.
/// @param pan    Stereo pan -100..100 (negative = left).
/// @param loop   Non-zero to loop continuously.
/// @param group  Target mix group; out-of-range falls back to SFX.
/// @return Voice id on success, -1 on failure.
static int64_t rt_sound_play_internal(
    void *sound, int64_t volume, int64_t pan, int loop, int64_t group) {
    if (!sound)
        return -1;

    volume = clamp_volume_100(volume);
    if (pan < -100)
        pan = -100;
    if (pan > 100)
        pan = 100;
    audio_state_lock();
    if (!audio_group_id_valid_unlocked(group))
        group = RT_MIXGROUP_SFX;
    rt_sound *snd = rt_sound_from_handle_locked(sound);
    if (!snd || !snd->sound || !g_audio_ctx || !vaud_sound_is_attached(snd->sound)) {
        audio_state_unlock();
        return -1;
    }

    int64_t effective = apply_group_volume(volume, group);
    float vol = (float)effective / 100.0f;
    float p = (float)pan / 100.0f;

    vaud_voice_id voice = loop ? vaud_play_loop_group(snd->sound, vol, p, group)
                               : vaud_play_ex_group(snd->sound, vol, p, group);
    int64_t result = (int64_t)voice;
    tracked_voice_prune_locked();
    tracked_voice_set(result, group, volume);
    audio_state_unlock();
    return result;
}

/// @brief Determine whether a pointer names a live runtime sound wrapper.
/// @details Looks up @p sound in the protected wrapper registry instead of
///          dereferencing arbitrary caller memory. A registered wrapper whose
///          backend sound was detached during shutdown still counts as a
///          handle; use @ref rt_sound_is_playable to require backend attachment.
/// @param sound Opaque pointer to examine; NULL is never a valid handle.
/// @return `1` for a registered sound wrapper, otherwise `0`.
int64_t rt_sound_is_handle(void *sound) {
    if (!sound)
        return 0;
    audio_state_lock();
    int64_t ok = rt_sound_from_handle_locked(sound) ? 1 : 0;
    audio_state_unlock();
    return ok;
}

/// @brief True when @p sound is a valid wrapper whose backend Sound is still
///        attached to the live audio context, i.e. it can actually play now.
/// @details Stricter than @ref rt_sound_is_handle: wrappers survive
///          `Audio.Shutdown()` in the registry with their ZannaAUD handles
///          detached, and such zombies must not register as playable
///          (VDOC-121).
/// @param sound Opaque pointer to examine.
/// @return `1` when @p sound is a registered wrapper attached to the active
///         backend context, otherwise `0`.
int64_t rt_sound_is_playable(void *sound) {
    if (!sound)
        return 0;
    audio_state_lock();
    rt_sound *snd = rt_sound_from_handle_locked(sound);
    int64_t ok = (snd && snd->sound && g_audio_ctx && vaud_sound_is_attached(snd->sound)) ? 1 : 0;
    audio_state_unlock();
    return ok;
}

/// @brief Play a sound effect at full logical volume and centered pan.
/// @param sound Opaque playable sound wrapper.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play(void *sound) {
    return rt_sound_play_internal(sound, 100, 0, 0, RT_MIXGROUP_SFX);
}

/// @brief Play a sound once with explicit volume and stereo pan.
/// @param sound Opaque playable sound wrapper.
/// @param volume Logical volume clamped to `[0, 100]`.
/// @param pan Stereo pan clamped to `[-100, 100]`.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_ex(void *sound, int64_t volume, int64_t pan) {
    return rt_sound_play_internal(sound, volume, pan, 0, RT_MIXGROUP_SFX);
}

/// @brief Play a sound continuously with explicit volume and stereo pan.
/// @param sound Opaque playable sound wrapper.
/// @param volume Logical volume clamped to `[0, 100]`.
/// @param pan Stereo pan clamped to `[-100, 100]`.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_loop(void *sound, int64_t volume, int64_t pan) {
    return rt_sound_play_internal(sound, volume, pan, 1, RT_MIXGROUP_SFX);
}

/// @brief Stop a playing voice immediately.
/// @details Removes the identifier from runtime tracking even when no backend
///          context exists. Negative identifiers are ignored.
/// @param voice_id Identifier returned by a sound playback operation.
void rt_voice_stop(int64_t voice_id) {
    if (voice_id < 0)
        return;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_stop_voice(g_audio_ctx, (vaud_voice_id)voice_id);
    tracked_voice_remove(voice_id);
    audio_state_unlock();
}

/// @brief Change a playing voice's logical pre-group volume.
/// @details Clamps @p volume to `[0, 100]`, records it in the tracked-voice
///          table, reapplies the voice's mix-group multiplier, and sends the
///          resulting gain to the live backend.
/// @param voice_id Identifier returned by a sound playback operation.
/// @param volume Requested logical volume percentage.
void rt_voice_set_volume(int64_t voice_id, int64_t volume) {
    if (voice_id < 0)
        return;

    volume = clamp_volume_100(volume);

    audio_state_lock();
    if (!g_audio_ctx) {
        audio_state_unlock();
        return;
    }
    int32_t index = tracked_voice_find(voice_id);
    int64_t group = RT_MIXGROUP_SFX;
    if (index >= 0) {
        g_tracked_voices[index].base_volume = volume;
        group = g_tracked_voices[index].group;
    } else if (vaud_voice_is_playing(g_audio_ctx, (vaud_voice_id)voice_id)) {
        tracked_voice_set(voice_id, group, volume);
    }
    int64_t effective = apply_group_volume(volume, group);
    vaud_set_voice_volume(g_audio_ctx, (vaud_voice_id)voice_id, (float)effective / 100.0f);
    audio_state_unlock();
}

/// @brief Change the stereo pan of a playing voice.
/// @param voice_id Identifier returned by a sound playback operation.
/// @param pan Pan position clamped to `[-100, 100]`, where negative values are
///        left and positive values are right.
void rt_voice_set_pan(int64_t voice_id, int64_t pan) {
    if (voice_id < 0)
        return;

    if (pan < -100)
        pan = -100;
    if (pan > 100)
        pan = 100;
    float p = (float)pan / 100.0f;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_voice_pan(g_audio_ctx, (vaud_voice_id)voice_id, p);
    audio_state_unlock();
}

/// @brief Check whether a voice is currently playing.
/// @details Stale identifiers are removed from runtime voice tracking when the
///          backend reports that playback has ended.
/// @param voice_id Identifier returned by a sound playback operation.
/// @return `1` while the backend voice is active, otherwise `0`.
int64_t rt_voice_is_playing(int64_t voice_id) {
    if (voice_id < 0)
        return 0;

    audio_state_lock();
    int64_t playing =
        (g_audio_ctx && vaud_voice_is_playing(g_audio_ctx, (vaud_voice_id)voice_id)) ? 1 : 0;
    if (!playing)
        tracked_voice_remove(voice_id);
    audio_state_unlock();
    return playing;
}

/// @brief Set a voice's playback-rate (pitch) multiplier.
/// @details 1.0 = native rate; the mixer clamps to 0.25–4.0 and resamples
///          with a fractional cursor, so both pitch and duration scale.
///          Safe no-op on finished/invalid voices.
/// @param voice_id Identifier returned by a sound playback operation.
/// @param pitch Requested playback-rate multiplier.
void rt_voice_set_pitch(int64_t voice_id, double pitch) {
    if (voice_id < 0)
        return;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_voice_pitch(g_audio_ctx, (vaud_voice_id)voice_id, (float)pitch);
    audio_state_unlock();
}

/// @brief Get a voice's playback-rate multiplier.
/// @param voice_id Identifier returned by a sound playback operation.
/// @return Current backend pitch multiplier, or `1.0` for an invalid voice or
///         unavailable context.
double rt_voice_get_pitch(int64_t voice_id) {
    if (voice_id < 0)
        return 1.0;

    audio_state_lock();
    double pitch =
        g_audio_ctx ? (double)vaud_get_voice_pitch(g_audio_ctx, (vaud_voice_id)voice_id) : 1.0;
    audio_state_unlock();
    return pitch;
}

/// @brief Set a direct per-voice lowpass cutoff in Hz (<= 0 disables).
/// @details Useful for scoped-focus/underwater effects; composes with the
///          occlusion sweep (the lower cutoff wins).
/// @param voice_id Identifier returned by a sound playback operation.
/// @param cutoff_hz Cutoff frequency in hertz; non-positive disables the filter.
void rt_voice_set_lowpass(int64_t voice_id, double cutoff_hz) {
    if (voice_id < 0)
        return;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_voice_lowpass(g_audio_ctx, (vaud_voice_id)voice_id, (float)cutoff_hz);
    audio_state_unlock();
}

/// @brief Set a voice's occlusion amount (0 = open .. 1 = fully occluded).
/// @details Maps to a perceptual lowpass sweep plus up to -6 dB attenuation;
///          the mixer smooths changes (~80 ms) so gameplay-driven line-of-
///          sight flips never zipper. The caller supplies the amount (e.g.
///          from its own occlusion raycasts).
/// @param voice_id Identifier returned by a sound playback operation.
/// @param amount Requested occlusion fraction.
void rt_voice_set_occlusion(int64_t voice_id, double amount) {
    if (voice_id < 0)
        return;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_voice_occlusion(g_audio_ctx, (vaud_voice_id)voice_id, (float)amount);
    audio_state_unlock();
}

/// @brief Enable or disable per-voice RMS metering.
/// @details Metering provides a lip-sync/source-level tap for the last mixed
///          block and is disabled by default.
/// @param voice_id Identifier returned by a sound playback operation.
/// @param enabled Non-zero to collect RMS levels; zero to disable collection.
void rt_voice_enable_metering(int64_t voice_id, int8_t enabled) {
    if (voice_id < 0)
        return;
    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_voice_metering(g_audio_ctx, (vaud_voice_id)voice_id, enabled ? 1 : 0);
    audio_state_unlock();
}

/// @brief Read the RMS source level of the last mixed block.
/// @param voice_id Identifier returned by a sound playback operation.
/// @return Most recent level, nominally `0..1`, or `0` when the voice is
///         invalid, unmetered, or no context exists.
double rt_voice_get_level(int64_t voice_id) {
    double level = 0.0;
    if (voice_id < 0)
        return 0.0;
    audio_state_lock();
    if (g_audio_ctx)
        level = (double)vaud_get_voice_level(g_audio_ctx, (vaud_voice_id)voice_id);
    audio_state_unlock();
    return level;
}

/// @brief Play a sound with volume (0-100), pan (-100..100), and pitch.
/// @details PlayEx plus a playback-rate multiplier applied atomically at
///          start so the first rendered chunk is already pitch-shifted.
/// @param sound Opaque playable sound wrapper.
/// @param volume Logical volume clamped to `[0, 100]`.
/// @param pan Stereo pan clamped to `[-100, 100]`.
/// @param pitch Requested playback-rate multiplier.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_ex2(void *sound, int64_t volume, int64_t pan, double pitch) {
    int64_t voice_id = rt_sound_play_ex(sound, volume, pan);
    if (voice_id >= 0)
        rt_voice_set_pitch(voice_id, pitch);
    return voice_id;
}

/// @brief Register, replace, or remove a sidechain-style ducking rule
///        between two mix groups (by name).
/// @details While anything in @p trigger_group is audible, @p target_group's
///          gain eases toward (1 - amount) over @p attack_sec and recovers
///          over @p release_sec. amount <= 0 removes the rule. Group names
///          are resolved through the same registry as `RegisterGroup`
///          (registering them on first use).
/// @param trigger_group Name of the group whose audible activity drives ducking.
/// @param target_group Name of the group whose gain should be reduced.
/// @param amount Requested attenuation fraction; non-positive removes the rule.
/// @param attack_sec Time in seconds to reach the attenuated gain.
/// @param release_sec Time in seconds to restore the target group's gain.
void rt_audio_set_group_ducking(rt_string trigger_group,
                                rt_string target_group,
                                double amount,
                                double attack_sec,
                                double release_sec) {
    if (!trigger_group || !target_group)
        return;
    if (!ensure_audio_init())
        return;

    int64_t trigger_id = rt_audio_register_group(trigger_group);
    int64_t target_id = rt_audio_register_group(target_group);
    if (trigger_id < 0 || target_id < 0)
        return;

    audio_state_lock();
    if (g_audio_ctx)
        vaud_set_group_duck(g_audio_ctx,
                            trigger_id,
                            target_id,
                            (float)amount,
                            (float)attack_sec,
                            (float)release_sec);
    audio_state_unlock();
}

//===----------------------------------------------------------------------===//
// Music Streaming
//===----------------------------------------------------------------------===//

/// @brief Load a music track for streaming playback (WAV, OGG, or MP3 auto-detected).
/// @details Unlike sounds, music streams from disk and is not fully decoded into memory.
///          Only one foreground music track plays at a time unless crossfade
///          coordination explicitly retains two streams.
/// @param path Runtime string containing a path without embedded NUL bytes.
/// @return New reference-counted music wrapper, or NULL when the path is
///         invalid or initialization, loading, or allocation fails.
void *rt_music_load(rt_string path) {
    if (!path)
        return NULL;

    const char *path_str = audio_path_cstr(path);
    if (!path_str)
        return NULL;

    if (!ensure_audio_init())
        return NULL;

    /* Detect format and load via appropriate ZannaAUD function */
    int fmt = detect_audio_format(path_str);
    vaud_music_t mus = NULL;
    void *wrapper_obj = NULL;

    audio_state_lock();
    if (!g_audio_ctx) {
        audio_state_unlock();
        return NULL;
    }
    switch (fmt) {
        case 2:
            mus = vaud_load_music_ogg(g_audio_ctx, path_str);
            break;
        case 3:
            mus = vaud_load_music_mp3(g_audio_ctx, path_str);
            break;
        default:
            mus = vaud_load_music(g_audio_ctx, path_str);
            break;
    }
    if (!mus) {
        audio_state_unlock();
        return NULL;
    }
    vaud_music_set_group(mus, RT_MIXGROUP_MUSIC);

    /* Allocate wrapper object */
    rt_music *wrapper = (rt_music *)rt_obj_new_i64(0, (int64_t)sizeof(rt_music));
    if (!wrapper) {
        vaud_free_music(mus);
        audio_state_unlock();
        return NULL;
    }

    wrapper->vptr = NULL;
    wrapper->magic = RT_MUSIC_MAGIC;
    wrapper->music = mus;
    wrapper->logical_volume = 100;
    wrapper->logical_loop = 0;
    wrapper->paused = 0;
    wrapper->prev = NULL;
    wrapper->next = NULL;
    rt_obj_set_finalizer(wrapper, rt_music_finalize);
    music_registry_add(wrapper);
    rt_audio_apply_music_volume(wrapper);
    wrapper_obj = wrapper;
    audio_state_unlock();

    return wrapper_obj;
}

/// @brief Release one caller-owned reference to a music wrapper.
/// @details Invalid and NULL handles are ignored. The final reference
///          unregisters the wrapper and releases its streaming backend object.
/// @param music Opaque music handle returned by @ref rt_music_load.
void rt_music_destroy(void *music) {
    if (!music)
        return;

    if (!rt_music_is_handle(music))
        return;

    if (rt_obj_release_check0(music))
        rt_obj_free(music);
}

/// @brief Validate that @p music points to a live `rt_music` wrapper.
/// @details Acquires the audio lock and performs a magic-tagged registry
///          lookup via @ref rt_music_from_handle_locked. Used externally
///          to reject ABI mismatches before forwarding into the rest of
///          the music API.
/// @param music Opaque pointer to examine.
/// @return 1 if @p music is a valid handle, 0 otherwise.
int64_t rt_music_is_handle(void *music) {
    if (!music)
        return 0;
    audio_state_lock();
    int64_t ok = rt_music_from_handle_locked(music) ? 1 : 0;
    audio_state_unlock();
    return ok;
}

/// @brief Start a music track as the foreground stream.
/// @details Stores the loop preference, cancels or releases unrelated active
///          music as needed, starts the backend stream, and reapplies logical
///          and music-group volume. Invalid or detached handles are ignored.
/// @param music Opaque live music wrapper.
/// @param loop Non-zero for continuous looping; zero for one-shot playback.
void rt_music_play(void *music, int64_t loop) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    rt_deferred_release_list releases = {0};

    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    mus->logical_loop = loop ? 1 : 0;
    mus->paused = 0;
    rt_audio_prepare_music_for_foreground_locked(mus, &releases);
    vaud_music_set_loop(mus->music, mus->logical_loop);
    vaud_music_play(mus->music, mus->logical_loop);
    rt_audio_apply_music_volume(mus);
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Stop a music track and reset its playback position.
/// @details Delegates to @ref rt_music_stop_related so an active crossfade is
///          cancelled consistently with its paired track.
/// @param music Opaque music wrapper; invalid handles are ignored.
void rt_music_stop(void *music) {
    rt_music_stop_related(music);
}

/// @brief Pause music playback at the current position.
/// @details Delegates to @ref rt_music_pause_related so both sides of an active
///          crossfade pause together.
/// @param music Opaque music wrapper; invalid handles are ignored.
void rt_music_pause(void *music) {
    rt_music_pause_related(music);
}

/// @brief Resume music playback from its paused position.
/// @details Delegates to @ref rt_music_resume_related so paired crossfade state
///          and its monotonic clock resume coherently.
/// @param music Opaque music wrapper; invalid handles are ignored.
void rt_music_resume(void *music) {
    rt_music_resume_related(music);
}

/// @brief Set the loop flag on a music track.
/// @details Updates the wrapper's `logical_loop`. If the track is the
///          fading-out side of an active crossfade, the underlying
///          stream's loop flag is forced to 0 (so the dying track ends
///          when the file ends, even if the user requested looping);
///          otherwise the logical loop value is mirrored to ZannaAUD.
/// @param music Music wrapper handle.
/// @param loop  Non-zero to enable continuous looping.
void rt_music_set_loop(void *music, int64_t loop) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus) {
        audio_state_unlock();
        return;
    }
    mus->logical_loop = loop ? 1 : 0;
    if (mus->music) {
        int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
        if (xf_idx >= 0 && g_crossfades[xf_idx].fade_out == mus)
            vaud_music_set_loop(mus->music, 0);
        else
            vaud_music_set_loop(mus->music, mus->logical_loop);
    }
    audio_state_unlock();
}

/// @brief Set a music track's logical pre-group volume.
/// @details Clamps the value to `[0, 100]`. When the track participates in a
///          crossfade, updates the corresponding curve anchor and reapplies
///          both envelopes; otherwise applies the volume directly.
/// @param music Opaque live music wrapper.
/// @param volume Requested logical volume percentage.
void rt_music_set_volume(void *music, int64_t volume) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    mus->logical_volume = clamp_volume_100(volume);
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0) {
        rt_music_crossfade_state *xf = &g_crossfades[xf_idx];
        if (xf->fade_out == mus)
            xf->vol_out = mus->logical_volume;
        if (xf->fade_in == mus)
            xf->vol_in = mus->logical_volume;
        rt_audio_reapply_crossfade_locked(xf);
    } else {
        rt_audio_apply_music_volume(mus);
    }
    audio_state_unlock();
}

/// @brief Get a music track's logical pre-group volume.
/// @param music Opaque live music wrapper.
/// @return Stored volume in `[0, 100]`, or `0` for an invalid/detached handle.
int64_t rt_music_get_volume(void *music) {
    if (!music)
        return 0;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return 0;
    }

    int64_t volume = mus->logical_volume;
    audio_state_unlock();
    return volume;
}

/// @brief Check whether a music track is currently playing.
/// @param music Opaque live music wrapper.
/// @return `1` when its backend stream is playing, otherwise `0`.
int64_t rt_music_is_playing(void *music) {
    if (!music)
        return 0;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return 0;
    }

    int64_t playing = vaud_music_is_playing(mus->music) ? 1 : 0;
    audio_state_unlock();

    return playing;
}

/// @brief Seek to a position measured from the start of a music track.
/// @details Clamps negative positions to zero and positions beyond a known
///          duration to the end. Active crossfade envelopes are reapplied after
///          seeking so the track retains its instantaneous transition gain.
/// @param music Opaque live music wrapper.
/// @param position_ms Requested zero-based position in milliseconds.
void rt_music_seek(void *music, int64_t position_ms) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    if (position_ms < 0)
        position_ms = 0;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    double seconds_d = (double)position_ms / 1000.0;
    float duration = vaud_music_get_duration(mus->music);
    if (duration > 0.0f && seconds_d > (double)duration)
        seconds_d = (double)duration;
    if (seconds_d > (double)FLT_MAX)
        seconds_d = (double)FLT_MAX;
    float seconds = (float)seconds_d;
    vaud_music_seek(mus->music, seconds);
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0)
        rt_audio_reapply_crossfade_locked(&g_crossfades[xf_idx]);
    else
        rt_audio_apply_music_volume(mus);
    audio_state_unlock();
}

/// @brief Get the current music playback position.
/// @param music Opaque live music wrapper.
/// @return Rounded milliseconds from the start, or `0` for an invalid handle,
///         unavailable stream, or non-positive/non-finite backend result.
int64_t rt_music_get_position(void *music) {
    if (!music)
        return 0;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return 0;
    }

    float seconds = vaud_music_get_position(mus->music);
    audio_state_unlock();
    return seconds_to_ms_i64(seconds);
}

/// @brief Get the total duration of a music track.
/// @param music Opaque live music wrapper.
/// @return Rounded duration in milliseconds, or `0` for an invalid handle,
///         unavailable stream, or non-positive/non-finite backend result.
int64_t rt_music_get_duration(void *music) {
    if (!music)
        return 0;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return 0;
    }

    float seconds = vaud_music_get_duration(mus->music);
    audio_state_unlock();
    return seconds_to_ms_i64(seconds);
}

/// @brief Pause a music track plus anything tied to it by an active crossfade.
/// @details If @p music is currently part of a crossfade slot, pauses
///          *both* sides (fade-out and fade-in), marks the slot as paused,
///          and snapshots the wall-clock so @ref rt_audio_update can resume
///          time-keeping without skipping forward when the user calls
///          @ref rt_music_resume_related. Outside of a crossfade,
///          delegates to the underlying `vaud_music_pause` and updates
///          the wrapper's `paused` flag.
/// @param music Music wrapper handle.
void rt_music_pause_related(void *music) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0) {
        rt_music_crossfade_state *xf = &g_crossfades[xf_idx];
        xf->paused = 1;
        xf->last_tick_ms = rt_timer_ms();
        if (xf->fade_out && ((rt_music *)xf->fade_out)->music)
            vaud_music_pause(((rt_music *)xf->fade_out)->music);
        if (xf->fade_in && ((rt_music *)xf->fade_in)->music)
            vaud_music_pause(((rt_music *)xf->fade_in)->music);
        if (xf->fade_out)
            ((rt_music *)xf->fade_out)->paused = 1;
        if (xf->fade_in)
            ((rt_music *)xf->fade_in)->paused = 1;
    } else {
        int was_playing = vaud_music_is_playing(mus->music);
        vaud_music_pause(mus->music);
        if (was_playing || mus->paused)
            mus->paused = 1;
    }
    audio_state_unlock();
}

/// @brief Resume a music track plus the crossfade slot, if any, that includes it.
/// @details Inverse of @ref rt_music_pause_related. When the track is the
///          subject of a paused crossfade, every other unrelated music
///          track is stopped first (so the crossfade resumes alone),
///          both sides of the slot are resumed, and the tick reference
///          is rebased to the current wall-clock so elapsed time does
///          not jump forward by the pause duration. Outside a crossfade
///          the wrapper's `paused` flag drives `vaud_music_resume`.
/// @param music Music wrapper handle.
void rt_music_resume_related(void *music) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    rt_deferred_release_list releases = {0};

    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0) {
        rt_music_crossfade_state *xf = &g_crossfades[xf_idx];
        if (xf->paused) {
            rt_audio_stop_unrelated_music_locked(xf->fade_out, xf->fade_in, &releases);
            xf->paused = 0;
            xf->last_tick_ms = rt_timer_ms();
            if (xf->fade_out && ((rt_music *)xf->fade_out)->music) {
                ((rt_music *)xf->fade_out)->paused = 0;
                vaud_music_resume(((rt_music *)xf->fade_out)->music);
            }
            if (xf->fade_in && ((rt_music *)xf->fade_in)->music) {
                ((rt_music *)xf->fade_in)->paused = 0;
                vaud_music_resume(((rt_music *)xf->fade_in)->music);
            }
        }
    } else {
        if (mus->paused) {
            rt_audio_prepare_music_for_foreground_locked(mus, &releases);
            mus->paused = 0;
            vaud_music_resume(mus->music);
        }
    }
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Stop a music track, cancelling its crossfade slot if it has one.
/// @details If @p music is one side of an active crossfade, that side's
///          stream is stopped while volumes on the other side are
///          restored to their logical value (so the surviving track
///          continues normally). Outside a crossfade, simply stops the
///          underlying stream and clears the wrapper's `paused` flag.
/// @param music Music wrapper handle.
void rt_music_stop_related(void *music) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    rt_deferred_release_list releases = {0};

    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0) {
        rt_music_crossfade_state *xf = &g_crossfades[xf_idx];
        int stop_fade_out = (xf->fade_out == mus);
        int stop_fade_in = (xf->fade_in == mus);
        rt_audio_crossfade_cancel_entry_locked(xf, stop_fade_out, stop_fade_in, 1, &releases);
    } else {
        mus->paused = 0;
        vaud_music_stop(mus->music);
    }
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Apply a uniform logical volume to both sides of a music crossfade.
/// @details When @p music is part of an active crossfade slot, sets the
///          logical volume on both sides (so the curve interpolates
///          against the same anchor) and re-derives the per-side gains
///          via @ref rt_audio_reapply_crossfade_locked. Outside a
///          crossfade behaves like @ref rt_music_set_volume on a single
///          track.
/// @param music  Music wrapper handle.
/// @param volume Logical volume 0–100; clamped before use.
void rt_music_set_crossfade_pair_volume(void *music, int64_t volume) {
    if (!music)
        return;

    rt_music *mus = (rt_music *)music;
    volume = clamp_volume_100(volume);

    audio_state_lock();
    mus = rt_music_from_handle_locked(music);
    if (!mus || !mus->music) {
        audio_state_unlock();
        return;
    }
    int32_t xf_idx = rt_audio_find_crossfade_by_music_locked(mus);
    if (xf_idx >= 0) {
        rt_music_crossfade_state *xf = &g_crossfades[xf_idx];
        if (xf->fade_out) {
            ((rt_music *)xf->fade_out)->logical_volume = volume;
            xf->vol_out = volume;
        }
        if (xf->fade_in) {
            ((rt_music *)xf->fade_in)->logical_volume = volume;
            xf->vol_in = volume;
        }
        rt_audio_reapply_crossfade_locked(xf);
    } else {
        mus->logical_volume = volume;
        rt_audio_apply_music_volume(mus);
    }
    audio_state_unlock();
}

//===----------------------------------------------------------------------===//
// Mix Groups — real implementation
//===----------------------------------------------------------------------===//

/// @brief Set the logical volume multiplier for a mix group.
/// @details Ignores invalid or currently unused group identifiers. The value is
///          clamped to `[0, 100]` and immediately reapplied to active music or
///          tracked voices assigned to the group.
/// @param group Registered mix-group identifier.
/// @param volume Requested group volume in percent.
void rt_audio_set_group_volume(int64_t group, int64_t volume) {
    audio_state_lock();
    if (!audio_group_id_valid_unlocked(group)) {
        audio_state_unlock();
        return;
    }
    g_group_volume[group] = clamp_volume_100(volume);
    if (group == RT_MIXGROUP_MUSIC)
        rt_audio_refresh_music_group_volumes_locked();
    else
        rt_audio_refresh_voice_group_volumes(group);
    audio_state_unlock();
}

/// @brief Read the logical volume multiplier for a mix group.
/// @param group Registered mix-group identifier.
/// @return Stored volume in `[0, 100]`, or the neutral default `100` when
///         @p group is invalid or unused.
int64_t rt_audio_get_group_volume(int64_t group) {
    audio_state_lock();
    int64_t volume = audio_group_id_valid_unlocked(group) ? g_group_volume[group] : 100;
    audio_state_unlock();
    return volume;
}

/// @brief Register a canonicalized named mix group.
/// @details Leading and trailing spaces/tabs are removed. Re-registering an
///          existing name returns its current identifier; a new name claims
///          the first available named-group slot with volume `100`.
/// @param group_name Runtime string naming the group.
/// @return Group identifier, or `-1` for an empty, overlong, or otherwise
///         invalid name, or when the fixed registry is full.
int64_t rt_audio_register_group(rt_string group_name) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_register_group_unlocked(name);
    audio_state_unlock();
    return id;
}

/// @brief Find a previously registered mix group by canonical name.
/// @param group_name Runtime string naming a built-in or named group.
/// @return Registered group identifier, or `-1` when the canonicalized name is
///         empty, invalid, or absent.
int64_t rt_audio_find_group(rt_string group_name) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_find_group_unlocked(name);
    audio_state_unlock();
    return id;
}

/// @brief Find a registered named mix group as an Option id.
/// @details Sentinel-free companion to @ref rt_audio_find_group. Built-in and
///          registered groups return `SomeI64(id)`, while missing groups return
///          None.
/// @param group_name Group name.
/// @return Opaque Zanna.Option containing the group id, or None.
void *rt_audio_find_group_option(rt_string group_name) {
    int64_t id = rt_audio_find_group(group_name);
    return id >= 0 ? rt_option_some_i64(id) : rt_option_none();
}

/// @brief Set the volume of a named mix group, registering it when necessary.
/// @details The canonicalized name is looked up or allocated under the
///          audio-state lock. Successful updates are clamped to `[0, 100]` and
///          immediately propagated to active music or tracked voices.
/// @param group_name Runtime string naming the target group.
/// @param volume Requested volume in percent. Invalid names or registry
///        exhaustion leave state unchanged.
void rt_audio_set_group_volume_named(rt_string group_name, int64_t volume) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_register_group_unlocked(name);
    if (id >= 0) {
        g_group_volume[id] = clamp_volume_100(volume);
        if (id == RT_MIXGROUP_MUSIC)
            rt_audio_refresh_music_group_volumes_locked();
        else
            rt_audio_refresh_voice_group_volumes(id);
    }
    audio_state_unlock();
}

/// @brief Read the volume of a named mix group.
/// @param group_name Runtime string naming a built-in or registered group.
/// @return Stored volume in `[0, 100]`, or the neutral default `100` when the
///         name is invalid or has not been registered.
int64_t rt_audio_get_group_volume_named(rt_string group_name) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_find_group_unlocked(name);
    int64_t volume = id >= 0 ? g_group_volume[id] : 100;
    audio_state_unlock();
    return volume;
}

/// @brief Obtain the canonical name associated with a mix-group identifier.
/// @param group_id Registered mix-group identifier.
/// @return Runtime-owned copy of the stored group name, or the shared empty
///         runtime string for an invalid/unused identifier.
rt_string rt_audio_group_name(int64_t group_id) {
    audio_state_lock();
    const char *name = audio_group_id_valid_unlocked(group_id) ? g_group_names[group_id] : "";
    rt_string result = rt_const_cstr(name);
    audio_state_unlock();
    return result;
}

/// @brief Check whether an identifier may be used with the runtime FX registry.
/// @param group Mix-group identifier to validate.
/// @return Non-zero when @p group is in range and currently registered.
static int8_t rt_audio_fx_group_valid(int64_t group) {
    audio_state_lock();
    int8_t ok = audio_group_id_valid_unlocked(group);
    audio_state_unlock();
    return ok;
}

/// @brief Append a low-pass filter to a mix group's effect chain.
/// @param group Registered mix-group identifier.
/// @param cutoff_hz Filter cutoff frequency in hertz.
/// @param q Filter resonance/quality factor.
/// @return Effect identifier, or `-1` when @p group is invalid or the effect
///         cannot be allocated.
int64_t rt_snd_group_add_lowpass(int64_t group, double cutoff_hz, double q) {
    if (!rt_audio_fx_group_valid(group))
        return -1;
    return rt_audio_fx_add_lowpass(group, cutoff_hz, q);
}

/// @brief Append a high-pass filter to a mix group's effect chain.
/// @param group Registered mix-group identifier.
/// @param cutoff_hz Filter cutoff frequency in hertz.
/// @param q Filter resonance/quality factor.
/// @return Effect identifier, or `-1` when @p group is invalid or the effect
///         cannot be allocated.
int64_t rt_snd_group_add_highpass(int64_t group, double cutoff_hz, double q) {
    if (!rt_audio_fx_group_valid(group))
        return -1;
    return rt_audio_fx_add_highpass(group, cutoff_hz, q);
}

/// @brief Append a peaking equalizer to a mix group's effect chain.
/// @param group Registered mix-group identifier.
/// @param freq_hz Center frequency in hertz.
/// @param q Filter bandwidth/quality factor.
/// @param gain_db Gain applied at the center frequency, in decibels.
/// @return Effect identifier, or `-1` when @p group is invalid or the effect
///         cannot be allocated.
int64_t rt_snd_group_add_peaking(int64_t group, double freq_hz, double q, double gain_db) {
    if (!rt_audio_fx_group_valid(group))
        return -1;
    return rt_audio_fx_add_peaking(group, freq_hz, q, gain_db);
}

/// @brief Append a feedback delay to a mix group's effect chain.
/// @param group Registered mix-group identifier.
/// @param delay_ms Delay time in milliseconds.
/// @param feedback Fraction of delayed output fed back into the delay line.
/// @param wet Wet-signal mix level.
/// @return Effect identifier, or `-1` when @p group is invalid or the effect
///         cannot be allocated.
int64_t rt_snd_group_add_delay(int64_t group, double delay_ms, double feedback, double wet) {
    if (!rt_audio_fx_group_valid(group))
        return -1;
    return rt_audio_fx_add_delay(group, delay_ms, feedback, wet);
}

/// @brief Update a group reverb insert's parameters in place.
/// @details Delegates effect lookup and parameter normalization to the FX
///          registry. This supports gradual zone transitions without replacing
///          the existing effect and its accumulated state.
/// @param group Mix-group identifier containing the effect.
/// @param fx_id Reverb effect identifier returned by
///        @ref rt_snd_group_add_reverb.
/// @param room_size Reverb room-size parameter.
/// @param damping High-frequency damping parameter.
/// @param wet Wet-signal mix level.
void rt_snd_group_set_reverb(
    int64_t group, int64_t fx_id, double room_size, double damping, double wet) {
    rt_audio_fx_set_reverb_params(group, fx_id, room_size, damping, wet);
}

/// @brief Append a reverb processor to a mix group's effect chain.
/// @param group Registered mix-group identifier.
/// @param room_size Reverb room-size parameter.
/// @param damping High-frequency damping parameter.
/// @param wet Wet-signal mix level.
/// @return Effect identifier, or `-1` when @p group is invalid or the effect
///         cannot be allocated.
int64_t rt_snd_group_add_reverb(int64_t group, double room_size, double damping, double wet) {
    if (!rt_audio_fx_group_valid(group))
        return -1;
    return rt_audio_fx_add_reverb(group, room_size, damping, wet);
}

/// @brief Enable or bypass one effect in a mix group's chain.
/// @param group Registered mix-group identifier.
/// @param fx_id Effect identifier returned by an add operation.
/// @param bypass Non-zero to bypass processing; zero to enable it.
void rt_snd_group_fx_bypass(int64_t group, int64_t fx_id, int8_t bypass) {
    if (!rt_audio_fx_group_valid(group))
        return;
    rt_audio_fx_set_bypass(group, fx_id, bypass);
}

/// @brief Remove one effect from a mix group's chain.
/// @param group Registered mix-group identifier.
/// @param fx_id Effect identifier returned by an add operation.
void rt_snd_group_remove_fx(int64_t group, int64_t fx_id) {
    if (!rt_audio_fx_group_valid(group))
        return;
    rt_audio_fx_remove(group, fx_id);
}

/// @brief Remove every effect from a mix group's processing chain.
/// @param group Registered mix-group identifier. Invalid groups are ignored.
void rt_snd_group_clear_fx(int64_t group) {
    if (!rt_audio_fx_group_valid(group))
        return;
    rt_audio_fx_clear_group(group);
}

//===----------------------------------------------------------------------===//
// Music Crossfade — real implementation
//===----------------------------------------------------------------------===//

/// @brief Begin a crossfade transition between two music tracks over the given duration.
/// @details Fades out the current track while fading in the new track simultaneously.
///          Both tracks are retained for the duration of the crossfade and
///          released after the transition outside the audio-state lock. A
///          non-positive duration performs an immediate transition. Invalid,
///          detached, or identical handle combinations are ignored.
/// @param current_music Opaque music handle to fade out; may be NULL.
/// @param new_music Opaque music handle to fade in; may be NULL.
/// @param duration_ms Transition duration in milliseconds.
void rt_music_crossfade_to(void *current_music, void *new_music, int64_t duration_ms) {
    rt_deferred_release_list releases = {0};

    audio_state_lock();

    rt_music *current = current_music ? rt_music_from_handle_locked(current_music) : NULL;
    rt_music *next = new_music ? rt_music_from_handle_locked(new_music) : NULL;
    if ((current_music && !current) || (new_music && !next)) {
        audio_state_unlock();
        return;
    }

    if (current && (!current->music || !vaud_music_is_attached(current->music)))
        current = NULL;
    if (next && (!next->music || !vaud_music_is_attached(next->music)))
        next = NULL;

    if ((!current && !next) || current == next) {
        audio_state_unlock();
        return;
    }

    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        rt_music_crossfade_state *xf = &g_crossfades[i];
        if (!xf->active)
            continue;

        int keep_fade_out = (xf->fade_out == current || xf->fade_out == next);
        int keep_fade_in = (xf->fade_in == current || xf->fade_in == next);
        if (!keep_fade_out && !keep_fade_in)
            continue;
        rt_audio_crossfade_cancel_entry_locked(
            xf, !keep_fade_out, !keep_fade_in, keep_fade_out || keep_fade_in, &releases);
    }

    if (duration_ms <= 0) {
        if (current) {
            current->paused = 0;
            vaud_music_stop(current->music);
        }
        if (next) {
            rt_audio_stop_unrelated_music_locked(current, next, &releases);
            next->paused = 0;
            vaud_music_set_loop(next->music, next->logical_loop);
            vaud_music_play(next->music, next->logical_loop);
            rt_audio_apply_music_volume(next);
        }
        audio_state_unlock();
        rt_audio_drain_releases(&releases);
        return;
    }

    int32_t slot = -1;
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (!g_crossfades[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (current) {
            current->paused = 0;
            vaud_music_stop(current->music);
        }
        if (next) {
            rt_audio_stop_unrelated_music_locked(current, next, &releases);
            next->paused = 0;
            vaud_music_set_loop(next->music, next->logical_loop);
            vaud_music_play(next->music, next->logical_loop);
            rt_audio_apply_music_volume(next);
        }
        audio_state_unlock();
        rt_audio_drain_releases(&releases);
        return;
    }

    rt_audio_stop_unrelated_music_locked(current, next, &releases);

    if (current)
        rt_obj_retain_maybe(current);
    if (next)
        rt_obj_retain_maybe(next);

    rt_music_crossfade_state *xf = &g_crossfades[slot];
    xf->fade_out = current;
    xf->fade_in = next;
    xf->duration = duration_ms;
    xf->elapsed = 0;
    xf->active = 1;
    xf->paused = 0;
    xf->last_tick_ms = rt_timer_ms();
    xf->vol_out = current ? current->logical_volume : 100;
    xf->vol_in = next ? next->logical_volume : 100;

    if (current) {
        current->paused = 0;
        vaud_music_set_loop(current->music, 0);
        rt_audio_reapply_crossfade_locked(xf);
    }
    if (next) {
        next->paused = 0;
        rt_audio_apply_music_volume_value(next, 0);
        vaud_music_set_loop(next->music, next->logical_loop);
        vaud_music_play(next->music, next->logical_loop);
    }
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Check whether any music crossfade is currently in progress.
/// @return Non-zero while at least one crossfade slot is active.
int8_t rt_music_is_crossfading(void) {
    audio_state_lock();
    int8_t active = 0;
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (g_crossfades[i].active) {
            active = 1;
            break;
        }
    }
    audio_state_unlock();
    return active;
}

/// @brief Advance every active music crossfade by an explicit time delta.
/// @details Paused crossfades only refresh their monotonic timestamp. Positive
///          deltas update active envelopes and defer any final reference
///          releases until after the audio-state lock is dropped.
/// @param dt_ms Elapsed milliseconds; non-positive values do not advance.
void rt_music_crossfade_update(int64_t dt_ms) {
    rt_deferred_release_list releases = {0};

    audio_state_lock();
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        if (!g_crossfades[i].active)
            continue;
        if (g_audio_paused || g_crossfades[i].paused) {
            g_crossfades[i].last_tick_ms = rt_timer_ms();
            continue;
        }
        if (dt_ms > 0) {
            g_crossfades[i].last_tick_ms = rt_timer_ms();
            rt_audio_update_crossfade_entry_locked(&g_crossfades[i], dt_ms, &releases);
        }
    }
    audio_state_unlock();
    rt_audio_drain_releases(&releases);
}

/// @brief Advance a single crossfade by `dt_ms` and either update envelopes or
///        finalize the transition.
/// @details Per-frame driver for the crossfade state machine. Each tick:
///          1. Clamp `dt_ms` to the remaining duration so we can't overshoot the
///             end-of-crossfade point — critical because an overshoot would leave
///             `elapsed > duration` and confuse the progress computation.
///          2. **End-of-crossfade branch** (`elapsed >= duration`): stop the
///             fade-out music at the backend, re-apply its base volume so a
///             later play-again starts from the user-configured level, resume
///             the fade-in track (clearing any lingering pause), and release
///             the crossfade's retained references via the deferred-release
///             list — `releases` is drained by the caller outside the audio
///             lock so finalizers (which may take other locks) don't deadlock.
///          3. **Normal advance branch**: push the updated `(elapsed, duration)`
///             through `rt_audio_reapply_crossfade_locked` to re-emit the
///             instantaneous fade-out / fade-in volumes. Zero-duration (jump
///             cut) keeps `elapsed = 0` so the reapply branch hits the
///             degenerate "fully transition in one tick" path.
///          `_locked` suffix: caller must hold the audio mutex.
/// @param xf Active crossfade slot to advance.
/// @param dt_ms Positive elapsed time in milliseconds.
/// @param releases Deferred-release list populated with references finalized by this tick.
static void rt_audio_update_crossfade_entry_locked(rt_music_crossfade_state *xf,
                                                   int64_t dt_ms,
                                                   rt_deferred_release_list *releases) {
    if (!xf || !xf->active)
        return;
    if (dt_ms <= 0)
        return;

    if (xf->duration <= 0) {
        xf->elapsed = 0;
    } else if (xf->elapsed >= xf->duration || dt_ms >= xf->duration - xf->elapsed) {
        xf->elapsed = xf->duration;
    } else {
        xf->elapsed += dt_ms;
    }

    if (xf->elapsed >= xf->duration) {
        if (xf->fade_out) {
            rt_music *fade_out = (rt_music *)xf->fade_out;
            if (fade_out->music)
                vaud_music_stop(fade_out->music);
            fade_out->paused = 0;
            rt_audio_apply_music_volume(fade_out);
        }
        if (xf->fade_in) {
            ((rt_music *)xf->fade_in)->paused = 0;
            rt_audio_apply_music_volume((rt_music *)xf->fade_in);
        }
        rt_audio_release_crossfade_refs_locked(xf, releases);
        return;
    }

    rt_audio_reapply_crossfade_locked(xf);
}

//===----------------------------------------------------------------------===//
// Group-Aware Sound Playback — real implementation
//===----------------------------------------------------------------------===//

/// @brief Play a sound at default volume in a selected mix group.
/// @param sound Opaque live sound wrapper.
/// @param group Registered target group; invalid values fall back to the
///        built-in sound-effects group.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_in_group(void *sound, int64_t group) {
    return rt_sound_play_internal(sound, 100, 0, 0, group);
}

/// @brief Play a sound with explicit volume and pan in a selected mix group.
/// @param sound Opaque live sound wrapper.
/// @param volume Logical pre-group volume, clamped to `[0, 100]`.
/// @param pan Stereo pan, clamped to `[-100, 100]`.
/// @param group Registered target group; invalid values fall back to the
///        built-in sound-effects group.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_ex_in_group(void *sound, int64_t volume, int64_t pan, int64_t group) {
    return rt_sound_play_internal(sound, volume, pan, 0, group);
}

/// @brief Play a looping sound with explicit volume and pan in a mix group.
/// @param sound Opaque live sound wrapper.
/// @param volume Logical pre-group volume, clamped to `[0, 100]`.
/// @param pan Stereo pan, clamped to `[-100, 100]`.
/// @param group Registered target group; invalid values fall back to the
///        built-in sound-effects group.
/// @return Backend voice identifier, or `-1` when playback cannot start.
int64_t rt_sound_play_loop_in_group(void *sound, int64_t volume, int64_t pan, int64_t group) {
    return rt_sound_play_internal(sound, volume, pan, 1, group);
}

#else /* !ZANNA_ENABLE_AUDIO */

//===----------------------------------------------------------------------===//
// Stub implementations when audio library is not available
//===----------------------------------------------------------------------===//

/// @brief Raise the canonical "audio support not compiled in" trap.
///
/// Mirror of `rt_graphics_unavailable_` in `rt_graphics_stubs.c`.
/// Shared trap sink used by every Sound / Music entry point in this
/// `#else` branch so failures from a `ZANNA_ENABLE_AUDIO=OFF` build all
/// surface at the offending call site with a consistent error code.
///
/// @param msg Diagnostic string; the convention is
///            `"<Class>.<Method>: audio support not compiled in"`.
///
/// @note Never returns under normal control flow.
static void rt_audio_unavailable_(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, 0, msg);
}

/// @brief Audio-disabled override of `rt_audio_is_available`.
/// @return Always `0` in this `#else` branch (audio not compiled in).
int8_t rt_audio_is_available(void) {
    return 0;
}

/// @brief Audio-disabled stub for `rt_audio_init` — silent no-op
///        returning failure so the bootstrap can branch on the result.
/// @return `0` (init failure).
int64_t rt_audio_init(void) {
    return 0;
}

/// @brief Audio-disabled stub for `rt_audio_shutdown` — silent no-op
///        since there's nothing to shut down.
void rt_audio_shutdown(void) {}

/// @brief Audio-disabled stub for `Audio.SetMasterVolume`.
/// @details Stores logical state so settings UIs round-trip even without a backend.
/// @param volume Requested volume, clamped to `0..100`.
void rt_audio_set_master_volume(int64_t volume) {
    volume = clamp_volume_100(volume);
#if RT_COMPILER_MSVC
    rt_atomic_store_i64(&g_master_volume, volume, __ATOMIC_RELEASE);
#else
    __atomic_store_n(&g_master_volume, volume, __ATOMIC_RELEASE);
#endif
}

/// @brief Audio-disabled stub for `Audio.MasterVolume`.
/// @return Stored logical master volume, defaulting to `100`.
int64_t rt_audio_get_master_volume(void) {
#if RT_COMPILER_MSVC
    return rt_atomic_load_i64(&g_master_volume, __ATOMIC_ACQUIRE);
#else
    return __atomic_load_n(&g_master_volume, __ATOMIC_ACQUIRE);
#endif
}

/// @brief Audio-disabled stub for `Audio.PauseAll`. Silent no-op.
void rt_audio_pause_all(void) {}

/// @brief Audio-disabled stub for `Audio.ResumeAll`. Silent no-op.
void rt_audio_resume_all(void) {}

/// @brief Audio-disabled stub for `Audio.Update`. Silent no-op.
void rt_audio_update(void) {}

/// @brief Audio-disabled stub for `Audio.StopAllSounds`. Silent no-op.
void rt_audio_stop_all_sounds(void) {}

/// @brief Audio-disabled stats stub for render callback count.
/// @return Always zero because no mixer thread exists.
int64_t rt_audio_get_render_calls(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for mixer lock misses.
/// @return Always zero because no mixer thread exists.
int64_t rt_audio_get_mixer_lock_misses(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend write calls.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_write_calls(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend partial writes.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_partial_writes(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend waits.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_waits(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend underrun/suspend observations.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_xruns(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend recovery attempts.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_recoveries(void) {
    return 0;
}

/// @brief Audio-disabled stats stub for backend write failures.
/// @return Always zero because no platform backend exists.
int64_t rt_audio_get_backend_write_failures(void) {
    return 0;
}

/// @brief Audio-disabled stub for `Sound.Load`. Returns `NULL` for a
///        null path; otherwise traps because callers need a usable
///        Sound handle to subsequently `Play`.
/// @param path Ignored.
/// @return `NULL` for a null path; otherwise does not return normally.
void *rt_sound_load(rt_string path) {
    if (!path)
        return NULL;
    (void)path;
    rt_audio_unavailable_("Sound.Load: audio support not compiled in");
    return NULL;
}

/// @brief Audio-disabled stub for loading a named sound asset.
/// @copydetails rt_sound_load_asset
void *rt_sound_load_asset(rt_string name) {
    if (!name)
        return NULL;
    (void)name;
    rt_audio_unavailable_("Sound.LoadAsset: audio support not compiled in");
    return NULL;
}

/// @brief Audio-disabled stub for `Sound.LoadMem`. Returns `NULL` for an
///        invalid buffer; otherwise traps for the same reason as `Sound.Load`.
/// @param data Ignored.
/// @param size Ignored.
/// @return `NULL` for an invalid buffer; otherwise does not return normally.
void *rt_sound_load_mem(const void *data, int64_t size) {
    if (!data || size <= 0)
        return NULL;
    (void)data;
    (void)size;
    rt_audio_unavailable_("Sound.LoadMem: audio support not compiled in");
    return NULL;
}

/// @brief Audio-disabled stub for `Sound.Destroy` — silent no-op (the
///        handle never existed to begin with).
/// @param sound Ignored.
void rt_sound_destroy(void *sound) {
    (void)sound;
}

/// @brief Audio-disabled handle query; no sound wrapper can exist.
/// @copydetails rt_sound_is_handle
int64_t rt_sound_is_handle(void *sound) {
    (void)sound;
    return 0;
}

/// @brief Audio-disabled stub: no sound can ever play.
/// @copydetails rt_sound_is_playable
int64_t rt_sound_is_playable(void *sound) {
    (void)sound;
    return 0;
}

/// @brief Audio-disabled stub for `Sound.Play`. Returns `-1` for a null
///        sound; otherwise traps so the absence of audio surfaces clearly.
/// @param sound Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play(void *sound) {
    if (!sound)
        return -1;
    (void)sound;
    rt_audio_unavailable_("Sound.Play: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Sound.PlayEx` (volume + pan variant
///        of `Play`). Returns `-1` for a null sound; otherwise traps.
/// @param sound  Ignored.
/// @param volume Ignored.
/// @param pan    Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play_ex(void *sound, int64_t volume, int64_t pan) {
    if (!sound)
        return -1;
    (void)sound;
    (void)volume;
    (void)pan;
    rt_audio_unavailable_("Sound.PlayEx: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Sound.PlayLoop` (looping variant of
///        `PlayEx`). Returns `-1` for a null sound; otherwise traps.
/// @param sound  Ignored.
/// @param volume Ignored.
/// @param pan    Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play_loop(void *sound, int64_t volume, int64_t pan) {
    if (!sound)
        return -1;
    (void)sound;
    (void)volume;
    (void)pan;
    rt_audio_unavailable_("Sound.PlayLoop: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Voice.Stop`. Silent no-op (any voice
///        id passed in must be from a fake/cached value since `Sound.Play`
///        traps).
/// @param voice_id Ignored.
void rt_voice_stop(int64_t voice_id) {
    (void)voice_id;
}

/// @brief Audio-disabled stub for `Voice.SetVolume`. Silent no-op.
/// @param voice_id Ignored.
/// @param volume   Ignored.
void rt_voice_set_volume(int64_t voice_id, int64_t volume) {
    (void)voice_id;
    (void)volume;
}

/// @brief Audio-disabled stub for `Voice.SetPan`. Silent no-op.
/// @param voice_id Ignored.
/// @param pan      Ignored.
void rt_voice_set_pan(int64_t voice_id, int64_t pan) {
    (void)voice_id;
    (void)pan;
}

/// @brief Audio-disabled stub for `Voice.IsPlaying`.
/// @param voice_id Ignored.
/// @return `0` (never playing).
int64_t rt_voice_is_playing(int64_t voice_id) {
    (void)voice_id;
    return 0;
}

/// @brief Audio-disabled stub for `Voice.set_Pitch`. Silent no-op.
/// @copydetails rt_voice_set_pitch
void rt_voice_set_pitch(int64_t voice_id, double pitch) {
    (void)voice_id;
    (void)pitch;
}

/// @brief Audio-disabled stub for `Voice.get_Pitch`.
/// @return `1.0` (native rate).
/// @copydetails rt_voice_get_pitch
double rt_voice_get_pitch(int64_t voice_id) {
    (void)voice_id;
    return 1.0;
}

/// @brief Audio-disabled stub for `Voice.SetLowpass`. Silent no-op.
/// @copydetails rt_voice_set_lowpass
void rt_voice_set_lowpass(int64_t voice_id, double cutoff_hz) {
    (void)voice_id;
    (void)cutoff_hz;
}

/// @brief Audio-disabled stub for `Voice.SetOcclusion`. Silent no-op.
/// @copydetails rt_voice_set_occlusion
void rt_voice_set_occlusion(int64_t voice_id, double amount) {
    (void)voice_id;
    (void)amount;
}

/// @brief Audio-disabled metering toggle; no voice state exists.
/// @copydetails rt_voice_enable_metering
void rt_voice_enable_metering(int64_t voice_id, int8_t enabled) {
    (void)voice_id;
    (void)enabled;
}

/// @brief Audio-disabled level query; always reports silence.
/// @copydetails rt_voice_get_level
double rt_voice_get_level(int64_t voice_id) {
    (void)voice_id;
    return 0.0;
}

/// @brief Audio-disabled stub for `Sound.PlayEx2`. Returns `-1` for a null
///        sound; otherwise traps so the absence of audio surfaces clearly.
/// @copydetails rt_sound_play_ex2
int64_t rt_sound_play_ex2(void *sound, int64_t volume, int64_t pan, double pitch) {
    if (!sound)
        return -1;
    (void)volume;
    (void)pan;
    (void)pitch;
    rt_audio_unavailable_("Sound.PlayEx2: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Audio.SetGroupDucking`. Silent no-op.
/// @copydetails rt_audio_set_group_ducking
void rt_audio_set_group_ducking(rt_string trigger_group,
                                rt_string target_group,
                                double amount,
                                double attack_sec,
                                double release_sec) {
    (void)trigger_group;
    (void)target_group;
    (void)amount;
    (void)attack_sec;
    (void)release_sec;
}

/// @brief Audio-disabled stub for `Music.Load`. Returns `NULL` for a
///        null path; otherwise traps because callers need a usable Music
///        handle for the rest of the API.
/// @param path Ignored.
/// @return `NULL` for a null path; otherwise does not return normally.
void *rt_music_load(rt_string path) {
    if (!path)
        return NULL;
    (void)path;
    rt_audio_unavailable_("Music.Load: audio support not compiled in");
    return NULL;
}

/// @brief Audio-disabled stub for `Music.Destroy`. Silent no-op.
/// @param music Ignored.
void rt_music_destroy(void *music) {
    (void)music;
}

/// @brief Audio-disabled handle query; no music wrapper can exist.
/// @copydetails rt_music_is_handle
int64_t rt_music_is_handle(void *music) {
    (void)music;
    return 0;
}

/// @brief Audio-disabled stub for `Music.Play`. No-ops for null music;
///        otherwise traps.
/// @param music Ignored.
/// @param loop  Ignored.
void rt_music_play(void *music, int64_t loop) {
    if (!music)
        return;
    (void)music;
    (void)loop;
    rt_audio_unavailable_("Music.Play: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.Stop`. No-ops for null music;
///        otherwise traps.
/// @param music Ignored.
void rt_music_stop(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Stop: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.Pause`. No-ops for null music;
///        otherwise traps.
/// @param music Ignored.
void rt_music_pause(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Pause: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.Resume`. No-ops for null music;
///        otherwise traps.
/// @param music Ignored.
void rt_music_resume(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Resume: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.SetLoop`. No-ops for null music;
///        otherwise traps.
/// @param music Ignored.
/// @param loop  Ignored.
void rt_music_set_loop(void *music, int64_t loop) {
    if (!music)
        return;
    (void)music;
    (void)loop;
    rt_audio_unavailable_("Music.SetLoop: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.SetVolume`. No-ops for null music;
///        otherwise traps.
/// @param music  Ignored.
/// @param volume Ignored.
void rt_music_set_volume(void *music, int64_t volume) {
    if (!music)
        return;
    (void)music;
    (void)volume;
    rt_audio_unavailable_("Music.SetVolume: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.Volume`. Returns `0` for null
///        music; otherwise traps.
/// @param music Ignored.
/// @return `0` for null music; otherwise does not return normally.
int64_t rt_music_get_volume(void *music) {
    if (!music)
        return 0;
    (void)music;
    rt_audio_unavailable_("Music.GetVolume: audio support not compiled in");
    return 0;
}

/// @brief Audio-disabled stub for `Music.IsPlaying`. Returns `0` for null
///        music; otherwise traps.
/// @param music Ignored.
/// @return `0` for null music; otherwise does not return normally.
int64_t rt_music_is_playing(void *music) {
    if (!music)
        return 0;
    (void)music;
    rt_audio_unavailable_("Music.IsPlaying: audio support not compiled in");
    return 0;
}

/// @brief Audio-disabled stub for `Music.Seek`. No-ops for null music;
///        otherwise traps.
/// @param music       Ignored.
/// @param position_ms Ignored.
void rt_music_seek(void *music, int64_t position_ms) {
    if (!music)
        return;
    (void)music;
    (void)position_ms;
    rt_audio_unavailable_("Music.Seek: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.Position`. Returns `0` for null
///        music; otherwise traps.
/// @param music Ignored.
/// @return `0` for null music; otherwise does not return normally.
int64_t rt_music_get_position(void *music) {
    if (!music)
        return 0;
    (void)music;
    rt_audio_unavailable_("Music.GetPosition: audio support not compiled in");
    return 0;
}

/// @brief Audio-disabled stub for `Music.Duration`. Returns `0` for null
///        music; otherwise traps.
/// @param music Ignored.
/// @return `0` for null music; otherwise does not return normally.
int64_t rt_music_get_duration(void *music) {
    if (!music)
        return 0;
    (void)music;
    rt_audio_unavailable_("Music.GetDuration: audio support not compiled in");
    return 0;
}

/// @brief Audio-disabled related-pause stub.
/// @copydetails rt_music_pause_related
void rt_music_pause_related(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Pause: audio support not compiled in");
}

/// @brief Audio-disabled related-resume stub.
/// @copydetails rt_music_resume_related
void rt_music_resume_related(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Resume: audio support not compiled in");
}

/// @brief Audio-disabled related-stop stub.
/// @copydetails rt_music_stop_related
void rt_music_stop_related(void *music) {
    if (!music)
        return;
    (void)music;
    rt_audio_unavailable_("Music.Stop: audio support not compiled in");
}

/// @brief Audio-disabled crossfade-pair volume stub.
/// @copydetails rt_music_set_crossfade_pair_volume
void rt_music_set_crossfade_pair_volume(void *music, int64_t volume) {
    if (!music)
        return;
    (void)music;
    (void)volume;
    rt_audio_unavailable_("Music.SetVolume: audio support not compiled in");
}

// Mix group stubs — these work without audio (just store state)

/// @brief Set the per-mix-group volume even in audio-disabled builds.
///
/// Group volumes are pure state (no playback dependency), so the
/// disabled build keeps them functional. Game code that reads back the
/// values it sets (e.g. for a settings UI) gets correct round-trips
/// regardless of whether the mixer is actually mixing anything.
///
/// Out-of-range groups are silently ignored; volume is clamped to
/// `0..100`.
///
/// @param group  Mix group index in `0..RT_MIXGROUP_COUNT-1`.
/// @param volume Volume `0..100`.
void rt_audio_set_group_volume(int64_t group, int64_t volume) {
    audio_state_lock();
    audio_groups_init_unlocked();
    if (!audio_group_id_valid_unlocked(group)) {
        audio_state_unlock();
        return;
    }
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
#if RT_COMPILER_MSVC
    rt_atomic_store_i64(&g_group_volume[group], volume, __ATOMIC_RELEASE);
#else
    __atomic_store_n(&g_group_volume[group], volume, __ATOMIC_RELEASE);
#endif
    audio_state_unlock();
}

/// @brief Read the per-mix-group volume in audio-disabled builds.
///
/// Counterpart of `rt_audio_set_group_volume`. Returns the in-memory
/// state, defaulting to `100` (full volume) when the group has not been
/// explicitly set or when the index is out of range.
///
/// @param group Mix group index.
/// @return Stored volume `0..100`, or `100` for out-of-range / unset.
int64_t rt_audio_get_group_volume(int64_t group) {
    audio_state_lock();
    audio_groups_init_unlocked();
    if (!audio_group_id_valid_unlocked(group)) {
        audio_state_unlock();
        return 100;
    }
#if RT_COMPILER_MSVC
    int64_t volume = rt_atomic_load_i64(&g_group_volume[group], __ATOMIC_ACQUIRE);
#else
    int64_t volume = __atomic_load_n(&g_group_volume[group], __ATOMIC_ACQUIRE);
#endif
    audio_state_unlock();
    return volume;
}

/// @brief Register a named mix group in audio-disabled builds.
/// @copydetails rt_audio_register_group
int64_t rt_audio_register_group(rt_string group_name) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_register_group_unlocked(name);
    audio_state_unlock();
    return id;
}

/// @brief Find a named mix group in audio-disabled builds.
/// @copydetails rt_audio_find_group
int64_t rt_audio_find_group(rt_string group_name) {
    char name[32];
    audio_group_copy_name(name, sizeof(name), group_name);
    audio_state_lock();
    int64_t id = audio_find_group_unlocked(name);
    audio_state_unlock();
    return id;
}

/// @brief Find a registered named mix group as an Option id.
/// @details Sentinel-free companion to @ref rt_audio_find_group. Built-in and
///          registered groups return `SomeI64(id)`, while missing groups return
///          None.
/// @param group_name Group name.
/// @return Opaque Zanna.Option containing the group id, or None.
void *rt_audio_find_group_option(rt_string group_name) {
    int64_t id = rt_audio_find_group(group_name);
    return id >= 0 ? rt_option_some_i64(id) : rt_option_none();
}

/// @brief Update a named mix-group volume in audio-disabled builds.
/// @copydetails rt_audio_set_group_volume_named
void rt_audio_set_group_volume_named(rt_string group_name, int64_t volume) {
    int64_t id = rt_audio_register_group(group_name);
    if (id >= 0)
        rt_audio_set_group_volume(id, volume);
}

/// @brief Query a named mix-group volume in audio-disabled builds.
/// @copydetails rt_audio_get_group_volume_named
int64_t rt_audio_get_group_volume_named(rt_string group_name) {
    int64_t id = rt_audio_find_group(group_name);
    return id >= 0 ? rt_audio_get_group_volume(id) : 100;
}

/// @brief Resolve a mix-group name in audio-disabled builds.
/// @copydetails rt_audio_group_name
rt_string rt_audio_group_name(int64_t group_id) {
    audio_state_lock();
    audio_groups_init_unlocked();
    rt_string name = audio_group_id_valid_unlocked(group_id)
                         ? rt_const_cstr(g_group_names[group_id])
                         : rt_str_empty();
    audio_state_unlock();
    return name;
}

/// @brief Audio-disabled stub for `Music.CrossfadeTo`. No-ops when both
///        handles are null; otherwise traps because crossfading is a
///        mixer-level operation that has no fallback without the backend.
/// @param current_music Ignored.
/// @param new_music     Ignored.
/// @param duration_ms   Ignored.
void rt_music_crossfade_to(void *current_music, void *new_music, int64_t duration_ms) {
    if (!current_music && !new_music)
        return;
    (void)current_music;
    (void)new_music;
    (void)duration_ms;
    rt_audio_unavailable_("Music.CrossfadeTo: audio support not compiled in");
}

/// @brief Audio-disabled stub for `Music.IsCrossfading`.
/// @return `0` (never crossfading).
int8_t rt_music_is_crossfading(void) {
    return 0;
}

/// @brief Audio-disabled stub for `Music.CrossfadeUpdate`. Silent no-op.
/// @param dt_ms Ignored.
void rt_music_crossfade_update(int64_t dt_ms) {
    (void)dt_ms;
}

/// @brief Audio-disabled stub for `Sound.PlayGroup`. Returns `-1` for a
///        null sound; otherwise traps.
/// @param sound Ignored.
/// @param group Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play_in_group(void *sound, int64_t group) {
    if (!sound)
        return -1;
    (void)sound;
    (void)group;
    rt_audio_unavailable_("Sound.PlayGroup: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Sound.PlayExGroup` (volume + pan +
///        mix-group routing variant of `Play`). Returns `-1` for a null
///        sound; otherwise traps.
/// @param sound  Ignored.
/// @param volume Ignored.
/// @param pan    Ignored.
/// @param group  Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play_ex_in_group(void *sound, int64_t volume, int64_t pan, int64_t group) {
    if (!sound)
        return -1;
    (void)sound;
    (void)volume;
    (void)pan;
    (void)group;
    rt_audio_unavailable_("Sound.PlayExGroup: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Sound.PlayLoopGroup` (looping
///        variant of `PlayExGroup`). Returns `-1` for a null sound;
///        otherwise traps.
/// @param sound  Ignored.
/// @param volume Ignored.
/// @param pan    Ignored.
/// @param group  Ignored.
/// @return `-1` for a null sound; otherwise does not return normally.
int64_t rt_sound_play_loop_in_group(void *sound, int64_t volume, int64_t pan, int64_t group) {
    if (!sound)
        return -1;
    (void)sound;
    (void)volume;
    (void)pan;
    (void)group;
    rt_audio_unavailable_("Sound.PlayLoopGroup: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled low-pass insertion stub.
/// @copydetails rt_snd_group_add_lowpass
int64_t rt_snd_group_add_lowpass(int64_t group, double cutoff_hz, double q) {
    (void)group;
    (void)cutoff_hz;
    (void)q;
    rt_audio_unavailable_("Audio.GroupAddLowpass: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled high-pass insertion stub.
/// @copydetails rt_snd_group_add_highpass
int64_t rt_snd_group_add_highpass(int64_t group, double cutoff_hz, double q) {
    (void)group;
    (void)cutoff_hz;
    (void)q;
    rt_audio_unavailable_("Audio.GroupAddHighpass: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled peaking-filter insertion stub.
/// @copydetails rt_snd_group_add_peaking
int64_t rt_snd_group_add_peaking(int64_t group, double freq_hz, double q, double gain_db) {
    (void)group;
    (void)freq_hz;
    (void)q;
    (void)gain_db;
    rt_audio_unavailable_("Audio.GroupAddPeaking: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled delay insertion stub.
/// @copydetails rt_snd_group_add_delay
int64_t rt_snd_group_add_delay(int64_t group, double delay_ms, double feedback, double wet) {
    (void)group;
    (void)delay_ms;
    (void)feedback;
    (void)wet;
    rt_audio_unavailable_("Audio.GroupAddDelay: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled reverb insertion stub.
/// @copydetails rt_snd_group_add_reverb
int64_t rt_snd_group_add_reverb(int64_t group, double room_size, double damping, double wet) {
    (void)group;
    (void)room_size;
    (void)damping;
    (void)wet;
    rt_audio_unavailable_("Audio.GroupAddReverb: audio support not compiled in");
    return -1;
}

/// @brief Audio-disabled stub for `Audio.GroupSetReverb`. Silent no-op.
/// @copydetails rt_snd_group_set_reverb
void rt_snd_group_set_reverb(
    int64_t group, int64_t fx_id, double room_size, double damping, double wet) {
    (void)group;
    (void)fx_id;
    (void)room_size;
    (void)damping;
    (void)wet;
    rt_audio_unavailable_("Audio.GroupSetReverb: audio support not compiled in");
}

/// @brief Audio-disabled effect-bypass stub.
/// @copydetails rt_snd_group_fx_bypass
void rt_snd_group_fx_bypass(int64_t group, int64_t fx_id, int8_t bypass) {
    (void)group;
    (void)fx_id;
    (void)bypass;
    rt_audio_unavailable_("Audio.GroupSetFxBypass: audio support not compiled in");
}

/// @brief Audio-disabled effect-removal stub.
/// @copydetails rt_snd_group_remove_fx
void rt_snd_group_remove_fx(int64_t group, int64_t fx_id) {
    (void)group;
    (void)fx_id;
    rt_audio_unavailable_("Audio.GroupRemoveFx: audio support not compiled in");
}

/// @brief Audio-disabled effect-chain clearing stub.
/// @copydetails rt_snd_group_clear_fx
void rt_snd_group_clear_fx(int64_t group) {
    (void)group;
    rt_audio_unavailable_("Audio.GroupClearFx: audio support not compiled in");
}

#endif /* ZANNA_ENABLE_AUDIO */
