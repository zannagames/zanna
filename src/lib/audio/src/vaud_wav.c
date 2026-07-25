//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD WAV File Parser
//
// Parses RIFF WAV files containing PCM audio data. Supports:
// - 8-bit unsigned PCM
// - 16-bit signed PCM
// - Mono and stereo
// - Any sample rate (will be resampled by caller if needed)
//
// WAV file format (simplified):
//   Offset  Size  Description
//   0       4     "RIFF" chunk ID
//   4       4     Chunk size (file size - 8)
//   8       4     "WAVE" format
//   12      4     "fmt " subchunk ID
//   16      4     Subchunk size (16 for PCM)
//   20      2     Audio format (1 = PCM)
//   22      2     Number of channels
//   24      4     Sample rate
//   28      4     Byte rate
//   32      2     Block align
//   34      2     Bits per sample
//   36      4     "data" subchunk ID
//   40      4     Data size
//   44      ...   PCM data
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief WAV file parser for ZannaAUD.
/// @details Parses RIFF chunks from memory or large-file-capable streams,
///          validates mono/stereo PCM and IEEE-float layouts, converts every
///          accepted source representation to the internal interleaved stereo
///          signed-16 format, and provides bounded eager and streaming decode
///          paths plus dependency-free cubic resampling.

#if !defined(_WIN32) && !defined(_FILE_OFFSET_BITS)
#define _FILE_OFFSET_BITS 64
#endif
#if !defined(_WIN32) && !defined(_LARGEFILE_SOURCE)
#define _LARGEFILE_SOURCE
#endif

#include "vaud_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/types.h>
#endif

//===----------------------------------------------------------------------===//
// WAV File Constants
//===----------------------------------------------------------------------===//

#define WAV_RIFF_ID 0x46464952 /* "RIFF" in little-endian */
#define WAV_WAVE_ID 0x45564157 /* "WAVE" in little-endian */
#define WAV_FMT_ID 0x20746D66  /* "fmt " in little-endian */
#define WAV_DATA_ID 0x61746164 /* "data" in little-endian */
#define WAV_FORMAT_PCM 1
#define WAV_FORMAT_IEEE_FLOAT 3

/// @brief Number of source frames converted per file-backed sound decode chunk.
/// @details File loads decode straight into the final stereo PCM buffer; this
///          chunk size bounds the temporary raw-byte buffer needed for format
///          conversion.
#define VAUD_WAV_LOAD_CHUNK_FRAMES 4096

/// @brief Maximum decoded in-memory WAV payload accepted by eager loaders.
/// @details Sound effects are decoded into stereo 16-bit PCM before being handed
///          to the mixer.  Keeping a hard cap avoids accidental multi-gigabyte
///          allocations when an input file has a very large or malformed data
///          chunk.  Long-form playback should use the streaming music path.
#define VAUD_WAV_MAX_DECODED_BYTES ((size_t)512u * 1024u * 1024u)

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

/// @copydoc vaud_wav_seek_stream
int vaud_wav_seek_stream(void *file, int64_t offset, int origin) {
    if (!file)
        return 0;
#if defined(_WIN32)
    return _fseeki64((FILE *)file, offset, origin) == 0;
#else
    return fseeko((FILE *)file, (off_t)offset, origin) == 0;
#endif
}

/// @brief Query a WAV stream position using the platform's 64-bit file API.
/// @param file Open standard-I/O stream.
/// @return Current byte offset, or -1 for a null stream or query failure.
static int64_t vaud_wav_tell_stream(FILE *file) {
    if (!file)
        return -1;
#if defined(_WIN32)
    return (int64_t)_ftelli64(file);
#else
    return (int64_t)ftello(file);
#endif
}

/// @brief Read a 16-bit little-endian value from a buffer.
/// @param p Pointer to at least two readable bytes.
/// @return Host-order unsigned value.
static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/// @brief Read a 32-bit little-endian value from a buffer.
/// @param p Pointer to at least four readable bytes.
/// @return Host-order unsigned value.
static inline uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// @brief Convert 8-bit unsigned sample to 16-bit signed.
/// @param sample Unsigned PCM sample with 128 as silence.
/// @return Signed-16 sample with the source range scaled by 256.
static inline int16_t u8_to_s16(uint8_t sample) {
    return (int16_t)(((int32_t)sample - 128) * 256);
}

/// @brief Convert 24-bit signed little-endian sample to 16-bit signed.
/// @param p Pointer to the three-byte two's-complement sample.
/// @return Most-significant signed 16 bits after sign extension.
static inline int16_t s24_to_s16(const uint8_t *p) {
    int32_t val = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (val & 0x800000)
        val |= (int32_t)0xFF000000; // sign extend
    return (int16_t)(val >> 8);
}

