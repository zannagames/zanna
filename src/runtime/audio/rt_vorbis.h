//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_vorbis.h
// Purpose: Vorbis audio decoder — decodes OGG Vorbis to PCM.
// Key invariants:
//   - Baseline Vorbis I decoder (floor type 1, residue types 0/1/2)
//   - Output is interleaved 16-bit signed PCM for up to eight channels
//   - Forward-only decoding (no seeking)
// Ownership/Lifetime:
//   - Caller owns vorbis_decoder and must call vorbis_decoder_free
// Links: rt_ogg.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the stateful Vorbis I packet decoder.
/// @details Callers feed the identification, comment, and setup packets in
///          order, then submit forward-only audio packets. Decoder state owns
///          all parsed configuration, overlap-add history, and the reusable PCM
///          output buffer returned by successful packet decoding.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque Vorbis decoder handle.
typedef struct vorbis_decoder vorbis_decoder_t;

/// @brief Create a Vorbis decoder.
/// @return Caller-owned decoder or NULL on allocation failure.
vorbis_decoder_t *vorbis_decoder_new(void);

/// @brief Free a Vorbis decoder and all its resources.
/// @details Invalidates any PCM or diagnostic pointer borrowed from @p dec.
/// @param dec Decoder returned by @ref vorbis_decoder_new; NULL is accepted.
void vorbis_decoder_free(vorbis_decoder_t *dec);

/// @brief Feed the three Vorbis header packets to the decoder.
/// @details Packets `0`, `1`, and `2` must be submitted successfully in order
///          before any audio packet can be decoded. Repeated or out-of-order
///          packets and incomplete identification/comment framing are rejected.
/// @param dec Decoder instance.
/// @param packet_data Borrowed packet data.
/// @param packet_len Length of packet data in bytes.
/// @param packet_num 0=identification, 1=comment, 2=setup.
/// @return `0` on success or `-1` on invalid/malformed/unsupported input.
int vorbis_decode_header(vorbis_decoder_t *dec,
                         const uint8_t *packet_data,
                         size_t packet_len,
                         int packet_num);

/// @brief Decode one audio packet into PCM samples.
/// @details When both output pointers are valid, they are reset to NULL and zero
///          before packet validation and remain inert on failure.
/// @param dec Decoder instance (headers must have been parsed first).
/// @param packet_data Borrowed audio-packet data.
/// @param packet_len Packet length in bytes.
/// @param out_pcm Receives decoder-owned interleaved signed 16-bit PCM, valid
///        until the next decode or decoder free.
/// @param out_samples Receives output sample-frame count per channel.
/// @return `0` on success or `-1` on invalid/malformed input/allocation failure.
int vorbis_decode_packet(vorbis_decoder_t *dec,
                         const uint8_t *packet_data,
                         size_t packet_len,
                         int16_t **out_pcm,
                         int *out_samples);

/// @brief Get the sample rate from the identification header.
/// @param dec Decoder instance, or NULL.
/// @return Parsed sample rate in hertz, or zero when unavailable.
int vorbis_get_sample_rate(const vorbis_decoder_t *dec);

/// @brief Get the number of channels from the identification header.
/// @param dec Decoder instance, or NULL.
/// @return Parsed channel count, or zero when unavailable.
int vorbis_get_channels(const vorbis_decoder_t *dec);

/// @brief Return the last decoder error message.
/// @details The pointer is owned by @p dec and remains valid until the next decoder call or
///          `vorbis_decoder_free`. Returns an empty string when no detailed error is available.
/// @param dec Decoder instance, or NULL.
/// @return Borrowed NUL-terminated diagnostic string.
const char *vorbis_last_error(const vorbis_decoder_t *dec);

#ifdef __cplusplus
}
#endif
