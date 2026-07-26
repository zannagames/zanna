//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD Core Implementation
//
// Platform-agnostic implementation of the ZannaAUD API. Provides audio context
// management, sound/music loading, and playback control. Platform-specific
// functionality is delegated to backend implementations.
//
// Key responsibilities:
// - Context lifecycle (create, destroy)
// - Sound effect loading and management
// - Music stream loading and management
// - Playback control (play, stop, volume, pan)
// - Thread synchronization for audio state
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Core implementation of the ZannaAUD API.
/// @details Owns platform-independent context, handle, voice, and streaming
///          state. Control paths synchronize with the realtime mixer and keep
///          file I/O and codec decoding outside render callbacks.

#if !defined(VAUD_PLATFORM_WINDOWS) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#if !defined(VAUD_PLATFORM_WINDOWS) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "vaud_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(VAUD_PLATFORM_WINDOWS)
#include <time.h>
#endif

/// @brief Maximum number of music streams serviced by one vaud_update() call.
/// @details Keeping the control-thread refill budget finite avoids a single
///          update tick monopolizing the caller when many streams need disk or
///          decoder work at once. Remaining streams are serviced by later calls.
#define VAUD_UPDATE_MAX_REFILLS_PER_CALL 4

/// @brief Duplicate a null-terminated string into heap storage.
/// @param s Source string, or NULL.
/// @return Allocated copy owned by the caller, or NULL on invalid input/failure.
static char *vaud_strdup(const char *s) {
    if (!s)
        return NULL;

    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (!copy)
        return NULL;

    memcpy(copy, s, len);
    return copy;
}

/// @brief Compute a safe resampled frame count and publish format errors.
/// @param in_frames Source frame count.
/// @param in_rate Positive source sample rate.
/// @param out_rate Positive destination sample rate.
/// @param out_frames Output frame count set on success.
/// @return 1 for a positive representable result; otherwise 0.
static int vaud_checked_resampled_frames(int64_t in_frames,
                                         int32_t in_rate,
                                         int32_t out_rate,
                                         int64_t *out_frames) {
    if (!out_frames)
        return 0;
    int64_t frames = vaud_resample_output_frames(in_frames, in_rate, out_rate);
    if (frames <= 0 || frames == INT64_MAX) {
        vaud_set_error(VAUD_ERR_FORMAT, "Resampled audio frame count is too large");
        return 0;
    }
    *out_frames = frames;
    return 1;
}

/// @brief Allocate an interleaved signed-16 PCM buffer with overflow checking.
/// @param frames Number of sample frames.
/// @param channels Interleaved channel count.
/// @return Heap buffer owned by the caller, or NULL after setting an error.
static int16_t *vaud_alloc_pcm_frames(int64_t frames, int32_t channels) {
    size_t bytes = 0;
    if (!vaud_pcm_s16_buffer_size(frames, channels, &bytes)) {
        vaud_set_error(VAUD_ERR_FORMAT, "PCM buffer size is too large");
        return NULL;
    }
    int16_t *samples = (int16_t *)malloc(bytes);
    if (!samples)
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate PCM buffer");
    return samples;
}

/// @brief Normalize a scalar to the inclusive unit interval.
/// @param value Candidate value.
/// @return Finite value clamped to [0, 1], or 0 for NaN/infinity.
static float vaud_clamp_unit_float(float value) {
    if (!isfinite(value))
        return 0.0f;
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

/// @brief Normalize a stereo pan value.
/// @param value Candidate pan.
/// @return Finite value clamped to [-1, 1], or center for NaN/infinity.
static float vaud_clamp_pan_float(float value) {
    if (!isfinite(value))
        return 0.0f;
    if (value < -1.0f)
        return -1.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

/// @brief Yield a control thread for approximately one millisecond.
static void vaud_control_sleep_1ms(void) {
#if defined(VAUD_PLATFORM_WINDOWS)
    Sleep(1);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/// @brief Test whether a context is absent or teardown has begun.
/// @param ctx Audio context to inspect.
/// @return Nonzero for NULL or a context with its destroying flag set.
static int vaud_context_is_destroying(vaud_context_t ctx) {
    return !ctx || vaud_atomic_load_i32(&ctx->destroying) != 0;
}

/// @brief Remove a sound from a context's compact loaded-sound registry.
/// @details The caller must hold @p ctx's mutex. Removal swaps in the final
///          element and preserves no ordering.
/// @param ctx Owning context.
/// @param sound Sound handle to remove.
static void sound_unregister_locked(vaud_context_t ctx, vaud_sound_t sound) {
    if (!ctx || !sound)
        return;

    for (int32_t i = 0; i < ctx->sound_count; i++) {
        if (ctx->loaded_sounds[i] != sound)
            continue;
        int32_t last = ctx->sound_count - 1;
        ctx->loaded_sounds[i] = ctx->loaded_sounds[last];
        ctx->loaded_sounds[last] = NULL;
        ctx->sound_count--;
        return;
    }
}

/// @brief Test whether a sound is still attached to the expected context.
/// @details Callers must hold @p ctx->mutex.  Keeping this check in one helper
///          documents the synchronization contract used by play calls racing
///          against detach/free operations.
/// @param sound Sound handle being played.
/// @param ctx Context whose mutex is currently held.
/// @return Non-zero when @p sound still belongs to @p ctx.
static int sound_is_attached_to_context_locked(vaud_sound_t sound, vaud_context_t ctx) {
    return sound && ctx && sound->ctx == ctx;
}

/// @brief Register a newly loaded sound with its context.
/// @details Locks the context, appends within VAUD_MAX_SOUNDS, and publishes an
///          error when the registry is full.
/// @param ctx Owning context.
/// @param sound Fully initialized sound handle.
/// @return 1 when registered; otherwise 0.
static int sound_register(vaud_context_t ctx, vaud_sound_t sound) {
    if (!ctx || !sound)
        return 0;

    vaud_mutex_lock(&ctx->mutex);
    if (ctx->sound_count < VAUD_MAX_SOUNDS) {
        ctx->loaded_sounds[ctx->sound_count++] = sound;
        vaud_mutex_unlock(&ctx->mutex);
        return 1;
    }
    vaud_mutex_unlock(&ctx->mutex);

    vaud_set_error(VAUD_ERR_INVALID_PARAM, "Maximum loaded sounds reached");
    return 0;
}

// Forward declarations for runtime codec APIs (avoids cross-layer #include)
typedef struct ogg_reader ogg_reader_t;
typedef struct vorbis_decoder vorbis_decoder_t;
typedef struct mp3_stream mp3_stream_t;

typedef struct {
    uint32_t serial_number;
    int64_t granule_position;
    uint8_t bos;
    uint8_t eos;
} ogg_packet_info_t;

/// @brief Open a forward-only OGG packet reader over a file.
/// @param path Path of the OGG container to open.
/// @return Owned reader handle, or NULL when the file cannot be opened or initialized.
ogg_reader_t *ogg_reader_open_file(const char *path);

/// @brief Close an OGG reader and release its packet and stream state.
/// @param r Reader returned by ogg_reader_open_file().
void ogg_reader_free(ogg_reader_t *r);

/// @brief Rewind a file-backed OGG reader and discard pending packet state.
/// @param r Reader to reset to the beginning of its container.
void ogg_reader_rewind(ogg_reader_t *r);

/// @brief Read the next complete OGG packet together with logical-stream metadata.
/// @details The returned packet storage remains owned by @p r and is valid only
///          until the next packet-read call on that reader.
/// @param r Reader whose interleaved logical streams are advanced.
/// @param out_data Receives the borrowed packet byte pointer.
/// @param out_len Receives the packet length in bytes.
/// @param out_info Receives serial, granule-position, and boundary metadata.
/// @return 1 when a packet was produced; 0 at end of input or on an error.
int ogg_reader_next_packet_ex(ogg_reader_t *r,
                              const uint8_t **out_data,
                              size_t *out_len,
                              ogg_packet_info_t *out_info);

/// @brief Allocate an empty incremental Vorbis decoder.
/// @return Owned decoder handle, or NULL on allocation failure.
vorbis_decoder_t *vorbis_decoder_new(void);

/// @brief Release a Vorbis decoder and all codec-owned buffers.
/// @param dec Decoder returned by vorbis_decoder_new().
void vorbis_decoder_free(vorbis_decoder_t *dec);

/// @brief Submit one of the three ordered Vorbis setup packets.
/// @param dec Decoder being initialized.
/// @param data Header-packet bytes.
/// @param len Header-packet length in bytes.
/// @param num Zero-based header index: identification, comment, then setup.
/// @return 0 on success, or -1 when the packet is invalid.
int vorbis_decode_header(vorbis_decoder_t *dec, const uint8_t *data, size_t len, int num);

/// @brief Decode one Vorbis audio packet to borrowed interleaved signed-16 PCM.
/// @param dec Initialized decoder.
/// @param data Audio-packet bytes.
/// @param len Audio-packet length in bytes.
/// @param out_pcm Receives codec-owned PCM valid until the next decode call.
/// @param out_samples Receives the decoded sample-frame count per channel.
/// @return 0 on success, or -1 when the packet cannot be decoded.
int vorbis_decode_packet(
    vorbis_decoder_t *dec, const uint8_t *data, size_t len, int16_t **out_pcm, int *out_samples);

/// @brief Read the sample rate parsed from the Vorbis identification header.
/// @param dec Initialized decoder.
/// @return Source samples per second.
int vorbis_get_sample_rate(const vorbis_decoder_t *dec);

/// @brief Read the channel count parsed from the Vorbis identification header.
/// @param dec Initialized decoder.
/// @return Number of interleaved source channels.
int vorbis_get_channels(const vorbis_decoder_t *dec);

/// @brief Open an MP3 file for incremental frame decoding.
/// @param filepath Path of the MP3 file.
/// @return Owned stream handle, or NULL on open, parse, or allocation failure.
mp3_stream_t *mp3_stream_open(const char *filepath);

/// @brief Decode the next MP3 frame to stream-owned signed-16 PCM.
/// @param stream Streaming decoder to advance.
/// @param out_pcm Receives an interleaved buffer valid until the next decode call.
/// @return Sample frames per channel, 0 at end of input, or -1 on decode failure.
int mp3_stream_decode_frame(mp3_stream_t *stream, int16_t **out_pcm);

/// @brief Read an MP3 stream's source sample rate.
/// @param stream Open stream.
/// @return Source samples per second.
int mp3_stream_sample_rate(const mp3_stream_t *stream);

/// @brief Read an MP3 stream's decoded channel count.
/// @param stream Open stream.
/// @return Number of interleaved channels.
int mp3_stream_channels(const mp3_stream_t *stream);

/// @brief Read the stream's estimated total decoded frame count.
/// @param stream Open stream.
/// @return Total samples per channel, or a non-positive value when unknown.
int mp3_stream_total_samples(const mp3_stream_t *stream);

/// @brief Reset an MP3 stream to its first audio frame.
/// @param stream Stream prepared for looped playback or an absolute seek.
void mp3_stream_rewind(mp3_stream_t *stream);

/// @brief Release an MP3 stream and its compressed and decoded buffers.
/// @param stream Stream returned by mp3_stream_open().
void mp3_stream_free(mp3_stream_t *stream);

//===----------------------------------------------------------------------===//
// Thread-Local Error State
//===----------------------------------------------------------------------===//

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Thread_local static const char *g_last_error = NULL;
_Thread_local static vaud_error_t g_last_error_code = VAUD_OK;
#elif defined(_WIN32)
__declspec(thread) static const char *g_last_error = NULL;
__declspec(thread) static vaud_error_t g_last_error_code = VAUD_OK;
#elif defined(__GNUC__) || defined(__clang__)
__thread static const char *g_last_error = NULL;
__thread static vaud_error_t g_last_error_code = VAUD_OK;
#else
static const char *g_last_error = NULL;
static vaud_error_t g_last_error_code = VAUD_OK;
#endif

/// @copydoc vaud_set_error
void vaud_set_error(vaud_error_t code, const char *msg) {
    g_last_error_code = code;
    g_last_error = msg;
}

/// @copydoc vaud_get_last_error
const char *vaud_get_last_error(void) {
    return g_last_error;
}

/// @copydoc vaud_clear_error
void vaud_clear_error(void) {
    g_last_error = NULL;
    g_last_error_code = VAUD_OK;
}

//===----------------------------------------------------------------------===//
// Threading Utilities
//===----------------------------------------------------------------------===//

#if defined(VAUD_PLATFORM_WINDOWS)

/// @copydoc vaud_mutex_init
int vaud_mutex_init(vaud_mutex_t *mutex) {
    return mutex && InitializeCriticalSectionAndSpinCount(mutex, 4000) ? 1 : 0;
}

/// @copydoc vaud_mutex_destroy
void vaud_mutex_destroy(vaud_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
}

/// @copydoc vaud_mutex_lock
void vaud_mutex_lock(vaud_mutex_t *mutex) {
    EnterCriticalSection(mutex);
}

/// @copydoc vaud_mutex_unlock
void vaud_mutex_unlock(vaud_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
}

/// @copydoc vaud_mutex_trylock
int vaud_mutex_trylock(vaud_mutex_t *mutex) {
    return TryEnterCriticalSection(mutex) ? 1 : 0;
}

#else /* POSIX */

/// @copydoc vaud_mutex_init
int vaud_mutex_init(vaud_mutex_t *mutex) {
    return mutex && pthread_mutex_init(mutex, NULL) == 0;
}

/// @copydoc vaud_mutex_destroy
void vaud_mutex_destroy(vaud_mutex_t *mutex) {
    pthread_mutex_destroy(mutex);
}

/// @copydoc vaud_mutex_lock
void vaud_mutex_lock(vaud_mutex_t *mutex) {
    pthread_mutex_lock(mutex);
}

/// @copydoc vaud_mutex_unlock
void vaud_mutex_unlock(vaud_mutex_t *mutex) {
    pthread_mutex_unlock(mutex);
}

/// @copydoc vaud_mutex_trylock
int vaud_mutex_trylock(vaud_mutex_t *mutex) {
    return pthread_mutex_trylock(mutex) == 0;
}

#endif

/// @brief Initialize a manual-reset event used by the music refill coordinator.
/// @details The event starts signaled because newly allocated music has no
///          refill in progress. Refill begin paths reset it while holding the
///          context mutex, and refill completion paths signal it after clearing
///          the in-progress flag.
/// @param event Event object to initialize.
/// @return Non-zero on success, zero if the platform object could not be created.
static int vaud_event_init(vaud_event_t *event) {
    if (!event)
        return 0;
#if defined(VAUD_PLATFORM_WINDOWS)
    *event = CreateEventW(NULL, TRUE, TRUE, NULL);
    return *event != NULL;
#else
    if (pthread_mutex_init(&event->mutex, NULL) != 0)
        return 0;
    if (pthread_cond_init(&event->cond, NULL) != 0) {
        pthread_mutex_destroy(&event->mutex);
        return 0;
    }
    event->signaled = 1;
    return 1;
#endif
}

/// @brief Destroy a refill event after all possible waiters have been released.
/// @param event Event object previously initialized by vaud_event_init().
static void vaud_event_destroy(vaud_event_t *event) {
    if (!event)
        return;
#if defined(VAUD_PLATFORM_WINDOWS)
    if (*event) {
        CloseHandle(*event);
        *event = NULL;
    }
#else
    pthread_cond_destroy(&event->cond);
    pthread_mutex_destroy(&event->mutex);
#endif
}

/// @brief Mark a refill event as unsignaled before non-realtime refill work starts.
/// @param event Event object previously initialized by vaud_event_init().
static void vaud_event_reset(vaud_event_t *event) {
    if (!event)
        return;
#if defined(VAUD_PLATFORM_WINDOWS)
    if (*event)
        ResetEvent(*event);
#else
    pthread_mutex_lock(&event->mutex);
    event->signaled = 0;
    pthread_mutex_unlock(&event->mutex);
#endif
}

/// @brief Wake every waiter blocked on refill completion.
/// @param event Event object previously initialized by vaud_event_init().
static void vaud_event_set(vaud_event_t *event) {
    if (!event)
        return;
#if defined(VAUD_PLATFORM_WINDOWS)
    if (*event)
        SetEvent(*event);
#else
    pthread_mutex_lock(&event->mutex);
    event->signaled = 1;
    pthread_cond_broadcast(&event->cond);
    pthread_mutex_unlock(&event->mutex);
#endif
}

/// @brief Wait until the refill event is signaled.
/// @details This is only used by non-realtime control paths. The mixer callback
///          must never block on this helper.
/// @param event Event object previously initialized by vaud_event_init().
static void vaud_event_wait(vaud_event_t *event) {
    if (!event)
        return;
#if defined(VAUD_PLATFORM_WINDOWS)
    if (*event)
        WaitForSingleObject(*event, INFINITE);
#else
    pthread_mutex_lock(&event->mutex);
    while (!event->signaled)
        pthread_cond_wait(&event->cond, &event->mutex);
    pthread_mutex_unlock(&event->mutex);
#endif
}

//===----------------------------------------------------------------------===//
// Version Functions
//===----------------------------------------------------------------------===//

#define VAUD_STR_IMPL(value) #value
#define VAUD_STR(value) VAUD_STR_IMPL(value)

/// @copydoc vaud_version
uint32_t vaud_version(void) {
    return (VAUD_VERSION_MAJOR << 16) | (VAUD_VERSION_MINOR << 8) | VAUD_VERSION_PATCH;
}

/// @copydoc vaud_version_string
const char *vaud_version_string(void) {
    return VAUD_STR(VAUD_VERSION_MAJOR) "." VAUD_STR(VAUD_VERSION_MINOR) "." VAUD_STR(
        VAUD_VERSION_PATCH);
}

//===----------------------------------------------------------------------===//
// Context Management
//===----------------------------------------------------------------------===//

/// @copydoc vaud_create
vaud_context_t vaud_create(void) {
    vaud_context_t ctx = (vaud_context_t)calloc(1, sizeof(struct vaud_context));
    if (!ctx) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate audio context");
        return NULL;
    }

    /* Initialize state */
    ctx->master_volume = VAUD_DEFAULT_MASTER_VOLUME;
    {
        const char *silent = getenv("ZANNA_AUDIO_SILENT");
        ctx->device_output_silent =
            silent && silent[0] != '\0' && !(silent[0] == '0' && silent[1] == '\0');
    }
    ctx->next_voice_id = 1; /* Start at 1 so 0 is never valid */
    ctx->frame_counter = 0;
    vaud_atomic_store_i32(&ctx->running, 1);
    vaud_atomic_store_i32(&ctx->destroying, 0);
    ctx->paused = 0;
    ctx->music_count = 0;

    /* Initialize all voices as inactive */
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        ctx->voices[i].state = VAUD_VOICE_INACTIVE;
        ctx->voices[i].sound = NULL;
        ctx->voices[i].id = VAUD_INVALID_VOICE;
        ctx->voices[i].group_id = 0;
    }

    /* Initialize music slots */
    for (int32_t i = 0; i < VAUD_MAX_MUSIC; i++) {
        ctx->active_music[i] = NULL;
    }

    /* Initialize mutex */
    if (!vaud_mutex_init(&ctx->mutex)) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to initialize audio mutex");
        free(ctx);
        return NULL;
    }

    /* Initialize platform backend */
    if (!vaud_platform_init(ctx)) {
        vaud_mutex_destroy(&ctx->mutex);
        free(ctx);
        return NULL;
    }

    return ctx;
}

