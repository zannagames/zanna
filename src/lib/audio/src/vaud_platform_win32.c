//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/audio/src/vaud_platform_win32.c
// Purpose: Implement event-driven WASAPI audio output and Windows timing.
// Key invariants:
//   - Negotiated formats are validated before any render-buffer writes.
//   - The worker owns its COM apartment and never outlives backend state.
//   - Every successful WASAPI buffer acquisition is paired with a release.
// Ownership/Lifetime:
//   - vaud_win32_data owns all COM interfaces, handles, and conversion buffers.
//   - The creating thread balances its COM reference during shutdown.
// Links: src/lib/audio/src/vaud_internal.h, src/lib/audio/CMakeLists.txt
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Windows WASAPI audio backend for ZannaAUD.
/// @details Negotiates a supported shared-mode endpoint format, converts from
///          the fixed stereo signed-16 mixer bus when required, and drives the
///          render client from an event-based worker with its own COM
///          apartment. Control operations remain on the creating thread so COM
///          and WASAPI ownership are balanced during pause, resume, and teardown.

#if defined(_WIN32)

#include "vaud_internal.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <audioclient.h>
#include <limits.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

//===----------------------------------------------------------------------===//
// COM GUIDs
//===----------------------------------------------------------------------===//

/* Define GUIDs we need (to avoid linking with uuid.lib) */
static const GUID VAUD_CLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

static const GUID VAUD_IID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

static const GUID VAUD_IID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};

static const GUID VAUD_IID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

static const GUID VAUD_KSDATAFORMAT_SUBTYPE_PCM = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static const GUID VAUD_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

/// @brief PCM representations accepted from a negotiated WASAPI endpoint.
typedef enum {
    VAUD_WIN32_SAMPLE_S16 = 0,
    VAUD_WIN32_SAMPLE_S24,
    VAUD_WIN32_SAMPLE_S32,
    VAUD_WIN32_SAMPLE_F32
} vaud_win32_sample_format;

//===----------------------------------------------------------------------===//
// Platform Data Structure
//===----------------------------------------------------------------------===//

/// @brief Windows WASAPI platform data.
typedef struct {
    IMMDevice *device;           ///< Audio endpoint device
    IAudioClient *client;        ///< Audio client interface
    IAudioRenderClient *render;  ///< Render client for buffer access
    HANDLE thread;               ///< Audio thread handle
    HANDLE event;                ///< Buffer event
    HANDLE stop_event;           ///< Stop signal event
    HANDLE ready_event;          ///< Worker COM-initialization handshake
    WAVEFORMATEX *format;        ///< Negotiated WASAPI render format
    int16_t *mix_buffer;         ///< Internal stereo mix buffer for format conversion
    UINT32 buffer_frames;        ///< Total buffer size in frames
    UINT32 render_channels;      ///< Channels in negotiated format
    UINT32 render_sample_rate;   ///< Sample rate in negotiated format
    UINT32 render_block_align;   ///< Bytes per rendered frame
    UINT32 render_bytes_sample;  ///< Bytes per channel sample
    UINT32 render_left_channel;  ///< Endpoint slot carrying front-left audio
    UINT32 render_right_channel; ///< Endpoint slot carrying front-right audio
    vaud_win32_sample_format render_sample_format; ///< Sample representation
    volatile LONG running;                         ///< Thread running flag
    volatile LONG paused;                          ///< Pause state
    volatile LONG thread_start_status;             ///< -1 failed, 0 pending, 1 ready
    volatile LONG thread_exited;                   ///< Worker finished all state and COM access
    unsigned worker_thread_id;                     ///< `_beginthreadex` worker identity
    int com_initialized;                           ///< This backend owns a COM apartment reference.
    DWORD owner_thread_id;                         ///< Thread that owns COM control operations.
    CRITICAL_SECTION pause_cs;                     ///< Protects pause state
} vaud_win32_data;

/// @brief Compare two non-null Windows GUID values byte-for-byte.
/// @param a First GUID.
/// @param b Second GUID.
/// @return 1 when both pointers are non-null and values match; otherwise 0.
static int vaud_win32_guid_equal(const GUID *a, const GUID *b) {
    return a && b && memcmp(a, b, sizeof(GUID)) == 0;
}

/// @brief Count set channel-mask bits without compiler-specific intrinsics.
/// @param mask WAVEFORMATEXTENSIBLE speaker mask.
/// @return Number of declared speaker positions.
static UINT32 vaud_win32_channel_mask_count(DWORD mask) {
    UINT32 count = 0;
    while (mask != 0) {
        mask &= mask - 1u;
        count++;
    }
    return count;
}

/// @brief Resolve one speaker bit to its interleaved channel slot.
/// @details WAVEFORMATEXTENSIBLE orders channels by ascending set-bit position.
/// @param mask Validated endpoint speaker mask.
/// @param speaker Single speaker bit to locate.
/// @param out_index Receives the zero-based interleaved slot.
/// @return 1 when the speaker is present; otherwise 0.
static int vaud_win32_channel_mask_index(DWORD mask, DWORD speaker, UINT32 *out_index) {
    if (!out_index || speaker == 0 || (speaker & (speaker - 1u)) != 0 || (mask & speaker) == 0) {
        return 0;
    }
    *out_index = vaud_win32_channel_mask_count(mask & (speaker - 1u));
    return 1;
}

