//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_decode.c
// Purpose: Audio format detection and decoding to WAV/PCM (WAV passthrough,
//   Ogg Vorbis, MP3). The audio engine/mixer lives in rt_audio.c.
//
// Key invariants:
//   - File paths remain strict UTF-8 through the native filesystem boundary.
//   - Decoded sizes and RIFF fields are checked before allocation or narrowing.
//   - Failure never publishes a partial caller-owned output buffer.
//
// Ownership/Lifetime:
//   - Successful conversion outputs transfer malloc-owned bytes to the caller.
//   - File streams and decoder state are closed or released on every return path.
//
// Links: rt_audio.h, rt_audio_internal.h, rt_audio.c, rt_file_stdio.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements audio-container detection and conversion to PCM WAV data.
/// @details The runtime recognizes RIFF/WAV, Ogg Vorbis, and MP3 inputs from
///          leading bytes. Ogg and MP3 decoders produce bounded interleaved
///          16-bit PCM, which is wrapped in an in-memory RIFF/WAVE container
///          for the common sound-loading path. Every returned byte buffer is
///          allocated with `malloc` and transfers ownership to the caller.

#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_audio_internal.h"
#include "rt_error.h"
#include "rt_file_stdio.h"
#include "rt_mixgroup.h"
#include "rt_mp3.h"
#include "rt_object.h"
#include "rt_ogg.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_time.h"
#include "rt_trap.h"
#include "rt_vorbis.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Identify an encoded audio format from its leading bytes.
/// @details Recognizes RIFF/WAV and Ogg capture signatures, MP3 streams with an
///          ID3 tag, and MP3 frame-sync headers. The input is borrowed and only
///          the first four bytes are inspected.
/// @param data Encoded byte buffer.
/// @param size Number of readable bytes in @p data.
/// @return `1` for RIFF/WAV, `2` for Ogg, `3` for MP3, or `0` for invalid,
///         undersized, or unrecognized input.
int detect_audio_format_mem(const void *data, size_t size) {
    if (!data || size < 4)
        return 0;
    const uint8_t *hdr = (const uint8_t *)data;
    if (hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F')
        return 1;
    if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S')
        return 2;
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3')
        return 3;
    if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0)
        return 3;
    return 0;
}

/// @brief Detect audio file format from magic bytes.
/// @details Reads at most four leading bytes and delegates recognition to
///          @ref detect_audio_format_mem.
/// @param filepath NUL-terminated path to the encoded audio file.
/// @return `1` for RIFF/WAV, `2` for Ogg, `3` for MP3, or `0` when the file
///         cannot be opened/read or its signature is unrecognized.
int detect_audio_format(const char *filepath) {
    FILE *af = rt_file_stdio_open_utf8(filepath, "rb");
    if (!af)
        return 0;
    uint8_t hdr[4];
    size_t n = fread(hdr, 1, sizeof(hdr), af);
    fclose(af);
    return detect_audio_format_mem(hdr, n);
}