/// @copydoc vaud_destroy
void vaud_destroy(vaud_context_t ctx) {
    if (!ctx)
        return;

    /* Stop running flag first */
    vaud_atomic_store_i32(&ctx->destroying, 1);
    vaud_atomic_store_i32(&ctx->running, 0);

    /* Shutdown platform (stops audio thread) */
    if (!vaud_platform_shutdown(ctx)) {
        vaud_atomic_store_i32(&ctx->destroying, 0);
        return;
    }

    /* Detach caller-owned sounds and stop voices that reference them. */
    vaud_mutex_lock(&ctx->mutex);
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        ctx->voices[i].state = VAUD_VOICE_INACTIVE;
        ctx->voices[i].sound = NULL;
    }
    for (int32_t i = 0; i < ctx->sound_count; i++) {
        if (ctx->loaded_sounds[i])
            ctx->loaded_sounds[i]->ctx = NULL;
        ctx->loaded_sounds[i] = NULL;
    }
    ctx->sound_count = 0;

    /* Detach all music streams so wrapper-owned finalizers can free them later
     * without touching a destroyed context. */
    for (int32_t i = 0; i < ctx->music_count; i++) {
        while (ctx->active_music[i] && ctx->active_music[i]->refill_in_progress) {
            vaud_music_t music = ctx->active_music[i];
            vaud_mutex_unlock(&ctx->mutex);
            if (music->refill_event_ready)
                vaud_event_wait(&music->refill_event);
            else
                vaud_control_sleep_1ms();
            vaud_mutex_lock(&ctx->mutex);
        }
        if (ctx->active_music[i]) {
            ctx->active_music[i]->state = VAUD_MUSIC_STOPPED;
            if (ctx->active_music[i]->refill_event_ready)
                vaud_event_set(&ctx->active_music[i]->refill_event);
            ctx->active_music[i]->ctx = NULL;
            ctx->active_music[i] = NULL;
        }
    }
    ctx->music_count = 0;
    vaud_mutex_unlock(&ctx->mutex);

    vaud_mutex_destroy(&ctx->mutex);
    free(ctx);
}

/// @copydoc vaud_set_master_volume
void vaud_set_master_volume(vaud_context_t ctx, float volume) {
    if (vaud_context_is_destroying(ctx))
        return;
    volume = vaud_clamp_unit_float(volume);

    vaud_mutex_lock(&ctx->mutex);
    ctx->master_volume = volume;
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_get_master_volume
float vaud_get_master_volume(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return 0.0f;
    /* H-3: setter holds mutex; reader must too (torn read on ARM64 otherwise) */
    vaud_mutex_lock(&ctx->mutex);
    float vol = ctx->master_volume;
    vaud_mutex_unlock(&ctx->mutex);
    return vol;
}

/// @copydoc vaud_pause_all
void vaud_pause_all(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return;

    vaud_mutex_lock(&ctx->mutex);
    ctx->paused = 1;
    vaud_mutex_unlock(&ctx->mutex);

    vaud_platform_pause(ctx);
}

/// @copydoc vaud_resume_all
void vaud_resume_all(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return;

    vaud_mutex_lock(&ctx->mutex);
    ctx->paused = 0;
    vaud_mutex_unlock(&ctx->mutex);

    vaud_platform_resume(ctx);
}

//===----------------------------------------------------------------------===//
// Sound Effect Loading
//===----------------------------------------------------------------------===//

/// @copydoc vaud_load_sound
vaud_sound_t vaud_load_sound(vaud_context_t ctx, const char *path) {
    if (vaud_context_is_destroying(ctx) || !path) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL context or path");
        return NULL;
    }

    int16_t *samples = NULL;
    int64_t frames = 0;
    int32_t sample_rate = 0;
    int32_t channels = 0;

    if (!vaud_wav_load_file(path, &samples, &frames, &sample_rate, &channels)) {
        return NULL;
    }

    /* Resample if necessary */
    int16_t *final_samples = samples;
    int64_t final_frames = frames;

    if (sample_rate != VAUD_SAMPLE_RATE) {
        if (!vaud_checked_resampled_frames(frames, sample_rate, VAUD_SAMPLE_RATE, &final_frames)) {
            free(samples);
            return NULL;
        }
        final_samples = vaud_alloc_pcm_frames(final_frames, VAUD_CHANNELS);

        if (!final_samples) {
            free(samples);
            return NULL;
        }

        vaud_resample(
            samples, frames, sample_rate, final_samples, final_frames, VAUD_SAMPLE_RATE, 2);
        free(samples);
    }

    /* Allocate sound structure */
    vaud_sound_t sound = (vaud_sound_t)malloc(sizeof(struct vaud_sound));
    if (!sound) {
        free(final_samples);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate sound structure");
        return NULL;
    }

    sound->ctx = ctx;
    sound->samples = final_samples;
    sound->frame_count = final_frames;
    sound->sample_rate = VAUD_SAMPLE_RATE;
    sound->channels = VAUD_CHANNELS;
    sound->source_channels = channels;
    sound->default_volume = VAUD_DEFAULT_SOUND_VOLUME;

    if (!sound_register(ctx, sound)) {
        free(sound->samples);
        free(sound);
        return NULL;
    }

    return sound;
}

