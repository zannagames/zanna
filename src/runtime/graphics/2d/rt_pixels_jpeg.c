//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_pixels_jpeg.c
// Purpose: Dependency-free baseline sequential JPEG decoding with checked
//   marker parsing, canonical Huffman tables, fixed-point IDCT, MCU/component
//   reconstruction, EXIF orientation, and opaque RGBA32 output.
// Key invariants:
//   - JPEG decode failures return NULL and report asset diagnostics.
//   - Decoding remains dependency-free and bounded by JPEG_MAX_PIXELS.
//   - Only 8-bit baseline DCT with one grayscale or three YCbCr components in
//     one interleaved sequential scan is accepted.
//   - Marker lengths, entropy codes, restart cadence, sampling ratios,
//     coefficient ranges, and EOI padding are validated before use.
//   - Direct-buffer decode requires encoded dimensions to match exactly and
//     rejects EXIF transforms that would need replacement storage.
// Ownership/Lifetime:
//   - Loaded Pixels objects are GC-managed and owned by the caller.
//   - Raw-buffer decode returns malloc-owned storage to the caller.
//   - Direct-buffer decode borrows caller storage and never frees it.
//   - Temporary file, component, orientation, and decode buffers are released
//     on every success and failure path.
// Links: src/runtime/graphics/2d/rt_pixels.h (public Pixels API),
//        src/runtime/graphics/2d/rt_pixels_io_internal.h (checked I/O),
//        src/runtime/graphics/2d/rt_pixels_io.c (format dispatch),
//        src/runtime/graphics/3d/assets/rt_asset_error.h (load diagnostics)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded baseline JPEG decoding to canonical Pixels RGBA.
///
/// The decoder implements the required marker, Huffman, dequantization,
/// inverse-DCT, sampling, restart, color-conversion, and EXIF orientation
/// stages directly. It deliberately distinguishes structurally corrupt data
/// from well-formed JPEG variants outside the supported baseline subset.

#include "rt_pixels_io_internal.h"

#include "rt_asset_error.h"
#include "rt_file_stdio.h"
#include "rt_trap.h"

//=============================================================================
// JPEG Decoder (Baseline DCT, Huffman-coded)
//=============================================================================

/// JPEG Start Of Image marker.
#define JPEG_SOI 0xFFD8
/// JPEG End Of Image marker.
#define JPEG_EOI 0xFFD9
/// Baseline DCT Start Of Frame marker.
#define JPEG_SOF0 0xFFC0 // Baseline DCT
/// Define Huffman Table marker.
#define JPEG_DHT 0xFFC4
/// Define Quantization Table marker.
#define JPEG_DQT 0xFFDB
/// Start Of Scan marker.
#define JPEG_SOS 0xFFDA
/// Define Restart Interval marker.
#define JPEG_DRI 0xFFDD
/// First restart marker; the remaining seven are contiguous.
#define JPEG_RST0 0xFFD0
/// Maximum accepted decoded pixel count.
#define JPEG_MAX_PIXELS ((size_t)64u * 1024u * 1024u)

/// @brief Structured outcome used to distinguish unsupported JPEG variants.
typedef enum {
    JPEG_DECODE_STATUS_OK = 0,
    JPEG_DECODE_STATUS_CORRUPT = 1,
    JPEG_DECODE_STATUS_UNSUPPORTED = 2,
} jpeg_decode_status_t;

/// Maps JPEG zigzag coefficient positions to natural 8x8 row-major indices.
static const uint8_t jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// @brief Canonical JPEG Huffman table and its derived decoder boundaries.
/// @details `bits[1..16]` and `huffval` retain the transmitted table;
///          `mincode`, `maxcode`, and `valptr` provide Annex C canonical-code
///          lookup without allocating a tree.
typedef struct {
    uint8_t bits[17];     // bits[i] = number of codes of length i (1..16)
    uint8_t huffval[256]; // symbol values
    // Derived tables for fast decode:
    int maxcode[18]; // max code value + 1 for each length (-1 if none)
    int valptr[17];  // index into huffval for first code of each length
    int mincode[17]; // minimum code for each length
} jpeg_huff_t;

/// @brief Mutable state for one in-memory baseline JPEG decode.
/// @details The context borrows the encoded byte buffer. Marker metadata,
///          quantization and Huffman tables, scan mappings, entropy state, and
///          differential DC predictors live here for the duration of decode.
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;

    // Image properties
    uint16_t width, height;
    uint8_t num_components;
    uint8_t comp_id[4];
    uint8_t comp_h_samp[4]; // horizontal sampling factor
    uint8_t comp_v_samp[4]; // vertical sampling factor
    uint8_t comp_qt[4];     // quantization table index

    // Quantization tables (up to 4)
    int32_t qt[4][64];
    int qt_valid[4];

    // Huffman tables (DC: 0-1, AC: 2-3)
    jpeg_huff_t huff[4];
    int huff_valid[4];

    // SOS component mapping
    uint8_t scan_comp_count;
    uint8_t scan_comp_idx[4]; // index into comp_* arrays
    uint8_t scan_dc_table[4];
    uint8_t scan_ac_table[4];

    // Restart interval
    uint16_t restart_interval;

    // Bitstream reader state
    uint32_t bitbuf;
    int bits_left;

    // DC prediction per component
    int32_t dc_pred[4];
} jpeg_ctx_t;

// ---------------------------------------------------------------------------
// JPEG baseline (sequential) decoder helpers.
// Together these implement Huffman + dequantize + inverse-DCT
// over the entropy-coded segment. Only the most common JPEG
// variant — 8-bit baseline DCT, 1 or 3 components, no progressive
// or arithmetic coding — is supported. Layout follows ITU-T T.81.
// ---------------------------------------------------------------------------

/// @brief Read one byte from the marker stream.
/// @param ctx Decoder context whose absolute position advances on success.
/// @return The unsigned byte value, or -1 at end of input.
static int jpeg_read_u8(jpeg_ctx_t *ctx) {
    if (ctx->pos >= ctx->len)
        return -1;
    return ctx->data[ctx->pos++];
}

/// @brief Read one big-endian 16-bit value from the marker stream.
/// @param ctx Decoder context whose absolute position advances by two on
///        success.
/// @return The decoded unsigned value as an `int`, or -1 on a short read.
static int jpeg_read_u16(jpeg_ctx_t *ctx) {
    if (ctx->pos + 2 > ctx->len)
        return -1;
    int val = (ctx->data[ctx->pos] << 8) | ctx->data[ctx->pos + 1];
    ctx->pos += 2;
    return val;
}

/// @brief Check whether the active marker segment has at least @p needed unread bytes.
/// @details The parser stores the absolute end offset for each JPEG marker segment. This
///          helper performs subtraction-based bounds checks so malformed length fields cannot
///          wrap an addition or let DQT/DHT/SOF/SOS readers consume bytes from the next marker.
/// @param ctx JPEG decoder context with the current stream position.
/// @param seg_end Absolute stream offset one byte past the current marker segment.
/// @param needed Number of bytes the caller needs to read from the current position.
/// @return 1 if the bytes are fully inside the segment; otherwise 0.
static int jpeg_segment_has(const jpeg_ctx_t *ctx, size_t seg_end, size_t needed) {
    return ctx && ctx->pos <= seg_end && needed <= seg_end - ctx->pos;
}

/// @brief Return whether a marker is a Start Of Frame variant this decoder does not implement.
/// @details The runtime decoder intentionally supports baseline DCT (SOF0) only.
///          Other SOF markers include progressive, lossless, arithmetic-coded,
///          and differential JPEG variants. Treating these as unsupported lets
///          file loads report a precise diagnostic instead of labeling a valid
///          but unsupported JPEG as corrupt.
/// @param marker Full 0xFFxx marker value.
/// @return Non-zero when @p marker is an unsupported SOF marker.
static int jpeg_marker_is_unsupported_sof(uint16_t marker) {
    return (marker >= 0xFFC0u && marker <= 0xFFCFu && marker != JPEG_SOF0 && marker != JPEG_DHT);
}