/// @brief Resolve the effective sample subtype and valid precision of a wave format.
/// @details Canonical PCM and IEEE-float tags are mapped to their subtype GUIDs;
///          extensible formats expose the embedded subtype and valid-bits field.
/// @param fmt Candidate endpoint format.
/// @param out_subtype Receives a borrowed subtype GUID pointer.
/// @param out_valid_bits Receives significant bits per channel sample.
/// @return 1 for a recognized base or extensible format; otherwise 0.
static int vaud_win32_format_subtype(const WAVEFORMATEX *fmt,
                                     const GUID **out_subtype,
                                     WORD *out_valid_bits) {
    if (!fmt || !out_subtype || !out_valid_bits)
        return 0;

    *out_subtype = NULL;
    *out_valid_bits = fmt->wBitsPerSample;

    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        *out_subtype = &VAUD_KSDATAFORMAT_SUBTYPE_PCM;
        return 1;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        *out_subtype = &VAUD_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        return 1;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        *out_valid_bits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample
                                                           : fmt->wBitsPerSample;
        *out_subtype = &ext->SubFormat;
        return 1;
    }
    return 0;
}

/// @brief Validate a WASAPI render format and cache its conversion parameters.
/// @details Accepts one to eight channels of signed 16/24/32-bit PCM or
///          32-bit IEEE float, verifies block alignment and byte rate exactly,
///          and rejects unsupported precision/subtype combinations before any
///          endpoint buffer can be written.
/// @param plat Backend state updated only after complete validation.
/// @param fmt Candidate shared-mode render format.
/// @return 1 when accepted and cached; otherwise 0.
static int vaud_win32_configure_render_format(vaud_win32_data *plat, const WAVEFORMATEX *fmt) {
    if (!plat || !fmt || fmt->nChannels == 0 || fmt->nChannels > 8 || fmt->nSamplesPerSec == 0 ||
        fmt->nBlockAlign == 0 || fmt->wBitsPerSample == 0 || (fmt->wBitsPerSample % 8) != 0) {
        return 0;
    }

    const GUID *subtype = NULL;
    WORD valid_bits = 0;
    if (!vaud_win32_format_subtype(fmt, &subtype, &valid_bits))
        return 0;
    if (valid_bits == 0 || valid_bits > fmt->wBitsPerSample)
        return 0;

    UINT32 bytes_per_sample = fmt->wBitsPerSample / 8u;
    if (bytes_per_sample == 0 || bytes_per_sample > 4)
        return 0;
    const uint64_t expected_block_align = (uint64_t)fmt->nChannels * bytes_per_sample;
    if (expected_block_align > UINT16_MAX || fmt->nBlockAlign != expected_block_align)
        return 0;
    const uint64_t expected_bytes_per_second = (uint64_t)fmt->nSamplesPerSec * expected_block_align;
    if (expected_bytes_per_second > UINT32_MAX ||
        fmt->nAvgBytesPerSec != expected_bytes_per_second) {
        return 0;
    }

    vaud_win32_sample_format sample_format;
    if (vaud_win32_guid_equal(subtype, &VAUD_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
        fmt->wBitsPerSample == 32 && valid_bits == 32) {
        sample_format = VAUD_WIN32_SAMPLE_F32;
    } else if (vaud_win32_guid_equal(subtype, &VAUD_KSDATAFORMAT_SUBTYPE_PCM)) {
        if (valid_bits < 16)
            return 0;
        if (fmt->wBitsPerSample == 16) {
            sample_format = VAUD_WIN32_SAMPLE_S16;
        } else if (fmt->wBitsPerSample == 24) {
            sample_format = VAUD_WIN32_SAMPLE_S24;
        } else if (fmt->wBitsPerSample == 32 && valid_bits <= 32) {
            sample_format = VAUD_WIN32_SAMPLE_S32;
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    UINT32 left_channel = 0;
    UINT32 right_channel = fmt->nChannels > 1 ? 1u : 0u;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)fmt;
        const DWORD mask = ext->dwChannelMask;
        if (mask != 0 && vaud_win32_channel_mask_count(mask) != fmt->nChannels)
            return 0;
        if (fmt->nChannels > 2 && mask == 0)
            return 0;
        if (fmt->nChannels > 1 && mask != 0 &&
            (!vaud_win32_channel_mask_index(mask, SPEAKER_FRONT_LEFT, &left_channel) ||
             !vaud_win32_channel_mask_index(mask, SPEAKER_FRONT_RIGHT, &right_channel))) {
            return 0;
        }
    } else if (fmt->nChannels > 2) {
        return 0;
    }

    plat->render_channels = fmt->nChannels;
    plat->render_sample_rate = fmt->nSamplesPerSec;
    plat->render_block_align = fmt->nBlockAlign;
    plat->render_bytes_sample = bytes_per_sample;
    plat->render_left_channel = left_channel;
    plat->render_right_channel = right_channel;
    plat->render_sample_format = sample_format;
    return 1;
}

/// @brief Copy a variable-length WAVEFORMATEX descriptor to ordinary heap storage.
/// @param fmt Source descriptor, including @c cbSize extension bytes.
/// @return Owned copy released with free(), or NULL on invalid input/allocation failure.
static WAVEFORMATEX *vaud_win32_copy_format(const WAVEFORMATEX *fmt) {
    if (!fmt)
        return NULL;
    size_t bytes = sizeof(WAVEFORMATEX) + (size_t)fmt->cbSize;
    WAVEFORMATEX *copy = (WAVEFORMATEX *)malloc(bytes);
    if (!copy)
        return NULL;
    memcpy(copy, fmt, bytes);
    return copy;
}

/// @brief Choose a shared-mode endpoint format that the backend can safely convert to.
/// @details Prefers the internal 44.1-kHz stereo signed-16 format, then a
///          validated closest match returned by WASAPI, and finally the
///          endpoint mix format. COM-allocated candidates are copied before
///          being released.
/// @param client Activated WASAPI audio client.
/// @return Owned selected format, or NULL when no supported representation exists.
static WAVEFORMATEX *vaud_win32_select_format(IAudioClient *client) {
    if (!client)
        return NULL;

    WAVEFORMATEX requested;
    memset(&requested, 0, sizeof(requested));
    requested.wFormatTag = WAVE_FORMAT_PCM;
    requested.nChannels = VAUD_CHANNELS;
    requested.nSamplesPerSec = VAUD_SAMPLE_RATE;
    requested.wBitsPerSample = 16;
    requested.nBlockAlign = requested.nChannels * requested.wBitsPerSample / 8;
    requested.nAvgBytesPerSec = requested.nSamplesPerSec * requested.nBlockAlign;

    WAVEFORMATEX *closest = NULL;
    WAVEFORMATEX *selected = NULL;
    HRESULT hr =
        IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED, &requested, &closest);
    if (hr == S_OK) {
        selected = vaud_win32_copy_format(&requested);
    } else if (hr == S_FALSE && closest) {
        vaud_win32_data scratch;
        memset(&scratch, 0, sizeof(scratch));
        if (vaud_win32_configure_render_format(&scratch, closest))
            selected = vaud_win32_copy_format(closest);
    }
    if (closest)
        CoTaskMemFree(closest);

    if (!selected) {
        WAVEFORMATEX *mix = NULL;
        hr = IAudioClient_GetMixFormat(client, &mix);
        if (SUCCEEDED(hr) && mix) {
            vaud_win32_data scratch;
            memset(&scratch, 0, sizeof(scratch));
            if (vaud_win32_configure_render_format(&scratch, mix))
                selected = vaud_win32_copy_format(mix);
        }
        if (mix)
            CoTaskMemFree(mix);
    }

    return selected;
}