/// @brief Pack decoded PCM samples into a self-contained WAV byte buffer.
/// @details Builds a 44-byte RIFF/WAVE header followed by the PCM data so
///          the result can be fed back to the WAV-based ZannaAUD loader.
///          Used by the OGG/MP3 paths to convert decoded streams into WAV
///          form for a single uniform sound-loading code path. Validates
///          channels (1 or 2 only) and sample rate (`> 0` and
///          `≤ RT_AUDIO_MAX_SAMPLE_RATE`) up front. The returned buffer is
///          `malloc`-allocated and ownership transfers to the caller.
/// @param pcm         Interleaved 16-bit signed PCM samples.
/// @param frames      Number of frames (samples per channel).
/// @param channels    Channel count (1 = mono, 2 = stereo).
/// @param sample_rate Sample rate in Hz.
/// @param out_data    Receives the newly-allocated WAV buffer.
/// @param out_len     Receives the buffer length in bytes.
/// @return 0 on success, -1 on validation or allocation failure.
static int build_wav_from_pcm(const int16_t *pcm,
                              size_t frames,
                              int channels,
                              int sample_rate,
                              uint8_t **out_data,
                              size_t *out_len) {
    if (out_data)
        *out_data = NULL;
    if (out_len)
        *out_len = 0;
    if (!pcm || !out_data || !out_len || frames == 0 || channels < 1 || channels > 2 ||
        sample_rate <= 0 || sample_rate > RT_AUDIO_MAX_SAMPLE_RATE)
        return -1;

    if (frames > SIZE_MAX / (size_t)channels)
        return -1;
    size_t sample_count = frames * (size_t)channels;
    if (sample_count > SIZE_MAX / sizeof(int16_t))
        return -1;

    size_t data_size = sample_count * sizeof(int16_t);
    if (data_size > RT_AUDIO_MAX_DECODED_SOUND_BYTES || data_size > UINT32_MAX - 36 ||
        data_size > SIZE_MAX - 44)
        return -1;

    size_t wav_size = 44 + data_size;
    uint8_t *wav = (uint8_t *)malloc(wav_size);
    if (!wav)
        return -1;

    memcpy(wav, "RIFF", 4);
    uint32_t riff_size = (uint32_t)(wav_size - 8);
    wav[4] = (uint8_t)(riff_size);
    wav[5] = (uint8_t)(riff_size >> 8);
    wav[6] = (uint8_t)(riff_size >> 16);
    wav[7] = (uint8_t)(riff_size >> 24);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    wav[16] = 16;
    wav[17] = wav[18] = wav[19] = 0;
    wav[20] = 1;
    wav[21] = 0;
    wav[22] = (uint8_t)channels;
    wav[23] = 0;
    wav[24] = (uint8_t)(sample_rate);
    wav[25] = (uint8_t)(sample_rate >> 8);
    wav[26] = (uint8_t)(sample_rate >> 16);
    wav[27] = (uint8_t)(sample_rate >> 24);
    uint64_t byte_rate64 = (uint64_t)sample_rate * (uint64_t)channels * 2u;
    if (byte_rate64 > UINT32_MAX) {
        free(wav);
        return -1;
    }
    uint32_t byte_rate = (uint32_t)byte_rate64;
    wav[28] = (uint8_t)(byte_rate);
    wav[29] = (uint8_t)(byte_rate >> 8);
    wav[30] = (uint8_t)(byte_rate >> 16);
    wav[31] = (uint8_t)(byte_rate >> 24);
    wav[32] = (uint8_t)(channels * 2);
    wav[33] = 0;
    wav[34] = 16;
    wav[35] = 0;
    memcpy(wav + 36, "data", 4);
    wav[40] = (uint8_t)(data_size);
    wav[41] = (uint8_t)(data_size >> 8);
    wav[42] = (uint8_t)(data_size >> 16);
    wav[43] = (uint8_t)(data_size >> 24);
    memcpy(wav + 44, pcm, data_size);

    *out_data = wav;
    *out_len = wav_size;
    return 0;
}

