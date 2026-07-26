//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD macOS Platform Backend
//
// Implements audio output using Apple's AudioQueue Services, part of the
// AudioToolbox framework. AudioQueue provides a simple callback-based API
// for audio playback with automatic buffer management.
//
// Key concepts:
// - AudioQueue: Manages audio output and buffer scheduling
// - AudioQueueBuffer: Pre-allocated buffers filled by our callback
// - Triple buffering: Three buffers cycle through fill/play/wait states
//
// Thread model:
// - AudioQueue runs its own real-time audio thread
// - Our callback (audio_callback) is called on that thread
// - We call vaud_mixer_render_device() to fill buffers, which is thread-safe
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief macOS AudioQueue audio backend for ZannaAUD.

#ifdef __APPLE__

#include "vaud_internal.h"
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdlib.h>

//===----------------------------------------------------------------------===//
// Platform Data Structure
//===----------------------------------------------------------------------===//

/// @brief Number of audio buffers (triple buffering for smooth playback).
#define VAUD_MACOS_NUM_BUFFERS 3

static mach_timebase_info_data_t g_vaud_macos_timebase = {0};
static pthread_once_t g_vaud_macos_timebase_once = PTHREAD_ONCE_INIT;

/// @brief Initialize mach timebase conversion factors exactly once.
/// @details `vaud_platform_now_ms` can be called from multiple threads.  Using
///          pthread_once avoids racing on a lazily initialized static timebase
///          while keeping the fast path lock-free after initialization.
static void vaud_macos_init_timebase(void) {
    (void)mach_timebase_info(&g_vaud_macos_timebase);
    if (g_vaud_macos_timebase.numer == 0)
        g_vaud_macos_timebase.numer = 1;
    if (g_vaud_macos_timebase.denom == 0)
        g_vaud_macos_timebase.denom = 1;
}

/// @brief macOS-specific platform data.
/// @details Stores AudioQueue state and buffer references.
typedef struct {
    AudioQueueRef queue;                                 ///< The audio queue
    AudioQueueBufferRef buffers[VAUD_MACOS_NUM_BUFFERS]; ///< Audio buffers
    AudioStreamBasicDescription format;                  ///< Audio format description
    volatile int paused;                                 ///< Pause state
} vaud_macos_data;

//===----------------------------------------------------------------------===//
// Audio Callback
//===----------------------------------------------------------------------===//

/// @brief AudioQueue callback - fills a buffer with mixed audio.
/// @details Called by the audio system when a buffer finishes playing and
///          needs to be refilled. Runs on the audio thread.
/// @param user_data Pointer to our audio context.
/// @param queue The audio queue.
/// @param buffer The buffer to fill.
static void audio_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    vaud_context_t ctx = (vaud_context_t)user_data;
    if (!ctx || !buffer)
        return;

    vaud_macos_data *plat = (vaud_macos_data *)ctx->platform_data;

    int running = plat ? __atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE) : 0;
    if (!plat || !running || __atomic_load_n(&plat->paused, __ATOMIC_ACQUIRE)) {
        /* Fill with silence when paused or stopping */
        memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
    } else {
        /* Render mixed audio into the buffer */
        int32_t frames = VAUD_BUFFER_FRAMES;
        vaud_mixer_render_device(ctx, (int16_t *)buffer->mAudioData, frames);
    }

    /* Set actual data size */
    buffer->mAudioDataByteSize = VAUD_BUFFER_FRAMES * VAUD_CHANNELS * sizeof(int16_t);

    if (!running)
        return;

    /* Re-enqueue the buffer for playback */
    OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    if (status != noErr) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to re-enqueue AudioQueue buffer");
    }
}

//===----------------------------------------------------------------------===//
// Platform Interface Implementation
//===----------------------------------------------------------------------===//

