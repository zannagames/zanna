//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/audio/src/vaud_platform_linux.c
// Purpose: Implement the Linux ALSA playback backend.
//
// Key invariants:
//   - One dedicated thread mixes and writes complete interleaved periods.
//   - PCM operations are serialized and every recovery result is checked.
//   - Pause waits terminate the backend on unexpected synchronization errors.
//
// Ownership/Lifetime:
//   - Platform state owns the ALSA handle, mix buffer, thread, mutexes, and
//     condition variable until vaud_platform_shutdown().
//
// Links: src/lib/audio/src/vaud_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Linux ALSA playback backend for ZannaAUD.
/// @details Configures an exact nonblocking stereo PCM stream and owns the
///          dedicated render/write thread. ALSA handle operations are
///          serialized across playback, pause, recovery, and shutdown, while
///          backend counters expose partial writes, waits, xruns, recoveries,
///          and terminal failures.

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(__linux__)

#include "vaud_internal.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//===----------------------------------------------------------------------===//
// Platform Data Structure
//===----------------------------------------------------------------------===//

/// @brief Linux ALSA platform data.
typedef struct {
    snd_pcm_t *pcm;              ///< ALSA PCM device handle
    pthread_t thread;            ///< Audio thread
    int thread_started;          ///< pthread_create succeeded
    int16_t *mix_buffer;         ///< Preallocated audio thread mixing buffer
    int running;                 ///< Thread running flag accessed with GCC atomics.
    int paused;                  ///< Pause state accessed with GCC atomics.
    pthread_mutex_t pcm_mutex;   ///< Serializes ALSA PCM handle operations.
    pthread_mutex_t pause_mutex; ///< Protects pause state
    pthread_cond_t pause_cond;   ///< Signal for pause/resume
} vaud_linux_data;

/// @brief Lock the ALSA PCM handle for one backend operation group.
/// @details ALSA PCM handles are shared between the audio thread and public
///          pause/resume/shutdown calls. This mutex keeps prepare, recover,
///          pause, drop, and write calls from racing each other.
/// @param plat Linux audio platform state.
static void alsa_pcm_lock(vaud_linux_data *plat) {
    if (plat)
        pthread_mutex_lock(&plat->pcm_mutex);
}

/// @brief Unlock a PCM operation group previously locked by alsa_pcm_lock().
/// @param plat Linux audio platform state.
static void alsa_pcm_unlock(vaud_linux_data *plat) {
    if (plat)
        pthread_mutex_unlock(&plat->pcm_mutex);
}

/// @brief Prepare an ALSA stream and publish any failure through ZannaAUD diagnostics.
/// @param ctx Context whose backend counters and error state are updated.
/// @param plat Linux state containing the locked PCM handle.
/// @return 1 when the stream was prepared; otherwise 0.
static int alsa_prepare_checked(vaud_context_t ctx, vaud_linux_data *plat) {
    int result = snd_pcm_prepare(plat->pcm);
    if (result < 0) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "ALSA failed to prepare the playback device");
        return 0;
    }
    return 1;
}

/// @brief Stop queued ALSA playback and publish any failure through diagnostics.
/// @param ctx Context whose backend counters and error state are updated.
/// @param plat Linux state containing the locked PCM handle.
/// @return 1 when queued frames were dropped; otherwise 0.
static int alsa_drop_checked(vaud_context_t ctx, vaud_linux_data *plat) {
    int result = snd_pcm_drop(plat->pcm);
    if (result < 0) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "ALSA failed to drop the playback stream");
        return 0;
    }
    return 1;
}