/// @brief Validate bytes after an EOI marker.
/// @details Some encoders append fill bytes after EOI. The decoder accepts only
///          inert 0x00/0xFF padding so a real trailing payload is still rejected.
/// @param data Complete JPEG byte buffer.
/// @param pos Offset immediately after EOI.
/// @param len Total byte length.
/// @return Non-zero when the tail is empty or contains only accepted padding.
static int jpeg_tail_is_fill(const uint8_t *data, size_t pos, size_t len) {
    if (!data || pos > len)
        return 0;
    while (pos < len) {
        if (data[pos] != 0x00u && data[pos] != 0xFFu)
            return 0;
        ++pos;
    }
    return 1;
}

/// @brief Read a TIFF-endian uint16 from an EXIF payload.
/// @param p Pointer to two bytes inside the TIFF payload.
/// @param big Non-zero for big-endian TIFF, zero for little-endian TIFF.
/// @return Decoded unsigned 16-bit value.
static uint16_t jpeg_tiff_read_u16(const uint8_t *p, int big) {
    return big ? (uint16_t)((uint16_t)p[0] << 8 | p[1]) : (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/// @brief Read a TIFF-endian uint32 from an EXIF payload.
/// @param p Pointer to four bytes inside the TIFF payload.
/// @param big Non-zero for big-endian TIFF, zero for little-endian TIFF.
/// @return Decoded unsigned 32-bit value.
static uint32_t jpeg_tiff_read_u32(const uint8_t *p, int big) {
    return big ? ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])
               : ((uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
}

/// @brief Pull the next entropy byte while removing JPEG byte stuffing.
/// @details A literal `0xFF` is encoded as `FF 00`; the zero is consumed and
///          `0xFF` is returned. Any unstuffed marker prefix, including a
///          restart marker, returns -1. Restart markers are instead consumed
///          and validated by `jpeg_consume_restart_marker()` at exact MCU
///          interval boundaries.
/// @param ctx Decoder context positioned inside entropy-coded scan data.
/// @return The next logical entropy byte, or -1 on truncation or an unexpected
///         marker.
static int jpeg_next_byte(jpeg_ctx_t *ctx) {
    while (ctx->pos < ctx->len) {
        uint8_t b = ctx->data[ctx->pos++];
        if (b != 0xFF)
            return b;
        if (ctx->pos >= ctx->len)
            return -1;
        uint8_t next = ctx->data[ctx->pos];
        if (next == 0x00) {
            ctx->pos++; // skip stuffed zero
            return 0xFF;
        }
        return -1; // unexpected marker inside entropy data
    }
    return -1;
}

/// @brief Consume and validate the restart marker expected at an MCU boundary.
/// @details Restart markers are only legal between restart intervals, after
///          byte-aligning the entropy bitstream. Handling them here prevents
///          marker bytes from being silently skipped in the low-level byte reader.
/// @param ctx JPEG decoder context positioned at the marker prefix.
/// @param expected_rst Marker sequence number in [0, 7].
/// @return 1 when the expected marker was consumed, 0 otherwise.
static int jpeg_consume_restart_marker(jpeg_ctx_t *ctx, int expected_rst) {
    if (!ctx || expected_rst < 0 || expected_rst > 7)
        return 0;
    ctx->bits_left = 0;
    ctx->bitbuf = 0;
    if (ctx->pos >= ctx->len || ctx->data[ctx->pos++] != 0xFFu)
        return 0;
    while (ctx->pos < ctx->len && ctx->data[ctx->pos] == 0xFFu)
        ctx->pos++;
    if (ctx->pos >= ctx->len)
        return 0;
    return ctx->data[ctx->pos++] == (uint8_t)(0xD0u + (uint8_t)expected_rst);
}

/// @brief Extract `count` MSB-first bits from the JPEG bitstream.
///
/// Refills the 32-bit shift register one stuffed byte at a time
/// until at least `count` bits are available, then shifts them out
/// of the high end. Returns -1 on EOF/marker error.
/// @param ctx Decoder context with entropy bit-buffer state.
/// @param count Number of bits to consume, from zero through 24.
/// @return The requested unsigned bit field as an `int`, zero for a zero-bit
///         request, or -1 for an invalid count or entropy-stream failure.
static int jpeg_get_bits(jpeg_ctx_t *ctx, int count) {
    if (count < 0 || count > 24)
        return -1;
    if (count == 0)
        return 0;
    while (ctx->bits_left < count) {
        int b = jpeg_next_byte(ctx);
        if (b < 0)
            return -1;
        ctx->bitbuf = (ctx->bitbuf << 8) | (uint32_t)b;
        ctx->bits_left += 8;
    }
    ctx->bits_left -= count;
    return (int)((ctx->bitbuf >> ctx->bits_left) & ((1u << count) - 1));
}

/// @brief Precompute Huffman lookup tables (mincode/maxcode/valptr) per ITU-T T.81 Annex C.
///
/// JPEG transmits Huffman tables as a count-per-bit-length list
/// (`bits[1..16]`). This expands them into the cumulative
/// `mincode`/`maxcode` per length used by `jpeg_huff_decode` for
/// fast symbol lookup.
/// @param h Table containing transmitted code-length counts and symbols;
///        derived arrays are written in place.
/// @return 1 when the table is not over-subscribed and has at least one symbol.
static int jpeg_build_huff(jpeg_huff_t *h) {
    int code = 0;
    int si = 0;
    int space = 1;
    if (!h)
        return 0;
    for (int i = 1; i <= 16; i++) {
        space = (space << 1) - h->bits[i];
        if (space < 0)
            return 0;
        h->mincode[i] = code;
        h->valptr[i] = si;
        if (h->bits[i] == 0) {
            h->maxcode[i] = -1;
        } else {
            h->maxcode[i] = code + h->bits[i] - 1;
            si += h->bits[i];
        }
        code = (code + h->bits[i]) << 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    return si > 0;
}

/// @brief Decode one canonical Huffman symbol from the entropy bitstream.
/// @param ctx Decoder context with entropy bit-buffer state.
/// @param h Previously validated and derived Huffman table.
/// @return The decoded byte-valued symbol, or -1 for a truncated or invalid
///         code.
static int jpeg_huff_decode(jpeg_ctx_t *ctx, jpeg_huff_t *h) {
    int code = 0;
    for (int i = 1; i <= 16; i++) {
        int bit = jpeg_get_bits(ctx, 1);
        if (bit < 0)
            return -1;
        code = (code << 1) | bit;
        if (h->maxcode[i] >= 0 && code <= h->maxcode[i]) {
            int idx = h->valptr[i] + (code - h->mincode[i]);
            if (idx < 0 || idx >= 256)
                return -1;
            return h->huffval[idx];
        }
    }
    return -1; // invalid code
}

/// @brief Sign-extend a `bits`-wide unsigned magnitude per ITU-T T.81 §F.2.1.4.
///
/// JPEG stores coefficients as (size, magnitude) where negative
/// values are encoded as 1s-complement. Convert back to a signed
/// integer by subtracting `(1 << bits) - 1` whenever the high bit
/// indicates negativity.
/// @param val Unsigned magnitude bits.
/// @param bits Encoded coefficient category width.
/// @return The reconstructed signed coefficient delta; zero for a zero-width
///         category.
static int jpeg_extend(int val, int bits) {
    if (bits == 0)
        return 0;
    int vt = (int)(1u << (unsigned)(bits - 1));
    if (val < vt)
        val -= (int)((1u << (unsigned)bits) - 1u);
    return val;
}

/// @brief Decode one 8×8 block of DCT coefficients into zig-zagged form.
///
/// Decodes the DC coefficient (delta-coded against `*dc_pred`),
/// then runs the AC loop: each Huffman symbol carries a
/// (zero-run, magnitude-bits) pair. Special symbols are EOB
/// (00 — fill remainder with zeros) and ZRL (F0 — skip 16 zeros).
/// Multiplies each non-zero coefficient by the matching quant
/// table entry and stores it in natural row order using `jpeg_zigzag`.
/// @param ctx Decoder context with entropy and DC-predictor state.
/// @param block Receives 64 dequantized coefficients in natural row order.
/// @param dc_ht Valid DC Huffman table for the component.
/// @param ac_ht Valid AC Huffman table for the component.
/// @param dc_pred In/out differential DC predictor for the component.
/// @param qt Component quantization table in natural row order.
/// @return 0 on success, -1 on bitstream / Huffman decode failure.
static int jpeg_decode_block(jpeg_ctx_t *ctx,
                             int32_t block[64],
                             jpeg_huff_t *dc_ht,
                             jpeg_huff_t *ac_ht,
                             int32_t *dc_pred,
                             const int32_t qt[64]) {
    memset(block, 0, sizeof(int32_t) * 64);

    // DC coefficient
    int s = jpeg_huff_decode(ctx, dc_ht);
    if (s < 0)
        return -1;
    if (s > 11)
        return -1;
    int dc_val = 0;
    if (s > 0) {
        dc_val = jpeg_get_bits(ctx, s);
        if (dc_val < 0)
            return -1;
        dc_val = jpeg_extend(dc_val, s);
    }
    if ((dc_val > 0 && *dc_pred > INT32_MAX - dc_val) ||
        (dc_val < 0 && *dc_pred < INT32_MIN - dc_val))
        return -1;
    *dc_pred += dc_val;
    {
        int64_t coeff = (int64_t)(*dc_pred) * (int64_t)qt[0];
        if (coeff < INT32_MIN || coeff > INT32_MAX)
            return -1;
        block[0] = (int32_t)coeff;
    }

    // AC coefficients
    for (int k = 1; k < 64; k++) {
        int rs = jpeg_huff_decode(ctx, ac_ht);
        if (rs < 0)
            return -1;
        int rrrr = (rs >> 4) & 0x0F; // zero run length
        int ssss = rs & 0x0F;        // coefficient size
        if (ssss > 10)
            return -1;

        if (ssss == 0) {
            if (rrrr == 0)
                break; // EOB
            if (rrrr == 15) {
                k += 15; // ZRL: skip 16 zeros
                continue;
            }
            return -1;
        }

        k += rrrr;
        if (k >= 64)
            return -1;

        int ac_val = jpeg_get_bits(ctx, ssss);
        if (ac_val < 0)
            return -1;
        ac_val = jpeg_extend(ac_val, ssss);
        {
            int64_t coeff = (int64_t)ac_val * (int64_t)qt[jpeg_zigzag[k]];
            if (coeff < INT32_MIN || coeff > INT32_MAX)
                return -1;
            block[jpeg_zigzag[k]] = (int32_t)coeff;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Inverse DCT — fixed-point version of the AAN (Arai/Agui/Nakajima)
// algorithm. Uses 12-bit fractional precision so we can do a full
// 1D IDCT in 32-bit integers without overflow. The full 8×8 IDCT
// is `jpeg_idct_block` which calls `_row` × 8 then `_col` × 8.
// ---------------------------------------------------------------------------
/// Fixed-point unit at twelve fractional bits.
#define JPEG_FIX_1 4096
/// Convert a positive floating-point IDCT constant to 12-bit fixed point.
/// @param x Compile-time coefficient.
/// @return Rounded signed 32-bit fixed-point coefficient.
#define JPEG_FIX(x) ((int32_t)((x) * 4096.0 + 0.5))

/// @brief Descale a fixed-point JPEG intermediate with symmetric rounding.
/// @details Uses 64-bit arithmetic so malicious quantized coefficients cannot overflow during
///          IDCT multiplication/addition. Negative values are rounded by magnitude to avoid
///          relying on implementation-defined signed right-shift behavior for the rounding step.
/// @param value Fixed-point value to descale.
/// @param shift Number of fractional bits to remove.
/// @return Rounded integer value after descaling.
static int64_t jpeg_descale_i64(int64_t value, int shift) {
    int64_t bias = INT64_C(1) << (shift - 1);
    if (value == INT64_MIN)
        return -((INT64_MAX >> shift) + 1);
    if (value < 0)
        return -(((-value) + bias) >> shift);
    return (value + bias) >> shift;
}

/// @brief Saturate a 64-bit IDCT workspace value to int32_t.
/// @details The final sample path clamps to 8-bit output later, but the intermediate workspace is
///          stored in int32_t arrays. Saturating avoids undefined signed overflow while preserving
///          deterministic decode failure containment for hostile inputs.
/// @param value IDCT intermediate value.
/// @return @p value clamped to the int32_t range.
static int32_t jpeg_saturate_i32(int64_t value) {
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}

/// @brief Apply the one-dimensional AAN IDCT to an eight-coefficient row.
/// @details All products use 64-bit intermediates and each published
///          workspace element saturates to signed 32-bit range.
/// @param row In/out pointer to eight consecutive workspace coefficients.
static void jpeg_idct_row(int32_t *row) {
    int64_t x0 = row[0], x1 = row[1], x2 = row[2], x3 = row[3];
    int64_t x4 = row[4], x5 = row[5], x6 = row[6], x7 = row[7];

    // Even part
    int64_t s0 = x0 + x4;
    int64_t s1 = x0 - x4;
    int64_t s2 =
        jpeg_descale_i64(x2 * JPEG_FIX(0.541196100) + x6 * JPEG_FIX(0.541196100 - 1.847759065), 12);
    int64_t s3 =
        jpeg_descale_i64(x2 * JPEG_FIX(0.541196100 + 0.765366865) + x6 * JPEG_FIX(0.541196100), 12);

    int64_t e0 = s0 + s3;
    int64_t e1 = s1 + s2;
    int64_t e2 = s1 - s2;
    int64_t e3 = s0 - s3;

    // Odd part
    int64_t t0 = x1 + x7;
    int64_t t1 = x5 + x3;
    int64_t t2 = x1 + x3;
    int64_t t3 = x5 + x7;
    int64_t z5 = jpeg_descale_i64((t2 - t3) * JPEG_FIX(1.175875602), 12);

    t0 = jpeg_descale_i64(t0 * JPEG_FIX(-0.899976223), 12);
    t1 = jpeg_descale_i64(t1 * JPEG_FIX(-2.562915447), 12);
    t2 = jpeg_descale_i64(t2 * JPEG_FIX(-1.961570560), 12) + z5;
    t3 = jpeg_descale_i64(t3 * JPEG_FIX(-0.390180644), 12) + z5;

    int64_t o0 = jpeg_descale_i64(x7 * JPEG_FIX(0.298631336), 12) + t0 + t2;
    int64_t o1 = jpeg_descale_i64(x5 * JPEG_FIX(2.053119869), 12) + t1 + t3;
    int64_t o2 = jpeg_descale_i64(x3 * JPEG_FIX(3.072711026), 12) + t1 + t2;
    int64_t o3 = jpeg_descale_i64(x1 * JPEG_FIX(1.501321110), 12) + t0 + t3;

    row[0] = jpeg_saturate_i32(e0 + o3);
    row[1] = jpeg_saturate_i32(e1 + o2);
    row[2] = jpeg_saturate_i32(e2 + o1);
    row[3] = jpeg_saturate_i32(e3 + o0);
    row[4] = jpeg_saturate_i32(e3 - o0);
    row[5] = jpeg_saturate_i32(e2 - o1);
    row[6] = jpeg_saturate_i32(e1 - o2);
    row[7] = jpeg_saturate_i32(e0 - o3);
}

/// @brief Apply the one-dimensional AAN IDCT to one workspace column.
/// @details The column pass performs the final five-bit descale before
///          saturating each stored result.
/// @param workspace In/out 8x8 row-major IDCT workspace.
/// @param col Zero-based column index in `[0, 7]`.
static void jpeg_idct_col(int32_t *workspace, int col) {
    int64_t x0 = workspace[col + 0 * 8], x1 = workspace[col + 1 * 8];
    int64_t x2 = workspace[col + 2 * 8], x3 = workspace[col + 3 * 8];
    int64_t x4 = workspace[col + 4 * 8], x5 = workspace[col + 5 * 8];
    int64_t x6 = workspace[col + 6 * 8], x7 = workspace[col + 7 * 8];

    int64_t s0 = x0 + x4;
    int64_t s1 = x0 - x4;
    int64_t s2 =
        jpeg_descale_i64(x2 * JPEG_FIX(0.541196100) + x6 * JPEG_FIX(0.541196100 - 1.847759065), 12);
    int64_t s3 =
        jpeg_descale_i64(x2 * JPEG_FIX(0.541196100 + 0.765366865) + x6 * JPEG_FIX(0.541196100), 12);

    int64_t e0 = s0 + s3;
    int64_t e1 = s1 + s2;
    int64_t e2 = s1 - s2;
    int64_t e3 = s0 - s3;

    int64_t t0 = x1 + x7;
    int64_t t1 = x5 + x3;
    int64_t t2 = x1 + x3;
    int64_t t3 = x5 + x7;
    int64_t z5 = jpeg_descale_i64((t2 - t3) * JPEG_FIX(1.175875602), 12);

    t0 = jpeg_descale_i64(t0 * JPEG_FIX(-0.899976223), 12);
    t1 = jpeg_descale_i64(t1 * JPEG_FIX(-2.562915447), 12);
    t2 = jpeg_descale_i64(t2 * JPEG_FIX(-1.961570560), 12) + z5;
    t3 = jpeg_descale_i64(t3 * JPEG_FIX(-0.390180644), 12) + z5;

    int64_t o0 = jpeg_descale_i64(x7 * JPEG_FIX(0.298631336), 12) + t0 + t2;
    int64_t o1 = jpeg_descale_i64(x5 * JPEG_FIX(2.053119869), 12) + t1 + t3;
    int64_t o2 = jpeg_descale_i64(x3 * JPEG_FIX(3.072711026), 12) + t1 + t2;
    int64_t o3 = jpeg_descale_i64(x1 * JPEG_FIX(1.501321110), 12) + t0 + t3;

    // Descale with rounding and shift for final output
    workspace[col + 0 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e0 + o3, 5));
    workspace[col + 1 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e1 + o2, 5));
    workspace[col + 2 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e2 + o1, 5));
    workspace[col + 3 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e3 + o0, 5));
    workspace[col + 4 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e3 - o0, 5));
    workspace[col + 5 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e2 - o1, 5));
    workspace[col + 6 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e1 - o2, 5));
    workspace[col + 7 * 8] = jpeg_saturate_i32(jpeg_descale_i64(e0 - o3, 5));
}