/// @brief Decode an Ogg/Vorbis stream end-to-end and re-emit as WAV bytes.
/// @details Drives @p reader packet-by-packet: locates the Vorbis bitstream
///          (first BOS packet whose payload starts with `\x01vorbis`),
///          feeds the three Vorbis setup headers to the decoder, then
///          decodes every audio packet, growing a single int16 PCM buffer
///          (doubling capacity with overflow guards against
///          `RT_AUDIO_MAX_DECODED_SOUND_BYTES`). Once the stream ends the
///          buffer is wrapped into WAV form via @ref build_wav_from_pcm.
///          Common entry point for both file-backed and memory-backed
///          OGG loaders.
/// @param reader   Ogg packet reader positioned at the start of the stream.
/// @param out_data Receives the malloc'd WAV buffer on success.
/// @param out_len  Receives the WAV buffer length on success.
/// @return 0 on success, -1 on malformed stream, decode error, oversize, or OOM.
static int ogg_decode_reader_to_wav(ogg_reader_t *reader, uint8_t **out_data, size_t *out_len) {
    if (!reader || !out_data || !out_len)
        return -1;

    vorbis_decoder_t *dec = vorbis_decoder_new();
    if (!dec)
        return -1;

    uint32_t vorbis_serial = 0;
    int have_vorbis_serial = 0;
    int header_num = 0;
    int have_audio_packet = 0;
    const uint8_t *pkt_data = NULL;
    size_t pkt_len = 0;
    ogg_packet_info_t info;

    while (ogg_reader_next_packet_ex(reader, &pkt_data, &pkt_len, &info)) {
        if (!have_vorbis_serial) {
            if (!info.bos || pkt_len < 7 || pkt_data[0] != 1 ||
                memcmp(pkt_data + 1, "vorbis", 6) != 0)
                continue;
            vorbis_serial = info.serial_number;
            have_vorbis_serial = 1;
        }
        if (info.serial_number != vorbis_serial)
            continue;
        if (header_num >= 3) {
            have_audio_packet = 1;
            break;
        }
        if (vorbis_decode_header(dec, pkt_data, pkt_len, header_num) != 0) {
            vorbis_decoder_free(dec);
            return -1;
        }
        header_num++;
    }

    if (!have_vorbis_serial || header_num < 3 || !have_audio_packet) {
        vorbis_decoder_free(dec);
        return -1;
    }

    int channels = vorbis_get_channels(dec);
    int sample_rate = vorbis_get_sample_rate(dec);
    if (channels < 1 || channels > 2 || sample_rate <= 0 ||
        sample_rate > RT_AUDIO_MAX_SAMPLE_RATE) {
        vorbis_decoder_free(dec);
        return -1;
    }

    int16_t *pcm_buf = NULL;
    size_t pcm_frames = 0;
    size_t pcm_cap = 0;

    do {
        if (info.serial_number != vorbis_serial)
            continue;
        int16_t *frame_pcm = NULL;
        int frame_samples = 0;
        if (vorbis_decode_packet(dec, pkt_data, pkt_len, &frame_pcm, &frame_samples) != 0) {
            free(pcm_buf);
            vorbis_decoder_free(dec);
            return -1;
        }
        if (frame_samples <= 0 || !frame_pcm)
            continue;

        size_t needed = pcm_frames + (size_t)frame_samples;
        if (needed < pcm_frames || needed > SIZE_MAX / (size_t)channels ||
            needed * (size_t)channels > SIZE_MAX / sizeof(int16_t) ||
            needed * (size_t)channels * sizeof(int16_t) > RT_AUDIO_MAX_DECODED_SOUND_BYTES) {
            free(pcm_buf);
            vorbis_decoder_free(dec);
            return -1;
        }
        if (needed > pcm_cap) {
            size_t new_cap = pcm_cap ? (pcm_cap > SIZE_MAX / 2 ? needed : pcm_cap * 2) : 65536;
            if (new_cap < needed)
                new_cap = needed;
            if (new_cap > SIZE_MAX / (size_t)channels ||
                new_cap * (size_t)channels > SIZE_MAX / sizeof(int16_t) ||
                new_cap * (size_t)channels * sizeof(int16_t) > RT_AUDIO_MAX_DECODED_SOUND_BYTES)
                new_cap = needed;
            int16_t *new_buf =
                (int16_t *)realloc(pcm_buf, new_cap * (size_t)channels * sizeof(int16_t));
            if (!new_buf) {
                free(pcm_buf);
                vorbis_decoder_free(dec);
                return -1;
            }
            pcm_buf = new_buf;
            pcm_cap = new_cap;
        }

        memcpy(pcm_buf + pcm_frames * (size_t)channels,
               frame_pcm,
               (size_t)frame_samples * (size_t)channels * sizeof(int16_t));
        pcm_frames += (size_t)frame_samples;
    } while (ogg_reader_next_packet_ex(reader, &pkt_data, &pkt_len, &info));

    vorbis_decoder_free(dec);
    if (pcm_frames == 0) {
        free(pcm_buf);
        return -1;
    }

    int rc = build_wav_from_pcm(pcm_buf, pcm_frames, channels, sample_rate, out_data, out_len);
    free(pcm_buf);
    return rc;
}

/// @brief Open @p filepath as an Ogg stream and decode it to a WAV buffer.
/// @details Thin wrapper around @ref ogg_decode_reader_to_wav that owns
///          the @ref ogg_reader_t lifetime for the file-backed case.
/// @param filepath Path to an `.ogg` container.
/// @param out_data Receives the malloc'd WAV buffer on success.
/// @param out_len  Receives the WAV buffer length on success.
/// @return 0 on success, -1 on open/decode failure.
int ogg_file_to_wav(const char *filepath, uint8_t **out_data, size_t *out_len) {
    ogg_reader_t *reader = ogg_reader_open_file(filepath);
    if (!reader)
        return -1;
    int rc = ogg_decode_reader_to_wav(reader, out_data, out_len);
    ogg_reader_free(reader);
    return rc;
}

