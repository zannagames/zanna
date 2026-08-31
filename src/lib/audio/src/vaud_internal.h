//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD Internal Definitions
//
// Internal structures and functions shared between the core library and
// platform backends. Not part of the public API.
//
// Key structures:
// - vaud_context: Main audio context with mixer, voice pool, platform data
// - vaud_sound: Loaded PCM audio data for sound effects
// - vaud_music: Streaming music state with file handle and buffers
// - vaud_voice: Individual playback instance state
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief ZannaAUD internal definitions (not public API).
/// @details Defines shared mixer state, streaming buffers, platform
///          synchronization types, atomic counter helpers, and internal
///          contracts used by the core and backend translation units.

#pragma once

#include "vaud.h"
#include <stddef.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Platform Threading Abstraction
//===----------------------------------------------------------------------===//

#if defined(VAUD_PLATFORM_WINDOWS)
#include <windows.h>
typedef CRITICAL_SECTION vaud_mutex_t;
typedef HANDLE vaud_thread_t;
typedef HANDLE vaud_event_t;
#else
#include <pthread.h>
typedef pthread_mutex_t vaud_mutex_t;
typedef pthread_t vaud_thread_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int signaled;
} vaud_event_t;
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/// @brief Atomically store a 32-bit control value with release semantics.
/// @param ptr Target control field.
/// @param value Value to publish.
static inline void vaud_atomic_store_i32(volatile int *ptr, int value) {
    (void)_InterlockedExchange((volatile long *)ptr, value);
}

/// @brief Atomically load a 32-bit control value with acquire semantics.
/// @param ptr Control field to read.
/// @return Current value.
static inline int vaud_atomic_load_i32(const volatile int *ptr) {
    return (int)_InterlockedCompareExchange((volatile long *)ptr, 0, 0);
}

/// @brief Atomically add to a relaxed 64-bit diagnostic counter.
/// @param ptr Counter to update.
/// @param value Increment.
static inline void vaud_atomic_add_u64(volatile uint64_t *ptr, uint64_t value) {
    (void)_InterlockedExchangeAdd64((volatile long long *)ptr, (long long)value);
}