/// @copydoc vaud_load_sound_mem
vaud_sound_t vaud_load_sound_mem(vaud_context_t ctx, const void *data, size_t size) {
    if (vaud_context_is_destroying(ctx) || !data || size == 0) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL context or data");
        return NULL;
    }

    int16_t *samples = NULL;
    int64_t frames = 0;
    int32_t sample_rate = 0;
    int32_t channels = 0;

    if (!vaud_wav_load_mem(data, size, &samples, &frames, &sample_rate, &channels)) {
        return NULL;
    }

    /* Resample if necessary */
    int16_t *final_samples = samples;
    int64_t final_frames = frames;

    if (sample_rate != VAUD_SAMPLE_RATE) {
        if (!vaud_checked_resampled_frames(frames, sample_rate, VAUD_SAMPLE_RATE, &final_frames)) {
            free(samples);
            return NULL;
        }
        final_samples = vaud_alloc_pcm_frames(final_frames, VAUD_CHANNELS);

        if (!final_samples) {
            free(samples);
            return NULL;
        }

        vaud_resample(
            samples, frames, sample_rate, final_samples, final_frames, VAUD_SAMPLE_RATE, 2);
        free(samples);
    }

    /* Allocate sound structure */
    vaud_sound_t sound = (vaud_sound_t)malloc(sizeof(struct vaud_sound));
    if (!sound) {
        free(final_samples);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate sound structure");
        return NULL;
    }

    sound->ctx = ctx;
    sound->samples = final_samples;
    sound->frame_count = final_frames;
    sound->sample_rate = VAUD_SAMPLE_RATE;
    sound->channels = VAUD_CHANNELS;
    sound->source_channels = channels;
    sound->default_volume = VAUD_DEFAULT_SOUND_VOLUME;

    if (!sound_register(ctx, sound)) {
        free(sound->samples);
        free(sound);
        return NULL;
    }

    return sound;
}

/// @copydoc vaud_free_sound
void vaud_free_sound(vaud_sound_t sound) {
    if (!sound)
        return;

    vaud_context_t ctx = sound->ctx;

    /* Stop any voices playing this sound */
    if (ctx) {
        vaud_mutex_lock(&ctx->mutex);
        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
            if (ctx->voices[i].sound == sound) {
                ctx->voices[i].state = VAUD_VOICE_INACTIVE;
                ctx->voices[i].sound = NULL;
            }
        }
        sound_unregister_locked(ctx, sound);
        sound->ctx = NULL;
        vaud_mutex_unlock(&ctx->mutex);
    }

    free(sound->samples);
    free(sound);
}

/// @copydoc vaud_detach_sound
void vaud_detach_sound(vaud_sound_t sound) {
    if (!sound)
        return;

    vaud_context_t ctx = sound->ctx;
    if (ctx) {
        vaud_mutex_lock(&ctx->mutex);
        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
            if (ctx->voices[i].sound == sound) {
                ctx->voices[i].state = VAUD_VOICE_INACTIVE;
                ctx->voices[i].sound = NULL;
            }
        }
        sound_unregister_locked(ctx, sound);
        sound->ctx = NULL;
        vaud_mutex_unlock(&ctx->mutex);
    }
}

/// @copydoc vaud_sound_is_attached
int vaud_sound_is_attached(vaud_sound_t sound) {
    return (sound && sound->ctx) ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Sound Effect Playback
//===----------------------------------------------------------------------===//

/// @copydoc vaud_play
vaud_voice_id vaud_play(vaud_sound_t sound) {
    return vaud_play_ex(sound, VAUD_DEFAULT_SOUND_VOLUME, VAUD_DEFAULT_PAN);
}

/// @copydoc vaud_play_ex
vaud_voice_id vaud_play_ex(vaud_sound_t sound, float volume, float pan) {
    return vaud_play_ex_group(sound, volume, pan, 0);
}

/// @copydoc vaud_play_ex_group
vaud_voice_id vaud_play_ex_group(vaud_sound_t sound, float volume, float pan, int64_t group_id) {
    if (!sound)
        return VAUD_INVALID_VOICE;

    vaud_context_t ctx = sound->ctx;
    if (!ctx)
        return VAUD_INVALID_VOICE;

    vaud_mutex_lock(&ctx->mutex);
    if (!sound_is_attached_to_context_locked(sound, ctx) ||
        vaud_atomic_load_i32(&ctx->destroying) != 0) {
        vaud_mutex_unlock(&ctx->mutex);
        return VAUD_INVALID_VOICE;
    }

    vaud_voice *voice = vaud_alloc_voice(ctx);
    if (!voice) {
        vaud_mutex_unlock(&ctx->mutex);
        return VAUD_INVALID_VOICE;
    }

    voice->sound = sound;
    voice->position = 0;
    voice->volume = vaud_clamp_unit_float(volume);
    voice->pan = vaud_clamp_pan_float(pan);
    voice->loop = 0;
    voice->group_id = group_id;
    vaud_voice_reset_dsp(voice);
    voice->state = VAUD_VOICE_PLAYING;

    vaud_voice_id id = voice->id;

    vaud_mutex_unlock(&ctx->mutex);

    return id;
}

/// @copydoc vaud_play_ex2
vaud_voice_id vaud_play_ex2(vaud_sound_t sound, float volume, float pan, float pitch) {
    vaud_voice_id id = vaud_play_ex(sound, volume, pan);
    if (id != VAUD_INVALID_VOICE && sound && sound->ctx)
        vaud_set_voice_pitch(sound->ctx, id, pitch);
    return id;
}

/// @copydoc vaud_play_loop
vaud_voice_id vaud_play_loop(vaud_sound_t sound, float volume, float pan) {
    return vaud_play_loop_group(sound, volume, pan, 0);
}

/// @copydoc vaud_play_loop_group
vaud_voice_id vaud_play_loop_group(vaud_sound_t sound, float volume, float pan, int64_t group_id) {
    if (!sound)
        return VAUD_INVALID_VOICE;

    vaud_context_t ctx = sound->ctx;
    if (!ctx)
        return VAUD_INVALID_VOICE;

    vaud_mutex_lock(&ctx->mutex);
    if (!sound_is_attached_to_context_locked(sound, ctx) ||
        vaud_atomic_load_i32(&ctx->destroying) != 0) {
        vaud_mutex_unlock(&ctx->mutex);
        return VAUD_INVALID_VOICE;
    }

    vaud_voice *voice = vaud_alloc_voice(ctx);
    if (!voice) {
        vaud_mutex_unlock(&ctx->mutex);
        return VAUD_INVALID_VOICE;
    }

    voice->sound = sound;
    voice->position = 0;
    voice->volume = vaud_clamp_unit_float(volume);
    voice->pan = vaud_clamp_pan_float(pan);
    voice->loop = 1;
    voice->group_id = group_id;
    vaud_voice_reset_dsp(voice);
    voice->state = VAUD_VOICE_PLAYING;

    vaud_voice_id id = voice->id;

    vaud_mutex_unlock(&ctx->mutex);

    return id;
}

/// @copydoc vaud_stop_voice
void vaud_stop_voice(vaud_context_t ctx, vaud_voice_id voice_id) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        voice->state = VAUD_VOICE_INACTIVE;
        voice->sound = NULL;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_set_voice_volume
void vaud_set_voice_volume(vaud_context_t ctx, vaud_voice_id voice_id, float volume) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        voice->volume = vaud_clamp_unit_float(volume);
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_set_voice_pan
void vaud_set_voice_pan(vaud_context_t ctx, vaud_voice_id voice_id, float pan) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        voice->pan = vaud_clamp_pan_float(pan);
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_set_voice_group
void vaud_set_voice_group(vaud_context_t ctx, vaud_voice_id voice_id, int64_t group_id) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice)
        voice->group_id = group_id;
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_voice_is_playing
int vaud_voice_is_playing(vaud_context_t ctx, vaud_voice_id voice_id) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return 0;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    int playing = (voice && voice->state == VAUD_VOICE_PLAYING) ? 1 : 0;
    vaud_mutex_unlock(&ctx->mutex);

    return playing;
}

/// @copydoc vaud_set_voice_pitch
void vaud_set_voice_pitch(vaud_context_t ctx, vaud_voice_id voice_id, float pitch) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        if (!(pitch > 0.0f)) /* also rejects NaN */
            pitch = 1.0f;
        if (pitch < VAUD_PITCH_MIN)
            pitch = VAUD_PITCH_MIN;
        if (pitch > VAUD_PITCH_MAX)
            pitch = VAUD_PITCH_MAX;
        /* Adopt the integer cursor as the fractional cursor when the voice
         * transitions from the fast path so playback continues seamlessly. */
        if (voice->pitch == 1.0f && pitch != 1.0f)
            voice->frac_pos = (double)voice->position;
        voice->pitch = pitch;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_get_voice_pitch
float vaud_get_voice_pitch(vaud_context_t ctx, vaud_voice_id voice_id) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return 1.0f;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    float pitch = voice ? voice->pitch : 1.0f;
    vaud_mutex_unlock(&ctx->mutex);
    return pitch;
}

/// @copydoc vaud_set_voice_lowpass
void vaud_set_voice_lowpass(vaud_context_t ctx, vaud_voice_id voice_id, float cutoff_hz) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        if (!(cutoff_hz > 0.0f)) /* <= 0 or NaN disables the filter */
            cutoff_hz = 0.0f;
        voice->lowpass_cutoff = cutoff_hz;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_set_voice_occlusion
void vaud_set_voice_occlusion(vaud_context_t ctx, vaud_voice_id voice_id, float amount) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;

    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        if (!(amount > 0.0f))
            amount = 0.0f;
        if (amount > 1.0f)
            amount = 1.0f;
        voice->occlusion_target = amount;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_set_voice_metering