/// @brief Convert 32-bit signed little-endian sample to 16-bit signed.
/// @param p Pointer to the four-byte two's-complement sample.
/// @return Most-significant signed 16 bits.
static inline int16_t s32_to_s16(const uint8_t *p) {
    uint32_t hi = read_u32_le(p) >> 16;
    return hi >= 0x8000u ? (int16_t)((int32_t)hi - 0x10000) : (int16_t)hi;
}

/// @brief Convert 32-bit IEEE float sample to 16-bit signed.
/// @details Non-finite values become silence and finite values outside the
///          normalized interval saturate.
/// @param p Pointer to the little-endian IEEE-754 binary32 sample.
/// @return Signed-16 PCM sample.
static inline int16_t f32_to_s16(const uint8_t *p) {
    uint32_t bits = read_u32_le(p);
    float val;
    memcpy(&val, &bits, sizeof(float));
    if (!isfinite(val))
        val = 0.0f;
    if (val >= 1.0f)
        return INT16_MAX;
    if (val <= -1.0f)
        return INT16_MIN;
    return (int16_t)(val < 0.0f ? val * 32768.0f : val * 32767.0f);
}

/// @brief Compute the decoded stereo PCM byte count for a WAV frame count.
/// @details All WAV loaders convert to the internal stereo S16 representation.
///          This helper centralizes overflow checks and enforces the eager-load
///          memory budget used to reject unusually large sound effects.
/// @param frame_count Number of source frames in the WAV data chunk.
/// @param out_bytes Receives `frame_count * 2 * sizeof(int16_t)` on success.
/// @return Non-zero when the decoded buffer size is valid and within budget.
static int wav_decoded_stereo_bytes(int64_t frame_count, size_t *out_bytes) {
    if (out_bytes)
        *out_bytes = 0;
    if (!out_bytes || frame_count <= 0 ||
        (uint64_t)frame_count > SIZE_MAX / (2u * sizeof(int16_t))) {
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV data size");
        return 0;
    }

    size_t bytes = (size_t)frame_count * 2u * sizeof(int16_t);
    if (bytes > VAUD_WAV_MAX_DECODED_BYTES) {
        vaud_set_error(VAUD_ERR_FORMAT, "Decoded WAV data exceeds eager-load limit");
        return 0;
    }
    *out_bytes = bytes;
    return 1;
}

/// @brief Convert one PCM frame from raw bytes to stereo 16-bit signed samples.
/// @details Handles 8/16/24/32-bit PCM and 32-bit float, mono and stereo.
///          Mono sources are duplicated to both output channels.
/// @param src Pointer to raw PCM data for one frame.
/// @param bits_per_sample Bits per sample (8, 16, 24, or 32).
/// @param channels Number of channels in source (1 or 2).
/// @param audio_format 1=PCM, 3=IEEE float.
/// @param left Output: left channel sample.
/// @param right Output: right channel sample.
static inline void decode_pcm_frame(const uint8_t *src,
                                    int32_t bits_per_sample,
                                    int32_t channels,
                                    int32_t audio_format,
                                    int16_t *left,
                                    int16_t *right) {
    int bytes_per_sample = bits_per_sample / 8;
    switch (bits_per_sample) {
        case 8:
            *left = u8_to_s16(src[0]);
            *right = (channels == 2) ? u8_to_s16(src[1]) : *left;
            break;
        case 16:
            *left = (int16_t)read_u16_le(src);
            *right = (channels == 2) ? (int16_t)read_u16_le(src + 2) : *left;
            break;
        case 24:
            *left = s24_to_s16(src);
            *right = (channels == 2) ? s24_to_s16(src + 3) : *left;
            break;
        case 32:
            if (audio_format == WAV_FORMAT_IEEE_FLOAT) {
                *left = f32_to_s16(src);
                *right = (channels == 2) ? f32_to_s16(src + 4) : *left;
            } else {
                // 32-bit integer PCM: take upper 16 bits
                *left = s32_to_s16(src);
                *right = (channels == 2) ? s32_to_s16(src + 4) : *left;
            }
            break;
        default:
            *left = *right = 0;
            break;
    }
    (void)bytes_per_sample;
}

//===----------------------------------------------------------------------===//
// WAV Header Parsing
//===----------------------------------------------------------------------===//

/// @brief WAV format information extracted from header.
typedef struct {
    int32_t sample_rate;
    int32_t channels;
    int32_t bits_per_sample;
    int32_t audio_format; /* 1=PCM, 3=IEEE float */
    int32_t byte_rate;
    int32_t block_align;
    int64_t data_offset; /* Byte offset to PCM data */
    int64_t data_size;   /* Size of PCM data in bytes */
} vaud_wav_info;

