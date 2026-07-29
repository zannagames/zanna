//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_io_internal.h
// Purpose: Shared includes, 64-bit file-seek macros, and size-overflow helpers
//   for the image codec translation units (rt_pixels_io.c BMP/GIF/dispatch,
//   rt_pixels_png.c, rt_pixels_jpeg.c).
//
// Key invariants:
//   - Every size calculation reports overflow before a codec allocates or
//     indexes storage.
//   - Exact stream helpers loop across short successful transfers and fail on
//     a zero-progress read or write.
//   - Image codecs use the shared UTF-8 stdio adapter's signed 64-bit
//     seek/tell operations rather than platform conditionals.
//
// Ownership/Lifetime:
//   - Helpers borrow streams and buffers for the duration of each call.
//   - Successful size helpers write caller-owned scalar outputs only.
//
// Links: src/runtime/graphics/2d/rt_pixels_io.c (BMP/GIF and dispatch),
//        src/runtime/graphics/2d/rt_pixels_png.c (PNG codec),
//        src/runtime/graphics/2d/rt_pixels_jpeg.c (JPEG codec),
//        src/runtime/io/rt_file_stdio.h (portable UTF-8 stdio)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Shares checked size and exact stdio operations among image codecs.

#pragma once

#include "rt_pixels.h"
#include "rt_pixels_internal.h"

#include "rt_bytes.h"
#include "rt_compress.h"
#include "rt_crc32.h"
#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Use the shared UTF-8 stdio wrapper's 64-bit seek/tell helpers so image codecs
// do not carry their own platform-specific branches.
/// Seek an image stream with the portable signed 64-bit stdio adapter.
/// @param fp Open stream.
/// @param off Signed byte offset.
/// @param whence Standard `SEEK_SET`, `SEEK_CUR`, or `SEEK_END` origin.
/// @return Zero on success and nonzero on failure.
#define px_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))

/// Query an image stream position with the portable signed 64-bit adapter.
/// @param fp Open stream.
/// @return Current byte offset, or a negative value on failure.
#define px_ftell(fp) rt_file_stdio_tell64((fp))

/// @brief Multiply two size_t values, returning 0 on overflow.
/// @details Writes the product to @p out only on success.  Used by the PNG
///   decoder to compute row-stride and total buffer sizes without wrapping.
/// @param a First factor.
/// @param b Second factor.
/// @param out Optional destination for the product.
/// @return Nonzero when multiplication is representable; zero on overflow.
static inline int px_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a)
        return 0;
    if (out)
        *out = a * b;
    return 1;
}

/// @brief Add two size_t values, returning 0 on overflow.
/// @details Writes the sum to @p out only on success.  Companion to px_mul_size
///   for computing PNG row strides that involve both multiplication and addition.
/// @param a First addend.
/// @param b Second addend.
/// @param out Optional destination for the sum.
/// @return Nonzero when addition is representable; zero on overflow.
static inline int px_add_size(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a)
        return 0;
    if (out)
        *out = a + b;
    return 1;
}

/// @brief Read exactly @p len bytes from @p f into @p data.
/// @details Wraps stdio in a short-read loop so image decoders do not rely on a
///          single `fread` returning the entire payload. A zero-length read is
///          considered successful and NULL @p data is accepted only in that case.
/// @param f Open binary stream.
/// @param data Destination byte buffer.
/// @param len Number of bytes to read.
/// @return 1 when every requested byte was read; otherwise 0.
static inline int px_read_exact(FILE *f, void *data, size_t len) {
    uint8_t *dst = (uint8_t *)data;
    size_t done = 0;
    if (!f || (!dst && len > 0))
        return 0;
    while (done < len) {
        size_t n = fread(dst + done, 1, len - done, f);
        if (n == 0)
            return 0;
        done += n;
    }
    return 1;
}

/// @brief Write exactly @p len bytes from @p data to @p f.
/// @details Mirrors @ref px_read_exact for save paths and treats short writes as
///          hard failures, which catches disk-full and interrupted stream cases
///          before the temp file is committed.
/// @param f Open binary stream.
/// @param data Source byte buffer.
/// @param len Number of bytes to write.
/// @return 1 when every requested byte was written; otherwise 0.
static inline int px_write_exact(FILE *f, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    size_t done = 0;
    if (!f || (!src && len > 0))
        return 0;
    while (done < len) {
        size_t n = fwrite(src + done, 1, len - done, f);
        if (n == 0)
            return 0;
        done += n;
    }
    return 1;
}