/// @brief Bound an endpoint render request to the fixed internal mixer capacity.
/// @details Downsampling requires additional internal source frames for linear
///          interpolation; this limit ensures the required input never exceeds
///          VAUD_BUFFER_FRAMES.
/// @param plat Backend state containing the negotiated endpoint sample rate.
/// @return Maximum endpoint frames safe for one render-client acquisition.
static UINT32 vaud_win32_max_render_frames(const vaud_win32_data *plat) {
    if (!plat || plat->render_sample_rate == 0)
        return VAUD_BUFFER_FRAMES;
    if (plat->render_sample_rate >= VAUD_SAMPLE_RATE)
        return VAUD_BUFFER_FRAMES;
    uint64_t frames =
        ((uint64_t)(VAUD_BUFFER_FRAMES - 2) * plat->render_sample_rate) / VAUD_SAMPLE_RATE;
    if (frames == 0)
        frames = 1;
    if (frames > VAUD_BUFFER_FRAMES)
        frames = VAUD_BUFFER_FRAMES;
    return (UINT32)frames;
}

/// @brief Sample one channel of the internal mixer period at an endpoint-frame position.
/// @details Uses direct indexing when rates match and bounded linear
///          interpolation otherwise, repeating the final source frame at the
///          period boundary.
/// @param mix Interleaved stereo signed-16 source period.
/// @param internal_frames Available source frames.
/// @param out_frame Destination frame index.
/// @param out_rate Negotiated destination sample rate.
/// @param channel Internal stereo channel index.
/// @return Interpolated sample in signed-16 amplitude units, or zero for invalid input.
static int32_t vaud_win32_resampled_sample(
    const int16_t *mix, UINT32 internal_frames, UINT32 out_frame, UINT32 out_rate, UINT32 channel) {
    if (!mix || internal_frames == 0 || out_rate == 0)
        return 0;
    if (out_rate == VAUD_SAMPLE_RATE) {
        UINT32 idx = out_frame < internal_frames ? out_frame : internal_frames - 1u;
        return mix[idx * VAUD_CHANNELS + channel];
    }

    double pos = ((double)out_frame * (double)VAUD_SAMPLE_RATE) / (double)out_rate;
    UINT32 idx = (UINT32)pos;
    if (idx >= internal_frames)
        idx = internal_frames - 1u;
    UINT32 next = (idx + 1u < internal_frames) ? idx + 1u : idx;
    double frac = pos - (double)idx;
    double a = (double)mix[idx * VAUD_CHANNELS + channel];
    double b = (double)mix[next * VAUD_CHANNELS + channel];
    return (int32_t)(a + (b - a) * frac);
}