/// @brief Apply explicit ALSA hardware and software parameters for ZannaAUD output.
/// @details This replaces `snd_pcm_set_params()` so the backend can control the period size,
///          buffer size, start threshold, and wakeup granularity independently. The requested
///          format remains stereo 44.1 kHz signed 16-bit PCM; if ALSA cannot provide that exact
///          mixer rate through the selected device/plugin, initialization fails instead of playing
///          at the wrong speed.
/// @param pcm Open playback PCM handle.
/// @return 0 on success, otherwise a negative ALSA error code.
static int alsa_configure_pcm(snd_pcm_t *pcm) {
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    snd_pcm_uframes_t period = VAUD_BUFFER_FRAMES;
    snd_pcm_uframes_t buffer = VAUD_BUFFER_FRAMES * 4;
    unsigned int rate = VAUD_SAMPLE_RATE;
    int dir = 0;
    int err = snd_pcm_hw_params_malloc(&hw);
    if (err < 0)
        return err;
    err = snd_pcm_sw_params_malloc(&sw);
    if (err < 0) {
        snd_pcm_hw_params_free(hw);
        return err;
    }

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, VAUD_CHANNELS)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0)
        goto done;
    if (rate != VAUD_SAMPLE_RATE) {
        err = -EINVAL;
        goto done;
    }
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0)
        goto done;
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0)
        goto done;

    if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0)
        goto done;
    if ((err = snd_pcm_sw_params_set_start_threshold(pcm, sw, period)) < 0)
        goto done;
    if ((err = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0)
        goto done;
    err = snd_pcm_sw_params(pcm, sw);

done:
    snd_pcm_sw_params_free(sw);
    snd_pcm_hw_params_free(hw);
    return err;
}

/// @brief Recover an ALSA write error and update backend telemetry.
/// @param ctx Audio context owning the diagnostic counters.
/// @param plat Linux platform state containing the PCM handle.
/// @param err Negative ALSA error from `snd_pcm_writei` or `snd_pcm_wait`.
/// @return 1 when recovery succeeded and writing may continue, 0 otherwise.
static int alsa_recover_write_error(vaud_context_t ctx, vaud_linux_data *plat, int err) {
    if (err == -EPIPE || err == -ESTRPIPE)
        vaud_stats_add(&ctx->stats.backend_xruns, 1);
    vaud_stats_add(&ctx->stats.backend_recoveries, 1);
    if (snd_pcm_recover(plat->pcm, err, 0) < 0) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        return 0;
    }
    return 1;
}

/// @brief Write a full interleaved PCM period to ALSA, handling partial writes and recovery.
/// @details ALSA may accept fewer frames than requested, return transient `EAGAIN`, or report an
///          underrun/suspend that can be recovered. This loop keeps the audio thread responsive by
///          waiting on `EAGAIN`, attempting recoveries for recoverable failures, and surfacing
///          every branch through `vaud_stats_t` counters.
/// @param ctx Audio context owning diagnostics.
/// @param plat Linux platform state.
/// @param buffer Interleaved stereo PCM frames.
/// @param frames Number of frames to write.
/// @return 1 when all frames were written, 0 when the write should be treated as failed.
static int alsa_write_all(vaud_context_t ctx,
                          vaud_linux_data *plat,
                          const int16_t *buffer,
                          snd_pcm_uframes_t frames) {
    snd_pcm_uframes_t written_total = 0;

    while (__atomic_load_n(&plat->running, __ATOMIC_ACQUIRE) && written_total < frames) {
        const int16_t *cursor = buffer + ((size_t)written_total * VAUD_CHANNELS);
        snd_pcm_uframes_t remaining = frames - written_total;
        vaud_stats_add(&ctx->stats.backend_write_calls, 1);
        alsa_pcm_lock(plat);
        snd_pcm_sframes_t written = snd_pcm_writei(plat->pcm, cursor, remaining);

        if (written > 0) {
            alsa_pcm_unlock(plat);
            if ((snd_pcm_uframes_t)written < remaining)
                vaud_stats_add(&ctx->stats.backend_partial_writes, 1);
            written_total += (snd_pcm_uframes_t)written;
            continue;
        }

        if (written == 0) {
            alsa_pcm_unlock(plat);
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            return 0;
        }

        if (written == -EAGAIN) {
            vaud_stats_add(&ctx->stats.backend_waits, 1);
            /* The handle remains open until the audio thread joins. Do not hold
             * pcm_mutex across the bounded readiness wait: pause and shutdown
             * need to issue drop/prepare promptly to wake or redirect playback. */
            alsa_pcm_unlock(plat);
            int wait_rc = snd_pcm_wait(plat->pcm, 100);
            if (wait_rc < 0) {
                alsa_pcm_lock(plat);
                int recovered = alsa_recover_write_error(ctx, plat, wait_rc);
                alsa_pcm_unlock(plat);
                if (!recovered)
                    return 0;
            }
            continue;
        }

        if (!alsa_recover_write_error(ctx, plat, (int)written)) {
            alsa_pcm_unlock(plat);
            return 0;
        }
        alsa_pcm_unlock(plat);
    }

    return written_total == frames;
}