/// @brief Derive the raw byte stride of one interleaved source frame.
/// @param info Parsed WAV channel and bit-depth metadata.
/// @param out_bytes Receives bytes per complete source frame.
/// @return 1 for a positive representable byte stride; otherwise 0.
static int wav_bytes_per_frame(const vaud_wav_info *info, int32_t *out_bytes) {
    if (!info || !out_bytes || info->channels <= 0 || info->bits_per_sample <= 0)
        return 0;
    if ((info->bits_per_sample % 8) != 0)
        return 0;
    int32_t bytes_per_sample = info->bits_per_sample / 8;
    if (bytes_per_sample <= 0 || bytes_per_sample > INT32_MAX / info->channels)
        return 0;
    *out_bytes = bytes_per_sample * info->channels;
    return *out_bytes > 0;
}

/// @brief Validate structural relationships in a parsed WAV format chunk.
/// @details Requires block alignment to match channel count and sample width.
///          Byte-rate mismatches are intentionally tolerated because playback
///          never relies on that advisory field.
/// @param info Parsed format metadata.
/// @return 1 when structurally decodable; otherwise 0 after setting an error.
static int validate_wav_format(const vaud_wav_info *info) {
    int32_t bytes_per_frame = 0;
    if (!wav_bytes_per_frame(info, &bytes_per_frame)) {
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV sample size");
        return 0;
    }

    if (info->block_align != bytes_per_frame) {
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV block align");
        return 0;
    }

    /* Some authoring tools write an inconsistent byte_rate while the block
     * alignment and data chunk are still valid. Playback relies on block_align
     * and sample_rate, so tolerate byte_rate mismatches instead of rejecting
     * otherwise decodable files. */

    return 1;
}

/// @brief Require the data chunk to contain only complete sample frames.
/// @param info Parsed format and data-chunk metadata.
/// @return 1 when data size is nonnegative and frame-aligned; otherwise 0.
static int validate_wav_data_alignment(const vaud_wav_info *info) {
    int32_t bytes_per_frame = 0;
    if (!wav_bytes_per_frame(info, &bytes_per_frame) || info->data_size < 0 ||
        (info->data_size % bytes_per_frame) != 0) {
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV data size");
        return 0;
    }
    return 1;
}

/// @brief Reset all successful-stream outputs to inert values after a failure.
/// @details Callers use this once pointer validation has succeeded so a later
///          parse or seek error cannot leave stale file metadata behind.
/// @param out_file Receives NULL.
/// @param out_data_offset Receives zero.
/// @param out_data_size Receives zero.
/// @param out_frames Receives zero.
/// @param out_sample_rate Receives zero.
/// @param out_channels Receives zero.
/// @param out_bits Receives zero.
/// @param out_format Receives zero.
static void reset_wav_stream_outputs(void **out_file,
                                     int64_t *out_data_offset,
                                     int64_t *out_data_size,
                                     int64_t *out_frames,
                                     int32_t *out_sample_rate,
                                     int32_t *out_channels,
                                     int32_t *out_bits,
                                     int32_t *out_format) {
    *out_file = NULL;
    *out_data_offset = 0;
    *out_data_size = 0;
    *out_frames = 0;
    *out_sample_rate = 0;
    *out_channels = 0;
    *out_bits = 0;
    *out_format = 0;
}