/// @brief Store one bounded mixer sample in the negotiated endpoint representation.
/// @param plat Backend state describing the destination sample format.
/// @param dst Destination bytes for one channel sample.
/// @param sample Sample in signed-16 amplitude units; clamped before conversion.
static void vaud_win32_store_sample(vaud_win32_data *plat, BYTE *dst, int32_t sample) {
    if (sample > 32767)
        sample = 32767;
    if (sample < -32768)
        sample = -32768;

    switch (plat->render_sample_format) {
        case VAUD_WIN32_SAMPLE_F32: {
            float f = (float)sample / 32768.0f;
            memcpy(dst, &f, sizeof(f));
            break;
        }
        case VAUD_WIN32_SAMPLE_S24: {
            int32_t s24 = sample * 256;
            dst[0] = (BYTE)(s24 & 0xFF);
            dst[1] = (BYTE)((s24 >> 8) & 0xFF);
            dst[2] = (BYTE)((s24 >> 16) & 0xFF);
            break;
        }
        case VAUD_WIN32_SAMPLE_S32: {
            int32_t s32 = sample * 65536;
            memcpy(dst, &s32, sizeof(s32));
            break;
        }
        case VAUD_WIN32_SAMPLE_S16:
        default: {
            int16_t s16 = (int16_t)sample;
            memcpy(dst, &s16, sizeof(s16));
            break;
        }
    }
}

/// @brief Render and convert one acquired WASAPI endpoint buffer.
/// @details Uses a zero-copy mixer call for the native stereo signed-16 format.
///          Other formats render to the preallocated internal bus, resample
///          linearly, downmix stereo to mono when needed, route left/right to
///          the negotiated front-speaker slots, and leave other channels silent.
/// @param ctx Audio context to advance.
/// @param plat Backend state describing the negotiated format.
/// @param buffer Acquired WASAPI destination buffer.
/// @param frames Number of endpoint frames available.
/// @return 1 when the buffer contains valid rendered or intentional silent data;
///         otherwise 0 so the caller can release it with the silent flag.
static int vaud_win32_render_to_buffer(vaud_context_t ctx,
                                       vaud_win32_data *plat,
                                       BYTE *buffer,
                                       UINT32 frames) {
    if (!ctx || !plat || !buffer || frames == 0)
        return 0;

    if (plat->render_block_align == 0 || (size_t)frames > SIZE_MAX / plat->render_block_align)
        return 0;

    if (plat->render_channels == VAUD_CHANNELS && plat->render_sample_rate == VAUD_SAMPLE_RATE &&
        plat->render_sample_format == VAUD_WIN32_SAMPLE_S16 && plat->render_block_align == 4 &&
        plat->render_left_channel == 0 && plat->render_right_channel == 1) {
        vaud_mixer_render_device(ctx, (int16_t *)buffer, (int32_t)frames);
        return 1;
    }

    memset(buffer, 0, (size_t)frames * plat->render_block_align);
    if (!plat->mix_buffer)
        return 1;

    UINT32 internal_frames = frames;
    if (plat->render_sample_rate != VAUD_SAMPLE_RATE) {
        uint64_t needed =
            ((uint64_t)(frames - 1u) * VAUD_SAMPLE_RATE) / plat->render_sample_rate + 2u;
        internal_frames = needed > VAUD_BUFFER_FRAMES ? VAUD_BUFFER_FRAMES : (UINT32)needed;
    }
    if (internal_frames == 0)
        internal_frames = 1;

    vaud_mixer_render_device(ctx, plat->mix_buffer, (int32_t)internal_frames);

    for (UINT32 i = 0; i < frames; i++) {
        int32_t left = vaud_win32_resampled_sample(
            plat->mix_buffer, internal_frames, i, plat->render_sample_rate, 0);
        int32_t right = vaud_win32_resampled_sample(
            plat->mix_buffer, internal_frames, i, plat->render_sample_rate, 1);

        for (UINT32 ch = 0; ch < plat->render_channels; ch++) {
            int32_t sample = 0;
            if (plat->render_channels == 1)
                sample = (left + right) / 2;
            else if (ch == plat->render_left_channel)
                sample = left;
            else if (ch == plat->render_right_channel)
                sample = right;

            BYTE *dst = buffer + (size_t)i * plat->render_block_align +
                        (size_t)ch * plat->render_bytes_sample;
            vaud_win32_store_sample(plat, dst, sample);
        }
    }
    return 1;
}

/// @brief Release heap-owned render conversion buffers and clear their pointers.
/// @param plat Backend state being unwound after initialization failure or shutdown.
static void vaud_win32_free_render_buffers(vaud_win32_data *plat) {
    if (!plat)
        return;
    free(plat->mix_buffer);
    plat->mix_buffer = NULL;
    free(plat->format);
    plat->format = NULL;
}