/// @brief Full 2D IDCT on an 8×8 coefficient block, producing 8-bit samples.
///
/// Applies `jpeg_idct_row` × 8 then `jpeg_idct_col` × 8 with the
/// AAN-required pre-/post-scale. After descaling and biasing by
/// 128 (level shift), the samples are clamped to [0, 255].
/// @param block Array of 64 dequantized coefficients in natural row order.
/// @param out Receives 64 level-shifted, clamped samples in row-major order.
static void jpeg_idct_block(const int32_t block[64], uint8_t out[64]) {
    int32_t workspace[64];
    for (int i = 0; i < 64; i++)
        workspace[i] = (int32_t)block[i];

    // IDCT on rows
    for (int i = 0; i < 8; i++)
        jpeg_idct_row(workspace + i * 8);

    // IDCT on columns
    for (int i = 0; i < 8; i++)
        jpeg_idct_col(workspace, i);

    // Level shift (+128) and clamp to [0, 255]
    for (int i = 0; i < 64; i++) {
        int val = (int)workspace[i] + 128;
        if (val < 0)
            val = 0;
        if (val > 255)
            val = 255;
        out[i] = (uint8_t)val;
    }
}

// Clamp int to [0, 255]
/// @brief Saturate an integer to the [0, 255] range as a byte.
/// @param val Integer channel sample.
/// @return @p val clamped to the unsigned eight-bit range.
static uint8_t jpeg_clamp(int val) {
    if (val < 0)
        return 0;
    if (val > 255)
        return 255;
    return (uint8_t)val;
}

