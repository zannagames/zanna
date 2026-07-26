//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_zstd.h
// Purpose: From-scratch Zstandard (RFC 8878) decompressor — decode-only, no
//   dictionary support. Unblocks KTX2 supercompression scheme 2 (the default
//   output of the standard glTF texture toolchain) without any external
//   dependency.
// Key invariants:
//   - Single-shot API modeled on rt_compress_inflate_raw: caller provides the
//     whole frame, receives a malloc-owned buffer, and frees it with free().
//   - max_output bounds the decoded size against corrupt/hostile headers.
//   - Frames requiring a dictionary are rejected (KTX2 never uses them).
//   - Content checksums (xxhash64 low 32 bits) are verified when present.
//   - Exactly one standard frame must consume the complete input; concatenated,
//     skippable, and trailing data are rejected.
// Ownership/Lifetime:
//   - *out_data is malloc-allocated on success; the caller owns and frees it.
//   - Caller-provided output remains caller-owned and may contain partial
//     decoded bytes after a failed exact-destination operation.
// Links: src/runtime/io/rt_compress.h (DEFLATE counterpart),
//   misc/plans/3d_overhaul/03-texture-pipeline.md
//
//===----------------------------------------------------------------------===//
#ifndef RT_ZSTD_H
#define RT_ZSTD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Decompress one complete Zstandard frame.
/// @details Output pointers are reset before parsing. A successful zero-byte
///          frame still returns a freeable non-NULL allocation.
/// @param data Complete frame bytes starting with the standard Zstandard magic.
/// @param len Byte length of @p data.
/// @param max_output Upper bound for the decompressed size (guards allocation).
/// @param out_data Receives a malloc-owned buffer with the decompressed bytes.
/// @param out_len Receives the decompressed byte count.
/// @return 1 on success; 0 on malformed input, unsupported features
///         (dictionaries), checksum mismatch, or output larger than
///         @p max_output. On failure no buffer is returned.
int rt_zstd_decompress_raw(
    const uint8_t *data, size_t len, size_t max_output, uint8_t **out_data, size_t *out_len);

/// @brief Decompress one complete Zstandard frame directly into caller-owned storage.
/// @details Uses the same frame parser, dictionary rejection, bounds validation, entropy decoder,
///          and optional content-checksum verification as @ref rt_zstd_decompress_raw, but never
///          allocates or replaces the final output buffer. Temporary entropy tables/literals may
///          still allocate independently of the destination. On failure the
///          destination remains owned by the caller but may be partially modified.
/// @param data Complete Zstandard frame bytes.
/// @param len Byte length of @p data.
/// @param output Caller-owned destination with room for exactly @p output_size bytes.
/// @param output_size Required decoded byte count and destination capacity.
/// @return 1 only when the complete frame decodes to exactly @p output_size bytes; 0 for invalid
///         arguments, malformed/truncated input, unsupported dictionaries, size mismatch, trailing
///         bytes, checksum failure, or temporary allocation failure.
int rt_zstd_decompress_into(const uint8_t *data, size_t len, uint8_t *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* RT_ZSTD_H */