int vaud_platform_init(vaud_context_t ctx) {
    if (!ctx)
        return 0;

    /* Allocate platform data */
    vaud_macos_data *plat = (vaud_macos_data *)calloc(1, sizeof(vaud_macos_data));
    if (!plat) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate macOS audio data");
        return 0;
    }

    ctx->platform_data = plat;
    __atomic_store_n(&plat->paused, 0, __ATOMIC_RELEASE);

    /* Configure audio format: 16-bit stereo PCM at 44.1kHz */
    plat->format.mSampleRate = (Float64)VAUD_SAMPLE_RATE;
    plat->format.mFormatID = kAudioFormatLinearPCM;
    plat->format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    plat->format.mBitsPerChannel = 16;
    plat->format.mChannelsPerFrame = VAUD_CHANNELS;
    plat->format.mFramesPerPacket = 1;
    plat->format.mBytesPerFrame = VAUD_CHANNELS * sizeof(int16_t);
    plat->format.mBytesPerPacket = plat->format.mBytesPerFrame;

    /* Create the audio queue */
    OSStatus status = AudioQueueNewOutput(&plat->format,
                                          audio_callback,
                                          ctx,  /* User data - our context */
                                          NULL, /* Run loop (NULL = internal) */
                                          NULL, /* Run loop mode */
                                          0,    /* Flags */
                                          &plat->queue);

    if (status != noErr) {
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to create AudioQueue");
        return 0;
    }

    /* Allocate and prime the audio buffers */
    UInt32 buffer_size = VAUD_BUFFER_FRAMES * VAUD_CHANNELS * sizeof(int16_t);

    for (int i = 0; i < VAUD_MACOS_NUM_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(plat->queue, buffer_size, &plat->buffers[i]);
        if (status != noErr) {
            /* Clean up already allocated buffers */
            for (int j = 0; j < i; j++) {
                AudioQueueFreeBuffer(plat->queue, plat->buffers[j]);
            }
            AudioQueueDispose(plat->queue, true);
            free(plat);
            ctx->platform_data = NULL;
            vaud_set_error(VAUD_ERR_PLATFORM, "Failed to allocate audio buffer");
            return 0;
        }

        /* Prime the buffer (fill with silence initially) */
        memset(plat->buffers[i]->mAudioData, 0, buffer_size);
        plat->buffers[i]->mAudioDataByteSize = buffer_size;

        /* Enqueue the buffer to start the callback chain */
        status = AudioQueueEnqueueBuffer(plat->queue, plat->buffers[i], 0, NULL);
        if (status != noErr) {
            for (int j = 0; j <= i; j++) {
                AudioQueueFreeBuffer(plat->queue, plat->buffers[j]);
            }
            AudioQueueDispose(plat->queue, true);
            free(plat);
            ctx->platform_data = NULL;
            vaud_set_error(VAUD_ERR_PLATFORM, "Failed to enqueue AudioQueue buffer");
            return 0;
        }
    }

    /* Start the audio queue */
    status = AudioQueueStart(plat->queue, NULL);
    if (status != noErr) {
        for (int i = 0; i < VAUD_MACOS_NUM_BUFFERS; i++) {
            AudioQueueFreeBuffer(plat->queue, plat->buffers[i]);
        }
        AudioQueueDispose(plat->queue, true);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to start AudioQueue");
        return 0;
    }

    return 1;
}

int vaud_platform_shutdown(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return 1;

    vaud_macos_data *plat = (vaud_macos_data *)ctx->platform_data;

    /* Stop and dispose the queue (this also frees buffers) */
    AudioQueueStop(plat->queue, true); /* true = immediate stop */
    AudioQueueDispose(plat->queue, true);

    free(plat);
    ctx->platform_data = NULL;
    return 1;
}

void vaud_platform_pause(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_macos_data *plat = (vaud_macos_data *)ctx->platform_data;
    __atomic_store_n(&plat->paused, 1, __ATOMIC_RELEASE);

    OSStatus status = AudioQueuePause(plat->queue);
    if (status != noErr) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to pause AudioQueue");
    }
}

void vaud_platform_resume(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_macos_data *plat = (vaud_macos_data *)ctx->platform_data;
    __atomic_store_n(&plat->paused, 0, __ATOMIC_RELEASE);

    OSStatus status = AudioQueueStart(plat->queue, NULL);
    if (status != noErr) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to resume AudioQueue");
    }
}

//===----------------------------------------------------------------------===//
// Timing
//===----------------------------------------------------------------------===//

int64_t vaud_platform_now_ms(void) {
    pthread_once(&g_vaud_macos_timebase_once, vaud_macos_init_timebase);

    uint64_t ticks = mach_absolute_time();
    long double nanos = ((long double)ticks * (long double)g_vaud_macos_timebase.numer) /
                        (long double)g_vaud_macos_timebase.denom;
    if (nanos > (long double)INT64_MAX)
        return INT64_MAX / 1000000;
    return (int64_t)(nanos / 1000000);
}

#endif /* __APPLE__ */