/// @brief Validate RGBA dimensions and compute their bounded pixel count.
/// @details Requires positive dimensions representable in `size_t`, checks
///          multiplication overflow, and applies `JPEG_MAX_PIXELS`.
/// @param width Candidate decoded width.
/// @param height Candidate decoded height.
/// @param out_count Receives the pixel count on success and zero on failure.
/// @return Nonzero when the dimensions and count are accepted; zero otherwise.
static int jpeg_rgba_pixel_count_checked(int64_t width, int64_t height, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (width <= 0 || height <= 0 || !out_count)
        return 0;
    if ((uint64_t)width > SIZE_MAX || (uint64_t)height > SIZE_MAX)
        return 0;
    if ((size_t)width > SIZE_MAX / (size_t)height)
        return 0;
    *out_count = (size_t)width * (size_t)height;
    if (*out_count > JPEG_MAX_PIXELS) {
        *out_count = 0;
        return 0;
    }
    return 1;
}

/// @brief Allocate zero-filled raw RGBA storage for bounded dimensions.
/// @param width Positive decoded width.
/// @param height Positive decoded height.
/// @return A malloc-owned `width * height` array, or null for invalid
///         dimensions, byte-size overflow, or allocation failure.
static uint32_t *jpeg_rgba_alloc(int64_t width, int64_t height) {
    size_t count;
    if (!jpeg_rgba_pixel_count_checked(width, height, &count))
        return NULL;
    if (count > SIZE_MAX / sizeof(uint32_t))
        return NULL;
    return (uint32_t *)calloc(count, sizeof(uint32_t));
}

/// @brief Apply an EXIF orientation transform to malloc-owned RGBA storage.
/// @details Orientations 2 through 8 allocate a correctly dimensioned
///          replacement, remap every source pixel, free the original, and
///          publish the new pointer and dimensions atomically after success.
///          Missing storage, invalid output pointers, orientation 1, and
///          out-of-range orientation values are treated as unchanged success.
/// @param pixels In/out pointer to malloc-owned raw RGBA storage.
/// @param width In/out image width.
/// @param height In/out image height.
/// @param orientation EXIF orientation value.
/// @return Nonzero when no transform is needed or replacement succeeds; zero
///         only when required replacement allocation fails.
static int jpeg_rgba_apply_orientation(uint32_t **pixels,
                                       int64_t *width,
                                       int64_t *height,
                                       int orientation) {
    uint32_t *src;
    uint32_t *dst;
    int64_t src_w;
    int64_t src_h;
    int64_t dst_w;
    int64_t dst_h;
    if (!pixels || !*pixels || !width || !height || orientation <= 1 || orientation > 8)
        return 1;
    src = *pixels;
    src_w = *width;
    src_h = *height;
    dst_w = (orientation >= 5) ? src_h : src_w;
    dst_h = (orientation >= 5) ? src_w : src_h;
    dst = jpeg_rgba_alloc(dst_w, dst_h);
    if (!dst)
        return 0;
    for (int64_t y = 0; y < src_h; ++y) {
        for (int64_t x = 0; x < src_w; ++x) {
            int64_t dx = x;
            int64_t dy = y;
            switch (orientation) {
                case 2:
                    dx = src_w - 1 - x;
                    dy = y;
                    break;
                case 3:
                    dx = src_w - 1 - x;
                    dy = src_h - 1 - y;
                    break;
                case 4:
                    dx = x;
                    dy = src_h - 1 - y;
                    break;
                case 5:
                    dx = y;
                    dy = x;
                    break;
                case 6:
                    dx = src_h - 1 - y;
                    dy = x;
                    break;
                case 7:
                    dx = src_h - 1 - y;
                    dy = src_w - 1 - x;
                    break;
                case 8:
                    dx = y;
                    dy = src_w - 1 - x;
                    break;
                default:
                    break;
            }
            dst[dy * dst_w + dx] = src[y * src_w + x];
        }
    }
    free(src);
    *pixels = dst;
    *width = dst_w;
    *height = dst_h;
    return 1;
}