void vaud_set_voice_metering(vaud_context_t ctx, vaud_voice_id voice_id, int enabled) {
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return;
    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice) {
        voice->metering = enabled ? 1 : 0;
        if (!enabled)
            voice->level = 0.0f;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_get_voice_level
float vaud_get_voice_level(vaud_context_t ctx, vaud_voice_id voice_id) {
    float level = 0.0f;
    if (vaud_context_is_destroying(ctx) || voice_id == VAUD_INVALID_VOICE)
        return 0.0f;
    vaud_mutex_lock(&ctx->mutex);
    vaud_voice *voice = vaud_find_voice(ctx, voice_id);
    if (voice && voice->metering)
        level = voice->level;
    vaud_mutex_unlock(&ctx->mutex);
    return level;
}

/// @copydoc vaud_set_group_duck
void vaud_set_group_duck(vaud_context_t ctx,
                         int64_t trigger_group,
                         int64_t target_group,
                         float amount,
                         float attack_sec,
                         float release_sec) {
    if (vaud_context_is_destroying(ctx))
        return;

    vaud_mutex_lock(&ctx->mutex);

    /* Re-registering a (trigger, target) pair replaces the rule; amount <= 0
     * removes it. */
    int32_t found = -1;
    for (int32_t i = 0; i < ctx->duck_rule_count; i++) {
        if (ctx->duck_rules[i].trigger_group == trigger_group &&
            ctx->duck_rules[i].target_group == target_group) {
            found = i;
            break;
        }
    }

    if (!(amount > 0.0f)) {
        if (found >= 0) {
            ctx->duck_rules[found] = ctx->duck_rules[ctx->duck_rule_count - 1];
            ctx->duck_rule_count--;
        }
        vaud_mutex_unlock(&ctx->mutex);
        return;
    }

    if (amount > 1.0f)
        amount = 1.0f;
    if (!(attack_sec > 0.0f))
        attack_sec = 0.001f;
    if (!(release_sec > 0.0f))
        release_sec = 0.001f;

    if (found < 0) {
        if (ctx->duck_rule_count >= VAUD_MAX_DUCK_RULES) {
            vaud_mutex_unlock(&ctx->mutex);
            return;
        }
        found = ctx->duck_rule_count++;
        ctx->duck_rules[found].gain = 1.0f;
    }
    ctx->duck_rules[found].trigger_group = trigger_group;
    ctx->duck_rules[found].target_group = target_group;
    ctx->duck_rules[found].amount = amount;
    ctx->duck_rules[found].attack_sec = attack_sec;
    ctx->duck_rules[found].release_sec = release_sec;

    vaud_mutex_unlock(&ctx->mutex);
}

//===----------------------------------------------------------------------===//
// Compressed stream read helpers
//===----------------------------------------------------------------------===//

/// @brief Recognize one Vorbis identification/comment/setup packet header.
/// @param data Packet bytes.
/// @param len Packet length.
/// @param type Expected Vorbis header type byte.
/// @return Nonzero when the packet starts with the typed `vorbis` signature.
static int packet_is_vorbis_header(const uint8_t *data, size_t len, uint8_t type) {
    return data && len >= 7 && data[0] == type && memcmp(data + 1, "vorbis", 6) == 0;
}

/// @brief Reset decoded-buffer cursors and stream-progress bookkeeping.
/// @param music Stream whose buffers are logically emptied.
static void vaud_music_clear_buffers(struct vaud_music *music) {
    if (!music)
        return;
    music->current_buffer = 0;
    music->buffer_position = 0;
    for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++) {
        music->buffer_frames[i] = 0;
        music->buffer_refilling[i] = 0;
    }
    music->leftover_frames = 0;
    music->resample_phase = 0.0;
    music->stream_output_generated = 0;
    music->stream_eof = 0;
    music->stream_loop_pending = 0;
}

/// @brief Ensure capacity for interleaved-stereo leftover decoded frames.
/// @param music Stream owning the leftover buffer.
/// @param frames Minimum required frame capacity.
/// @return 1 when capacity is available; otherwise 0.
static int vaud_music_reserve_leftover(struct vaud_music *music, int32_t frames) {
    if (!music || frames <= 0)
        return 0;
    if (frames <= music->leftover_cap)
        return 1;

    if (frames > INT32_MAX - 256)
        return 0;
    int32_t new_cap = frames + 256;

    size_t bytes = 0;
    if (!vaud_pcm_s16_buffer_size(new_cap, VAUD_CHANNELS, &bytes))
        return 0;

    int16_t *new_buf = (int16_t *)realloc(music->leftover_buf, bytes);
    if (!new_buf)
        return 0;

    music->leftover_buf = new_buf;
    music->leftover_cap = new_cap;
    return 1;
}

/// @brief Decode the three Vorbis headers for a selected logical stream.
/// @param reader OGG packet reader positioned at or before the headers.
/// @param serial Logical stream serial number to accept.
/// @param dec Decoder receiving headers in order.
/// @return 1 after all three headers decode; otherwise 0.
static int vaud_ogg_parse_headers_for_serial(ogg_reader_t *reader,
                                             uint32_t serial,
                                             vorbis_decoder_t *dec) {
    const uint8_t *pkt = NULL;
    size_t pkt_len = 0;
    ogg_packet_info_t info;
    int header_num = 0;
    while (ogg_reader_next_packet_ex(reader, &pkt, &pkt_len, &info)) {
        if (info.serial_number != serial)
            continue;
        if (vorbis_decode_header(dec, pkt, pkt_len, header_num) != 0)
            return 0;
        header_num++;
        if (header_num == 3)
            return 1;
    }
    return 0;
}

/// @brief Reset a music decoder/file to its first source frame.
/// @details Clears output state, recreates the Vorbis decoder and reparses
///          headers, rewinds MP3 state, or seeks WAV data to its payload offset.
/// @param music Stream to rewind.
/// @return 1 when its format-specific source is ready; otherwise 0.
static int vaud_music_reset_source(struct vaud_music *music) {
    if (!music)
        return 0;

    vaud_music_clear_buffers(music);
    music->position = 0;
    music->source_position = 0;

    if (music->format == 1) {
        if (!music->ogg_reader || music->ogg_serial == 0)
            return 0;
        ogg_reader_rewind((ogg_reader_t *)music->ogg_reader);
        if (music->vorbis_dec) {
            vorbis_decoder_free((vorbis_decoder_t *)music->vorbis_dec);
            music->vorbis_dec = NULL;
        }
        music->vorbis_dec = vorbis_decoder_new();
        if (!music->vorbis_dec)
            return 0;
        return vaud_ogg_parse_headers_for_serial((ogg_reader_t *)music->ogg_reader,
                                                 music->ogg_serial,
                                                 (vorbis_decoder_t *)music->vorbis_dec);
    }

    if (music->format == 2) {
        if (!music->mp3_stream)
            return 0;
        mp3_stream_rewind((mp3_stream_t *)music->mp3_stream);
        return 1;
    }

    if (!music->file)
        return 0;
    return vaud_wav_seek_stream(music->file, music->data_offset, SEEK_SET);
}

/// @brief Read decoded PCM frames from an OGG Vorbis stream.
/// @details Drains saved excess first, decodes packets from the selected logical
///          stream, converts mono to stereo, and preserves frames beyond the
///          caller's capacity for the next request.
/// @param music Initialized OGG music stream.
/// @param output Interleaved-stereo signed-16 output.
/// @param max_frames Output frame capacity.
/// @return Number of frames written.
static int32_t ogg_stream_read_frames(struct vaud_music *music,
                                      int16_t *output,
                                      int32_t max_frames) {
    if (!music->ogg_reader || !music->vorbis_dec)
        return 0;
    int32_t written = 0;

    // Drain leftover frames from previous decode
    if (music->leftover_frames > 0) {
        int32_t n = (music->leftover_frames < max_frames) ? music->leftover_frames : max_frames;
        memcpy(output, music->leftover_buf, (size_t)n * 2 * sizeof(int16_t));
        music->leftover_frames -= n;
        if (music->leftover_frames > 0)
            memmove(music->leftover_buf,
                    music->leftover_buf + n * 2,
                    (size_t)music->leftover_frames * 2 * sizeof(int16_t));
        written += n;
    }

    // Decode packets until we have enough frames
    while (written < max_frames) {
        const uint8_t *pkt = NULL;
        size_t pkt_len = 0;
        ogg_packet_info_t info;
        do {
            if (!ogg_reader_next_packet_ex(
                    (ogg_reader_t *)music->ogg_reader, &pkt, &pkt_len, &info)) {
                pkt = NULL;
                break;
            }
        } while (music->ogg_serial != 0 && info.serial_number != music->ogg_serial);
        if (!pkt)
            break;

        int16_t *pcm = NULL;
        int samples = 0;
        if (vorbis_decode_packet(
                (vorbis_decoder_t *)music->vorbis_dec, pkt, pkt_len, &pcm, &samples) != 0 ||
            samples <= 0 || !pcm)
            continue;

        int32_t space = max_frames - written;
        int32_t to_copy = (samples < space) ? samples : space;
        int ch = vorbis_get_channels((vorbis_decoder_t *)music->vorbis_dec);
        if (ch < 1 || ch > 2)
            break;
        if (ch == 1) {
            for (int32_t i = 0; i < to_copy; i++) {
                int16_t s = pcm[i];
                output[(written + i) * 2] = s;
                output[(written + i) * 2 + 1] = s;
            }
        } else {
            memcpy(output + written * 2, pcm, (size_t)to_copy * 2 * sizeof(int16_t));
        }
        written += to_copy;

        // Save excess to leftover
        if (samples > to_copy) {
            int excess = samples - to_copy;
            if (vaud_music_reserve_leftover(music, excess)) {
                if (ch == 1) {
                    for (int i = 0; i < excess; i++) {
                        int16_t s = pcm[to_copy + i];
                        music->leftover_buf[i * 2] = s;
                        music->leftover_buf[i * 2 + 1] = s;
                    }
                } else {
                    memcpy(music->leftover_buf,
                           pcm + to_copy * 2,
                           (size_t)excess * 2 * sizeof(int16_t));
                }
                music->leftover_frames = excess;
            } else {
                music->leftover_frames = 0;
            }
        }
    }
    return written;
}

/// @brief Read decoded PCM frames from an MP3 stream.
/// @details Drains saved excess, decodes frames until capacity or EOF, converts
///          mono to stereo, and preserves excess decoded frames.
/// @param music Initialized MP3 music stream.
/// @param output Interleaved-stereo signed-16 output.
/// @param max_frames Output frame capacity.
/// @return Number of frames written.
static int32_t mp3_stream_read_frames_music(struct vaud_music *music,
                                            int16_t *output,
                                            int32_t max_frames) {
    if (!music->mp3_stream)
        return 0;
    int32_t written = 0;

    // Drain leftover
    if (music->leftover_frames > 0) {
        int32_t n = (music->leftover_frames < max_frames) ? music->leftover_frames : max_frames;
        memcpy(output, music->leftover_buf, (size_t)n * 2 * sizeof(int16_t));
        music->leftover_frames -= n;
        if (music->leftover_frames > 0)
            memmove(music->leftover_buf,
                    music->leftover_buf + n * 2,
                    (size_t)music->leftover_frames * 2 * sizeof(int16_t));
        written += n;
    }

    while (written < max_frames) {
        int16_t *pcm = NULL;
        int samples = mp3_stream_decode_frame((mp3_stream_t *)music->mp3_stream, &pcm);
        if (samples <= 0 || !pcm)
            break;

        int ch = mp3_stream_channels((mp3_stream_t *)music->mp3_stream);
        if (ch < 1 || ch > 2)
            break;
        int32_t space = max_frames - written;
        int32_t to_copy = (samples < space) ? samples : space;
        if (ch == 1) {
            for (int32_t i = 0; i < to_copy; i++) {
                int16_t s = pcm[i];
                output[(written + i) * 2] = s;
                output[(written + i) * 2 + 1] = s;
            }
        } else {
            memcpy(output + written * 2, pcm, (size_t)to_copy * 2 * sizeof(int16_t));
        }
        written += to_copy;

        if (samples > to_copy) {
            int excess = samples - to_copy;
            if (vaud_music_reserve_leftover(music, excess)) {
                if (ch == 1) {
                    for (int i = 0; i < excess; i++) {
                        int16_t s = pcm[to_copy + i];
                        music->leftover_buf[i * 2] = s;
                        music->leftover_buf[i * 2 + 1] = s;
                    }
                } else {
                    memcpy(music->leftover_buf,
                           pcm + to_copy * 2,
                           (size_t)excess * 2 * sizeof(int16_t));
                }
                music->leftover_frames = excess;
            } else {
                music->leftover_frames = 0;
            }
        }
    }
    return written;
}

/// @brief Allocate WAV raw-byte scratch sufficient for one output buffer.
/// @details Accounts for the larger source-frame request required by resampling
///          and replaces any previous scratch buffer only after allocation succeeds.
/// @param music WAV stream to prepare; non-WAV streams need no buffer.
/// @return 1 when ready or unnecessary; otherwise 0.
static int vaud_music_prepare_wav_read_buffer(struct vaud_music *music) {
    if (!music || music->format != 0)
        return 1;

    int32_t bytes_per_sample = music->bits_per_sample / 8;
    int32_t bytes_per_frame = bytes_per_sample * music->channels;
    if (bytes_per_sample <= 0 || bytes_per_frame <= 0)
        return 0;

    int64_t raw_frames = VAUD_MUSIC_BUFFER_FRAMES;
    int32_t source_rate =
        music->source_sample_rate > 0 ? music->source_sample_rate : music->sample_rate;
    if (source_rate != VAUD_SAMPLE_RATE)
        raw_frames =
            ((int64_t)(VAUD_MUSIC_BUFFER_FRAMES - 1) * source_rate + VAUD_SAMPLE_RATE - 1) /
                VAUD_SAMPLE_RATE +
            2;
    if (raw_frames <= 0 || raw_frames > (int64_t)(SIZE_MAX / (size_t)bytes_per_frame))
        return 0;

    size_t needed = (size_t)raw_frames * (size_t)bytes_per_frame;
    uint8_t *buf = (uint8_t *)malloc(needed);
    if (!buf)
        return 0;

    free(music->wav_read_buf);
    music->wav_read_buf = buf;
    music->wav_read_cap = needed;
    return 1;
}

