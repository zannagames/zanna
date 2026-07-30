//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_compress.h
// Purpose: DEFLATE/GZIP compression and decompression (RFC 1951, RFC 1952) with no external
// dependencies, providing Bytes-in/Bytes-out API.
//
// Key invariants:
//   - No external dependencies; implements DEFLATE internally.
//   - Public compression levels are integers in [1, 9]; level 1 favors speed
//     and level 9 searches more deeply for matches.
//   - Managed decompression APIs trap on invalid, corrupt, truncated, or
//     over-limit input; native helpers report failure with a zero status.
//   - GZIP adds a standard header/trailer; raw DEFLATE does not.
//
// Ownership/Lifetime:
//   - Returned Bytes objects are GC-managed; callers should not free them directly.
//   - Input Bytes are not consumed or modified.
//
// Links: src/runtime/io/rt_compress.c (implementation),
//        docs/adr/0229-bounded-native-gzip-decoding.md,
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares dependency-free raw DEFLATE, zlib, and GZIP operations.
 * @details Exposes managed Bytes and string convenience APIs together with
 * allocation-free or malloc-returning native helpers for bounded decoder
 * integrations. Inputs are borrowed and every successful managed result is
 * newly owned by the caller.
 */
#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// DEFLATE Compression/Decompression (RFC 1951)
//=========================================================================

/// @brief Compress data using DEFLATE algorithm with default level.
/// @param data Bytes object containing data to compress.
/// @return New Bytes object with compressed data.
/// @note Default compression level is 6.
/// @note Traps if data is NULL.
void *rt_compress_deflate(void *data);

/// @brief Compress data using DEFLATE algorithm with specified level.
/// @param data Bytes object containing data to compress.
/// @param level Compression level 1-9 (1=fast, 9=best compression).
/// @return New Bytes object with compressed data.
/// @note Traps if data is NULL or level is out of range.
void *rt_compress_deflate_lvl(void *data, int64_t level);

/// @brief Decompress DEFLATE-compressed data.
/// @param data Bytes object containing compressed data.
/// @return New Bytes object with decompressed data.
/// @note Traps if data is NULL, corrupted, or truncated.
void *rt_compress_inflate(void *data);

/// @brief Decompress DEFLATE-compressed data with an explicit maximum output size.
/// @param data Bytes object containing compressed data.
/// @param max_output Maximum allowed decompressed byte count.
/// @return New Bytes object with decompressed data.
/// @note Traps if data is NULL, max_output is negative, corrupted, truncated, or exceeds
/// max_output.
void *rt_compress_inflate_limit(void *data, int64_t max_output);

/// @brief Internal helper: decompress raw DEFLATE data into malloc-owned bytes.
/// @details Worker-safe path for decoders that must not allocate runtime Bytes. The returned
///          buffer is owned by the caller and must be freed with free().
/// @param data Borrowed complete raw RFC 1951 stream.
/// @param len Number of accessible encoded bytes.
/// @param max_output Maximum allowed decoded byte count.
/// @param out_data Receives a `malloc`-owned buffer on success and `NULL` on
/// failure.
/// @param out_len Receives the decoded byte count on success and zero on
/// failure.
/// @return 1 on success, 0 on invalid input, corrupt data, or allocation failure.
int rt_compress_inflate_raw(
    const uint8_t *data, size_t len, size_t max_output, uint8_t **out_data, size_t *out_len);