/// @brief Join the WASAPI worker thread before backend resources are released.
/// @details A finite first wait makes hung shutdowns visible through the error
///          channel. Published contexts retain ownership after a bounded retry
///          fails; unpublished initialization failures must prove completion
///          before their otherwise unreachable context can be released.
/// @param ctx Audio context used for diagnostics.
/// @param plat Win32 backend state containing the thread handle.
/// @param timeout_ms First wait timeout in milliseconds.
/// @param must_complete Nonzero during unpublished initialization failure, where
///        no caller can retain the context for a later cleanup retry.
/// @return 1 after worker exit is proven; 0 on self-join or a bounded shutdown failure.
static int vaud_win32_join_thread(vaud_context_t ctx,
                                  vaud_win32_data *plat,
                                  DWORD timeout_ms,
                                  int must_complete) {
    if (!plat || !plat->thread)
        return 1;
    if (plat->worker_thread_id != 0 && plat->worker_thread_id == GetCurrentThreadId()) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI audio thread cannot join itself");
        return 0;
    }
    DWORD wait_rc = WaitForSingleObject(plat->thread, timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Timed out waiting for WASAPI audio thread");
        if (plat->client)
            (void)IAudioClient_Stop(plat->client);
        if (plat->stop_event)
            (void)SetEvent(plat->stop_event);
        wait_rc = WaitForSingleObject(plat->thread, must_complete ? INFINITE : timeout_ms);
    }
    if (wait_rc != WAIT_OBJECT_0) {
        ULONGLONG deadline = GetTickCount64();
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed waiting for WASAPI audio thread");
        if (plat->client)
            (void)IAudioClient_Stop(plat->client);
        if (plat->stop_event)
            (void)SetEvent(plat->stop_event);
        if (deadline <= ULLONG_MAX - timeout_ms)
            deadline += timeout_ms;
        else
            deadline = ULLONG_MAX;
        while (!InterlockedCompareExchange(&plat->thread_exited, 0, 0)) {
            if (!must_complete && GetTickCount64() >= deadline)
                return 0;
            Sleep(1);
        }
    }
    if (!CloseHandle(plat->thread)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to close WASAPI audio thread handle");
    }
    plat->thread = NULL;
    plat->worker_thread_id = 0;
    return 1;
}

//===----------------------------------------------------------------------===//
// Audio Thread
//===----------------------------------------------------------------------===//