/// @brief Decode an in-memory Ogg buffer to a WAV byte buffer.
/// @details Thin wrapper around @ref ogg_decode_reader_to_wav for the
///          memory-backed case. Used by @ref rt_sound_load_mem and the
///          asset-loader fast path.
/// @param data     Bytes of an Ogg container.
/// @param size     Length of @p data in bytes.
/// @param out_data Receives the malloc'd WAV buffer on success.
/// @param out_len  Receives the WAV buffer length on success.
/// @return 0 on success, -1 on open/decode failure.
int ogg_mem_to_wav(const void *data, size_t size, uint8_t **out_data, size_t *out_len) {
    ogg_reader_t *reader = ogg_reader_open_mem((const uint8_t *)data, size);
    if (!reader)
        return -1;
    int rc = ogg_decode_reader_to_wav(reader, out_data, out_len);
    ogg_reader_free(reader);
    return rc;
}

/// @brief Decode an in-memory MP3 frame stream to a WAV byte buffer.
/// @details Spins up an @ref mp3_decoder_t, runs `mp3_decode_file` over
///          the byte range (which streams every frame and concatenates
///          PCM), and then hands the resulting int16 PCM to
///          @ref build_wav_from_pcm. Validates returned channel count
///          (1 or 2) and sample rate (`> 0`, `≤ RT_AUDIO_MAX_SAMPLE_RATE`)
///          before wrapping.
/// @param data     MP3 byte stream (may start with an ID3 tag).
/// @param size     Length of @p data in bytes.
/// @param out_data Receives the malloc'd WAV buffer on success.
/// @param out_len  Receives the WAV buffer length on success.
/// @return 0 on success, -1 on decode error, validation failure, or OOM.
int mp3_data_to_wav(const uint8_t *data, size_t size, uint8_t **out_data, size_t *out_len) {
    if (!data || size == 0)
        return -1;

    mp3_decoder_t *dec = mp3_decoder_new();
    if (!dec)
        return -1;

    int16_t *pcm = NULL;
    int samples = 0;
    int channels = 0;
    int sample_rate = 0;
    int rc = mp3_decode_file(dec, data, size, &pcm, &samples, &channels, &sample_rate);
    mp3_decoder_free(dec);

    if (rc != 0 || !pcm || samples <= 0 || channels < 1 || channels > 2 || sample_rate <= 0 ||
        sample_rate > RT_AUDIO_MAX_SAMPLE_RATE) {
        free(pcm);
        return -1;
    }

    rc = build_wav_from_pcm(pcm, (size_t)samples, channels, sample_rate, out_data, out_len);
    free(pcm);
    return rc;
}

/// @brief Decode an MP3 file to a WAV-format memory buffer.
/// @details Reads the entire source file into temporary storage, rejecting
///          empty files and files larger than 256 MiB, then delegates decoding
///          to @ref mp3_data_to_wav. Temporary MP3 bytes are released before
///          return.
/// @param filepath NUL-terminated path to the MP3 file.
/// @param out_data Receives a `malloc`-allocated WAV buffer on success.
/// @param out_len Receives the WAV buffer length in bytes on success.
/// @return `0` on success, or `-1` on file, size, allocation, or decode failure.
int mp3_file_to_wav(const char *filepath, uint8_t **out_data, size_t *out_len) {
    FILE *mf = rt_file_stdio_open_utf8(filepath, "rb");
    if (!mf)
        return -1;
    if (fseek(mf, 0, SEEK_END) != 0) {
        fclose(mf);
        return -1;
    }
    long mf_len = ftell(mf);
    if (fseek(mf, 0, SEEK_SET) != 0) {
        fclose(mf);
        return -1;
    }
    if (mf_len <= 0 || mf_len > 256 * 1024 * 1024) {
        fclose(mf);
        return -1;
    }
    uint8_t *mf_data = (uint8_t *)malloc((size_t)mf_len);
    if (!mf_data) {
        fclose(mf);
        return -1;
    }
    if (fread(mf_data, 1, (size_t)mf_len, mf) != (size_t)mf_len) {
        free(mf_data);
        fclose(mf);
        return -1;
    }
    fclose(mf);

    int rc = mp3_data_to_wav(mf_data, (size_t)mf_len, out_data, out_len);
    free(mf_data);
    return rc;
}