/// @brief Parse WAV header from memory buffer.
/// @param data Pointer to WAV file data.
/// @param size Size of data in bytes.
/// @param info Output: parsed format information.
/// @return 1 on success, 0 on failure.
static int parse_wav_header(const uint8_t *data, size_t size, vaud_wav_info *info) {
    if (!data || !info) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL parameter");
        return 0;
    }

    memset(info, 0, sizeof(*info));

    if (size < 44) {
        vaud_set_error(VAUD_ERR_FORMAT, "WAV file too small");
        return 0;
    }

    /* Check RIFF header */
    if (read_u32_le(data) != WAV_RIFF_ID) {
        vaud_set_error(VAUD_ERR_FORMAT, "Not a RIFF file");
        return 0;
    }

    /* Check WAVE format */
    if (read_u32_le(data + 8) != WAV_WAVE_ID) {
        vaud_set_error(VAUD_ERR_FORMAT, "Not a WAVE file");
        return 0;
    }

    /* Find and parse fmt chunk */
    size_t offset = 12;
    int found_fmt = 0;
    int found_data = 0;

    while (offset + 8 <= size) {
        uint32_t chunk_id = read_u32_le(data + offset);
        uint32_t chunk_size = read_u32_le(data + offset + 4);
        size_t available = size - offset - 8;
        if ((size_t)chunk_size > available) {
            vaud_set_error(VAUD_ERR_FORMAT, "WAV chunk extends beyond file");
            return 0;
        }

        if (chunk_id == WAV_FMT_ID) {
            if (chunk_size < 16) {
                vaud_set_error(VAUD_ERR_FORMAT, "Invalid fmt chunk");
                return 0;
            }

            const uint8_t *fmt = data + offset + 8;
            uint16_t audio_format = read_u16_le(fmt);

            if (audio_format != WAV_FORMAT_PCM && audio_format != WAV_FORMAT_IEEE_FLOAT) {
                vaud_set_error(VAUD_ERR_FORMAT, "Only PCM and IEEE float formats supported");
                return 0;
            }

            info->audio_format = (int32_t)audio_format;
            info->channels = read_u16_le(fmt + 2);
            info->sample_rate = (int32_t)read_u32_le(fmt + 4);
            info->byte_rate = (int32_t)read_u32_le(fmt + 8);
            info->block_align = read_u16_le(fmt + 12);
            info->bits_per_sample = read_u16_le(fmt + 14);

            /* H-7: guard against division-by-zero in resampling (malformed file) */
            if (info->sample_rate <= 0 || info->sample_rate > 384000) {
                vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV sample rate");
                return 0;
            }

            if (info->channels < 1 || info->channels > 2) {
                vaud_set_error(VAUD_ERR_FORMAT, "Only mono and stereo supported");
                return 0;
            }

            if (audio_format == WAV_FORMAT_IEEE_FLOAT) {
                if (info->bits_per_sample != 32) {
                    vaud_set_error(VAUD_ERR_FORMAT, "Only 32-bit IEEE float supported");
                    return 0;
                }
            } else {
                if (info->bits_per_sample != 8 && info->bits_per_sample != 16 &&
                    info->bits_per_sample != 24 && info->bits_per_sample != 32) {
                    vaud_set_error(VAUD_ERR_FORMAT, "Only 8/16/24/32-bit PCM supported");
                    return 0;
                }
            }

            if (!validate_wav_format(info))
                return 0;

            found_fmt = 1;
        } else if (chunk_id == WAV_DATA_ID) {
            info->data_offset = (int64_t)(offset + 8);
            info->data_size = (int64_t)chunk_size;
            found_data = 1;
        }

        /* Move to next chunk (chunks are word-aligned) */
        offset += 8 + (size_t)chunk_size;
        if ((chunk_size & 1) && offset < size)
            offset++; /* Pad byte */

        if (found_fmt && found_data)
            break;
    }

    if (!found_fmt) {
        vaud_set_error(VAUD_ERR_FORMAT, "Missing fmt chunk");
        return 0;
    }

    if (!found_data) {
        vaud_set_error(VAUD_ERR_FORMAT, "Missing data chunk");
        return 0;
    }

    return validate_wav_data_alignment(info);
}