/// @brief Service event-driven WASAPI render-buffer requests.
/// @details Initializes a worker-owned COM apartment, reports readiness to the
///          creating thread, bounds repeated padding/acquisition failures, and
///          pairs every successful buffer acquisition with exactly one release.
/// @param arg Pointer to the owning audio context.
/// @return Always zero after stop, startup failure, or terminal endpoint failure.
static unsigned __stdcall audio_thread_func(void *arg) {
    vaud_context_t ctx = (vaud_context_t)arg;
    vaud_win32_data *plat = (vaud_win32_data *)ctx->platform_data;
    unsigned consecutive_padding_failures = 0;
    unsigned consecutive_buffer_failures = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_initialized = SUCCEEDED(hr) ? 1 : 0;
    if (FAILED(hr)) {
        InterlockedExchange(&plat->thread_start_status, -1);
        InterlockedExchange(&plat->running, 0);
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize COM on WASAPI audio thread");
        (void)SetEvent(plat->ready_event);
        InterlockedExchange(&plat->thread_exited, 1);
        return 0;
    }
    InterlockedExchange(&plat->thread_start_status, 1);
    if (!SetEvent(plat->ready_event)) {
        InterlockedExchange(&plat->thread_start_status, -1);
        InterlockedExchange(&plat->running, 0);
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to signal WASAPI audio thread readiness");
        if (com_initialized)
            CoUninitialize();
        InterlockedExchange(&plat->thread_exited, 1);
        return 0;
    }

    /* Events to wait on */
    HANDLE events[2] = {plat->event, plat->stop_event};

    while (InterlockedCompareExchange(&plat->running, 0, 0)) {
        /* Wait for buffer space or stop signal */
        DWORD wait_result = WaitForMultipleObjects(2, events, FALSE, 100);

        if (!InterlockedCompareExchange(&plat->running, 0, 0) || wait_result == WAIT_OBJECT_0 + 1) {
            /* Stop event signaled */
            break;
        }

        if (wait_result == WAIT_TIMEOUT) {
            continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI event wait failed");
            InterlockedExchange(&plat->running, 0);
            break;
        }

        /* Check pause state */
        EnterCriticalSection(&plat->pause_cs);
        int is_paused = (int)InterlockedCompareExchange(&plat->paused, 0, 0);
        if (is_paused) {
            LeaveCriticalSection(&plat->pause_cs);
            continue;
        }

        /* Get available buffer space */
        UINT32 padding = 0;
        hr = IAudioClient_GetCurrentPadding(plat->client, &padding);
        if (FAILED(hr)) {
            if (++consecutive_padding_failures >= 8) {
                vaud_stats_add(&ctx->stats.backend_write_failures, 1);
                vaud_set_error(VAUD_ERR_PLATFORM,
                               "WASAPI repeatedly failed to report buffer padding");
                InterlockedExchange(&plat->running, 0);
                LeaveCriticalSection(&plat->pause_cs);
                break;
            }
            LeaveCriticalSection(&plat->pause_cs);
            continue;
        }
        consecutive_padding_failures = 0;
        if (padding > plat->buffer_frames) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI reported invalid buffer padding");
            InterlockedExchange(&plat->running, 0);
            LeaveCriticalSection(&plat->pause_cs);
            break;
        }

        UINT32 available = plat->buffer_frames - padding;
        if (available == 0) {
            LeaveCriticalSection(&plat->pause_cs);
            continue;
        }

        /* Limit to our standard buffer size */
        UINT32 max_frames = vaud_win32_max_render_frames(plat);
        if (available > max_frames)
            available = max_frames;

        /* Get buffer from render client */
        BYTE *buffer = NULL;
        hr = IAudioRenderClient_GetBuffer(plat->render, available, &buffer);
        if (FAILED(hr)) {
            if (++consecutive_buffer_failures >= 8) {
                vaud_stats_add(&ctx->stats.backend_write_failures, 1);
                vaud_set_error(VAUD_ERR_PLATFORM,
                               "WASAPI repeatedly failed to acquire a render buffer");
                InterlockedExchange(&plat->running, 0);
                LeaveCriticalSection(&plat->pause_cs);
                break;
            }
            LeaveCriticalSection(&plat->pause_cs);
            continue;
        }
        consecutive_buffer_failures = 0;
        if (!buffer) {
            hr = IAudioRenderClient_ReleaseBuffer(
                plat->render, available, AUDCLNT_BUFFERFLAGS_SILENT);
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            vaud_set_error(VAUD_ERR_PLATFORM,
                           FAILED(hr) ? "WASAPI failed to release a null render buffer"
                                      : "WASAPI returned a null render buffer");
            InterlockedExchange(&plat->running, 0);
            LeaveCriticalSection(&plat->pause_cs);
            break;
        }

        /* Render mixed audio in the negotiated WASAPI format. */
        DWORD release_flags = vaud_win32_render_to_buffer(ctx, plat, buffer, available)
                                  ? 0
                                  : AUDCLNT_BUFFERFLAGS_SILENT;

        /* Release buffer */
        hr = IAudioRenderClient_ReleaseBuffer(plat->render, available, release_flags);
        if (FAILED(hr)) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI failed to release a render buffer");
            InterlockedExchange(&plat->running, 0);
            LeaveCriticalSection(&plat->pause_cs);
            break;
        }
        LeaveCriticalSection(&plat->pause_cs);
    }

    InterlockedExchange(&plat->running, 0);
    if (com_initialized)
        CoUninitialize();
    InterlockedExchange(&plat->thread_exited, 1);
    return 0;
}

//===----------------------------------------------------------------------===//
// Platform Interface Implementation
//===----------------------------------------------------------------------===//