/// @brief Compute encoded bytes per source WAV frame.
/// @param music WAV stream metadata.
/// @return Positive byte count, or 0 for invalid/overflowing metadata.
static int32_t vaud_music_wav_bytes_per_frame(const struct vaud_music *music) {
    if (!music || music->channels <= 0 || music->bits_per_sample <= 0)
        return 0;
    int32_t bytes_per_sample = music->bits_per_sample / 8;
    if (bytes_per_sample <= 0 || bytes_per_sample > INT32_MAX / music->channels)
        return 0;
    return bytes_per_sample * music->channels;
}

/// @brief Clamp a WAV read request to complete frames remaining in the data chunk.
/// @param music WAV stream whose source position may be normalized.
/// @param requested Requested source frames.
/// @return Safe nonnegative frame count.
static int32_t vaud_music_clamp_wav_read_frames(struct vaud_music *music, int32_t requested) {
    if (!music || music->format != 0 || requested <= 0)
        return 0;

    int32_t bytes_per_frame = vaud_music_wav_bytes_per_frame(music);
    if (bytes_per_frame <= 0 || music->data_size <= 0)
        return 0;

    int64_t total_source_frames = music->data_size / bytes_per_frame;
    if (music->source_position < 0)
        music->source_position = 0;
    if (music->source_position >= total_source_frames)
        return 0;

    int64_t remaining = total_source_frames - music->source_position;
    return remaining < requested ? (int32_t)remaining : requested;
}

/// @brief Advance a WAV source cursor with saturation.
/// @param music WAV stream to update.
/// @param frames_read Positive number of source frames consumed.
static void vaud_music_advance_wav_source(struct vaud_music *music, int32_t frames_read) {
    if (!music || music->format != 0 || frames_read <= 0)
        return;
    if (music->source_position > INT64_MAX - frames_read)
        music->source_position = INT64_MAX;
    else
        music->source_position += frames_read;
}

/// @brief Calculate source frames needed to produce one mixer-rate buffer.
/// @param source_rate Positive source rate.
/// @return Required frames including interpolation lookahead, or 0 on invalid range.
static int32_t vaud_music_resample_source_request(int32_t source_rate) {
    if (source_rate <= 0)
        return 0;
    int64_t raw_needed =
        ((int64_t)(VAUD_MUSIC_BUFFER_FRAMES - 1) * source_rate + VAUD_SAMPLE_RATE - 1) /
            VAUD_SAMPLE_RATE +
        2;
    if (raw_needed <= 0 || raw_needed > INT32_MAX)
        return 0;
    return (int32_t)raw_needed;
}

/// @brief Prepend source frames to a stream's stereo leftover queue.
/// @param music Stream owning the queue.
/// @param frames Interleaved-stereo frames to prepend.
/// @param frame_count Number of frames.
/// @return 1 on success or empty input; 0 on overflow/allocation failure.
static int vaud_music_stash_leftover(struct vaud_music *music,
                                     const int16_t *frames,
                                     int32_t frame_count) {
    if (!music)
        return 0;
    if (frame_count <= 0)
        return 1;

    int32_t existing = music->leftover_frames;
    if (existing < 0)
        existing = 0;
    if (frame_count > INT32_MAX - existing) {
        music->leftover_frames = 0;
        return 0;
    }
    int32_t total = frame_count + existing;
    if (!vaud_music_reserve_leftover(music, total)) {
        music->leftover_frames = 0;
        return 0;
    }
    if (existing > 0) {
        memmove(music->leftover_buf + frame_count * 2,
                music->leftover_buf,
                (size_t)existing * 2 * sizeof(int16_t));
    }
    memmove(music->leftover_buf, frames, (size_t)frame_count * 2 * sizeof(int16_t));
    music->leftover_frames = total;
    return 1;
}

/// @brief Read WAV source frames while honoring previously stashed frames.
/// @param music Initialized WAV stream.
/// @param output Interleaved-stereo signed-16 destination.
/// @param max_frames Destination capacity.
/// @return Number of frames copied and decoded.
static int32_t wav_stream_read_frames_with_leftover(struct vaud_music *music,
                                                    int16_t *output,
                                                    int32_t max_frames) {
    if (!music || !output || max_frames <= 0 || !music->file)
        return 0;

    int32_t written = 0;
    if (music->leftover_frames > 0) {
        int32_t n = music->leftover_frames < max_frames ? music->leftover_frames : max_frames;
        memcpy(output, music->leftover_buf, (size_t)n * 2 * sizeof(int16_t));
        music->leftover_frames -= n;
        if (music->leftover_frames > 0) {
            memmove(music->leftover_buf,
                    music->leftover_buf + n * 2,
                    (size_t)music->leftover_frames * 2 * sizeof(int16_t));
        }
        written += n;
    }

    int32_t to_read = vaud_music_clamp_wav_read_frames(music, max_frames - written);
    if (to_read <= 0)
        return written;

    int32_t read = vaud_wav_read_frames_buffered(music->file,
                                                 output + written * 2,
                                                 to_read,
                                                 music->channels,
                                                 music->bits_per_sample,
                                                 music->audio_format,
                                                 music->wav_read_buf,
                                                 music->wav_read_cap);
    vaud_music_advance_wav_source(music, read);
    return written + read;
}

/// @brief Dispatch source-frame reading by compressed/WAV format.
/// @param music Initialized stream.
/// @param output Interleaved-stereo destination.
/// @param max_frames Destination capacity.
/// @return Number of decoded source-rate frames.
static int32_t vaud_music_read_source_frames(struct vaud_music *music,
                                             int16_t *output,
                                             int32_t max_frames) {
    if (!music || !output || max_frames <= 0)
        return 0;
    if (music->format == 1)
        return ogg_stream_read_frames(music, output, max_frames);
    if (music->format == 2)
        return mp3_stream_read_frames_music(music, output, max_frames);
    return wav_stream_read_frames_with_leftover(music, output, max_frames);
}

/// @brief Clamp a floating-point music sample into signed 16-bit range.
/// @param sample Interpolated sample value.
/// @return Sample clipped to int16_t limits.
static inline int16_t vaud_music_clamp_double_to_s16(double sample) {
    if (!isfinite(sample))
        return 0;
    if (sample > 32767.0)
        return INT16_MAX;
    if (sample < -32768.0)
        return INT16_MIN;
    return (int16_t)(sample < 0.0 ? sample - 0.5 : sample + 0.5);
}

/// @brief Read one interleaved music sample with edge clamping.
/// @param input Interleaved stereo source frames.
/// @param frame Requested source frame.
/// @param frame_count Number of available source frames.
/// @param channel Stereo channel to read.
/// @return Source sample as double for interpolation math.
static inline double vaud_music_sample_clamped(const int16_t *input,
                                               int64_t frame,
                                               int32_t frame_count,
                                               int32_t channel) {
    if (frame < 0)
        frame = 0;
    if (frame >= frame_count)
        frame = frame_count - 1;
    return (double)input[frame * VAUD_CHANNELS + channel];
}

/// @brief Interpolate one streamed music channel using a cubic Catmull-Rom kernel.
/// @param input Interleaved stereo source frames.
/// @param frame Base source frame.
/// @param frac Fractional distance from @p frame to the next frame.
/// @param frame_count Number of available source frames.
/// @param channel Stereo channel to interpolate.
/// @return Interpolated sample in floating-point PCM units.
static inline double vaud_music_cubic_sample(
    const int16_t *input, int64_t frame, double frac, int32_t frame_count, int32_t channel) {
    double y0 = vaud_music_sample_clamped(input, frame - 1, frame_count, channel);
    double y1 = vaud_music_sample_clamped(input, frame, frame_count, channel);
    double y2 = vaud_music_sample_clamped(input, frame + 1, frame_count, channel);
    double y3 = vaud_music_sample_clamped(input, frame + 2, frame_count, channel);

    double a0 = (-0.5 * y0) + (1.5 * y1) - (1.5 * y2) + (0.5 * y3);
    double a1 = y0 - (2.5 * y1) + (2.0 * y2) - (0.5 * y3);
    double a2 = (-0.5 * y0) + (0.5 * y2);
    double a3 = y1;
    return ((a0 * frac + a1) * frac + a2) * frac + a3;
}

/// @brief Decode source-rate frames and resample them into one output buffer.
/// @details Maintains fractional phase and stashes unconsumed source frames so
///          interpolation remains continuous across buffer boundaries.
/// @param music Stream owning resampling scratch and phase.
/// @param out Interleaved-stereo mixer-rate destination.
/// @param source_rate Positive source sample rate.
/// @return Number of output frames produced.
static int32_t vaud_music_resample_source_into(struct vaud_music *music,
                                               int16_t *out,
                                               int32_t source_rate) {
    int32_t raw_needed = vaud_music_resample_source_request(source_rate);
    if (raw_needed <= 0)
        return 0;

    if (!music->resample_buf || music->resample_cap < raw_needed) {
        int64_t new_cap = (int64_t)raw_needed + 64;
        int16_t *new_buf = vaud_alloc_pcm_frames(new_cap, VAUD_CHANNELS);
        if (!new_buf) {
            return 0;
        }
        free(music->resample_buf);
        music->resample_buf = new_buf;
        music->resample_cap = new_cap;
    }

    int32_t raw_read = vaud_music_read_source_frames(music, music->resample_buf, raw_needed);
    if (raw_read <= 0)
        return 0;

    double phase = music->resample_phase;
    double ratio = (double)source_rate / (double)VAUD_SAMPLE_RATE;
    int source_exhausted = raw_read < raw_needed;
    int32_t out_frames = 0;
    const double phase_epsilon = 1.0e-9;

    while (out_frames < VAUD_MUSIC_BUFFER_FRAMES) {
        if (phase + phase_epsilon >= (double)raw_read)
            break;
        int64_t in_idx = (int64_t)phase;
        if (in_idx < 0 || in_idx >= raw_read)
            break;

        double frac = phase - (double)in_idx;
        if (in_idx + 2 >= raw_read && !source_exhausted)
            break;

        for (int32_t ch = 0; ch < VAUD_CHANNELS; ch++) {
            double sample =
                vaud_music_cubic_sample(music->resample_buf, in_idx, frac, raw_read, ch);
            out[out_frames * VAUD_CHANNELS + ch] = vaud_music_clamp_double_to_s16(sample);
        }

        out_frames++;
        phase += ratio;
    }

    int64_t consumed = (int64_t)(phase + phase_epsilon);
    if (consumed < 0)
        consumed = 0;
    if (consumed > raw_read)
        consumed = raw_read;
    phase -= (double)consumed;

    int32_t remaining = raw_read - (int32_t)consumed;
    if (remaining > 0) {
        vaud_music_stash_leftover(music, music->resample_buf + consumed * VAUD_CHANNELS, remaining);
    }
    music->resample_phase = phase;

    return out_frames;
}

/// @brief Clamp produced frames to known duration and update progress.
/// @param music Stream whose generated-frame count is advanced.
/// @param frames Candidate positive output frame count.
/// @return Frames committed after duration clamping.
static int32_t vaud_music_commit_output_frames(struct vaud_music *music, int32_t frames) {
    if (!music || frames <= 0)
        return 0;
    if (music->frame_count > 0) {
        if (music->stream_output_generated >= music->frame_count)
            return 0;
        int64_t remaining = music->frame_count - music->stream_output_generated;
        if ((int64_t)frames > remaining)
            frames = (int32_t)remaining;
    }
    music->stream_output_generated += frames;
    return frames;
}