/// @brief Parse a WAV header directly from an open file stream.
/// @param file Open FILE* positioned anywhere in the file.
/// @param info Output: parsed format information.
/// @return 1 on success, 0 on failure.
static int parse_wav_stream(FILE *file, vaud_wav_info *info) {
    if (!file || !info) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL parameter");
        return 0;
    }

    if (!vaud_wav_seek_stream(file, 0, SEEK_END)) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to seek WAV file");
        return 0;
    }
    int64_t file_size = vaud_wav_tell_stream(file);
    if (file_size < 44) {
        vaud_set_error(VAUD_ERR_FORMAT, "WAV file too small");
        return 0;
    }

    if (!vaud_wav_seek_stream(file, 0, SEEK_SET)) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to seek WAV file");
        return 0;
    }

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to read WAV header");
        return 0;
    }

    if (read_u32_le(header) != WAV_RIFF_ID) {
        vaud_set_error(VAUD_ERR_FORMAT, "Not a RIFF file");
        return 0;
    }
    if (read_u32_le(header + 8) != WAV_WAVE_ID) {
        vaud_set_error(VAUD_ERR_FORMAT, "Not a WAVE file");
        return 0;
    }

    memset(info, 0, sizeof(*info));
    int found_fmt = 0;
    int found_data = 0;

    while (1) {
        uint8_t chunk_header[8];
        size_t header_read = fread(chunk_header, 1, sizeof(chunk_header), file);
        if (header_read == 0)
            break;
        if (header_read != sizeof(chunk_header)) {
            vaud_set_error(VAUD_ERR_FORMAT, "Truncated WAV chunk header");
            return 0;
        }

        uint32_t chunk_id = read_u32_le(chunk_header);
        uint32_t chunk_size = read_u32_le(chunk_header + 4);
        int64_t chunk_data_offset = vaud_wav_tell_stream(file);
        if (chunk_data_offset < 0) {
            vaud_set_error(VAUD_ERR_FILE, "Failed to read WAV chunk offset");
            return 0;
        }

        int64_t padded_size = (int64_t)chunk_size + (int64_t)(chunk_size & 1u);
        if ((int64_t)chunk_data_offset + padded_size > file_size) {
            vaud_set_error(VAUD_ERR_FORMAT, "WAV chunk extends beyond file");
            return 0;
        }

        if (chunk_id == WAV_FMT_ID) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                vaud_set_error(VAUD_ERR_FORMAT, "Invalid fmt chunk");
                return 0;
            }

            uint16_t audio_format = read_u16_le(fmt);
            if (audio_format != WAV_FORMAT_PCM && audio_format != WAV_FORMAT_IEEE_FLOAT) {
                vaud_set_error(VAUD_ERR_FORMAT, "Only PCM and IEEE float formats supported");
                return 0;
            }

            info->audio_format = (int32_t)audio_format;
            info->channels = read_u16_le(fmt + 2);
            info->sample_rate = (int32_t)read_u32_le(fmt + 4);
            info->byte_rate = (int32_t)read_u32_le(fmt + 8);
            info->block_align = read_u16_le(fmt + 12);
            info->bits_per_sample = read_u16_le(fmt + 14);

            if (info->sample_rate <= 0 || info->sample_rate > 384000) {
                vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV sample rate");
                return 0;
            }

            if (info->channels < 1 || info->channels > 2) {
                vaud_set_error(VAUD_ERR_FORMAT, "Only mono and stereo supported");
                return 0;
            }

            if (audio_format == WAV_FORMAT_IEEE_FLOAT) {
                if (info->bits_per_sample != 32) {
                    vaud_set_error(VAUD_ERR_FORMAT, "Only 32-bit IEEE float supported");
                    return 0;
                }
            } else if (info->bits_per_sample != 8 && info->bits_per_sample != 16 &&
                       info->bits_per_sample != 24 && info->bits_per_sample != 32) {
                vaud_set_error(VAUD_ERR_FORMAT, "Only 8/16/24/32-bit PCM supported");
                return 0;
            }

            if (!validate_wav_format(info))
                return 0;

            int64_t remaining = (int64_t)chunk_size - (int64_t)sizeof(fmt);
            if (remaining > 0 && !vaud_wav_seek_stream(file, remaining, SEEK_CUR)) {
                vaud_set_error(VAUD_ERR_FILE, "Failed to skip fmt chunk");
                return 0;
            }
            found_fmt = 1;
        } else if (chunk_id == WAV_DATA_ID) {
            info->data_offset = (int64_t)chunk_data_offset;
            info->data_size = (int64_t)chunk_size;
            if (!vaud_wav_seek_stream(file, (int64_t)chunk_size, SEEK_CUR)) {
                vaud_set_error(VAUD_ERR_FILE, "Failed to skip data chunk");
                return 0;
            }
            found_data = 1;
        } else if (chunk_size > 0 && !vaud_wav_seek_stream(file, (int64_t)chunk_size, SEEK_CUR)) {
            vaud_set_error(VAUD_ERR_FILE, "Failed to skip WAV chunk");
            return 0;
        }

        if ((chunk_size & 1u) != 0 && !vaud_wav_seek_stream(file, 1, SEEK_CUR)) {
            vaud_set_error(VAUD_ERR_FILE, "Failed to skip WAV chunk padding");
            return 0;
        }

        if (found_fmt && found_data)
            break;
    }

    if (!found_fmt) {
        vaud_set_error(VAUD_ERR_FORMAT, "Missing fmt chunk");
        return 0;
    }
    if (!found_data) {
        vaud_set_error(VAUD_ERR_FORMAT, "Missing data chunk");
        return 0;
    }
    if (info->data_offset + info->data_size > file_size) {
        vaud_set_error(VAUD_ERR_FORMAT, "Data chunk extends beyond file");
        return 0;
    }

    return validate_wav_data_alignment(info);
}

//===----------------------------------------------------------------------===//
// PCM Conversion
//===----------------------------------------------------------------------===//

/// @brief Convert PCM data to internal format (16-bit stereo).
/// @param data Source PCM data.
/// @param info Format information.
/// @param out_samples Output: allocated stereo 16-bit samples.
/// @param out_frames Output: number of frames.
/// @return 1 on success, 0 on failure.
static int convert_pcm_to_stereo_s16(const uint8_t *data,
                                     const vaud_wav_info *info,
                                     int16_t **out_samples,
                                     int64_t *out_frames) {
    int32_t bytes_per_sample = info->bits_per_sample / 8;
    int32_t bytes_per_frame = bytes_per_sample * info->channels;
    if (bytes_per_sample <= 0 || bytes_per_frame <= 0 || info->data_size <= 0 ||
        (info->data_size % bytes_per_frame) != 0) {
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV data size");
        return 0;
    }
    int64_t frame_count = info->data_size / bytes_per_frame;
    size_t decoded_bytes = 0;
    if (!wav_decoded_stereo_bytes(frame_count, &decoded_bytes)) {
        return 0;
    }

    /* Allocate output buffer (always stereo) */
    int16_t *samples = (int16_t *)malloc(decoded_bytes);
    if (!samples) {
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate sample buffer");
        return 0;
    }

    const uint8_t *src = data + info->data_offset;
    int16_t *dst = samples;

    for (int64_t i = 0; i < frame_count; i++) {
        int16_t left, right;
        decode_pcm_frame(
            src, info->bits_per_sample, info->channels, info->audio_format, &left, &right);
        *dst++ = left;
        *dst++ = right;
        src += bytes_per_frame;
    }

    *out_samples = samples;
    *out_frames = frame_count;
    return 1;
}