/// @copydoc vaud_platform_init
int vaud_platform_init(vaud_context_t ctx) {
    if (!ctx)
        return 0;

    HRESULT hr;

    /* Allocate platform data */
    vaud_win32_data *plat = (vaud_win32_data *)calloc(1, sizeof(vaud_win32_data));
    if (!plat) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate Windows audio data");
        return 0;
    }

    ctx->platform_data = plat;
    plat->owner_thread_id = GetCurrentThreadId();
    InterlockedExchange(&plat->running, 0);
    InterlockedExchange(&plat->paused, 0);
    InterlockedExchange(&plat->thread_start_status, 0);
    InterlockedExchange(&plat->thread_exited, 0);

    if (!InitializeCriticalSectionEx(&plat->pause_cs, 4000, 0)) {
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to initialize WASAPI synchronization");
        return 0;
    }

    /* Initialize COM */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        DeleteCriticalSection(&plat->pause_cs);
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize COM");
        return 0;
    }
    plat->com_initialized = (hr == S_OK || hr == S_FALSE);

    /* Create device enumerator */
    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&VAUD_CLSID_MMDeviceEnumerator,
                          NULL,
                          CLSCTX_ALL,
                          &VAUD_IID_IMMDeviceEnumerator,
                          (void **)&enumerator);

    if (FAILED(hr) || !enumerator) {
        if (enumerator)
            IMMDeviceEnumerator_Release(enumerator);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to create device enumerator");
        return 0;
    }

    /* Get default audio endpoint */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &plat->device);

    IMMDeviceEnumerator_Release(enumerator);

    if (FAILED(hr) || !plat->device) {
        if (plat->device)
            IMMDevice_Release(plat->device);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to get audio endpoint");
        return 0;
    }

    /* Activate audio client */
    hr = IMMDevice_Activate(
        plat->device, &VAUD_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&plat->client);

    if (FAILED(hr) || !plat->client) {
        if (plat->client)
            IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to activate audio client");
        return 0;
    }

    /* Negotiate a shared-mode WASAPI format and convert from the internal mixer format. */
    plat->format = vaud_win32_select_format(plat->client);
    if (!plat->format || !vaud_win32_configure_render_format(plat, plat->format)) {
        free(plat->format);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to negotiate audio format");
        return 0;
    }

    plat->mix_buffer =
        (int16_t *)malloc((size_t)VAUD_BUFFER_FRAMES * VAUD_CHANNELS * sizeof(int16_t));
    if (!plat->mix_buffer) {
        free(plat->format);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate Windows mix buffer");
        return 0;
    }

    /* Initialize audio client in shared mode with event callback */
    REFERENCE_TIME buffer_duration = 500000; /* 50ms in 100ns units */

    hr = IAudioClient_Initialize(plat->client,
                                 AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 buffer_duration,
                                 0,
                                 plat->format,
                                 NULL);

    if (FAILED(hr)) {
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to initialize audio client");
        return 0;
    }

    /* Get actual buffer size */
    hr = IAudioClient_GetBufferSize(plat->client, &plat->buffer_frames);
    if (FAILED(hr) || plat->buffer_frames == 0) {
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to get buffer size");
        return 0;
    }

    /* Create events */
    plat->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    plat->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    plat->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!plat->event || !plat->stop_event || !plat->ready_event) {
        if (plat->event)
            CloseHandle(plat->event);
        if (plat->stop_event)
            CloseHandle(plat->stop_event);
        if (plat->ready_event)
            CloseHandle(plat->ready_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to create events");
        return 0;
    }

    /* Set event handle */
    hr = IAudioClient_SetEventHandle(plat->client, plat->event);
    if (FAILED(hr)) {
        CloseHandle(plat->event);
        CloseHandle(plat->stop_event);
        CloseHandle(plat->ready_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to set event handle");
        return 0;
    }

    /* Get render client */
    hr =
        IAudioClient_GetService(plat->client, &VAUD_IID_IAudioRenderClient, (void **)&plat->render);

    if (FAILED(hr) || !plat->render) {
        if (plat->render)
            IAudioRenderClient_Release(plat->render);
        CloseHandle(plat->event);
        CloseHandle(plat->stop_event);
        CloseHandle(plat->ready_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to get render client");
        return 0;
    }

    /* Start audio thread */
    InterlockedExchange(&plat->running, 1);
    uintptr_t thread_handle =
        _beginthreadex(NULL, 0, audio_thread_func, ctx, 0, &plat->worker_thread_id);
    plat->thread = thread_handle ? (HANDLE)thread_handle : NULL;

    if (!plat->thread) {
        InterlockedExchange(&plat->running, 0);
        IAudioRenderClient_Release(plat->render);
        CloseHandle(plat->event);
        CloseHandle(plat->stop_event);
        CloseHandle(plat->ready_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to create audio thread");
        return 0;
    }

    DWORD ready_wait = WaitForSingleObject(plat->ready_event, 5000);
    LONG start_status = InterlockedCompareExchange(&plat->thread_start_status, 0, 0);
    if (ready_wait != WAIT_OBJECT_0 || start_status != 1) {
        InterlockedExchange(&plat->running, 0);
        (void)SetEvent(plat->stop_event);
        vaud_win32_join_thread(ctx, plat, 5000, 1);
        IAudioRenderClient_Release(plat->render);
        CloseHandle(plat->event);
        CloseHandle(plat->stop_event);
        CloseHandle(plat->ready_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI audio thread failed to initialize");
        return 0;
    }
    CloseHandle(plat->ready_event);
    plat->ready_event = NULL;

    /* Start audio client */
    hr = IAudioClient_Start(plat->client);
    if (FAILED(hr)) {
        InterlockedExchange(&plat->running, 0);
        SetEvent(plat->stop_event);
        vaud_win32_join_thread(ctx, plat, 5000, 1);
        IAudioRenderClient_Release(plat->render);
        CloseHandle(plat->event);
        CloseHandle(plat->stop_event);
        IAudioClient_Release(plat->client);
        IMMDevice_Release(plat->device);
        vaud_win32_free_render_buffers(plat);
        DeleteCriticalSection(&plat->pause_cs);
        if (plat->com_initialized)
            CoUninitialize();
        free(plat);
        ctx->platform_data = NULL;
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to start audio client");
        return 0;
    }

    return 1;
}

/// @copydoc vaud_platform_shutdown
int vaud_platform_shutdown(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return 1;

    vaud_win32_data *plat = (vaud_win32_data *)ctx->platform_data;
    if (plat->owner_thread_id != GetCurrentThreadId()) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI shutdown must run on the context owner thread");
        return 0;
    }

    /* Signal thread to stop */
    InterlockedExchange(&plat->running, 0);
    if (plat->stop_event && !SetEvent(plat->stop_event)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to signal the WASAPI audio thread to stop");
    }

    /* Wait for thread */
    if (!vaud_win32_join_thread(ctx, plat, 5000, 0))
        return 0;

    /* Stop audio client */
    if (plat->client) {
        const HRESULT stop_hr = IAudioClient_Stop(plat->client);
        if (FAILED(stop_hr)) {
            vaud_stats_add(&ctx->stats.backend_write_failures, 1);
            vaud_set_error(VAUD_ERR_PLATFORM, "Failed to stop the WASAPI audio client");
        }
    }

    /* Release interfaces */
    if (plat->render)
        IAudioRenderClient_Release(plat->render);
    if (plat->client)
        IAudioClient_Release(plat->client);
    if (plat->device)
        IMMDevice_Release(plat->device);

    /* Close handles */
    if (plat->event && !CloseHandle(plat->event)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to close the WASAPI render event");
    }
    if (plat->stop_event && !CloseHandle(plat->stop_event)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to close the WASAPI stop event");
    }
    if (plat->ready_event && !CloseHandle(plat->ready_event)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to close the WASAPI readiness event");
    }

    int com_initialized = plat->com_initialized;
    vaud_win32_free_render_buffers(plat);
    DeleteCriticalSection(&plat->pause_cs);
    free(plat);
    ctx->platform_data = NULL;

    if (com_initialized)
        CoUninitialize();
    return 1;
}

/// @copydoc vaud_platform_pause
void vaud_platform_pause(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_win32_data *plat = (vaud_win32_data *)ctx->platform_data;
    if (plat->owner_thread_id != GetCurrentThreadId()) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI pause must run on the context owner thread");
        return;
    }

    EnterCriticalSection(&plat->pause_cs);
    if (InterlockedCompareExchange(&plat->paused, 0, 0)) {
        LeaveCriticalSection(&plat->pause_cs);
        return;
    }

    InterlockedExchange(&plat->paused, 1);
    HRESULT stop_hr = plat->client ? IAudioClient_Stop(plat->client) : E_POINTER;
    HRESULT reset_hr = SUCCEEDED(stop_hr) ? IAudioClient_Reset(plat->client) : stop_hr;
    if (FAILED(reset_hr)) {
        if (FAILED(stop_hr) || (plat->client && SUCCEEDED(IAudioClient_Start(plat->client)))) {
            InterlockedExchange(&plat->paused, 0);
        }
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to pause and reset WASAPI client");
    }
    LeaveCriticalSection(&plat->pause_cs);
}

/// @copydoc vaud_platform_resume
void vaud_platform_resume(vaud_context_t ctx) {
    if (!ctx || !ctx->platform_data)
        return;

    vaud_win32_data *plat = (vaud_win32_data *)ctx->platform_data;
    if (plat->owner_thread_id != GetCurrentThreadId()) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "WASAPI resume must run on the context owner thread");
        return;
    }

    EnterCriticalSection(&plat->pause_cs);
    if (!InterlockedCompareExchange(&plat->paused, 0, 0)) {
        LeaveCriticalSection(&plat->pause_cs);
        return;
    }
    if (!InterlockedCompareExchange(&plat->running, 0, 0) || !plat->client) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Cannot resume an inactive WASAPI client");
        LeaveCriticalSection(&plat->pause_cs);
        return;
    }
    HRESULT hr = IAudioClient_Start(plat->client);
    if (FAILED(hr)) {
        vaud_stats_add(&ctx->stats.backend_write_failures, 1);
        vaud_set_error(VAUD_ERR_PLATFORM, "Failed to resume WASAPI client");
        LeaveCriticalSection(&plat->pause_cs);
        return;
    }
    InterlockedExchange(&plat->paused, 0);
    LeaveCriticalSection(&plat->pause_cs);
}