//===----------------------------------------------------------------------===//
// Audio Thread
//===----------------------------------------------------------------------===//

/// @brief Continuously mix bounded periods and drain them to the ALSA device.
/// @details Sleeps on the pause condition, terminates on synchronization or
///          unrecoverable PCM failure, and never performs allocation after
///          startup.
/// @param arg Pointer to the owning audio context.
/// @return Always NULL after shutdown, initialization failure, or backend failure.
static void *audio_thread_func(void *arg) {
    vaud_context_t ctx = (vaud_context_t)arg;
    vaud_linux_data *plat = (vaud_linux_data *)ctx->platform_data;

    int16_t *buffer = plat->mix_buffer;
    if (!buffer)
        return NULL;

    while (__atomic_load_n(&plat->running, __ATOMIC_ACQUIRE)) {
        /* Check for pause state */
        pthread_mutex_lock(&plat->pause_mutex);
        while (__atomic_load_n(&plat->paused, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&plat->running, __ATOMIC_ACQUIRE)) {
            int wait_result = pthread_cond_wait(&plat->pause_cond, &plat->pause_mutex);
            if (wait_result != 0) {
                vaud_stats_add(&ctx->stats.backend_write_failures, 1);
                vaud_set_error(VAUD_ERR_PLATFORM, "ALSA pause condition wait failed");
                __atomic_store_n(&plat->running, 0, __ATOMIC_RELEASE);
                break;
            }
        }
        pthread_mutex_unlock(&plat->pause_mutex);

        if (!__atomic_load_n(&plat->running, __ATOMIC_ACQUIRE))
            break;

        /* Render mixed audio */
        vaud_mixer_render_device(ctx, buffer, VAUD_BUFFER_FRAMES);

        /* Write the whole period; ALSA may legally accept only part of it. */
        if (!alsa_write_all(ctx, plat, buffer, VAUD_BUFFER_FRAMES)) {
            if (!__atomic_load_n(&plat->running, __ATOMIC_ACQUIRE))
                break;
            alsa_pcm_lock(plat);
            int prepared = alsa_prepare_checked(ctx, plat);
            alsa_pcm_unlock(plat);
            if (!prepared) {
                __atomic_store_n(&plat->running, 0, __ATOMIC_RELEASE);
                break;
            }
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            nanosleep(&ts, NULL);
        }
    }

    return NULL;
}

//===----------------------------------------------------------------------===//
// Platform Interface Implementation
//===----------------------------------------------------------------------===//

/// @copydoc vaud_platform_init
int vaud_platform_init(vaud_context_t ctx) {
    if (!ctx)
        return 0;

    /* Allocate platform data */
    vaud_linux_data *plat = (vaud_linux_data *)calloc(1, sizeof(vaud_linux_data));
    if (!plat) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate Linux audio data");
        return 0;
    }

    ctx->platform_data = plat;
    __atomic_store_n(&plat->running, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&plat->paused, 0, __ATOMIC_RELAXED);

    /* Initialize synchronization primitives */
    int err = pthread_mutex_init(&plat->pause_mutex, NULL);
    if (err != 0) {
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize ALSA pause mutex");
        return 0;
    }
    err = pthread_cond_init(&plat->pause_cond, NULL);
    if (err != 0) {
        pthread_mutex_destroy(&plat->pause_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize ALSA pause condition");
        return 0;
    }
    err = pthread_mutex_init(&plat->pcm_mutex, NULL);
    if (err != 0) {
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pause_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize ALSA PCM mutex");
        return 0;
    }

    /* ALSA's error callback is process-global and has no getter. Do not replace
     * an embedding application's handler merely to silence an optional probe. */
    err = snd_pcm_open(&plat->pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        pthread_mutex_destroy(&plat->pause_mutex);
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pcm_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to open ALSA device");
        return 0;
    }

    /* Configure PCM parameters */
    err = alsa_configure_pcm(plat->pcm);

    if (err < 0) {
        snd_pcm_close(plat->pcm);
        pthread_mutex_destroy(&plat->pause_mutex);
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pcm_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to configure ALSA device");
        return 0;
    }

    err = snd_pcm_nonblock(plat->pcm, 1);
    if (err < 0) {
        snd_pcm_close(plat->pcm);
        pthread_mutex_destroy(&plat->pause_mutex);
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pcm_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to configure ALSA nonblocking mode");
        return 0;
    }

    plat->mix_buffer = (int16_t *)malloc(VAUD_BUFFER_FRAMES * VAUD_CHANNELS * sizeof(int16_t));
    if (!plat->mix_buffer) {
        snd_pcm_close(plat->pcm);
        pthread_mutex_destroy(&plat->pause_mutex);
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pcm_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate ALSA mix buffer");
        return 0;
    }

    /* Start the audio thread */
    __atomic_store_n(&plat->running, 1, __ATOMIC_RELEASE);
    err = pthread_create(&plat->thread, NULL, audio_thread_func, ctx);
    if (err != 0) {
        __atomic_store_n(&plat->running, 0, __ATOMIC_RELEASE);
        free(plat->mix_buffer);
        snd_pcm_close(plat->pcm);
        pthread_mutex_destroy(&plat->pause_mutex);
        pthread_cond_destroy(&plat->pause_cond);
        pthread_mutex_destroy(&plat->pcm_mutex);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to create audio thread");
        return 0;
    }
    plat->thread_started = 1;

    return 1;
}