//===----------------------------------------------------------------------===//
// Public Functions
//===----------------------------------------------------------------------===//

/// @copydoc vaud_wav_load_file
int vaud_wav_load_file(const char *path,
                       int16_t **out_samples,
                       int64_t *out_frames,
                       int32_t *out_sample_rate,
                       int32_t *out_channels) {
    if (!path || !out_samples || !out_frames || !out_sample_rate || !out_channels) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL parameter");
        return 0;
    }

    *out_samples = NULL;
    *out_frames = 0;
    *out_sample_rate = 0;
    *out_channels = 0;

    FILE *file = fopen(path, "rb");
    if (!file) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to open WAV file");
        return 0;
    }

    vaud_wav_info info;
    if (!parse_wav_stream(file, &info)) {
        fclose(file);
        return 0;
    }

    int32_t bytes_per_frame = 0;
    if (!wav_bytes_per_frame(&info, &bytes_per_frame)) {
        fclose(file);
        vaud_set_error(VAUD_ERR_FORMAT, "Invalid WAV sample size");
        return 0;
    }

    int64_t frame_count = info.data_size / bytes_per_frame;
    size_t decoded_bytes = 0;
    if (!wav_decoded_stereo_bytes(frame_count, &decoded_bytes)) {
        fclose(file);
        return 0;
    }

    int16_t *samples = (int16_t *)malloc(decoded_bytes);
    if (!samples) {
        fclose(file);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate sample buffer");
        return 0;
    }

    int32_t chunk_frames = VAUD_WAV_LOAD_CHUNK_FRAMES;
    if (frame_count < chunk_frames)
        chunk_frames = (int32_t)frame_count;
    size_t temp_size = (size_t)chunk_frames * (size_t)bytes_per_frame;
    uint8_t *temp = (uint8_t *)malloc(temp_size);
    if (!temp) {
        free(samples);
        fclose(file);
        vaud_set_error(VAUD_ERR_ALLOC, "Failed to allocate WAV decode buffer");
        return 0;
    }

    if (!vaud_wav_seek_stream(file, info.data_offset, SEEK_SET)) {
        free(temp);
        free(samples);
        fclose(file);
        vaud_set_error(VAUD_ERR_FILE, "Failed to seek WAV data");
        return 0;
    }

    int64_t written = 0;
    while (written < frame_count) {
        int64_t remaining = frame_count - written;
        int32_t request = remaining < chunk_frames ? (int32_t)remaining : chunk_frames;
        int32_t got = vaud_wav_read_frames_buffered(file,
                                                    samples + (written * 2),
                                                    request,
                                                    info.channels,
                                                    info.bits_per_sample,
                                                    info.audio_format,
                                                    temp,
                                                    temp_size);
        if (got != request) {
            free(temp);
            free(samples);
            fclose(file);
            vaud_set_error(VAUD_ERR_FILE, "Failed to read WAV data");
            return 0;
        }
        written += got;
    }

    free(temp);
    fclose(file);

    *out_samples = samples;
    *out_frames = frame_count;
    *out_sample_rate = info.sample_rate;
    *out_channels = info.channels;
    return 1;
}

/// @copydoc vaud_wav_load_mem
int vaud_wav_load_mem(const void *data,
                      size_t size,
                      int16_t **out_samples,
                      int64_t *out_frames,
                      int32_t *out_sample_rate,
                      int32_t *out_channels) {
    if (!data || !out_samples || !out_frames || !out_sample_rate || !out_channels) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL parameter");
        return 0;
    }

    /* Parse header */
    vaud_wav_info info;
    if (!parse_wav_header((const uint8_t *)data, size, &info)) {
        return 0;
    }

    /* Verify data is within bounds */
    if (info.data_offset + info.data_size > (int64_t)size) {
        vaud_set_error(VAUD_ERR_FORMAT, "Data chunk extends beyond file");
        return 0;
    }

    /* Convert to internal format */
    if (!convert_pcm_to_stereo_s16((const uint8_t *)data, &info, out_samples, out_frames)) {
        return 0;
    }

    *out_sample_rate = info.sample_rate;
    *out_channels = info.channels;
    return 1;
}