//===----------------------------------------------------------------------===//
// Music Buffer Fill (with resampling)
//===----------------------------------------------------------------------===//

/// @copydoc vaud_music_fill_buffer
int32_t vaud_music_fill_buffer(struct vaud_music *music, int32_t buf_idx) {
    if (!music || buf_idx < 0 || buf_idx >= VAUD_MUSIC_BUFFER_COUNT)
        return 0;
    if (music->frame_count > 0 && music->stream_output_generated >= music->frame_count)
        return 0;

    int16_t *out = music->buffers[buf_idx];
    int32_t source_rate =
        music->source_sample_rate > 0 ? music->source_sample_rate : music->sample_rate;

    // Compressed format streaming (OGG/MP3) — decode to temp, resample if needed
    if (music->format == 1 || music->format == 2) {
        int32_t raw_frames;
        if (source_rate == VAUD_SAMPLE_RATE) {
            // Decode directly into output buffer (no resampling)
            raw_frames = (music->format == 1)
                             ? ogg_stream_read_frames(music, out, VAUD_MUSIC_BUFFER_FRAMES)
                             : mp3_stream_read_frames_music(music, out, VAUD_MUSIC_BUFFER_FRAMES);
            return vaud_music_commit_output_frames(music, raw_frames);
        }
        return vaud_music_commit_output_frames(
            music, vaud_music_resample_source_into(music, out, source_rate));
    }

    // WAV path (format == 0)
    if (!music->file)
        return 0;

    if (source_rate == VAUD_SAMPLE_RATE) {
        int32_t to_read = vaud_music_clamp_wav_read_frames(music, VAUD_MUSIC_BUFFER_FRAMES);
        if (to_read <= 0)
            return 0;
        int32_t read = vaud_wav_read_frames_buffered(music->file,
                                                     out,
                                                     to_read,
                                                     music->channels,
                                                     music->bits_per_sample,
                                                     music->audio_format,
                                                     music->wav_read_buf,
                                                     music->wav_read_cap);
        vaud_music_advance_wav_source(music, read);
        return vaud_music_commit_output_frames(music, read);
    }

    return vaud_music_commit_output_frames(
        music, vaud_music_resample_source_into(music, out, source_rate));
}

/// @copydoc vaud_music_seek_output_frame
int vaud_music_seek_output_frame(struct vaud_music *music, int64_t target_frame) {
    if (!music)
        return 0;

    if (target_frame < 0)
        target_frame = 0;
    if (music->frame_count > 0 && target_frame > music->frame_count)
        target_frame = music->frame_count;

    if (!vaud_music_reset_source(music))
        return 0;

    if (music->frame_count > 0 && target_frame == music->frame_count) {
        music->position = target_frame;
        music->stream_output_generated = target_frame;
        music->buffer_frames[0] = 0;
        music->buffer_position = 0;
        music->current_buffer = 0;
        music->stream_eof = 1;
        return 1;
    }

    int64_t remaining = target_frame;
    while (remaining > 0) {
        int32_t got = vaud_music_fill_buffer(music, 0);
        if (got <= 0) {
            music->position = target_frame - remaining;
            music->buffer_frames[0] = 0;
            music->buffer_position = 0;
            music->stream_eof = 1;
            return 0;
        }
        if (remaining < got) {
            music->buffer_frames[0] = got;
            music->buffer_position = (int32_t)remaining;
            music->position = target_frame;
            music->current_buffer = 0;
            music->stream_eof = 0;
            return 1;
        }
        remaining -= got;
    }

    music->buffer_frames[0] = vaud_music_fill_buffer(music, 0);
    music->buffer_position = 0;
    music->position = target_frame;
    music->current_buffer = 0;
    music->stream_eof = music->buffer_frames[0] <= 0;
    return music->buffer_frames[0] > 0;
}

/// @copydoc vaud_music_prefill_locked
void vaud_music_prefill_locked(struct vaud_music *music) {
    if (!music)
        return;

    if (music->stream_loop_pending) {
        music->stream_loop_pending = 0;
        if (music->loop) {
            if (!vaud_music_seek_output_frame(music, 0)) {
                music->state = VAUD_MUSIC_STOPPED;
                return;
            }
        }
    }

    for (int32_t n = 0; n < VAUD_MUSIC_BUFFER_COUNT && !music->stream_eof; n++) {
        int32_t idx = (music->current_buffer + n) % VAUD_MUSIC_BUFFER_COUNT;
        if (music->buffer_refilling[idx] || music->buffer_frames[idx] > 0)
            continue;
        int32_t read = vaud_music_fill_buffer(music, idx);
        music->buffer_frames[idx] = read;
        if (read <= 0) {
            music->stream_eof = 1;
            break;
        }
    }
}

/// @brief Fill empty stream buffers while a forced refill owns every buffer slot.
/// @details Seek and stopped-to-playing transitions mark all stream buffers as
///          refilling before dropping the context mutex.  That prevents the
///          realtime mixer from consuming partially replaced buffers, but also
///          means the generic prefill helper will intentionally skip those
///          slots.  This helper is only used by forced refills and therefore
///          ignores the per-slot refilling marker while still preserving already
///          primed buffers, such as the first buffer populated by seek.
/// @param music Music stream currently protected by `refill_in_progress`.
static void vaud_music_prefill_forced(vaud_music_t music) {
    if (!music)
        return;

    for (int32_t n = 0; n < VAUD_MUSIC_BUFFER_COUNT && !music->stream_eof; n++) {
        int32_t idx = (music->current_buffer + n) % VAUD_MUSIC_BUFFER_COUNT;
        if (music->buffer_frames[idx] > 0)
            continue;
        int32_t read = vaud_music_fill_buffer(music, idx);
        music->buffer_frames[idx] = read;
        if (read <= 0) {
            music->stream_eof = 1;
            break;
        }
    }
}

/// @brief Test whether a playing/paused stream needs control-thread refill work.
/// @details The caller must hold the owning context mutex.
/// @param music Stream to inspect.
/// @return 1 for pending loop rewind or an empty unclaimed non-EOF buffer.
static int vaud_music_needs_refill_locked(vaud_music_t music) {
    if (!music || music->refill_in_progress)
        return 0;
    if (music->state != VAUD_MUSIC_PLAYING && music->state != VAUD_MUSIC_PAUSED)
        return 0;
    if (music->stream_loop_pending)
        return 1;
    for (int32_t n = 0; n < VAUD_MUSIC_BUFFER_COUNT; n++) {
        int32_t idx = (music->current_buffer + n) % VAUD_MUSIC_BUFFER_COUNT;
        if (music->buffer_frames[idx] <= 0 && !music->buffer_refilling[idx] && !music->stream_eof)
            return 1;
    }
    return 0;
}

/// @brief Claim refill work and identify buffers that may be decoded unlocked.
/// @details The caller holds the context mutex. The helper resets the completion
///          event, marks ownership, and either claims every slot for a loop
///          rewind or returns individual empty buffer indices.
/// @param music Stream to claim.
/// @param fill_indices Output array with VAUD_MUSIC_BUFFER_COUNT capacity.
/// @param fill_count Output number of claimed indices.
/// @param loop_pending Output nonzero when a rewind must precede filling.
/// @return 1 when work was claimed; otherwise 0.
static int vaud_music_begin_refill_locked(vaud_music_t music,
                                          int32_t *fill_indices,
                                          int32_t *fill_count,
                                          int *loop_pending) {
    if (!fill_indices || !fill_count || !loop_pending)
        return 0;
    *fill_count = 0;
    *loop_pending = 0;
    if (!vaud_music_needs_refill_locked(music))
        return 0;

    if (music->refill_event_ready)
        vaud_event_reset(&music->refill_event);
    music->refill_in_progress = 1;
    if (music->stream_loop_pending) {
        *loop_pending = 1;
        for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++)
            music->buffer_refilling[i] = 1;
        return 1;
    }

    for (int32_t n = 0; n < VAUD_MUSIC_BUFFER_COUNT && !music->stream_eof; n++) {
        int32_t idx = (music->current_buffer + n) % VAUD_MUSIC_BUFFER_COUNT;
        if (music->buffer_frames[idx] <= 0 && !music->buffer_refilling[idx]) {
            music->buffer_refilling[idx] = 1;
            fill_indices[(*fill_count)++] = idx;
        }
    }

    if (*fill_count == 0) {
        music->refill_in_progress = 0;
        if (music->refill_event_ready)
            vaud_event_set(&music->refill_event);
        return 0;
    }
    return 1;
}

/// @brief Claim every stream buffer for an exclusive seek/play refill.
/// @param music Stream protected by its context mutex.
/// @return 1 when ownership was acquired; otherwise 0.
static int vaud_music_begin_forced_refill_locked(vaud_music_t music) {
    if (!music || music->refill_in_progress)
        return 0;
    if (music->refill_event_ready)
        vaud_event_reset(&music->refill_event);
    music->refill_in_progress = 1;
    for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++)
        music->buffer_refilling[i] = 1;
    return 1;
}

/// @brief Release all refill claims and signal completion.
/// @details The caller must hold the owning context mutex.
/// @param music Stream whose refill operation has finished.
static void vaud_music_finish_refill_locked(vaud_music_t music) {
    if (music) {
        for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++)
            music->buffer_refilling[i] = 0;
        music->refill_in_progress = 0;
        if (music->refill_event_ready)
            vaud_event_set(&music->refill_event);
    }
}

/// @brief Empty all decoded stream buffers and clear per-slot claims.
/// @param music Stream whose buffer cursors are reset.
static void vaud_music_clear_stream_buffers(vaud_music_t music) {
    if (!music)
        return;
    for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++) {
        music->buffer_frames[i] = 0;
        music->buffer_refilling[i] = 0;
    }
    music->buffer_position = 0;
    music->current_buffer = 0;
    music->stream_loop_pending = 0;
}

/// @brief Wait until a music stream's control-thread refill has completed.
/// @details Refill operations decode while the caller has temporarily released
///          the context mutex.  Callers that need to detach, destroy, or mutate
///          stream-owned resources must wait for `refill_in_progress` to clear.
///          This helper deliberately polls with a short sleep instead of waiting
///          indefinitely on the refill event; if an event signal is lost or the
///          event object was never initialized, teardown can still make progress
///          to the next state check and report a diagnostic instead of blocking
///          forever inside the event primitive.
/// @param ctx Context whose mutex protects @p music.
/// @param music Music stream whose refill flag should become clear.
static void vaud_music_wait_for_refill(vaud_context_t ctx, vaud_music_t music) {
    if (!ctx || !music)
        return;

    int64_t wait_start = vaud_platform_now_ms();
    int warned = 0;
    for (;;) {
        vaud_mutex_lock(&ctx->mutex);
        int done = !music->refill_in_progress;
        vaud_mutex_unlock(&ctx->mutex);
        if (done)
            return;
        if (!warned && vaud_platform_now_ms() - wait_start >= 5000) {
            vaud_set_error(VAUD_ERR_PLATFORM, "Timed out waiting for music refill completion");
            warned = 1;
        }
        vaud_control_sleep_1ms();
    }
}

//===----------------------------------------------------------------------===//
// Music Loading and Playback
//===----------------------------------------------------------------------===//

/// @brief Append a fully initialized music stream to a context registry.
/// @param ctx Context that will service and mix @p music.
/// @param music Unregistered stream whose context pointer already names @p ctx.
/// @return 1 when registered; otherwise 0 after publishing a capacity error.
static int music_register(vaud_context_t ctx, vaud_music_t music);

/// @brief Allocate a music handle, refill event, and fixed decoded-buffer ring.
/// @details The returned stream is initialized to the stopped state but is not
///          inserted into @p ctx; callers may safely pass it to vaud_free_music()
///          while completing codec-specific initialization.
/// @param ctx Context to associate with the provisional stream.
/// @return Owned unregistered stream, or NULL on allocation/event failure.
static vaud_music_t music_alloc_unregistered(vaud_context_t ctx);