/// @copydoc vaud_platform_shutdown
int vaud_platform_shutdown(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return 1;

    vaud_linux_data *plat = (vaud_linux_data *)ctx->platform_data;

    /* Signal thread to stop */
    pthread_mutex_lock(&plat->pause_mutex);
    __atomic_store_n(&plat->running, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&plat->paused, 0, __ATOMIC_RELEASE); /* Unpause to allow thread to exit */
    pthread_cond_signal(&plat->pause_cond);
    pthread_mutex_unlock(&plat->pause_mutex);

    /* Abort any blocking snd_pcm_writei() so shutdown cannot hang behind ALSA. */
    alsa_pcm_lock(plat);
    (void)alsa_drop_checked(ctx, plat);
    alsa_pcm_unlock(plat);

    /* Wait for thread to finish */
    if (plat->thread_started)
        pthread_join(plat->thread, NULL);

    /* Close ALSA device */
    alsa_pcm_lock(plat);
    snd_pcm_close(plat->pcm);
    plat->pcm = NULL;
    alsa_pcm_unlock(plat);

    /* Clean up synchronization */
    pthread_cond_destroy(&plat->pause_cond);
    pthread_mutex_destroy(&plat->pause_mutex);
    pthread_mutex_destroy(&plat->pcm_mutex);

    free(plat->mix_buffer);
    free(plat);
    ctx->platform_data = NULL;
    return 1;
}

/// @copydoc vaud_platform_pause
void vaud_platform_pause(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_linux_data *plat = (vaud_linux_data *)ctx->platform_data;

    pthread_mutex_lock(&plat->pause_mutex);
    __atomic_store_n(&plat->paused, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&plat->pause_mutex);

    alsa_pcm_lock(plat);
    int rc = snd_pcm_pause(plat->pcm, 1);
    if (rc < 0) {
        if (rc == -ENOSYS) {
            (void)alsa_drop_checked(ctx, plat);
            (void)alsa_prepare_checked(ctx, plat);
        } else if (snd_pcm_recover(plat->pcm, rc, 0) < 0) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            (void)alsa_drop_checked(ctx, plat);
            (void)alsa_prepare_checked(ctx, plat);
        }
    }
    alsa_pcm_unlock(plat);
}

/// @copydoc vaud_platform_resume
void vaud_platform_resume(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_linux_data *plat = (vaud_linux_data *)ctx->platform_data;

    /* Resume ALSA playback */
    alsa_pcm_lock(plat);
    int rc = snd_pcm_pause(plat->pcm, 0);
    if (rc < 0) {
        if (rc == -ENOSYS) {
            (void)alsa_prepare_checked(ctx, plat);
        } else if (snd_pcm_recover(plat->pcm, rc, 0) < 0) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            (void)alsa_prepare_checked(ctx, plat);
        }
    }
    alsa_pcm_unlock(plat);

    pthread_mutex_lock(&plat->pause_mutex);
    __atomic_store_n(&plat->paused, 0, __ATOMIC_RELEASE);
    pthread_cond_signal(&plat->pause_cond);
    pthread_mutex_unlock(&plat->pause_mutex);
}

//===----------------------------------------------------------------------===//
// Timing
//===----------------------------------------------------------------------===//

/// @copydoc vaud_platform_now_ms
int64_t vaud_platform_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#endif /* __linux__ */