/// @copydoc vaud_wav_open_stream
int vaud_wav_open_stream(const char *path,
                         void **out_file,
                         int64_t *out_data_offset,
                         int64_t *out_data_size,
                         int64_t *out_frames,
                         int32_t *out_sample_rate,
                         int32_t *out_channels,
                         int32_t *out_bits,
                         int32_t *out_format) {
    if (!path || !out_file || !out_data_offset || !out_data_size || !out_frames ||
        !out_sample_rate || !out_channels || !out_bits || !out_format) {
        vaud_set_error(VAUD_ERR_INVALID_PARAM, "NULL parameter");
        return 0;
    }

    reset_wav_stream_outputs(out_file,
                             out_data_offset,
                             out_data_size,
                             out_frames,
                             out_sample_rate,
                             out_channels,
                             out_bits,
                             out_format);

    /* Open file */
    FILE *file = fopen(path, "rb");
    if (!file) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to open music file");
        return 0;
    }

    vaud_wav_info info;
    if (!parse_wav_stream(file, &info)) {
        fclose(file);
        return 0;
    }

    /* Calculate frame count */
    int32_t bytes_per_sample = info.bits_per_sample / 8;
    int32_t bytes_per_frame = bytes_per_sample * info.channels;
    int64_t frame_count = info.data_size / bytes_per_frame;

    *out_file = file;
    *out_data_offset = info.data_offset;
    *out_data_size = info.data_size;
    *out_frames = frame_count;
    *out_sample_rate = info.sample_rate;
    *out_channels = info.channels;
    *out_bits = info.bits_per_sample;
    *out_format = info.audio_format;

    /* Seek to start of data */
    if (!vaud_wav_seek_stream(file, info.data_offset, SEEK_SET)) {
        reset_wav_stream_outputs(out_file,
                                 out_data_offset,
                                 out_data_size,
                                 out_frames,
                                 out_sample_rate,
                                 out_channels,
                                 out_bits,
                                 out_format);
        fclose(file);
        vaud_set_error(VAUD_ERR_FILE, "Failed to seek WAV data");
        return 0;
    }

    return 1;
}

/// @copydoc vaud_wav_read_frames
int32_t vaud_wav_read_frames(void *file,
                             int16_t *samples,
                             int32_t frames,
                             int32_t channels,
                             int32_t bits_per_sample,
                             int32_t audio_format) {
    if (!file || !samples || frames <= 0)
        return 0;

    if (channels <= 0 || bits_per_sample <= 0)
        return 0;
    int32_t bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample > 0 && channels > INT32_MAX / bytes_per_sample)
        return 0;
    int32_t bytes_per_frame = bytes_per_sample * channels;
    if (bytes_per_sample <= 0 || bytes_per_frame <= 0)
        return 0;
    if ((size_t)frames > SIZE_MAX / (size_t)bytes_per_frame)
        return 0;
    size_t buffer_size = (size_t)frames * (size_t)bytes_per_frame;

    /* Temporary buffer for reading raw data */
    uint8_t *temp = (uint8_t *)malloc(buffer_size);
    if (!temp)
        return 0;

    int32_t frames_read = vaud_wav_read_frames_buffered(
        file, samples, frames, channels, bits_per_sample, audio_format, temp, buffer_size);

    free(temp);
    return frames_read;
}

/// @copydoc vaud_wav_read_frames_buffered
int32_t vaud_wav_read_frames_buffered(void *file,
                                      int16_t *samples,
                                      int32_t frames,
                                      int32_t channels,
                                      int32_t bits_per_sample,
                                      int32_t audio_format,
                                      uint8_t *temp,
                                      size_t temp_size) {
    if (!file || !samples || !temp || frames <= 0)
        return 0;

    if (channels <= 0 || bits_per_sample <= 0)
        return 0;
    int32_t bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample > 0 && channels > INT32_MAX / bytes_per_sample)
        return 0;
    int32_t bytes_per_frame = bytes_per_sample * channels;
    if (bytes_per_sample <= 0 || bytes_per_frame <= 0)
        return 0;

    if ((size_t)frames > SIZE_MAX / (size_t)bytes_per_frame)
        return 0;
    size_t buffer_size = (size_t)frames * (size_t)bytes_per_frame;
    if (buffer_size > temp_size)
        return 0;

    FILE *f = (FILE *)file;
    size_t bytes_read = fread(temp, 1, buffer_size, f);
    if (bytes_read < buffer_size && ferror(f)) {
        vaud_set_error(VAUD_ERR_FILE, "Failed to read WAV stream");
        return 0;
    }
    if ((bytes_read % (size_t)bytes_per_frame) != 0) {
        vaud_set_error(VAUD_ERR_FORMAT, "Truncated WAV frame in stream");
        return 0;
    }
    int32_t frames_read = (int32_t)(bytes_read / (size_t)bytes_per_frame);

    /* Convert to 16-bit stereo */
    const uint8_t *src = temp;
    int16_t *dst = samples;

    for (int32_t i = 0; i < frames_read; i++) {
        int16_t left, right;
        decode_pcm_frame(src, bits_per_sample, channels, audio_format, &left, &right);
        *dst++ = left;
        *dst++ = right;
        src += bytes_per_frame;
    }

    return frames_read;
}

//===----------------------------------------------------------------------===//
// Resampling
//===----------------------------------------------------------------------===//