/// @brief Internal helper: decode one complete RFC 1950 zlib stream into caller-owned storage.
/// @details Validates the CMF/FLG header (DEFLATE method, legal 32 KiB-or-smaller window,
///          FCHECK, and no preset dictionary), consumes exactly the wrapped RFC 1951 stream,
///          requires exactly @p output_size decoded bytes, and verifies the big-endian Adler-32
///          trailer before reporting success. The decoder writes directly to @p output and never
///          allocates or replaces that destination buffer.
/// @param data Complete zlib stream, including its two-byte header and four-byte trailer.
/// @param len Size of @p data in bytes.
/// @param output Caller-owned destination with room for exactly @p output_size bytes.
/// @param output_size Required decoded byte count and destination capacity.
/// @return 1 on complete, checksum-valid success; 0 for invalid arguments, malformed/truncated
///         input, a preset dictionary, output-size mismatch, trailing DEFLATE bytes, or checksum
///         mismatch. On failure, bytes already written to @p output are unspecified.
int rt_compress_inflate_zlib_into(const uint8_t *data,
                                  size_t len,
                                  uint8_t *output,
                                  size_t output_size);

//=========================================================================
// GZIP Compression/Decompression (RFC 1952)
//=========================================================================

/// @brief Compress data using GZIP format with default level.
/// @param data Bytes object containing data to compress.
/// @return New Bytes object with gzip-compressed data.
/// @note Default compression level is 6.
/// @note Traps if data is NULL.
void *rt_compress_gzip(void *data);

/// @brief Compress data using GZIP format with specified level.
/// @param data Bytes object containing data to compress.
/// @param level Compression level 1-9 (1=fast, 9=best compression).
/// @return New Bytes object with gzip-compressed data.
/// @note Traps if data is NULL or level is out of range.
void *rt_compress_gzip_lvl(void *data, int64_t level);

/// @brief Decompress GZIP-compressed data.
/// @param data Bytes object containing gzip data.
/// @return New Bytes object with decompressed data.
/// @note Traps if data is NULL, corrupted, truncated, or CRC mismatch.
void *rt_compress_gunzip(void *data);

/// @brief Decode a complete GZIP stream into bounded malloc-owned storage.
/// @details Validates every concatenated member and enforces both an aggregate
///          byte ceiling and, when nonzero, `encoded_size * ratio + slack`.
///          The smaller limit is applied while DEFLATE output grows. This
///          native integration helper catches decoder traps and never
///          allocates managed runtime objects.
/// @param data Borrowed complete RFC 1952 stream.
/// @param len Encoded byte count.
/// @param max_output Absolute aggregate decoded-byte ceiling.
/// @param max_expansion_ratio Maximum decoded/encoded multiplier; zero
///        disables the ratio constraint.
/// @param expansion_slack Additional decoded bytes permitted by the ratio
///        constraint, useful for small valid payloads.
/// @param out_data Receives malloc-owned decoded bytes on success and NULL on
///        failure; release with `free()`.
/// @param out_len Receives the aggregate decoded byte count on success and zero
///        on failure.
/// @return One on complete checksum-valid success, otherwise zero.
int rt_compress_gunzip_raw(const uint8_t *data,
                           size_t len,
                           size_t max_output,
                           size_t max_expansion_ratio,
                           size_t expansion_slack,
                           uint8_t **out_data,
                           size_t *out_len);

//=========================================================================
// String Convenience Methods
//=========================================================================

/// @brief Compress a string using DEFLATE.
/// @param text String to compress (as UTF-8 bytes).
/// @return New Bytes object with compressed data.
/// @note Traps if text is NULL.
void *rt_compress_deflate_str(rt_string text);

/// @brief Decompress DEFLATE data to a string.
/// @param data Bytes object containing compressed data.
/// @return String from decompressed UTF-8 bytes.
/// @note Traps if data is NULL or corrupted.
rt_string rt_compress_inflate_str(void *data);

/// @brief Compress a string using GZIP.
/// @param text String to compress (as UTF-8 bytes).
/// @return New Bytes object with gzip data.
/// @note Traps if text is NULL.
void *rt_compress_gzip_str(rt_string text);

/// @brief Decompress GZIP data to a string.
/// @param data Bytes object containing gzip data.
/// @return String from decompressed UTF-8 bytes.
/// @note Traps if data is NULL, corrupted, or CRC mismatch.
rt_string rt_compress_gunzip_str(void *data);

#ifdef __cplusplus
}
#endif