/// @brief Decode a JPEG image from a memory buffer to malloc-owned raw RGBA32 pixels.
/// @details This internal variant preserves the public raw-buffer contract while
///          returning a structured status for callers that need diagnostics. When
///          @p direct_pixels is non-NULL, decoded pixels are written into caller-owned
///          storage only after the encoded dimensions have been validated against
///          @p direct_width and @p direct_height. Direct storage is not
///          transactional: later EOI or trailing-data failure can leave decoded
///          bytes in the caller's buffer even though the function returns zero.
///          On ordinary output, success transfers malloc ownership through
///          @p out_pixels; failure clears all three output values.
/// @param data Pointer to JPEG data.
/// @param len Length of @p data.
/// @param out_pixels Receives malloc-owned RGBA32 pixels on success.
/// @param out_width Receives decoded width on success.
/// @param out_height Receives decoded height on success.
/// @param out_status Receives success, corrupt, or unsupported status when non-NULL.
/// @param direct_pixels Optional caller-owned destination buffer for direct decode.
/// @param direct_width Expected width when @p direct_pixels is non-NULL.
/// @param direct_height Expected height when @p direct_pixels is non-NULL.
/// @return 1 on success; 0 on corrupt data, unsupported JPEG variants, or allocation failure.
static int rt_jpeg_decode_buffer_rgba32_ex(const uint8_t *data,
                                           size_t len,
                                           uint32_t **out_pixels,
                                           int64_t *out_width,
                                           int64_t *out_height,
                                           jpeg_decode_status_t *out_status,
                                           uint32_t *direct_pixels,
                                           int64_t direct_width,
                                           int64_t direct_height) {
    uint32_t *pixels = NULL;
    int pixels_owned = 0;
    jpeg_decode_status_t decode_status = JPEG_DECODE_STATUS_CORRUPT;
    if (out_status)
        *out_status = JPEG_DECODE_STATUS_CORRUPT;
    if (!data || len < 2 || data[0] != 0xFF || data[1] != 0xD8)
        return 0;
    if (!out_pixels || !out_width || !out_height)
        return 0;
    if (direct_pixels && (direct_width <= 0 || direct_height <= 0))
        return 0;
    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;

    /* jpeg_ctx_t.data is uint8_t* but we only read from it. Use memcpy
     * to reinterpret the pointer without triggering -Wcast-qual. */
    uint8_t *file_data;
    memcpy(&file_data, &data, sizeof(file_data));

    jpeg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data = file_data;
    ctx.len = len;
    ctx.pos = 2; // past SOI

    uint8_t **comp_data = NULL;
    int exif_orientation = 1; // default: no rotation
    int64_t decoded_width = 0;
    int64_t decoded_height = 0;
    int saw_scan = 0;
    int saw_eoi = 0;

    // Parse markers
    while (ctx.pos + 1 < ctx.len) {
        int b1 = jpeg_read_u8(&ctx);
        if (b1 != 0xFF)
            break;

        // Skip padding 0xFF bytes
        int marker;
        do {
            marker = jpeg_read_u8(&ctx);
        } while (marker == 0xFF && ctx.pos < ctx.len);

        if (marker < 0)
            break;

        uint16_t mk = (uint16_t)(0xFF00 | marker);

        if (mk == JPEG_EOI) {
            saw_eoi = 1;
            if (!jpeg_tail_is_fill(ctx.data, ctx.pos, ctx.len))
                goto jpeg_fail;
            ctx.pos = ctx.len;
            break;
        }

        // Markers without length
        if (mk >= JPEG_RST0 && mk <= JPEG_RST0 + 7)
            continue;
        if (jpeg_marker_is_unsupported_sof(mk)) {
            decode_status = JPEG_DECODE_STATUS_UNSUPPORTED;
            goto jpeg_fail;
        }

        // Read marker length
        int seg_len = jpeg_read_u16(&ctx);
        if (seg_len < 2)
            break;
        size_t data_len = (size_t)(seg_len - 2);
        size_t seg_start = ctx.pos;
        if (data_len > ctx.len - seg_start)
            break;
        size_t seg_end = seg_start + data_len;

        switch (mk) {
            case JPEG_DQT: {
                // Parse quantization table(s)
                while (ctx.pos < seg_end) {
                    if (!jpeg_segment_has(&ctx, seg_end, 1))
                        goto jpeg_fail;
                    int info = jpeg_read_u8(&ctx);
                    if (info < 0)
                        goto jpeg_fail;
                    int precision = (info >> 4) & 0x0F; // 0=8bit, 1=16bit
                    int table_id = info & 0x0F;
                    if (precision != 0 || table_id > 3)
                        goto jpeg_fail;
                    if (!jpeg_segment_has(&ctx, seg_end, 64u))
                        goto jpeg_fail;
                    for (int i = 0; i < 64; i++) {
                        int v = jpeg_read_u8(&ctx);
                        if (v < 0)
                            goto jpeg_fail;
                        ctx.qt[table_id][jpeg_zigzag[i]] = v;
                    }
                    ctx.qt_valid[table_id] = 1;
                }
                if (ctx.pos != seg_end)
                    goto jpeg_fail;
                break;
            }
            case JPEG_DHT: {
                // Parse Huffman table(s)
                while (ctx.pos < seg_end) {
                    if (!jpeg_segment_has(&ctx, seg_end, 17))
                        goto jpeg_fail;
                    int info = jpeg_read_u8(&ctx);
                    if (info < 0)
                        goto jpeg_fail;
                    int table_class = (info >> 4) & 0x0F; // 0=DC, 1=AC
                    int table_id = info & 0x0F;
                    if (table_class > 1 || table_id > 1)
                        goto jpeg_fail;
                    int idx = table_class * 2 + table_id; // DC0, DC1, AC0, AC1
                    jpeg_huff_t *ht = &ctx.huff[idx];
                    int total = 0;
                    ht->bits[0] = 0;
                    for (int i = 1; i <= 16; i++) {
                        int n = jpeg_read_u8(&ctx);
                        if (n < 0)
                            goto jpeg_fail;
                        ht->bits[i] = (uint8_t)n;
                        total += n;
                    }
                    if (total > 256)
                        goto jpeg_fail;
                    if (!jpeg_segment_has(&ctx, seg_end, (size_t)total))
                        goto jpeg_fail;
                    for (int i = 0; i < total; i++) {
                        int v = jpeg_read_u8(&ctx);
                        if (v < 0)
                            goto jpeg_fail;
                        if (table_class == 0 && v > 11)
                            goto jpeg_fail;
                        if (table_class == 1 &&
                            (((v & 0x0F) > 10) || ((v & 0x0F) == 0 && v != 0x00 && v != 0xF0)))
                            goto jpeg_fail;
                        ht->huffval[i] = (uint8_t)v;
                    }
                    if (!jpeg_build_huff(ht))
                        goto jpeg_fail;
                    ctx.huff_valid[idx] = 1;
                }
                if (ctx.pos != seg_end)
                    goto jpeg_fail;
                break;
            }
            case JPEG_SOF0: {
                // Baseline DCT
                if (data_len < 6)
                    goto jpeg_fail;
                int prec = jpeg_read_u8(&ctx);
                if (prec != 8)
                    goto jpeg_fail; // Only 8-bit precision
                int h = jpeg_read_u16(&ctx);
                int w = jpeg_read_u16(&ctx);
                if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
                    goto jpeg_fail;
                if ((size_t)w > SIZE_MAX / (size_t)h || (size_t)w * (size_t)h > JPEG_MAX_PIXELS)
                    goto jpeg_fail;
                ctx.width = (uint16_t)w;
                ctx.height = (uint16_t)h;
                int nf = jpeg_read_u8(&ctx);
                if (nf != 1 && nf != 3)
                    goto jpeg_fail;
                if ((size_t)nf > (seg_end - ctx.pos) / 3)
                    goto jpeg_fail;
                ctx.num_components = (uint8_t)nf;
                for (int i = 0; i < nf; i++) {
                    int comp_id = jpeg_read_u8(&ctx);
                    int samp = jpeg_read_u8(&ctx);
                    int qt = jpeg_read_u8(&ctx);
                    if (comp_id < 0 || samp < 0 || qt < 0)
                        goto jpeg_fail;
                    ctx.comp_h_samp[i] = (uint8_t)((samp >> 4) & 0x0F);
                    ctx.comp_v_samp[i] = (uint8_t)(samp & 0x0F);
                    if (ctx.comp_h_samp[i] < 1 || ctx.comp_h_samp[i] > 4 ||
                        ctx.comp_v_samp[i] < 1 || ctx.comp_v_samp[i] > 4 || qt > 3)
                        goto jpeg_fail;
                    ctx.comp_id[i] = (uint8_t)comp_id;
                    ctx.comp_qt[i] = (uint8_t)qt;
                }
                if (ctx.pos != seg_end)
                    goto jpeg_fail;
                break;
            }
            case JPEG_DRI: {
                if (data_len != 2)
                    goto jpeg_fail;
                int ri = jpeg_read_u16(&ctx);
                if (ri >= 0)
                    ctx.restart_interval = (uint16_t)ri;
                break;
            }
            case JPEG_SOS: {
                // Start of Scan — decode the entropy-coded data
                if (saw_scan || pixels || comp_data) {
                    decode_status = JPEG_DECODE_STATUS_UNSUPPORTED;
                    goto jpeg_fail;
                }
                saw_scan = 1;
                if (!jpeg_segment_has(&ctx, seg_end, 1))
                    goto jpeg_fail;
                int ns = jpeg_read_u8(&ctx);
                if (ns < 1 || ns > 4)
                    goto jpeg_fail;
                if (ctx.num_components != 1 && ctx.num_components != 3)
                    goto jpeg_fail;
                if (ns != ctx.num_components) {
                    decode_status = JPEG_DECODE_STATUS_UNSUPPORTED;
                    goto jpeg_fail;
                }
                if (!jpeg_segment_has(&ctx, seg_end, 3) || (size_t)ns > (seg_end - ctx.pos - 3) / 2)
                    goto jpeg_fail;
                ctx.scan_comp_count = (uint8_t)ns;
                for (int i = 0; i < ns; i++) {
                    int cs = jpeg_read_u8(&ctx);
                    int td_ta = jpeg_read_u8(&ctx);
                    if (cs < 0 || td_ta < 0)
                        goto jpeg_fail;
                    // Find component index
                    int ci = -1;
                    for (int j = 0; j < ctx.num_components; j++) {
                        if (ctx.comp_id[j] == (uint8_t)cs) {
                            ci = j;
                            break;
                        }
                    }
                    if (ci < 0)
                        goto jpeg_fail;
                    for (int j = 0; j < i; j++) {
                        if (ctx.scan_comp_idx[j] == (uint8_t)ci)
                            goto jpeg_fail;
                    }
                    ctx.scan_comp_idx[i] = (uint8_t)ci;
                    int dc_table = (td_ta >> 4) & 0x0F;
                    int ac_table = td_ta & 0x0F;
                    if (dc_table > 1 || ac_table > 1)
                        goto jpeg_fail;
                    ctx.scan_dc_table[i] = (uint8_t)dc_table;
                    ctx.scan_ac_table[i] = (uint8_t)ac_table;
                }
                // Skip Ss, Se, Ah/Al (spectral selection — always 0,63,0 for baseline)
                int ss = jpeg_read_u8(&ctx);
                int se = jpeg_read_u8(&ctx);
                int ah_al = jpeg_read_u8(&ctx);
                if (ss < 0 || se < 0 || ah_al < 0)
                    goto jpeg_fail;
                if (ss != 0 || se != 63 || ah_al != 0) {
                    decode_status = JPEG_DECODE_STATUS_UNSUPPORTED;
                    goto jpeg_fail;
                }
                if (ctx.pos != seg_end)
                    goto jpeg_fail;

                // Now at the start of entropy-coded data
                ctx.bitbuf = 0;
                ctx.bits_left = 0;
                memset(ctx.dc_pred, 0, sizeof(ctx.dc_pred));

                // Determine MCU layout
                int max_h = 1, max_v = 1;
                for (int i = 0; i < ctx.num_components; i++) {
                    if (ctx.comp_h_samp[i] > max_h)
                        max_h = ctx.comp_h_samp[i];
                    if (ctx.comp_v_samp[i] > max_v)
                        max_v = ctx.comp_v_samp[i];
                }

                int mcu_w = max_h * 8;
                int mcu_h = max_v * 8;
                int mcus_x = (ctx.width + mcu_w - 1) / mcu_w;
                int mcus_y = (ctx.height + mcu_h - 1) / mcu_h;

                // Allocate component buffers (full-resolution for each component's
                // sampled size)
                comp_data = (uint8_t **)calloc((size_t)ctx.num_components, sizeof(uint8_t *));
                if (!comp_data)
                    goto jpeg_fail;
                for (int i = 0; i < ctx.num_components; i++) {
                    int cw = mcus_x * ctx.comp_h_samp[i] * 8;
                    int ch = mcus_y * ctx.comp_v_samp[i] * 8;
                    if (cw <= 0 || ch <= 0 || (size_t)cw > SIZE_MAX / (size_t)ch)
                        goto jpeg_fail;
                    comp_data[i] = (uint8_t *)calloc((size_t)cw * (size_t)ch, 1);
                    if (!comp_data[i])
                        goto jpeg_fail;
                }

                // Decode MCUs
                int32_t block[64];
                uint8_t idct_out[64];
                int restart_count = 0;
                int expected_rst = 0;

                for (int mcu_y = 0; mcu_y < mcus_y; mcu_y++) {
                    for (int mcu_x = 0; mcu_x < mcus_x; mcu_x++) {
                        // Handle restart markers
                        if (ctx.restart_interval > 0 && restart_count > 0 &&
                            (restart_count % ctx.restart_interval) == 0) {
                            if (!jpeg_consume_restart_marker(&ctx, expected_rst))
                                goto jpeg_fail;
                            expected_rst = (expected_rst + 1) & 7;
                            memset(ctx.dc_pred, 0, sizeof(ctx.dc_pred));
                        }
                        restart_count++;

                        // For each component in the scan
                        for (int si = 0; si < ctx.scan_comp_count; si++) {
                            int ci = ctx.scan_comp_idx[si];
                            int h_samp = ctx.comp_h_samp[ci];
                            int v_samp = ctx.comp_v_samp[ci];
                            int qt_idx = ctx.comp_qt[ci];
                            int dc_idx = ctx.scan_dc_table[si];
                            int ac_idx = ctx.scan_ac_table[si] + 2;

                            if (qt_idx >= 4 || dc_idx >= 2 || ac_idx >= 4 ||
                                !ctx.qt_valid[qt_idx] || !ctx.huff_valid[dc_idx] ||
                                !ctx.huff_valid[ac_idx])
                                goto jpeg_fail;

                            // Decode each block in this component's MCU contribution
                            for (int bv = 0; bv < v_samp; bv++) {
                                for (int bh = 0; bh < h_samp; bh++) {
                                    if (jpeg_decode_block(&ctx,
                                                          block,
                                                          &ctx.huff[dc_idx],
                                                          &ctx.huff[ac_idx],
                                                          &ctx.dc_pred[ci],
                                                          ctx.qt[qt_idx]) != 0)
                                        goto jpeg_fail;

                                    jpeg_idct_block(block, idct_out);

                                    // Place decoded 8x8 block into component buffer
                                    int comp_stride = mcus_x * h_samp * 8;
                                    int bx = (mcu_x * h_samp + bh) * 8;
                                    int by = (mcu_y * v_samp + bv) * 8;
                                    for (int yy = 0; yy < 8; yy++) {
                                        for (int xx = 0; xx < 8; xx++) {
                                            comp_data[ci][(by + yy) * comp_stride + (bx + xx)] =
                                                idct_out[yy * 8 + xx];
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Convert component buffers to RGBA pixels
                decoded_width = (int64_t)ctx.width;
                decoded_height = (int64_t)ctx.height;
                if (direct_pixels) {
                    if (direct_width != decoded_width || direct_height != decoded_height)
                        goto jpeg_fail;
                    if (exif_orientation > 1 && exif_orientation <= 8) {
                        decode_status = JPEG_DECODE_STATUS_UNSUPPORTED;
                        goto jpeg_fail;
                    }
                    pixels = direct_pixels;
                    pixels_owned = 0;
                } else {
                    pixels = jpeg_rgba_alloc(decoded_width, decoded_height);
                    if (!pixels)
                        goto jpeg_fail;
                    pixels_owned = 1;
                }

                if (ctx.num_components == 1) {
                    // Grayscale
                    int comp_stride = mcus_x * ctx.comp_h_samp[0] * 8;
                    for (int y = 0; y < ctx.height; y++) {
                        for (int x = 0; x < ctx.width; x++) {
                            uint8_t gray = comp_data[0][y * comp_stride + x];
                            pixels[y * ctx.width + x] = ((uint32_t)gray << 24) |
                                                        ((uint32_t)gray << 16) |
                                                        ((uint32_t)gray << 8) | 0xFF;
                        }
                    }
                } else if (ctx.num_components == 3) {
                    // YCbCr -> RGB with chroma upsampling
                    int y_stride = mcus_x * ctx.comp_h_samp[0] * 8;
                    int cb_stride = mcus_x * ctx.comp_h_samp[1] * 8;
                    int cr_stride = mcus_x * ctx.comp_h_samp[2] * 8;
                    if (ctx.comp_h_samp[1] == 0 || ctx.comp_v_samp[1] == 0 ||
                        ctx.comp_h_samp[2] == 0 || ctx.comp_v_samp[2] == 0 ||
                        max_h % ctx.comp_h_samp[1] != 0 || max_v % ctx.comp_v_samp[1] != 0 ||
                        max_h % ctx.comp_h_samp[2] != 0 || max_v % ctx.comp_v_samp[2] != 0)
                        goto jpeg_fail;
                    int h_ratio = max_h / ctx.comp_h_samp[1]; // chroma upsample factor
                    int v_ratio = max_v / ctx.comp_v_samp[1];
                    int cr_h_ratio = max_h / ctx.comp_h_samp[2];
                    int cr_v_ratio = max_v / ctx.comp_v_samp[2];

                    for (int y = 0; y < ctx.height; y++) {
                        for (int x = 0; x < ctx.width; x++) {
                            int yy_val = comp_data[0][y * y_stride + x];
                            int cb_x = x / h_ratio;
                            int cb_y = y / v_ratio;
                            int cr_x = x / cr_h_ratio;
                            int cr_y = y / cr_v_ratio;
                            int cb_val = comp_data[1][cb_y * cb_stride + cb_x] - 128;
                            int cr_val = comp_data[2][cr_y * cr_stride + cr_x] - 128;

                            // YCbCr -> RGB (ITU-R BT.601)
                            /* +128 before the >>8 rounds to nearest instead of
                             * truncating (removes the consistent ≤1-LSB downward bias). */
                            int r = yy_val + ((cr_val * 359 + 128) >> 8);
                            int g = yy_val - ((cb_val * 88 + cr_val * 183 + 128) >> 8);
                            int b = yy_val + ((cb_val * 454 + 128) >> 8);

                            pixels[y * ctx.width + x] = ((uint32_t)jpeg_clamp(r) << 24) |
                                                        ((uint32_t)jpeg_clamp(g) << 16) |
                                                        ((uint32_t)jpeg_clamp(b) << 8) | 0xFF;
                        }
                    }
                } else {
                    goto jpeg_fail;
                }

                // Finished SOS — skip to after entropy data (already consumed via bitstream)
                break;
            }
            default:
                // APP1 (0xFFE1): EXIF data — extract orientation tag
                if (mk == 0xFFE1 && data_len >= 14) {
                    const uint8_t *exif = ctx.data + seg_start;
                    if (memcmp(exif, "Exif\0\0", 6) == 0) {
                        const uint8_t *tiff = exif + 6;
                        size_t tiff_len = data_len - 6;
                        int big;
                        uint16_t tiff_magic;
                        if (tiff[0] == 'M' && tiff[1] == 'M')
                            big = 1;
                        else if (tiff[0] == 'I' && tiff[1] == 'I')
                            big = 0;
                        else
                            goto exif_done;
                        tiff_magic = jpeg_tiff_read_u16(tiff + 2, big);
                        if (tiff_magic != 42u)
                            goto exif_done;
                        uint32_t ifd_off = jpeg_tiff_read_u32(tiff + 4, big);
                        if (tiff_len >= 2u && ifd_off <= tiff_len - 2u) {
                            uint16_t count = jpeg_tiff_read_u16(tiff + ifd_off, big);
                            /* Bound the entry count to what the segment can actually hold: a
                             * forged 16-bit count (up to 65535) would otherwise drive a long
                             * pointer-arithmetic loop before the per-iteration bounds check below
                             * breaks. No OOB risk either way — this is DoS-amplification hardening,
                             * relevant on the MJPEG path where every AVI frame may carry EXIF. */
                            size_t exif_max_entries = (tiff_len - ifd_off - 2u) / 12u;
                            if ((size_t)count > exif_max_entries)
                                count = (uint16_t)exif_max_entries;
                            for (int ei = 0; ei < count; ei++) {
                                size_t entry = ifd_off + 2 + (size_t)ei * 12;
                                if (entry > tiff_len || tiff_len - entry < 12u)
                                    break;
                                uint16_t tag = jpeg_tiff_read_u16(tiff + entry, big);
                                if (tag == 0x0112) { // Orientation
                                    uint16_t field_type = jpeg_tiff_read_u16(tiff + entry + 2, big);
                                    uint32_t value_count =
                                        jpeg_tiff_read_u32(tiff + entry + 4, big);
                                    if (field_type == 3u && value_count == 1u)
                                        exif_orientation =
                                            (int)jpeg_tiff_read_u16(tiff + entry + 8, big);
                                    break;
                                }
                            }
                        }
                    }
                }
            exif_done:
                ctx.pos = seg_end;
                break;
        }
    }

    if (!saw_eoi)
        goto jpeg_fail;

    // Apply EXIF orientation transform without creating GC-managed Pixels.
    if (!direct_pixels && pixels &&
        !jpeg_rgba_apply_orientation(&pixels, &decoded_width, &decoded_height, exif_orientation))
        goto jpeg_fail;

    // Cleanup
    if (comp_data) {
        for (int i = 0; i < ctx.num_components; i++)
            free(comp_data[i]);
        free(comp_data);
    }
    if (!pixels)
        return 0;
    *out_pixels = direct_pixels ? NULL : pixels;
    *out_width = decoded_width;
    *out_height = decoded_height;
    if (out_status)
        *out_status = JPEG_DECODE_STATUS_OK;
    return 1;

jpeg_fail:
    if (pixels_owned)
        free(pixels);
    if (comp_data) {
        for (int i = 0; i < ctx.num_components; i++)
            free(comp_data[i]);
        free(comp_data);
    }
    *out_pixels = NULL;
    *out_width = 0;
    *out_height = 0;
    if (out_status)
        *out_status = decode_status;
    return 0;
}

/// @brief Decode baseline JPEG bytes to malloc-owned raw RGBA32 pixels.
/// @details Supports one-component grayscale and three-component YCbCr,
///          restart intervals, common sampling factors, and EXIF orientations.
///          Output words use canonical opaque `0xRRGGBBFF`. On failure the
///          output pointer is null and dimensions are zero.
/// @param data Borrowed encoded JPEG bytes beginning with SOI.
/// @param len Number of readable bytes in @p data.
/// @param out_pixels Receives malloc-owned RGBA words on success; the caller
///        releases them with `free()`.
/// @param out_width Receives the post-orientation width on success.
/// @param out_height Receives the post-orientation height on success.
/// @return Nonzero on success; zero for invalid arguments, corrupt or
///         unsupported input, bounds failure, or allocation failure.
int rt_jpeg_decode_buffer_rgba32(const uint8_t *data,
                                 size_t len,
                                 uint32_t **out_pixels,
                                 int64_t *out_width,
                                 int64_t *out_height) {
    return rt_jpeg_decode_buffer_rgba32_ex(
        data, len, out_pixels, out_width, out_height, NULL, NULL, 0, 0);
}

/// @brief Decode a JPEG image from a memory buffer into caller-owned RGBA32 pixels.
/// @details This avoids the intermediate malloc buffer used by
///          @c rt_jpeg_decode_buffer_rgba32 when a caller already owns a frame-sized
///          destination. The destination is only accepted for non-orienting JPEGs
///          whose encoded dimensions exactly match @p dst_width and @p dst_height.
///          Output words are opaque `0xRRGGBBFF`. A zero return does not
///          promise to preserve destination contents because structural
///          validation continues after scan reconstruction.
/// @param data Borrowed encoded JPEG bytes beginning with SOI.
/// @param len Number of readable bytes in @p data.
/// @param dst_pixels Caller-owned destination with room for
///        `dst_width * dst_height` words.
/// @param dst_width Required encoded width.
/// @param dst_height Required encoded height.
/// @return Nonzero on complete success; zero for invalid arguments,
///         dimension mismatch, orientation requirements, corrupt or
///         unsupported input, or bounds failure.
int rt_jpeg_decode_buffer_into_rgba32(
    const uint8_t *data, size_t len, uint32_t *dst_pixels, int64_t dst_width, int64_t dst_height) {
    uint32_t *unused_pixels = NULL;
    int64_t decoded_width = 0;
    int64_t decoded_height = 0;
    if (!dst_pixels)
        return 0;
    return rt_jpeg_decode_buffer_rgba32_ex(data,
                                           len,
                                           &unused_pixels,
                                           &decoded_width,
                                           &decoded_height,
                                           NULL,
                                           dst_pixels,
                                           dst_width,
                                           dst_height);
}

/// @brief Decode JPEG bytes into a new GC-managed Pixels object.
/// @details Uses the malloc-owned raw decoder, validates the returned pixel
///          count again, copies canonical words into embedded Pixels storage,
///          and frees the intermediate buffer on every path.
/// @param data Pointer to JPEG data (must start with 0xFFD8 SOI marker).
/// @param len Length of data in bytes.
/// @return New caller-owned Pixels object, or NULL on failure. The borrowed
///         encoded @p data is never freed.
void *rt_jpeg_decode_buffer(const uint8_t *data, size_t len) {
    uint32_t *raw_pixels = NULL;
    int64_t width = 0;
    int64_t height = 0;
    size_t pixel_count = 0;
    rt_pixels_impl *pixels;
    if (!rt_jpeg_decode_buffer_rgba32(data, len, &raw_pixels, &width, &height))
        return NULL;
    if (!jpeg_rgba_pixel_count_checked(width, height, &pixel_count)) {
        free(raw_pixels);
        return NULL;
    }
    pixels = pixels_alloc(width, height);
    if (!pixels) {
        free(raw_pixels);
        return NULL;
    }
    memcpy(pixels->data, raw_pixels, pixel_count * sizeof(uint32_t));
    free(raw_pixels);
    return pixels;
}

/// @brief Load a bounded baseline JPEG file into a Pixels object.
/// @details Reads at most 256 MiB through the UTF-8 stdio adapter, decodes at
///          most `JPEG_MAX_PIXELS`, and reports missing, unreadable, too-large,
///          unsupported-variant, or corrupt outcomes through the asset-error
///          context. Temporary file and raw RGBA buffers are always released.
/// @param path Runtime string handle naming the UTF-8 input path; null or an
///        invalid string traps.
/// @return A caller-owned GC-managed Pixels handle on success; null on load,
///         decode, bounds, or allocation failure.
void *rt_pixels_load_jpeg(void *path) {
    rt_asset_error_begin_load();
    if (!path) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadJpeg: path must not be null");
        return NULL;
    }

    const char *filepath = rt_string_cstr((rt_string)path);
    if (!filepath) {
        rt_asset_error_end_load_failure();
        rt_trap("Pixels.LoadJpeg: invalid path");
        return NULL;
    }

    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f) {
        rt_asset_error_setf(RT_ASSET_ERROR_NOT_FOUND, "Pixels.LoadJpeg: '%s' not found", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (px_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Pixels.LoadJpeg: failed to seek '%s'", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    int64_t file_len = px_ftell(f);
    if (px_fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Pixels.LoadJpeg: failed to seek '%s'", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    if (file_len <= 0 || file_len > INT64_C(256) * 1024 * 1024) {
        fclose(f);
        rt_asset_error_setf(file_len > INT64_C(256) * 1024 * 1024 ? RT_ASSET_ERROR_TOO_LARGE
                                                                  : RT_ASSET_ERROR_BAD_MAGIC,
                            "Pixels.LoadJpeg: '%s' is not a supported JPEG",
                            filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }

    uint8_t *file_data = (uint8_t *)malloc((size_t)file_len);
    if (!file_data) {
        fclose(f);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    if (!px_read_exact(f, file_data, (size_t)file_len)) {
        free(file_data);
        fclose(f);
        rt_asset_error_setf(
            RT_ASSET_ERROR_UNREADABLE, "Pixels.LoadJpeg: failed to read '%s'", filepath);
        rt_asset_error_end_load_failure();
        return NULL;
    }
    fclose(f);

    uint32_t *raw_pixels = NULL;
    int64_t width = 0;
    int64_t height = 0;
    jpeg_decode_status_t decode_status = JPEG_DECODE_STATUS_CORRUPT;
    void *result = NULL;
    if (rt_jpeg_decode_buffer_rgba32_ex(file_data,
                                        (size_t)file_len,
                                        &raw_pixels,
                                        &width,
                                        &height,
                                        &decode_status,
                                        NULL,
                                        0,
                                        0)) {
        size_t pixel_count = 0;
        if (jpeg_rgba_pixel_count_checked(width, height, &pixel_count)) {
            rt_pixels_impl *pixels = pixels_alloc(width, height);
            if (pixels) {
                memcpy(pixels->data, raw_pixels, pixel_count * sizeof(uint32_t));
                result = pixels;
            }
        }
        free(raw_pixels);
    }
    free(file_data);
    if (result) {
        rt_asset_error_end_load_success();
    } else if (decode_status == JPEG_DECODE_STATUS_UNSUPPORTED) {
        rt_asset_error_setf(RT_ASSET_ERROR_UNSUPPORTED,
                            "Pixels.LoadJpeg: '%s' uses an unsupported JPEG variant",
                            filepath);
        rt_asset_error_end_load_failure();
    } else {
        rt_asset_error_setf(
            RT_ASSET_ERROR_CORRUPT, "Pixels.LoadJpeg: '%s' is not a supported JPEG", filepath);
        rt_asset_error_end_load_failure();
    }
    return result;
}