//===----------------------------------------------------------------------===//
// Timing
//===----------------------------------------------------------------------===//

static INIT_ONCE g_vaud_qpc_init_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER g_vaud_qpc_frequency = {0};

/// @brief Cache the process performance-counter frequency exactly once.
/// @param init_once Windows one-time initialization token.
/// @param parameter Unused caller parameter.
/// @param context Unused output context.
/// @return TRUE so Windows records the initialization as complete; an
///         unavailable frequency is represented by a cached zero.
static BOOL CALLBACK vaud_qpc_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    (void)init_once;
    (void)parameter;
    (void)context;
    if (!QueryPerformanceFrequency(&g_vaud_qpc_frequency) || g_vaud_qpc_frequency.QuadPart <= 0) {
        g_vaud_qpc_frequency.QuadPart = 0;
    }
    return TRUE;
}

/// @copydoc vaud_platform_now_ms
int64_t vaud_platform_now_ms(void) {
    (void)InitOnceExecuteOnce(&g_vaud_qpc_init_once, vaud_qpc_init_once, NULL, NULL);
    if (g_vaud_qpc_frequency.QuadPart <= 0)
        return (int64_t)GetTickCount64();

    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        return (int64_t)GetTickCount64();

    if (counter.QuadPart < 0)
        return (int64_t)GetTickCount64();
    const int64_t whole_seconds = counter.QuadPart / g_vaud_qpc_frequency.QuadPart;
    const int64_t remainder = counter.QuadPart % g_vaud_qpc_frequency.QuadPart;
    if (whole_seconds > INT64_MAX / 1000)
        return INT64_MAX;
    const int64_t remainder_ms =
        (int64_t)(((double)remainder * 1000.0) / (double)g_vaud_qpc_frequency.QuadPart);
    return whole_seconds * 1000 + remainder_ms;
}

#endif /* _WIN32 */