/// @copydoc vaud_load_music
vaud_music_t vaud_load_music(vaud_context_t ctx, const char *path) {
    if (vaud_context_is_destroying(ctx) || !path) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL context or path");
        return NULL;
    }

    void *file = NULL;
    int64_t data_offset = 0;
    int64_t data_size = 0;
    int64_t frames = 0;
    int32_t sample_rate = 0;
    int32_t channels = 0;
    int32_t bits = 0;
    int32_t audio_format = 0;

    if (!vaud_wav_open_stream(path,
                              &file,
                              &data_offset,
                              &data_size,
                              &frames,
                              &sample_rate,
                              &channels,
                              &bits,
                              &audio_format)) {
        return NULL;
    }

    vaud_music_t music = music_alloc_unregistered(ctx);
    if (!music) {
        fclose((FILE *)file);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate music structure");
        return NULL;
    }

    music->ctx = ctx;
    music->file = file;
    music->data_offset = data_offset;
    music->data_size = data_size;
    if (!vaud_checked_resampled_frames(
            frames, sample_rate, VAUD_SAMPLE_RATE, &music->frame_count)) {
        vaud_free_music(music);
        return NULL;
    }
    music->sample_rate = VAUD_SAMPLE_RATE;
    music->source_sample_rate = sample_rate;
    music->channels = channels;
    music->bits_per_sample = bits;
    music->audio_format = audio_format;
    music->state = VAUD_MUSIC_STOPPED;
    music->position = 0;
    music->loop = 0;
    music->volume = VAUD_DEFAULT_MUSIC_VOLUME;
    music->group_id = 0;
    music->current_buffer = 0;
    music->buffer_position = 0;
    music->resample_buf = NULL;
    music->resample_cap = 0;

    if (!vaud_music_prepare_wav_read_buffer(music)) {
        vaud_free_music(music);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate WAV read buffer");
        return NULL;
    }

    /* Prime the stream at position 0. */
    if (!vaud_music_seek_output_frame(music, 0)) {
        vaud_free_music(music);
        return NULL;
    }
    vaud_music_prefill_locked(music);

    /* Add to context's music list (H-4: return NULL and free if list is full) */
    if (!music_register(ctx, music)) {
        vaud_free_music(music);
        return NULL;
    }

    return music;
}

/// @copydoc music_register
static int music_register(vaud_context_t ctx, vaud_music_t music) {
    if (vaud_context_is_destroying(ctx) || !music)
        return 0;

    vaud_mutex_lock(&ctx->mutex);
    if (ctx->music_count < VAUD_MAX_MUSIC) {
        ctx->active_music[ctx->music_count++] = music;
        vaud_mutex_unlock(&ctx->mutex);
        return 1;
    }
    vaud_mutex_unlock(&ctx->mutex);
    vaud_set_error(VAUD_ERR_INVALID_PARAM, "Maximum simultaneous music streams reached");
    return 0;
}

/// @copydoc music_alloc_unregistered
static vaud_music_t music_alloc_unregistered(vaud_context_t ctx) {
    vaud_music_t music = (vaud_music_t)calloc(1, sizeof(struct vaud_music));
    if (!music)
        return NULL;

    if (!vaud_event_init(&music->refill_event)) {
        free(music);
        return NULL;
    }
    music->refill_event_ready = 1;

    for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++) {
        music->buffers[i] = (int16_t *)malloc(VAUD_MUSIC_BUFFER_FRAMES * 2 * sizeof(int16_t));
        if (!music->buffers[i]) {
            for (int32_t j = 0; j < i; j++)
                free(music->buffers[j]);
            vaud_event_destroy(&music->refill_event);
            free(music);
            return NULL;
        }
        music->buffer_frames[i] = 0;
    }

    music->ctx = ctx;
    music->state = VAUD_MUSIC_STOPPED;
    music->volume = VAUD_DEFAULT_MUSIC_VOLUME;
    music->group_id = 0;
    return music;
}

/// @copydoc vaud_load_music_ogg
vaud_music_t vaud_load_music_ogg(vaud_context_t ctx, const char *path) {
    if (vaud_context_is_destroying(ctx) || !path)
        return NULL;

    ogg_reader_t *reader = ogg_reader_open_file(path);
    if (!reader)
        return NULL;

    vorbis_decoder_t *dec = vorbis_decoder_new();
    if (!dec) {
        ogg_reader_free(reader);
        return NULL;
    }

    uint32_t vorbis_serial = 0;
    int header_num = 0;
    int64_t last_granule = -1;
    const uint8_t *pkt = NULL;
    size_t pkt_len = 0;
    ogg_packet_info_t info;
    while (ogg_reader_next_packet_ex(reader, &pkt, &pkt_len, &info)) {
        if (vorbis_serial == 0) {
            if (!info.bos || !packet_is_vorbis_header(pkt, pkt_len, 1))
                continue;
            vorbis_serial = info.serial_number;
        }
        if (info.serial_number != vorbis_serial)
            continue;
        if (header_num < 3) {
            if (vorbis_decode_header(dec, pkt, pkt_len, header_num) != 0) {
                vorbis_decoder_free(dec);
                ogg_reader_free(reader);
                return NULL;
            }
            header_num++;
            continue;
        }
        if (info.granule_position >= 0)
            last_granule = info.granule_position;
    }

    if (vorbis_serial == 0 || header_num < 3) {
        vorbis_decoder_free(dec);
        ogg_reader_free(reader);
        return NULL;
    }

    vaud_music_t music = music_alloc_unregistered(ctx);
    if (!music) {
        vorbis_decoder_free(dec);
        ogg_reader_free(reader);
        return NULL;
    }

    music->format = 1;
    music->ogg_reader = reader;
    music->vorbis_dec = NULL;
    music->ogg_serial = vorbis_serial;
    music->source_sample_rate = vorbis_get_sample_rate(dec);
    music->sample_rate = VAUD_SAMPLE_RATE;
    music->channels = vorbis_get_channels(dec);
    if (music->source_sample_rate <= 0 || music->source_sample_rate > 384000 ||
        music->channels <= 0 || music->channels > 2) {
        vorbis_decoder_free(dec);
        vaud_free_music(music);
        return NULL;
    }
    music->filepath = vaud_strdup(path);
    if (!music->filepath) {
        vorbis_decoder_free(dec);
        vaud_free_music(music);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to copy music path");
        return NULL;
    }
    if (last_granule >= 0) {
        if (!vaud_checked_resampled_frames(
                last_granule, music->source_sample_rate, VAUD_SAMPLE_RATE, &music->frame_count)) {
            vorbis_decoder_free(dec);
            vaud_free_music(music);
            return NULL;
        }
    } else {
        music->frame_count = 0;
    }

    vorbis_decoder_free(dec);
    if (!vaud_music_seek_output_frame(music, 0)) {
        vaud_free_music(music);
        return NULL;
    }
    vaud_music_prefill_locked(music);
    if (!music_register(ctx, music)) {
        vaud_free_music(music);
        return NULL;
    }
    return music;
}

/// @copydoc vaud_load_music_mp3
vaud_music_t vaud_load_music_mp3(vaud_context_t ctx, const char *path) {
    if (vaud_context_is_destroying(ctx) || !path)
        return NULL;

    mp3_stream_t *stream = mp3_stream_open(path);
    if (!stream)
        return NULL;

    vaud_music_t music = music_alloc_unregistered(ctx);
    if (!music) {
        mp3_stream_free(stream);
        return NULL;
    }

    music->format = 2;
    music->mp3_stream = stream;
    music->source_sample_rate = mp3_stream_sample_rate(stream);
    music->sample_rate = VAUD_SAMPLE_RATE;
    music->channels = mp3_stream_channels(stream);
    if (music->source_sample_rate <= 0 || music->source_sample_rate > 384000 ||
        music->channels <= 0 || music->channels > 2) {
        vaud_free_music(music);
        return NULL;
    }
    music->filepath = vaud_strdup(path);
    if (!music->filepath) {
        vaud_free_music(music);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to copy music path");
        return NULL;
    }
    int total_samples = mp3_stream_total_samples(stream);
    if (total_samples > 0) {
        if (!vaud_checked_resampled_frames(
                total_samples, music->source_sample_rate, VAUD_SAMPLE_RATE, &music->frame_count)) {
            vaud_free_music(music);
            return NULL;
        }
    } else {
        music->frame_count = 0;
    }

    if (!vaud_music_seek_output_frame(music, 0)) {
        vaud_free_music(music);
        return NULL;
    }
    vaud_music_prefill_locked(music);
    if (!music_register(ctx, music)) {
        vaud_free_music(music);
        return NULL;
    }
    return music;
}