/// @copydoc vaud_resample_output_frames
int64_t vaud_resample_output_frames(int64_t in_frames, int32_t in_rate, int32_t out_rate) {
    if (in_frames <= 0 || in_rate <= 0 || out_rate <= 0)
        return 0;
    if (in_rate == out_rate)
        return in_frames;
    if (in_frames > (INT64_MAX - in_rate + 1) / out_rate)
        return INT64_MAX;
    return (in_frames * out_rate + in_rate - 1) / in_rate;
}

/// @copydoc vaud_pcm_s16_buffer_size
int vaud_pcm_s16_buffer_size(int64_t frames, int32_t channels, size_t *out_bytes) {
    if (!out_bytes || frames <= 0 || channels <= 0)
        return 0;
    if ((uint64_t)frames > SIZE_MAX / (uint64_t)channels)
        return 0;
    size_t samples = (size_t)frames * (size_t)channels;
    if (samples > SIZE_MAX / sizeof(int16_t))
        return 0;
    *out_bytes = samples * sizeof(int16_t);
    return 1;
}

/// @brief Clamp a floating-point PCM value into signed 16-bit range.
/// @param sample Interpolated sample value.
/// @return The sample clipped to int16_t limits.
static inline int16_t clamp_double_to_s16(double sample) {
    if (!isfinite(sample))
        return 0;
    if (sample > 32767.0)
        return INT16_MAX;
    if (sample < -32768.0)
        return INT16_MIN;
    return (int16_t)(sample < 0.0 ? sample - 0.5 : sample + 0.5);
}

/// @brief Read one sample with edge clamping for interpolation kernels.
/// @param input Interleaved input PCM buffer.
/// @param frame Requested frame index.
/// @param in_frames Number of frames in @p input.
/// @param channels Number of interleaved channels.
/// @param channel Channel to read.
/// @return Signed 16-bit sample at the clamped frame/channel.
static inline double sample_s16_clamped(
    const int16_t *input, int64_t frame, int64_t in_frames, int32_t channels, int32_t channel) {
    if (frame < 0)
        frame = 0;
    if (frame >= in_frames)
        frame = in_frames - 1;
    return (double)input[frame * channels + channel];
}

/// @brief Interpolate one channel using a Catmull-Rom cubic kernel.
/// @param input Interleaved input PCM buffer.
/// @param frame Base input frame index.
/// @param frac Fractional distance from @p frame to the next frame.
/// @param in_frames Number of frames in @p input.
/// @param channels Number of interleaved channels.
/// @param channel Channel to interpolate.
/// @return Interpolated floating-point PCM sample.
static inline double cubic_sample_s16(const int16_t *input,
                                      int64_t frame,
                                      double frac,
                                      int64_t in_frames,
                                      int32_t channels,
                                      int32_t channel) {
    double y0 = sample_s16_clamped(input, frame - 1, in_frames, channels, channel);
    double y1 = sample_s16_clamped(input, frame, in_frames, channels, channel);
    double y2 = sample_s16_clamped(input, frame + 1, in_frames, channels, channel);
    double y3 = sample_s16_clamped(input, frame + 2, in_frames, channels, channel);

    double a0 = (-0.5 * y0) + (1.5 * y1) - (1.5 * y2) + (0.5 * y3);
    double a1 = y0 - (2.5 * y1) + (2.0 * y2) - (0.5 * y3);
    double a2 = (-0.5 * y0) + (0.5 * y2);
    double a3 = y1;
    return ((a0 * frac + a1) * frac + a2) * frac + a3;
}

/// @copydoc vaud_resample
void vaud_resample(const int16_t *input,
                   int64_t in_frames,
                   int32_t in_rate,
                   int16_t *output,
                   int64_t out_frames,
                   int32_t out_rate,
                   int32_t channels) {
    if (!input || !output || in_frames <= 0 || out_frames <= 0 || in_rate <= 0 || out_rate <= 0 ||
        channels <= 0)
        return;

    if (in_rate == out_rate && in_frames == out_frames) {
        if ((uint64_t)in_frames <= SIZE_MAX / (size_t)channels / sizeof(int16_t)) {
            size_t bytes = (size_t)in_frames * (size_t)channels * sizeof(int16_t);
            memmove(output, input, bytes);
        }
        return;
    }

    /* Dependency-free cubic interpolation resampler. */
    double ratio = (double)in_rate / (double)out_rate;

    for (int64_t out_idx = 0; out_idx < out_frames; out_idx++) {
        double in_pos = out_idx * ratio;
        int64_t in_idx = (int64_t)in_pos;
        double frac = in_pos - in_idx;

        /* Clamp to valid range */
        if (in_idx >= in_frames - 1) {
            in_idx = in_frames - 1;
            frac = 0.0;
        }

        for (int32_t ch = 0; ch < channels; ch++) {
            double interp = cubic_sample_s16(input, in_idx, frac, in_frames, channels, ch);
            output[out_idx * channels + ch] = clamp_double_to_s16(interp);
        }
    }
}