/// @brief Atomically load a relaxed 64-bit diagnostic counter.
/// @param ptr Counter to read.
/// @return Current counter value.
static inline uint64_t vaud_atomic_load_u64(const volatile uint64_t *ptr) {
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)ptr, 0, 0);
}
#else
/// @brief Atomically store a 32-bit control value with release semantics.
/// @param ptr Target control field.
/// @param value Value to publish.
static inline void vaud_atomic_store_i32(volatile int *ptr, int value) {
    __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

/// @brief Atomically load a 32-bit control value with acquire semantics.
/// @param ptr Control field to read.
/// @return Current value.
static inline int vaud_atomic_load_i32(const volatile int *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

/// @brief Atomically add to a relaxed 64-bit diagnostic counter.
/// @param ptr Counter to update.
/// @param value Increment.
static inline void vaud_atomic_add_u64(volatile uint64_t *ptr, uint64_t value) {
    __atomic_fetch_add(ptr, value, __ATOMIC_RELAXED);
}

/// @brief Atomically load a relaxed 64-bit diagnostic counter.
/// @param ptr Counter to read.
/// @return Current counter value.
static inline uint64_t vaud_atomic_load_u64(const volatile uint64_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}
#endif

/// @brief Atomically increment one audio diagnostic counter.
/// @param counter Counter field inside vaud_context::stats.
/// @param amount Amount to add, usually 1.
static inline void vaud_stats_add(volatile uint64_t *counter, uint64_t amount) {
    if (counter && amount)
        vaud_atomic_add_u64(counter, amount);
}

//===----------------------------------------------------------------------===//
// Voice State
//===----------------------------------------------------------------------===//

/// @brief State of a playback voice.
typedef enum {
    VAUD_VOICE_INACTIVE = 0, ///< Voice is available
    VAUD_VOICE_PLAYING,      ///< Voice is actively playing
    VAUD_VOICE_STOPPING      ///< Voice is fading out (future use)
} vaud_voice_state;

/// @brief Individual sound playback instance.
/// @details Tracks position, volume, and pan for one playing sound.
/// @invariant If state != INACTIVE, sound must be valid.
typedef struct {
    vaud_voice_state state; ///< Current voice state
    vaud_sound_t sound;     ///< Source sound data (NULL if inactive)
    int64_t position;       ///< Current sample position (in frames)
    float volume;           ///< Voice volume (0.0 to 1.0)
    float pan;              ///< Stereo pan (-1.0 to 1.0)
    int loop;               ///< Loop flag (0 = one-shot, 1 = loop)
    int32_t id;             ///< Unique voice ID for external reference
    int64_t start_time;     ///< Frame count when voice started (for age-based stealing)
    int64_t group_id;       ///< Logical mix-group id for optional bus processing.
    float pitch;            ///< Playback-rate multiplier (0.25–4.0; 1.0 = native rate)
    double frac_pos;        ///< Fractional frame cursor (authoritative when resampling)
    float lowpass_cutoff;   ///< Direct per-voice lowpass cutoff in Hz (<= 0 = bypass)
    float occlusion_target; ///< Requested occlusion 0 (open) .. 1 (fully occluded)
    float occlusion_smooth; ///< Smoothed occlusion actually applied (anti-zipper)
    float lp_state_l;       ///< One-pole lowpass filter state, left channel
    float lp_state_r;       ///< One-pole lowpass filter state, right channel
    int metering;           ///< Per-voice RMS metering enabled (zero cost when off)
    float level;            ///< RMS of the last mixed block (pre-gain source level)
} vaud_voice;

//===----------------------------------------------------------------------===//
// Group Ducking
//===----------------------------------------------------------------------===//

/// @brief Playback-rate (pitch) clamp range for per-voice resampling.
#define VAUD_PITCH_MIN 0.25f
#define VAUD_PITCH_MAX 4.0f

/// @brief Reset a voice's DSP state (pitch, fractional cursor, filters) to
///        pass-through defaults. Called whenever a voice is (re)started so a
///        recycled pool slot never inherits a previous sound's pitch/filter.
/// @param voice Voice slot whose DSP fields are reinitialized.
static inline void vaud_voice_reset_dsp(vaud_voice *voice) {
    voice->pitch = 1.0f;
    voice->frac_pos = 0.0;
    voice->lowpass_cutoff = 0.0f;
    voice->occlusion_target = 0.0f;
    voice->occlusion_smooth = 0.0f;
    voice->metering = 0;
    voice->level = 0.0f;
    voice->lp_state_l = 0.0f;
    voice->lp_state_r = 0.0f;
}

/// @brief Maximum simultaneous (trigger, target) ducking rules per context.
#define VAUD_MAX_DUCK_RULES 8

/// @brief Sidechain-style group ducking rule.
/// @details While any voice in @c trigger_group is audible, the gain applied
///          to @c target_group eases toward (1 - amount) at the attack rate;
///          otherwise it recovers toward 1.0 at the release rate. Rates are
///          per-second fractions derived from the configured attack/release
///          times; @c gain is the current envelope value.
typedef struct {
    int64_t trigger_group; ///< Group whose activity triggers the duck.
    int64_t target_group;  ///< Group whose gain is reduced.
    float amount;          ///< Duck depth 0..1 (gain floor = 1 - amount).
    float attack_sec;      ///< Seconds to reach the ducked gain.
    float release_sec;     ///< Seconds to recover to unity.
    float gain;            ///< Current envelope gain (1.0 = no duck).
} vaud_duck_rule;

//===----------------------------------------------------------------------===//
// Sound Structure
//===----------------------------------------------------------------------===//

/// @brief Loaded sound effect data.
/// @details Contains PCM audio data in the internal format (16-bit stereo, 44.1kHz).
/// @invariant samples != NULL after successful load.
/// @invariant frame_count > 0 after successful load.
struct vaud_sound {
    vaud_context_t ctx;      ///< Owning context
    int16_t *samples;        ///< Interleaved stereo PCM data
    int64_t frame_count;     ///< Number of frames (samples / channels)
    int32_t sample_rate;     ///< Internal sample rate (always VAUD_SAMPLE_RATE).
    int32_t channels;        ///< Internal channel count (always VAUD_CHANNELS).
    int32_t source_channels; ///< Original source channel count for pan semantics.
    float default_volume;    ///< Default playback volume
};

//===----------------------------------------------------------------------===//
// Music Structure
//===----------------------------------------------------------------------===//

/// @brief Music stream state.
typedef enum {
    VAUD_MUSIC_STOPPED = 0, ///< Not playing
    VAUD_MUSIC_PLAYING,     ///< Actively playing
    VAUD_MUSIC_PAUSED       ///< Paused at current position
} vaud_music_state;

/// @brief Streaming music instance.
/// @details Manages file I/O, buffering, and playback state for streamed audio.
struct vaud_music {
    vaud_context_t ctx;         ///< Owning context
    void *file;                 ///< FILE pointer for streaming
    int64_t data_offset;        ///< Offset to PCM data in file
    int64_t data_size;          ///< Total PCM data size in bytes
    int64_t frame_count;        ///< Total output frames after any resampling
    int64_t source_position;    ///< Source frames consumed from a WAV data chunk
    int32_t sample_rate;        ///< Playback sample rate (always mixer rate)
    int32_t source_sample_rate; ///< Source file/stream sample rate before resampling
    int32_t channels;           ///< File channel count
    int32_t bits_per_sample;    ///< Bits per sample in file
    int32_t audio_format;       ///< Source sample encoding (PCM or IEEE float)

    vaud_music_state state; ///< Current playback state
    int64_t position;       ///< Current frame position
    int loop;               ///< Loop flag
    float volume;           ///< Playback volume
    int64_t group_id;       ///< Logical mix-group id for optional bus processing.

    // Streaming buffers
    int16_t *buffers[VAUD_MUSIC_BUFFER_COUNT];      ///< Decoded audio buffers
    int32_t buffer_frames[VAUD_MUSIC_BUFFER_COUNT]; ///< Frames in each buffer
    int32_t current_buffer;                         ///< Index of buffer being played
    int32_t buffer_position;                        ///< Frame position within current buffer
    int stream_eof;                                 ///< Decoder reached EOF while pre-filling.
    int stream_loop_pending;                        ///< Mixer requested a loop rewind.
    int refill_in_progress;                         ///< Non-realtime thread is mutating buffers.
    int refill_event_ready;    ///< Refill completion event has been initialized.
    vaud_event_t refill_event; ///< Signals that non-realtime refill work is done.
    int buffer_refilling[VAUD_MUSIC_BUFFER_COUNT]; ///< Per-buffer refill ownership flags.
    int64_t stream_output_generated;               ///< Output frames decoded since last reset/seek.

    // Resampling support (allocated when sample_rate != VAUD_SAMPLE_RATE)
    int16_t *resample_buf; ///< Temp buffer for raw frames before resampling
    int64_t resample_cap;  ///< Capacity of resample_buf in frames
    double resample_phase; ///< Fractional source-frame offset carried between buffers.

    // WAV streaming scratch buffer (allocated before playback, never in mixer callback)
    uint8_t *wav_read_buf; ///< Raw bytes for WAV frame decode
    size_t wav_read_cap;   ///< Capacity of wav_read_buf in bytes

    // Format-specific streaming (0=WAV, 1=OGG, 2=MP3)
    int format;          ///< Audio format identifier
    void *ogg_reader;    ///< ogg_reader_t* for OGG streaming
    void *vorbis_dec;    ///< vorbis_decoder_t* for OGG streaming
    void *mp3_stream;    ///< mp3_stream_t* for MP3 streaming
    uint32_t ogg_serial; ///< Selected Vorbis logical stream for OGG playback
    char *filepath;      ///< Retained source path (for diagnostics/future reopen paths)

    // Leftover PCM from last decode (variable packet sizes)
    int16_t *leftover_buf;   ///< Excess decoded frames
    int32_t leftover_frames; ///< Number of leftover frames
    int32_t leftover_cap;    ///< Capacity of leftover buffer in frames
};

//===----------------------------------------------------------------------===//
// Context Structure
//===----------------------------------------------------------------------===//

/// @brief Main audio context.
/// @details Contains all audio state: mixer, voices, loaded resources, platform data.
/// @invariant voices array is always valid.
/// @invariant platform_data is valid after successful vaud_create().
struct vaud_context {
    // Mixer state
    float master_volume;                ///< Master volume (0.0 to 1.0)
    int device_output_silent;           ///< Zero device-bound PCM after mixing when requested.
    vaud_voice voices[VAUD_MAX_VOICES]; ///< Voice pool
    int32_t next_voice_id;              ///< Counter for unique voice IDs
    int64_t frame_counter;              ///< Total frames rendered (for timing)

    // Sidechain-style group ducking rules (updated in the mixer render pass).
    vaud_duck_rule duck_rules[VAUD_MAX_DUCK_RULES]; ///< Active ducking rules.
    int32_t duck_rule_count;                        ///< Number of active rules.

    // Music (single active music stream for simplicity)
    vaud_music_t active_music[VAUD_MAX_MUSIC]; ///< Active music streams
    int32_t music_count;                       ///< Number of active music streams

    // Loaded sound handles retained so vaud_destroy can detach them safely.
    vaud_sound_t loaded_sounds[VAUD_MAX_SOUNDS]; ///< Caller-owned sound handles.
    int32_t sound_count;                         ///< Number of tracked sound handles.

    // H-1: Pre-allocated mix accumulator — avoids malloc() inside the real-time audio callback.
    int32_t accum_buf[VAUD_BUFFER_FRAMES * VAUD_CHANNELS]; ///< 32-bit mix accumulator (RT-safe)
    int32_t group_accum_buf[VAUD_BUFFER_FRAMES * VAUD_CHANNELS]; ///< Per-group scratch bus.
    float group_fx_buf[VAUD_BUFFER_FRAMES * VAUD_CHANNELS]; ///< Float scratch for group effects.
    int16_t last_output_buf[VAUD_BUFFER_FRAMES * VAUD_CHANNELS]; ///< Last rendered period fallback.
    int32_t last_output_frames;                      ///< Number of valid frames in last_output_buf.
    vaud_group_effects_query_fn group_effects_query; ///< Optional group-effects query hook.
    vaud_group_effects_process_fn group_effects_process; ///< Optional group-effects processor.
    void *group_effects_userdata; ///< User data forwarded to group-effects hooks.
    vaud_stats_t stats;           ///< Mixer/backend diagnostic counters.

    // Thread synchronization
    vaud_mutex_t mutex;      ///< Protects voice and music state
    volatile int running;    ///< Audio thread running flag
    volatile int paused;     ///< Global pause flag
    volatile int destroying; ///< Context teardown is in progress.

    // Background music streamer (see vaud.c): keeps ring buffers refilled
    // when the app thread stalls between vaud_update() calls.
    vaud_thread_t streamer_thread; ///< Streamer thread handle (valid when started).
    int streamer_thread_started;   ///< Nonzero while streamer_thread is joinable.
    volatile int streamer_running; ///< Streamer loop control flag.

    // Platform-specific data
    void *platform_data; ///< Platform backend state (AudioQueue, ALSA, WASAPI)
};

//===----------------------------------------------------------------------===//
// Mixer Functions
//===----------------------------------------------------------------------===//

/// @brief Render mixed audio into output buffer.
/// @details Called by platform backend to fill audio buffers. Mixes all active
///          voices and music into the output buffer. Thread-safe.
/// @param ctx Audio context.
/// @param output Output buffer (interleaved stereo 16-bit PCM).
/// @param frames Number of frames to render.
void vaud_mixer_render(vaud_context_t ctx, int16_t *output, int32_t frames);

/// @brief Render one device-bound audio buffer, applying the process-level
///        silent-output policy after advancing normal mixer state.
/// @details Unlike vaud_mixer_render(), this entry point zeros the completed PCM
///          when ZANNA_AUDIO_SILENT was enabled as the context was created.
///          Platform backends must use this function; offline mixer tests should
///          continue to call vaud_mixer_render() directly.
/// @param ctx Audio context.
/// @param output Device-bound interleaved signed-16 PCM buffer.
/// @param frames Number of frames to advance and render.
void vaud_mixer_render_device(vaud_context_t ctx, int16_t *output, int32_t frames);

/// @brief Allocate a voice for playback.
/// @details Finds an inactive voice or steals the oldest if none available.
/// @param ctx Audio context.
/// @return Pointer to allocated voice, or NULL if context is invalid.
vaud_voice *vaud_alloc_voice(vaud_context_t ctx);

/// @brief Find a voice by ID.
/// @param ctx Audio context.
/// @param id Voice ID to find.
/// @return Pointer to voice, or NULL if not found.
vaud_voice *vaud_find_voice(vaud_context_t ctx, vaud_voice_id id);

//===----------------------------------------------------------------------===//
// WAV Parser Functions
//===----------------------------------------------------------------------===//

/// @brief Parse WAV file from disk.
/// @param path File path.
/// @param out_samples Output: allocated sample buffer (caller must free).
/// @param out_frames Output: number of frames.
/// @param out_sample_rate Output: sample rate.
/// @param out_channels Output: channel count.
/// @return 1 on success, 0 on failure.
int vaud_wav_load_file(const char *path,
                       int16_t **out_samples,
                       int64_t *out_frames,
                       int32_t *out_sample_rate,
                       int32_t *out_channels);

/// @brief Parse WAV file from memory.
/// @param data Pointer to WAV data.
/// @param size Size of data in bytes.
/// @param out_samples Output: allocated sample buffer (caller must free).
/// @param out_frames Output: number of frames.
/// @param out_sample_rate Output: sample rate.
/// @param out_channels Output: channel count.
/// @return 1 on success, 0 on failure.
int vaud_wav_load_mem(const void *data,
                      size_t size,
                      int16_t **out_samples,
                      int64_t *out_frames,
                      int32_t *out_sample_rate,
                      int32_t *out_channels);

/// @brief Open WAV file for streaming (music).
/// @param path File path.
/// @param out_file Output: file handle.
/// @param out_data_offset Output: byte offset to PCM data.
/// @param out_data_size Output: size of PCM data in bytes.
/// @param out_frames Output: total frame count.
/// @param out_sample_rate Output: sample rate.
/// @param out_channels Output: channel count.
/// @param out_bits Output: bits per sample.
/// @param out_format Output: WAV encoding identifier (PCM or IEEE float).
/// @return 1 on success, 0 on failure.
int vaud_wav_open_stream(const char *path,
                         void **out_file,
                         int64_t *out_data_offset,
                         int64_t *out_data_size,
                         int64_t *out_frames,
                         int32_t *out_sample_rate,
                         int32_t *out_channels,
                         int32_t *out_bits,
                         int32_t *out_format);

/// @brief Seek a WAV stream using a 64-bit file offset.
/// @param file File handle returned by vaud_wav_open_stream().
/// @param offset Signed byte offset interpreted according to @p origin.
/// @param origin Standard seek origin constant.
/// @return 1 on success, 0 on invalid input, range failure, or I/O error.
int vaud_wav_seek_stream(void *file, int64_t offset, int origin);

/// @brief Read frames from a streaming WAV file.
/// @param file File handle from vaud_wav_open_stream.
/// @param samples Output buffer (must hold frames * channels samples).
/// @param frames Number of frames to read.
/// @param channels Channel count.
/// @param bits_per_sample Bits per sample.
/// @param audio_format WAV encoding identifier.
/// @return Number of frames actually read.
int32_t vaud_wav_read_frames(void *file,
                             int16_t *samples,
                             int32_t frames,
                             int32_t channels,
                             int32_t bits_per_sample,
                             int32_t audio_format);

/// @brief Read frames using caller-owned raw byte storage.
/// @details Same conversion behavior as `vaud_wav_read_frames`, but avoids
///          allocating inside the mixer path.
/// @param file File handle from vaud_wav_open_stream().
/// @param samples Output interleaved signed-16 sample buffer.
/// @param frames Maximum frames to read.
/// @param channels Source channel count.
/// @param bits_per_sample Source bits per sample.
/// @param audio_format Source WAV encoding identifier.
/// @param temp Caller-owned raw byte scratch storage.
/// @param temp_size Scratch capacity in bytes.
/// @return Number of complete frames decoded.
int32_t vaud_wav_read_frames_buffered(void *file,
                                      int16_t *samples,
                                      int32_t frames,
                                      int32_t channels,
                                      int32_t bits_per_sample,
                                      int32_t audio_format,
                                      uint8_t *temp,
                                      size_t temp_size);

//===----------------------------------------------------------------------===//
// Resampling Functions
//===----------------------------------------------------------------------===//

/// @brief Resample audio to target sample rate.
/// @details Uses cubic interpolation for better quality while keeping the
///          implementation dependency-free.
/// @param input Input samples (interleaved stereo).
/// @param in_frames Number of input frames.
/// @param in_rate Input sample rate.
/// @param output Output buffer (must be pre-allocated).
/// @param out_frames Expected output frame count.
/// @param out_rate Output sample rate (VAUD_SAMPLE_RATE).
/// @param channels Number of channels.
void vaud_resample(const int16_t *input,
                   int64_t in_frames,
                   int32_t in_rate,
                   int16_t *output,
                   int64_t out_frames,
                   int32_t out_rate,
                   int32_t channels);

/// @brief Calculate output frame count after resampling.
/// @param in_frames Input frame count.
/// @param in_rate Input sample rate.
/// @param out_rate Output sample rate.
/// @return Required output frame count.
int64_t vaud_resample_output_frames(int64_t in_frames, int32_t in_rate, int32_t out_rate);

/// @brief Compute the byte size of an interleaved int16 PCM buffer.
/// @param frames Number of sample frames.
/// @param channels Interleaved channel count.
/// @param out_bytes Output byte count set on success.
/// @return 1 on success, 0 if frames/channels are invalid or overflow size_t.
int vaud_pcm_s16_buffer_size(int64_t frames, int32_t channels, size_t *out_bytes);

/// @brief Fill a music buffer with frames, resampling if necessary.
/// @details Reads raw frames from the music's WAV file and, when the source
///          sample rate differs from VAUD_SAMPLE_RATE, resamples via cubic
///          interpolation into the output buffer.
/// @param music Music stream instance.
/// @param buf_idx Index of the buffer to fill (0..VAUD_MUSIC_BUFFER_COUNT-1).
/// @return Number of output frames written to the buffer.
int32_t vaud_music_fill_buffer(struct vaud_music *music, int32_t buf_idx);

/// @brief Fill all empty music buffers that can be safely prepared off-callback.
/// @details For registered streams this is used only on already-claimed buffer
///          slots or while the caller holds exclusive stream ownership. Loaders
///          may call this before publishing a stream to the context because no
///          other thread can observe it yet.
/// @param music Stream whose available buffers are filled.
void vaud_music_prefill_locked(struct vaud_music *music);

/// @brief Rewind or seek a music stream to the given output frame.
/// @details Resets format-specific decoder state, primes buffer 0, and leaves
///          the stream ready for subsequent playback from the requested frame.
/// @param music Stream to reposition.
/// @param target_frame Nonnegative frame index in output-rate coordinates.
/// @return 1 on success, 0 if the stream could not be rewound or sought.
int vaud_music_seek_output_frame(struct vaud_music *music, int64_t target_frame);

//===----------------------------------------------------------------------===//
// Platform Backend Interface
//===----------------------------------------------------------------------===//

/// @brief Initialize platform audio backend.
/// @details Allocates platform_data, opens audio device, starts audio thread.
/// @param ctx Audio context.
/// @return 1 on success, 0 on failure.
int vaud_platform_init(vaud_context_t ctx);

/// @brief Shutdown platform audio backend.
/// @details Stops the audio thread, closes the device, and frees platform data
///          only after worker termination is proven.
/// @param ctx Audio context.
/// @return 1 after complete teardown; 0 when teardown cannot safely proceed.
int vaud_platform_shutdown(vaud_context_t ctx);

/// @brief Pause platform audio output.
/// @param ctx Audio context.
void vaud_platform_pause(vaud_context_t ctx);

/// @brief Resume platform audio output.
/// @param ctx Audio context.
void vaud_platform_resume(vaud_context_t ctx);

/// @brief Get current time in milliseconds.
/// @return Monotonic time in milliseconds.
int64_t vaud_platform_now_ms(void);

//===----------------------------------------------------------------------===//
// Thread Utilities
//===----------------------------------------------------------------------===//

/// @brief Initialize a mutex.
/// @param mutex Mutex storage to initialize.
/// @return 1 on success, 0 on failure.
int vaud_mutex_init(vaud_mutex_t *mutex);

/// @brief Destroy a mutex.
/// @param mutex Initialized mutex storage.
void vaud_mutex_destroy(vaud_mutex_t *mutex);

/// @brief Lock a mutex.
/// @param mutex Initialized mutex to acquire.
void vaud_mutex_lock(vaud_mutex_t *mutex);

/// @brief Unlock a mutex.
/// @param mutex Held mutex to release.
void vaud_mutex_unlock(vaud_mutex_t *mutex);

/// @brief Try to lock a mutex without blocking.
/// @param mutex Initialized mutex to acquire.
/// @return 1 if the lock was acquired, 0 otherwise.
int vaud_mutex_trylock(vaud_mutex_t *mutex);

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

/// @brief Set the thread-local error state.
/// @param code Error code.
/// @param msg Error message.
void vaud_set_error(vaud_error_t code, const char *msg);

#ifdef __cplusplus
}
#endif