/// @copydoc vaud_update
void vaud_update(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return;

    int32_t refills_processed = 0;
    vaud_mutex_lock(&ctx->mutex);
    while (refills_processed < VAUD_UPDATE_MAX_REFILLS_PER_CALL) {
        vaud_music_t music = NULL;
        int32_t fill_indices[VAUD_MUSIC_BUFFER_COUNT];
        int32_t fill_count = 0;
        int loop_pending = 0;

        for (int32_t i = 0; i < ctx->music_count; i++) {
            vaud_music_t candidate = ctx->active_music[i];
            if (vaud_music_begin_refill_locked(
                    candidate, fill_indices, &fill_count, &loop_pending)) {
                music = candidate;
                break;
            }
        }

        if (!music)
            break;

        vaud_mutex_unlock(&ctx->mutex);

        int32_t filled_frames[VAUD_MUSIC_BUFFER_COUNT] = {0};
        int refill_failed = 0;
        if (loop_pending) {
            if (!music->loop || !vaud_music_seek_output_frame(music, 0)) {
                refill_failed = 1;
            } else {
                for (int32_t n = 0; n < VAUD_MUSIC_BUFFER_COUNT && !music->stream_eof; n++) {
                    int32_t idx = (music->current_buffer + n) % VAUD_MUSIC_BUFFER_COUNT;
                    if (music->buffer_frames[idx] > 0)
                        continue;
                    int32_t read = vaud_music_fill_buffer(music, idx);
                    music->buffer_frames[idx] = read;
                    if (read <= 0) {
                        music->stream_eof = 1;
                        break;
                    }
                }
            }
        } else {
            for (int32_t i = 0; i < fill_count; i++) {
                int32_t idx = fill_indices[i];
                filled_frames[idx] = vaud_music_fill_buffer(music, idx);
                if (filled_frames[idx] <= 0)
                    break;
            }
        }

        vaud_mutex_lock(&ctx->mutex);
        if (music->ctx == ctx) {
            if (loop_pending) {
                if (refill_failed) {
                    music->state = VAUD_MUSIC_STOPPED;
                    music->stream_eof = 1;
                }
                music->stream_loop_pending = 0;
            } else {
                for (int32_t i = 0; i < fill_count; i++) {
                    int32_t idx = fill_indices[i];
                    music->buffer_frames[idx] = filled_frames[idx];
                    if (filled_frames[idx] <= 0) {
                        music->stream_eof = 1;
                        break;
                    }
                }
            }
        }
        vaud_music_finish_refill_locked(music);
        refills_processed++;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_free_music
void vaud_free_music(vaud_music_t music) {
    if (!music)
        return;

    vaud_context_t ctx = music->ctx;

    /* Remove from context's music list. This waits behind any in-progress
     * refill/seek because those operations hold the same mutex while decoding. */
    if (ctx) {
        vaud_music_wait_for_refill(ctx, music);
        vaud_mutex_lock(&ctx->mutex);
        for (int32_t i = 0; i < ctx->music_count; i++) {
            if (ctx->active_music[i] == music) {
                /* Shift remaining entries */
                for (int32_t j = i; j < ctx->music_count - 1; j++) {
                    ctx->active_music[j] = ctx->active_music[j + 1];
                }
                ctx->music_count--;
                ctx->active_music[ctx->music_count] = NULL;
                break;
            }
        }
        music->state = VAUD_MUSIC_STOPPED;
        music->refill_in_progress = 0;
        if (music->refill_event_ready)
            vaud_event_set(&music->refill_event);
        music->ctx = NULL;
        vaud_mutex_unlock(&ctx->mutex);
    }

    /* Close file */
    if (music->file) {
        fclose((FILE *)music->file);
    }

    /* Free compressed format decoders */
    if (music->ogg_reader)
        ogg_reader_free((ogg_reader_t *)music->ogg_reader);
    if (music->vorbis_dec)
        vorbis_decoder_free((vorbis_decoder_t *)music->vorbis_dec);
    if (music->mp3_stream)
        mp3_stream_free((mp3_stream_t *)music->mp3_stream);
    free(music->filepath);
    free(music->leftover_buf);

    /* Free buffers */
    for (int32_t i = 0; i < VAUD_MUSIC_BUFFER_COUNT; i++) {
        free(music->buffers[i]);
    }
    free(music->resample_buf);
    free(music->wav_read_buf);

    if (music->refill_event_ready)
        vaud_event_destroy(&music->refill_event);

    free(music);
}

/// @copydoc vaud_detach_music
void vaud_detach_music(vaud_music_t music) {
    if (!music)
        return;

    vaud_context_t ctx = music->ctx;
    if (ctx) {
        vaud_music_wait_for_refill(ctx, music);
        vaud_mutex_lock(&ctx->mutex);
        for (int32_t i = 0; i < ctx->music_count; i++) {
            if (ctx->active_music[i] == music) {
                for (int32_t j = i; j < ctx->music_count - 1; j++)
                    ctx->active_music[j] = ctx->active_music[j + 1];
                ctx->music_count--;
                ctx->active_music[ctx->music_count] = NULL;
                break;
            }
        }
        music->refill_in_progress = 0;
        if (music->refill_event_ready)
            vaud_event_set(&music->refill_event);
        music->state = VAUD_MUSIC_STOPPED;
        music->ctx = NULL;
        vaud_mutex_unlock(&ctx->mutex);
    }
    if (!ctx) {
        music->state = VAUD_MUSIC_STOPPED;
        music->ctx = NULL;
    }
}

/// @copydoc vaud_music_is_attached
int vaud_music_is_attached(vaud_music_t music) {
    return (music && music->ctx) ? 1 : 0;
}

/// @copydoc vaud_music_play
void vaud_music_play(vaud_music_t music, int loop) {
    if (!music || vaud_context_is_destroying(music->ctx))
        return;

    vaud_context_t ctx = music->ctx;
    vaud_music_wait_for_refill(ctx, music);

    vaud_mutex_lock(&ctx->mutex);
    music->loop = loop ? 1 : 0;
    if (music->state == VAUD_MUSIC_STOPPED) {
        if (!vaud_music_begin_forced_refill_locked(music)) {
            vaud_mutex_unlock(&ctx->mutex);
            return;
        }
        vaud_mutex_unlock(&ctx->mutex);

        int ok = vaud_music_seek_output_frame(music, 0);
        if (ok)
            vaud_music_prefill_forced(music);

        vaud_mutex_lock(&ctx->mutex);
        if (music->ctx == ctx) {
            music->state = ok ? VAUD_MUSIC_PLAYING : VAUD_MUSIC_STOPPED;
            if (!ok)
                music->stream_eof = 1;
        }
        vaud_music_finish_refill_locked(music);
        vaud_mutex_unlock(&ctx->mutex);
        return;
    } else {
        music->state = VAUD_MUSIC_PLAYING;
    }
    vaud_mutex_unlock(&ctx->mutex);

    vaud_update(ctx);
}

/// @copydoc vaud_music_stop
void vaud_music_stop(vaud_music_t music) {
    if (!music || vaud_context_is_destroying(music->ctx))
        return;

    vaud_context_t ctx = music->ctx;
    vaud_music_wait_for_refill(ctx, music);

    vaud_mutex_lock(&ctx->mutex);

    music->state = VAUD_MUSIC_STOPPED;
    music->refill_in_progress = 0;
    if (music->refill_event_ready)
        vaud_event_set(&music->refill_event);
    music->position = 0;
    music->source_position = 0;
    music->stream_output_generated = 0;
    music->stream_eof = 0;
    vaud_music_clear_stream_buffers(music);

    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_music_pause
void vaud_music_pause(vaud_music_t music) {
    if (!music || vaud_context_is_destroying(music->ctx))
        return;

    vaud_mutex_lock(&music->ctx->mutex);
    if (music->state == VAUD_MUSIC_PLAYING) {
        music->state = VAUD_MUSIC_PAUSED;
    }
    vaud_mutex_unlock(&music->ctx->mutex);
}

/// @copydoc vaud_music_resume
void vaud_music_resume(vaud_music_t music) {
    if (!music || vaud_context_is_destroying(music->ctx))
        return;

    vaud_context_t ctx = music->ctx;
    vaud_mutex_lock(&music->ctx->mutex);
    if (music->state == VAUD_MUSIC_PAUSED) {
        music->state = VAUD_MUSIC_PLAYING;
    }
    vaud_mutex_unlock(&music->ctx->mutex);
    vaud_update(ctx);
}

/// @copydoc vaud_music_set_loop
void vaud_music_set_loop(vaud_music_t music, int loop) {
    if (!music)
        return;

    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        music->loop = loop ? 1 : 0;
        vaud_mutex_unlock(&music->ctx->mutex);
    } else {
        music->loop = loop ? 1 : 0;
    }
}

/// @copydoc vaud_music_set_volume
void vaud_music_set_volume(vaud_music_t music, float volume) {
    if (!music)
        return;

    volume = vaud_clamp_unit_float(volume);

    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        music->volume = volume;
        vaud_mutex_unlock(&music->ctx->mutex);
    } else {
        music->volume = volume;
    }
}

/// @copydoc vaud_music_set_group
void vaud_music_set_group(vaud_music_t music, int64_t group_id) {
    if (!music)
        return;

    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        music->group_id = group_id;
        vaud_mutex_unlock(&music->ctx->mutex);
    } else {
        music->group_id = group_id;
    }
}

/// @copydoc vaud_music_get_volume
float vaud_music_get_volume(vaud_music_t music) {
    if (!music)
        return 0.0f;
    /* H-3: read volume under mutex (setter holds it; torn read possible on ARM64) */
    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        float vol = music->volume;
        vaud_mutex_unlock(&music->ctx->mutex);
        return vol;
    }
    return music->volume;
}

/// @copydoc vaud_music_is_playing
int vaud_music_is_playing(vaud_music_t music) {
    if (!music)
        return 0;
    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        int playing = (music->state == VAUD_MUSIC_PLAYING) ? 1 : 0;
        vaud_mutex_unlock(&music->ctx->mutex);
        return playing;
    }
    return (music->state == VAUD_MUSIC_PLAYING) ? 1 : 0;
}

/// @copydoc vaud_music_seek
void vaud_music_seek(vaud_music_t music, float seconds) {
    if (!music || vaud_context_is_destroying(music->ctx))
        return;
    if (!isfinite(seconds))
        return;

    vaud_context_t ctx = music->ctx;
    vaud_music_wait_for_refill(ctx, music);

    double target = (double)seconds * (double)music->sample_rate;
    if (target < 0.0)
        target = 0.0;
    if (music->frame_count > 0 && target > (double)music->frame_count)
        target = (double)music->frame_count;
    if (target > (double)INT64_MAX)
        target = (double)INT64_MAX;

    int64_t target_frame = (int64_t)target;

    vaud_mutex_lock(&ctx->mutex);
    if (!vaud_music_begin_forced_refill_locked(music)) {
        vaud_mutex_unlock(&ctx->mutex);
        return;
    }
    vaud_mutex_unlock(&ctx->mutex);

    int ok = vaud_music_seek_output_frame(music, target_frame);
    if (ok)
        vaud_music_prefill_forced(music);

    vaud_mutex_lock(&ctx->mutex);
    if (music->ctx == ctx) {
        if (!ok) {
            music->state = VAUD_MUSIC_STOPPED;
            music->stream_eof = 1;
            vaud_music_clear_stream_buffers(music);
        }
    }
    vaud_music_finish_refill_locked(music);
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_music_get_position
float vaud_music_get_position(vaud_music_t music) {
    if (!music)
        return 0.0f;
    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        float position =
            music->sample_rate > 0 ? (float)music->position / (float)music->sample_rate : 0.0f;
        vaud_mutex_unlock(&music->ctx->mutex);
        return position;
    }
    return music->sample_rate > 0 ? (float)music->position / (float)music->sample_rate : 0.0f;
}

/// @copydoc vaud_music_get_duration
float vaud_music_get_duration(vaud_music_t music) {
    if (!music)
        return 0.0f;
    if (music->ctx && !vaud_context_is_destroying(music->ctx)) {
        vaud_mutex_lock(&music->ctx->mutex);
        float duration =
            music->sample_rate > 0 ? (float)music->frame_count / (float)music->sample_rate : 0.0f;
        vaud_mutex_unlock(&music->ctx->mutex);
        return duration;
    }
    return music->sample_rate > 0 ? (float)music->frame_count / (float)music->sample_rate : 0.0f;
}

//===----------------------------------------------------------------------===//
// Utility Functions
//===----------------------------------------------------------------------===//

/// @copydoc vaud_get_active_voice_count
int32_t vaud_get_active_voice_count(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return 0;

    int32_t count = 0;
    vaud_mutex_lock(&ctx->mutex);
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        if (ctx->voices[i].state == VAUD_VOICE_PLAYING) {
            count++;
        }
    }
    vaud_mutex_unlock(&ctx->mutex);

    return count;
}

/// @copydoc vaud_stop_all_sounds
void vaud_stop_all_sounds(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return;

    vaud_mutex_lock(&ctx->mutex);
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        ctx->voices[i].state = VAUD_VOICE_INACTIVE;
        ctx->voices[i].sound = NULL;
    }
    vaud_mutex_unlock(&ctx->mutex);
}

/// @copydoc vaud_get_latency_ms
float vaud_get_latency_ms(vaud_context_t ctx) {
    if (vaud_context_is_destroying(ctx))
        return 0.0f;

    /* Latency is approximately buffer size in frames / sample rate */
    return (float)VAUD_BUFFER_FRAMES / (float)VAUD_SAMPLE_RATE * 1000.0f;
}

/// @brief Copy mixer/backend diagnostic counters from an audio context.
/// @details The fields are read atomically so callers can sample them while the mixer thread is
///          running. A NULL destination is ignored; a NULL context produces a zero-filled snapshot.
/// @param ctx Audio context to inspect.
/// @param out_stats Destination statistics snapshot.
void vaud_get_stats(vaud_context_t ctx, vaud_stats_t *out_stats) {
    if (!out_stats)
        return;

    memset(out_stats, 0, sizeof(*out_stats));
    if (!ctx)
        return;

    out_stats->render_calls = vaud_atomic_load_u64(&ctx->stats.render_calls);
    out_stats->mixer_lock_misses = vaud_atomic_load_u64(&ctx->stats.mixer_lock_misses);
    out_stats->backend_write_calls = vaud_atomic_load_u64(&ctx->stats.backend_write_calls);
    out_stats->backend_partial_writes = vaud_atomic_load_u64(&ctx->stats.backend_partial_writes);
    out_stats->backend_waits = vaud_atomic_load_u64(&ctx->stats.backend_waits);
    out_stats->backend_xruns = vaud_atomic_load_u64(&ctx->stats.backend_xruns);
    out_stats->backend_recoveries = vaud_atomic_load_u64(&ctx->stats.backend_recoveries);
    out_stats->backend_write_failures = vaud_atomic_load_u64(&ctx->stats.backend_write_failures);
}

/// @copydoc vaud_set_group_effects_processor
void vaud_set_group_effects_processor(vaud_context_t ctx,
                                      vaud_group_effects_query_fn query_fn,
                                      vaud_group_effects_process_fn process_fn,
                                      void *userdata) {
    if (vaud_context_is_destroying(ctx))
        return;

    vaud_mutex_lock(&ctx->mutex);
    ctx->group_effects_query = process_fn ? query_fn : NULL;
    ctx->group_effects_process = process_fn;
    ctx->group_effects_userdata = process_fn ? userdata : NULL;
    vaud_mutex_unlock(&ctx->mutex);
}
