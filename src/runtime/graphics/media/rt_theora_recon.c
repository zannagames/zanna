//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_theora_recon.c
// Purpose: Theora frame reconstruction — DC-prediction undo, column/row IDCT,
//          residual block build, loop filtering, and motion-compensated frame
//          reconstruction. Split out of rt_theora.c; shares decoder state and
//          decode helpers via rt_theora_internal.h.
//
// Key invariants:
//   - Operates on a fully decoded theora_priv_t (coefficients, modes, MVs) and
//     writes reconstructed planes; it never reads the bitstream directly except
//     through the decode_* helpers it drives during a frame.
//   - IDCT/loop-filter math mirrors the Theora spec's fixed-point pipeline.
//
// Ownership/Lifetime:
//   - Borrows the decoder/priv state owned by the enclosing rt_theora object.
//
// Links: src/runtime/graphics/media/rt_theora.c (bitstream/header/coeff decode),
//        src/runtime/graphics/media/rt_theora_internal.h (shared types/helpers)
//
//===----------------------------------------------------------------------===//

#include "rt_theora.h"
#include "rt_theora_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Restore absolute DC values from the delta-coded representation in the bitstream.
/// @details Theora encodes DC coefficients as deltas from a spatial prediction
///   (`compute_dc_pred`). This function adds the prediction back, iterating blocks
///   in raster order (required by the spatial predictor) per plane. The three
///   `lastdc[rfi]` values track the most recent decoded DC per reference frame index
///   so that blocks referencing different frames accumulate their own DC histories.
/// @param priv Private coded-block state whose DC coefficients are restored in place.
static void undo_dc_prediction(theora_priv_t *priv) {
    int plane;
    for (plane = 0; plane < 3; plane++) {
        int16_t lastdc[3] = {0, 0, 0};
        int count = priv->plane_block_counts[plane];
        for (int raster = 0; raster < count; raster++) {
            int bi = priv->plane_raster_to_coded[plane][raster];
            if (!priv->bcoded[bi])
                continue;
            {
                int dcpred = compute_dc_pred(priv, lastdc, bi);
                int32_t dc = (int32_t)priv->coeffs[bi][0] + dcpred;
                int rfi = theora_ref_index_for_mode(priv->mbmodes[priv->blocks[bi].mb]);
                priv->coeffs[bi][0] = trunc_i16(dc);
                lastdc[rfi] = priv->coeffs[bi][0];
            }
        }
    }
}

#define TH_FIX(x) ((int64_t)((x) * 4096.0 + 0.5))
#define TH_DESCALE(x, n) theora_idct_descale_i64((x), (n))

/// @brief Clamp a 64-bit intermediate to the int32 range used by the IDCT workspace.
/// @details Malformed streams can synthesize unusually large coefficients before final int16
///          truncation. Clamping after each fixed-point descale avoids signed-overflow UB while
///          preserving valid-stream results.
/// @param value Fixed-point intermediate after scaling.
/// @return @p value clamped to [INT32_MIN, INT32_MAX].
static int32_t theora_idct_clamp_i32(int64_t value) {
    if (value > INT32_MAX)
        return INT32_MAX;
    if (value < INT32_MIN)
        return INT32_MIN;
    return (int32_t)value;
}

/// @brief Descale a 64-bit fixed-point IDCT intermediate with rounding.
/// @details This is the 64-bit equivalent of Theora's integer IDCT descale macro. The expression is
///          evaluated in int64_t before the final clamp so coefficient*constant products cannot
///          overflow signed int32_t.
/// @param value Fixed-point value to descale.
/// @param shift Number of fractional bits to remove.
/// @return Rounded and clamped int32_t result.
static int32_t theora_idct_descale_i64(int64_t value, int shift) {
    int64_t bias;
    if (shift <= 0)
        return theora_idct_clamp_i32(value);
    bias = INT64_C(1) << (shift - 1);
    return theora_idct_clamp_i32((value + bias) >> shift);
}

/// @brief Heap snapshot of mutable per-frame Theora decode arrays.
/// @details The bitstream decode passes mutate these arrays before the frame is reconstructed.
///          Keeping a snapshot lets the public frame decode API leave the decoder in its prior
///          state when a malformed packet fails halfway through.
typedef struct {
    uint8_t *bcoded;
    uint8_t *sb_partcoded;
    uint8_t *sb_fullcoded;
    uint8_t *qiis;
    uint8_t *mbmodes;
    uint8_t *tis;
    uint8_t *ncoeffs;
    int8_t (*mvects)[2];
    int16_t (*coeffs)[64];
} theora_decode_snapshot_t;

/// @brief Allocate and copy the mutable decode arrays from @p priv into @p snapshot.
/// @param priv Decoder private state whose arrays are already allocated.
/// @param snapshot Destination snapshot structure; cleared on failure.
/// @return 1 on success, 0 on allocation failure or invalid state.
static int theora_decode_snapshot_take(const theora_priv_t *priv,
                                       theora_decode_snapshot_t *snapshot) {
    size_t block_count;
    size_t superblock_count;
    size_t macroblock_count;
    if (!priv || !snapshot || priv->total_blocks < 0 || priv->total_superblocks < 0 ||
        priv->total_macroblocks < 0)
        return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    block_count = (size_t)priv->total_blocks;
    superblock_count = (size_t)priv->total_superblocks;
    macroblock_count = (size_t)priv->total_macroblocks;
    snapshot->bcoded = block_count ? (uint8_t *)malloc(block_count) : NULL;
    snapshot->sb_partcoded = superblock_count ? (uint8_t *)malloc(superblock_count) : NULL;
    snapshot->sb_fullcoded = superblock_count ? (uint8_t *)malloc(superblock_count) : NULL;
    snapshot->qiis = block_count ? (uint8_t *)malloc(block_count) : NULL;
    snapshot->mbmodes = macroblock_count ? (uint8_t *)malloc(macroblock_count) : NULL;
    snapshot->tis = block_count ? (uint8_t *)malloc(block_count) : NULL;
    snapshot->ncoeffs = block_count ? (uint8_t *)malloc(block_count) : NULL;
    snapshot->mvects = block_count ? (int8_t (*)[2])malloc(block_count * sizeof(int8_t[2])) : NULL;
    snapshot->coeffs =
        block_count ? (int16_t (*)[64])malloc(block_count * sizeof(int16_t[64])) : NULL;
    if ((block_count && (!snapshot->bcoded || !snapshot->qiis || !snapshot->tis ||
                         !snapshot->ncoeffs || !snapshot->mvects || !snapshot->coeffs)) ||
        (superblock_count && (!snapshot->sb_partcoded || !snapshot->sb_fullcoded)) ||
        (macroblock_count && !snapshot->mbmodes))
        return 0;
    if (block_count) {
        memcpy(snapshot->bcoded, priv->bcoded, block_count);
        memcpy(snapshot->qiis, priv->qiis, block_count);
        memcpy(snapshot->tis, priv->tis, block_count);
        memcpy(snapshot->ncoeffs, priv->ncoeffs, block_count);
        memcpy(snapshot->mvects, priv->mvects, block_count * sizeof(int8_t[2]));
        memcpy(snapshot->coeffs, priv->coeffs, block_count * sizeof(int16_t[64]));
    }
    if (superblock_count) {
        memcpy(snapshot->sb_partcoded, priv->sb_partcoded, superblock_count);
        memcpy(snapshot->sb_fullcoded, priv->sb_fullcoded, superblock_count);
    }
    if (macroblock_count)
        memcpy(snapshot->mbmodes, priv->mbmodes, macroblock_count);
    return 1;
}

/// @brief Restore mutable decode arrays from a previously taken snapshot.
/// @param priv Decoder private state to restore.
/// @param snapshot Snapshot returned by @ref theora_decode_snapshot_take.
static void theora_decode_snapshot_restore(theora_priv_t *priv,
                                           const theora_decode_snapshot_t *snapshot) {
    size_t block_count;
    size_t superblock_count;
    size_t macroblock_count;
    if (!priv || !snapshot)
        return;
    block_count = (size_t)priv->total_blocks;
    superblock_count = (size_t)priv->total_superblocks;
    macroblock_count = (size_t)priv->total_macroblocks;
    if (block_count) {
        memcpy(priv->bcoded, snapshot->bcoded, block_count);
        memcpy(priv->qiis, snapshot->qiis, block_count);
        memcpy(priv->tis, snapshot->tis, block_count);
        memcpy(priv->ncoeffs, snapshot->ncoeffs, block_count);
        memcpy(priv->mvects, snapshot->mvects, block_count * sizeof(int8_t[2]));
        memcpy(priv->coeffs, snapshot->coeffs, block_count * sizeof(int16_t[64]));
    }
    if (superblock_count) {
        memcpy(priv->sb_partcoded, snapshot->sb_partcoded, superblock_count);
        memcpy(priv->sb_fullcoded, snapshot->sb_fullcoded, superblock_count);
    }
    if (macroblock_count)
        memcpy(priv->mbmodes, snapshot->mbmodes, macroblock_count);
}

/// @brief Release heap memory held by a decode snapshot and clear its pointers.
/// @param snapshot Snapshot to free. NULL-safe.
static void theora_decode_snapshot_free(theora_decode_snapshot_t *snapshot) {
    if (!snapshot)
        return;
    free(snapshot->bcoded);
    free(snapshot->sb_partcoded);
    free(snapshot->sb_fullcoded);
    free(snapshot->qiis);
    free(snapshot->mbmodes);
    free(snapshot->tis);
    free(snapshot->ncoeffs);
    free(snapshot->mvects);
    free(snapshot->coeffs);
    memset(snapshot, 0, sizeof(*snapshot));
}

/// @brief Apply one horizontal pass of the separable AAN integer IDCT to an 8-element row.
/// @details Implements the factored 8-point IDCT butterfly using 12-bit fixed-point
///   constants (via TH_FIX / TH_DESCALE). Operates in-place on @p row. This pass
///   leaves the values in a partially-scaled state; the column pass (`idct_col`) adds
///   the final >>5 descale to produce the pixel-range residual output.
/// @param row In/out pointer to 8 int32_t values representing one DCT row.
static void idct_row(int32_t *row) {
    int32_t x0 = row[0], x1 = row[1], x2 = row[2], x3 = row[3];
    int32_t x4 = row[4], x5 = row[5], x6 = row[6], x7 = row[7];
    int32_t s0 = x0 + x4;
    int32_t s1 = x0 - x4;
    int32_t s2 = TH_DESCALE(x2 * TH_FIX(0.541196100) + x6 * TH_FIX(0.541196100 - 1.847759065), 12);
    int32_t s3 = TH_DESCALE(x2 * TH_FIX(0.541196100 + 0.765366865) + x6 * TH_FIX(0.541196100), 12);
    int32_t e0 = s0 + s3;
    int32_t e1 = s1 + s2;
    int32_t e2 = s1 - s2;
    int32_t e3 = s0 - s3;
    int32_t t0 = x1 + x7;
    int32_t t1 = x5 + x3;
    int32_t t2 = x1 + x3;
    int32_t t3 = x5 + x7;
    int32_t z5 = TH_DESCALE((t2 - t3) * TH_FIX(1.175875602), 12);
    t0 = TH_DESCALE(t0 * TH_FIX(-0.899976223), 12);
    t1 = TH_DESCALE(t1 * TH_FIX(-2.562915447), 12);
    t2 = TH_DESCALE(t2 * TH_FIX(-1.961570560), 12) + z5;
    t3 = TH_DESCALE(t3 * TH_FIX(-0.390180644), 12) + z5;
    row[0] = e0 + TH_DESCALE(x1 * TH_FIX(1.501321110), 12) + t0 + t3;
    row[1] = e1 + TH_DESCALE(x3 * TH_FIX(3.072711026), 12) + t1 + t2;
    row[2] = e2 + TH_DESCALE(x5 * TH_FIX(2.053119869), 12) + t1 + t3;
    row[3] = e3 + TH_DESCALE(x7 * TH_FIX(0.298631336), 12) + t0 + t2;
    row[4] = e3 - (TH_DESCALE(x7 * TH_FIX(0.298631336), 12) + t0 + t2);
    row[5] = e2 - (TH_DESCALE(x5 * TH_FIX(2.053119869), 12) + t1 + t3);
    row[6] = e1 - (TH_DESCALE(x3 * TH_FIX(3.072711026), 12) + t1 + t2);
    row[7] = e0 - (TH_DESCALE(x1 * TH_FIX(1.501321110), 12) + t0 + t3);
}

/// @brief Apply one vertical pass of the separable AAN integer IDCT to column @p col.
/// @details Identical butterfly structure to `idct_row` but reads from column-strided
///   positions (`workspace[col + k*8]`) and includes the final >>5 right-shift on
///   every output value. After both passes the 64-element workspace holds 8-bit-range
///   residuals ready for clamping by `idct_block`. Called once per column (0..7).
/// @param workspace 64-element int32_t scratch buffer (8×8, row-major).
/// @param col       Column index in [0, 7].
static void idct_col(int32_t *workspace, int col) {
    int32_t x0 = workspace[col + 0 * 8], x1 = workspace[col + 1 * 8];
    int32_t x2 = workspace[col + 2 * 8], x3 = workspace[col + 3 * 8];
    int32_t x4 = workspace[col + 4 * 8], x5 = workspace[col + 5 * 8];
    int32_t x6 = workspace[col + 6 * 8], x7 = workspace[col + 7 * 8];
    int32_t s0 = x0 + x4;
    int32_t s1 = x0 - x4;
    int32_t s2 = TH_DESCALE(x2 * TH_FIX(0.541196100) + x6 * TH_FIX(0.541196100 - 1.847759065), 12);
    int32_t s3 = TH_DESCALE(x2 * TH_FIX(0.541196100 + 0.765366865) + x6 * TH_FIX(0.541196100), 12);
    int32_t e0 = s0 + s3;
    int32_t e1 = s1 + s2;
    int32_t e2 = s1 - s2;
    int32_t e3 = s0 - s3;
    int32_t t0 = x1 + x7;
    int32_t t1 = x5 + x3;
    int32_t t2 = x1 + x3;
    int32_t t3 = x5 + x7;
    int32_t z5 = TH_DESCALE((t2 - t3) * TH_FIX(1.175875602), 12);
    t0 = TH_DESCALE(t0 * TH_FIX(-0.899976223), 12);
    t1 = TH_DESCALE(t1 * TH_FIX(-2.562915447), 12);
    t2 = TH_DESCALE(t2 * TH_FIX(-1.961570560), 12) + z5;
    t3 = TH_DESCALE(t3 * TH_FIX(-0.390180644), 12) + z5;
    workspace[col + 0 * 8] = TH_DESCALE(e0 + TH_DESCALE(x1 * TH_FIX(1.501321110), 12) + t0 + t3, 5);
    workspace[col + 1 * 8] = TH_DESCALE(e1 + TH_DESCALE(x3 * TH_FIX(3.072711026), 12) + t1 + t2, 5);
    workspace[col + 2 * 8] = TH_DESCALE(e2 + TH_DESCALE(x5 * TH_FIX(2.053119869), 12) + t1 + t3, 5);
    workspace[col + 3 * 8] = TH_DESCALE(e3 + TH_DESCALE(x7 * TH_FIX(0.298631336), 12) + t0 + t2, 5);
    workspace[col + 4 * 8] =
        TH_DESCALE(e3 - (TH_DESCALE(x7 * TH_FIX(0.298631336), 12) + t0 + t2), 5);
    workspace[col + 5 * 8] =
        TH_DESCALE(e2 - (TH_DESCALE(x5 * TH_FIX(2.053119869), 12) + t1 + t3), 5);
    workspace[col + 6 * 8] =
        TH_DESCALE(e1 - (TH_DESCALE(x3 * TH_FIX(3.072711026), 12) + t1 + t2), 5);
    workspace[col + 7 * 8] =
        TH_DESCALE(e0 - (TH_DESCALE(x1 * TH_FIX(1.501321110), 12) + t0 + t3), 5);
}

/// @brief Perform a full separable 8×8 IDCT on a block of 64 quantized DCT coefficients.
/// @details Copies @p in to an int32_t workspace, applies `idct_row` across all 8 rows,
///   then `idct_col` down all 8 columns, and clips each int32_t result to int16_t range
///   via `trunc_i16` before storing to @p out. The two-pass separable structure keeps
///   the butterfly table small while correctly handling the scaling introduced by
///   the AAN row pass.
/// @param in  64 quantized DCT coefficients in zigzag order (after de-zigzag by caller).
/// @param out 64 spatial-domain residual values, clamped to int16_t range.
static void idct_block(const int16_t in[64], int16_t out[64]) {
    int32_t ws[64];
    for (int i = 0; i < 64; i++)
        ws[i] = in[i];
    for (int i = 0; i < 8; i++)
        idct_row(ws + i * 8);
    for (int i = 0; i < 8; i++)
        idct_col(ws, i);
    for (int i = 0; i < 64; i++)
        out[i] = trunc_i16(ws[i]);
}

/// @brief Dequantize and IDCT one block's coefficients to produce spatial-domain residuals.
/// @details Three cases:
///   - `ncoeffs == 0`: no coefficients; output is all zeros (block is identical to prediction).
///   - `ncoeffs == 1`: DC-only shortcut; repeats the single DC value across all 64 positions
///     as allowed by the Theora spec for flat areas.
///   - General: builds a full 64-element coefficient array by dequantizing DC with
///     `qmat[qti][plane][qi_dc]` and AC with `qmat[qti][plane][qi_ac]`, then calls `idct_block`.
///   The quantization type `qti` is 0 for intra blocks (mode 1) and 1 for inter blocks.
/// @param dec Decoder supplying precomputed quantization matrices.
/// @param priv Private block modes, selectors, and coefficients.
/// @param bi Block index in [0, total_blocks).
/// @param fh Frame header supplying qi[] and nqi.
/// @param out 64-element output residual array.
static void build_residual_block(const theora_decoder_t *dec,
                                 const theora_priv_t *priv,
                                 int bi,
                                 const theora_frame_header_t *fh,
                                 int16_t out[64]) {
    const theora_block_info_t *b = &priv->blocks[bi];
    int qti = priv->mbmodes[b->mb] == 1 ? 0 : 1;
    int qi_dc = fh->qi[0];
    int qi_ac = fh->qi[priv->qiis[bi]];
    int16_t coeff_block[64];
    memset(out, 0, sizeof(int16_t) * 64);
    if (priv->ncoeffs[bi] == 0)
        return;
    if (priv->ncoeffs[bi] <= 1) {
        int dc = ((int)priv->coeffs[bi][0] * dec->qmat[qti][b->plane][qi_dc][0] + 15) >> 5;
        for (int i = 0; i < 64; i++)
            out[i] = (int16_t)dc;
        return;
    }
    memset(coeff_block, 0, sizeof(coeff_block));
    coeff_block[0] =
        (int16_t)(((int)priv->coeffs[bi][0] * dec->qmat[qti][b->plane][qi_dc][0] + 15) >> 5);
    for (int zi = 1; zi < priv->ncoeffs[bi] && zi < 64; zi++) {
        int idx = theora_zigzag[zi];
        coeff_block[idx] =
            (int16_t)(((int)priv->coeffs[bi][zi] * dec->qmat[qti][b->plane][qi_ac][idx] + 15) >> 5);
    }
    idct_block(coeff_block, out);
}

/// @brief Resolve the current, reference, and golden plane buffer pointers for @p plane.
/// @details Provides a single dispatch point that maps plane index (0=Y, 1=Cb, 2=Cr)
///   to the three per-plane buffer pointers and the correct stride (y_stride for luma,
///   c_stride for chroma). Used by `reconstruct_frame` and `apply_loop_filter` so
///   each does not need its own plane-dispatch switch.
/// @param dec Decoder owning all plane buffers.
/// @param plane 0=Y, 1=Cb, 2=Cr.
/// @param cur Output: writable current-frame buffer.
/// @param ref Output: read-only reference (previous) frame buffer.
/// @param gold Output: read-only golden frame buffer.
/// @param stride Output: row stride in bytes for this plane.
static void select_plane_buffers(const theora_decoder_t *dec,
                                 int plane,
                                 uint8_t **cur,
                                 const uint8_t **ref,
                                 const uint8_t **gold,
                                 int *stride) {
    if (plane == 0) {
        *cur = dec->cur_y;
        *ref = dec->ref_y;
        *gold = dec->gold_y;
        *stride = dec->y_stride;
    } else if (plane == 1) {
        *cur = dec->cur_cb;
        *ref = dec->ref_cb;
        *gold = dec->gold_cb;
        *stride = dec->c_stride;
    } else {
        *cur = dec->cur_cr;
        *ref = dec->ref_cr;
        *gold = dec->gold_cr;
        *stride = dec->c_stride;
    }
}

/// @brief Compute the loop-filter delta for a boundary given residual @p r and @p limit.
/// @details Implements the Theora loop filter response function (VP3 spec §6.5):
///   - |r| >= 2*limit: no filtering (return 0).
///   - limit <= |r| < 2*limit: push back by `sign(r) * (2*limit - |r|)`.
///   - |r| < limit: pass through as-is.
///   The response is applied to the two pixels straddling a block boundary to reduce
///   DCT ringing artefacts. A limit of 0 (all pixels are unfiltered) short-circuits
///   the entire loop-filter pass.
/// @param r Signed four-tap boundary residual.
/// @param limit Non-negative filter strength limit.
/// @return Signed correction delta to be added/subtracted from the two boundary pixels.
static int loop_filter_limit_response(int r, int limit) {
    if (limit <= 0)
        return 0;
    if (r <= -2 * limit)
        return 0;
    if (r <= -limit)
        return -r - 2 * limit;
    if (r < limit)
        return r;
    if (r < 2 * limit)
        return -r + 2 * limit;
    return 0;
}

/// @brief Apply the Theora loop filter along a vertical block boundary (filtering
///        pixel pairs horizontally across the boundary at column @p fx).
/// @details For each of the 8 rows in the block row, computes the 4-tap boundary
///   residual `r = (p[fx] - 3*p[fx+1] + 3*p[fx+2] - p[fx+3] + 4) >> 3` and applies
///   `loop_filter_limit_response` to get the delta. The delta is added to p[fx+1]
///   and subtracted from p[fx+2], clamped to [0, 255].
/// @param plane  Plane data buffer.
/// @param stride Row stride in bytes.
/// @param fx     First column of the 4-tap window (boundary is between fx+1 and fx+2).
/// @param fy     First row of the 8-row block.
/// @param limit  Filter limit from `dec->loop_filter_limits[qi]`.
static void apply_horizontal_filter(uint8_t *plane, int stride, int fx, int fy, int limit) {
    for (int by = 0; by < 8; by++) {
        int row = fy + by;
        int r = (plane[row * stride + fx] - 3 * plane[row * stride + fx + 1] +
                 3 * plane[row * stride + fx + 2] - plane[row * stride + fx + 3] + 4) >>
                3;
        int delta = loop_filter_limit_response(r, limit);
        int p1 = plane[row * stride + fx + 1] + delta;
        int p2 = plane[row * stride + fx + 2] - delta;
        plane[row * stride + fx + 1] = (uint8_t)clampi(p1, 0, 255);
        plane[row * stride + fx + 2] = (uint8_t)clampi(p2, 0, 255);
    }
}

/// @brief Apply the Theora loop filter along a horizontal block boundary (filtering
///        pixel pairs vertically across the boundary at row @p fy).
/// @details Symmetric to `apply_horizontal_filter` but operates on a 4-row vertical
///   window, computing the residual column-by-column across 8 columns of the block.
///   Modifies `p[(fy+1)*stride+x]` and `p[(fy+2)*stride+x]` in-place.
/// @param plane  Plane data buffer.
/// @param stride Row stride in bytes.
/// @param fx     First column of the 8-column block.
/// @param fy     First row of the 4-row window (boundary is between fy+1 and fy+2).
/// @param limit  Filter limit from `dec->loop_filter_limits[qi]`.
static void apply_vertical_filter(uint8_t *plane, int stride, int fx, int fy, int limit) {
    for (int bx = 0; bx < 8; bx++) {
        int r = (plane[fy * stride + fx + bx] - 3 * plane[(fy + 1) * stride + fx + bx] +
                 3 * plane[(fy + 2) * stride + fx + bx] - plane[(fy + 3) * stride + fx + bx] + 4) >>
                3;
        int delta = loop_filter_limit_response(r, limit);
        int p1 = plane[(fy + 1) * stride + fx + bx] + delta;
        int p2 = plane[(fy + 2) * stride + fx + bx] - delta;
        plane[(fy + 1) * stride + fx + bx] = (uint8_t)clampi(p1, 0, 255);
        plane[(fy + 2) * stride + fx + bx] = (uint8_t)clampi(p2, 0, 255);
    }
}

/// @brief Apply the Theora deblocking loop filter to all three planes of the current frame.
/// @details Iterates over all coded blocks and applies horizontal and vertical boundary
///   filters at each block edge. Four cases:
///   - Left edge (bx > 0): horizontal filter at column bx*8-2.
///   - Top edge (by > 0): vertical filter at row by*8-2.
///   - Right edge (if right neighbor is uncoded): horizontal filter at bx*8+6.
///   - Bottom edge (if bottom neighbor is uncoded): vertical filter at by*8+6.
///   The limit is looked up from `dec->loop_filter_limits[fh->qi[0]]`; a limit of 0
///   skips the entire filter pass (no-op for very low bitrate / already-flat frames).
/// @param dec Decoder supplying current planes and filter limits.
/// @param priv Private block layout and coded flags.
/// @param fh Current frame header selecting the quantizer/filter entry.
static void apply_loop_filter(theora_decoder_t *dec,
                              const theora_priv_t *priv,
                              const theora_frame_header_t *fh) {
    int limit;
    if (!dec || !priv || !fh)
        return;
    limit = dec->loop_filter_limits[fh->qi[0]];
    if (limit <= 0)
        return;

    for (int plane = 0; plane < 3; plane++) {
        uint8_t *plane_data = NULL;
        const uint8_t *unused_ref = NULL;
        const uint8_t *unused_gold = NULL;
        int stride = 0;
        int block_cols = priv->plane_block_cols[plane];
        int block_rows = priv->plane_block_rows[plane];
        int width = priv->plane_width[plane];
        int height = priv->plane_height[plane];

        select_plane_buffers(dec, plane, &plane_data, &unused_ref, &unused_gold, &stride);
        for (int by = 0; by < block_rows; by++) {
            for (int bx = 0; bx < block_cols; bx++) {
                int bi = priv->plane_raster_to_coded[plane][by * block_cols + bx];
                if (!priv->bcoded[bi])
                    continue;

                if (bx > 0) {
                    int fx = bx * 8 - 2;
                    int fy = by * 8;
                    apply_horizontal_filter(plane_data, stride, fx, fy, limit);
                }
                if (by > 0) {
                    int fx = bx * 8;
                    int fy = by * 8 - 2;
                    apply_vertical_filter(plane_data, stride, fx, fy, limit);
                }
                if ((bx + 1) * 8 < width) {
                    int bj = priv->plane_raster_to_coded[plane][by * block_cols + (bx + 1)];
                    if (!priv->bcoded[bj]) {
                        int fx = bx * 8 + 6;
                        int fy = by * 8;
                        apply_horizontal_filter(plane_data, stride, fx, fy, limit);
                    }
                }
                if ((by + 1) * 8 < height) {
                    int bj = priv->plane_raster_to_coded[plane][(by + 1) * block_cols + bx];
                    if (!priv->bcoded[bj]) {
                        int fx = bx * 8;
                        int fy = by * 8 + 6;
                        apply_vertical_filter(plane_data, stride, fx, fy, limit);
                    }
                }
            }
        }
    }
}

/// @brief Copy the motion-compensated prediction block using full-pixel (integer-pel)
///        addressing from the reference plane.
/// @details Clamps the source coordinates to [0, max_w-1] / [0, max_h-1] at every
///   pixel to handle motion vectors that extend beyond the frame boundary. The
///   clamped-border padding matches the Theora spec's boundary extension rules.
/// @param dst 8x8 output block (row-major, row stride = 8).
/// @param src Reference plane buffer.
/// @param stride Reference plane row stride.
/// @param bw Block width, possibly less than 8 at the right edge.
/// @param bh Block height, possibly less than 8 at the bottom edge.
/// @param sx Top-left source x coordinate before clamping.
/// @param sy Top-left source y coordinate before clamping.
/// @param max_w Reference plane width.
/// @param max_h Reference plane height.
static void copy_pred_whole(uint8_t *dst,
                            const uint8_t *src,
                            int stride,
                            int bw,
                            int bh,
                            int sx,
                            int sy,
                            int max_w,
                            int max_h) {
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int px = clampi(sx + x, 0, max_w - 1);
            int py = clampi(sy + y, 0, max_h - 1);
            dst[y * 8 + x] = src[py * stride + px];
        }
    }
}

/// @brief Copy a half-pixel-interpolated prediction block by averaging two reference
///        pixels at (x0+x, y0+y) and (x1+x, y1+y) per output position.
/// @details Used for INTER_MV_HALF (half-pixel motion-compensated mode) where the
///   effective MV has a fractional component. Both source coordinates are clamped
///   independently to the frame boundary. The rounding `(a + b + 1) >> 1` uses
///   round-half-up matching the Theora spec's half-pixel interpolation rule.
/// @param dst 8x8 output block with row stride 8.
/// @param src Reference plane buffer.
/// @param stride Reference plane row stride.
/// @param bw Block width.
/// @param bh Block height.
/// @param x0 First source x coordinate.
/// @param y0 First source y coordinate.
/// @param x1 Second source x coordinate.
/// @param y1 Second source y coordinate.
/// @param max_w Reference plane width.
/// @param max_h Reference plane height.
static void copy_pred_half(uint8_t *dst,
                           const uint8_t *src,
                           int stride,
                           int bw,
                           int bh,
                           int x0,
                           int y0,
                           int x1,
                           int y1,
                           int max_w,
                           int max_h) {
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int ax = clampi(x0 + x, 0, max_w - 1);
            int ay = clampi(y0 + y, 0, max_h - 1);
            int bx = clampi(x1 + x, 0, max_w - 1);
            int by = clampi(y1 + y, 0, max_h - 1);
            dst[y * 8 + x] = (uint8_t)((src[ay * stride + ax] + src[by * stride + bx] + 1) >> 1);
        }
    }
}

/// @brief Convert a Theora motion-vector component to the full-pixel component of a
///        half-pixel MV by truncating toward zero.
/// @details Theora encodes MVs in units of half-pixels. The full-pixel base coordinate
///   is `sign(mv) * floor(|mv| / 2)`. This function computes that value. For even MVs
///   this is exact; for odd MVs it gives the floor. Paired with `motion_trunc_away_zero`
///   for the second reference position in half-pixel interpolation.
/// @param mv Signed half-pixel motion component.
/// @return Full-pixel component truncated toward zero.
static int motion_trunc_toward_zero(int mv) {
    int s = mv < 0 ? -1 : 1;
    int a = mv < 0 ? -mv : mv;
    return s * (a / 2);
}

/// @brief Compute the second full-pixel component of a half-pixel MV by truncating
///        away from zero (ceiling of |mv| / 2).
/// @details For an odd MV the second reference pixel is one position further in the
///   direction of motion than the result of `motion_trunc_toward_zero`. For even MVs
///   both functions return the same value (no fractional component). The two functions
///   together construct the pair (x0, x1) passed to `copy_pred_half`.
/// @param mv Signed half-pixel motion component.
/// @return Full-pixel component rounded away from zero.
static int motion_trunc_away_zero(int mv) {
    int s = mv < 0 ? -1 : 1;
    int a = mv < 0 ? -mv : mv;
    return s * ((a + 1) / 2);
}

/// @brief Reconstruct the current frame by combining motion-compensated predictions
///        with decoded residuals for every block in all three planes.
/// @details Per-block pipeline:
///   1. `select_plane_buffers` resolves cur/ref/gold pointers and stride.
///   2. `build_residual_block` dequantizes and IDCTs the block's coefficients into
///      a 64-element int16_t residual (or all-zeros for uncoded blocks).
///   3. The prediction block (`pred`) is constructed based on the MB mode:
///      - INTRA: flat 128 (no prediction needed — residual is the signal).
///      - INTER_NOMV: copy from co-located ref position (zero MV).
///      - INTER_MV / INTER_MV_LAST / INTER_MV_LAST2: whole-pixel or half-pixel
///        copy from ref frame using the stored MV.
///      - GOLDEN modes: same as INTER but references the golden frame buffer.
///   4. The 8-bit prediction is summed with the int16_t residual, clamped to [0,255],
///      and written back to the current plane at the block's raster position.
/// @param dec Decoder owning current and reference planes.
/// @param priv Private decoded block, coefficient, mode, and motion state.
/// @param fh Current frame header selecting quantizer/filter values.
static void reconstruct_frame(theora_decoder_t *dec,
                              const theora_priv_t *priv,
                              const theora_frame_header_t *fh) {
    for (int bi = 0; bi < priv->total_blocks; bi++) {
        const theora_block_info_t *b = &priv->blocks[bi];
        uint8_t pred[64];
        int16_t resid[64];
        uint8_t *cur;
        const uint8_t *ref;
        const uint8_t *gold;
        int stride;
        int plane_w = priv->plane_width[b->plane];
        int plane_h = priv->plane_height[b->plane];
        int px = b->bx * 8;
        int py = b->by * 8;
        int bw = (px + 8 <= plane_w) ? 8 : (plane_w - px);
        int bh = (py + 8 <= plane_h) ? 8 : (plane_h - py);
        int mode = priv->mbmodes[b->mb];

        select_plane_buffers(dec, b->plane, &cur, &ref, &gold, &stride);
        memset(pred, 128, sizeof(pred));
        memset(resid, 0, sizeof(resid));

        if (!priv->bcoded[bi]) {
            copy_pred_whole(pred, ref, stride, bw, bh, px, py, plane_w, plane_h);
        } else if (mode != 1) {
            const uint8_t *src = theora_ref_index_for_mode(mode) == 2 ? gold : ref;
            int mvx = priv->mvects[bi][0];
            int mvy = priv->mvects[bi][1];
            if (((mvx | mvy) & 1) == 0) {
                copy_pred_whole(
                    pred, src, stride, bw, bh, px + mvx / 2, py + mvy / 2, plane_w, plane_h);
            } else {
                copy_pred_half(pred,
                               src,
                               stride,
                               bw,
                               bh,
                               px + motion_trunc_toward_zero(mvx),
                               py + motion_trunc_toward_zero(mvy),
                               px + motion_trunc_away_zero(mvx),
                               py + motion_trunc_away_zero(mvy),
                               plane_w,
                               plane_h);
            }
        }

        if (priv->bcoded[bi])
            build_residual_block(dec, priv, bi, fh, resid);

        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < bw; x++) {
                int idx = y * 8 + x;
                int val = pred[idx] + resid[idx];
                cur[(py + y) * stride + (px + x)] = (uint8_t)clampi(val, 0, 255);
            }
        }
    }
    apply_loop_filter(dec, priv, fh);
}

/// @brief Public: decode one compressed video packet into YUV420/422/444 planes.
///
/// Multi-pass pipeline:
///   1. Frame header (type, QI list).
///   2. Block coded-flags bitmap.
///   3. Macroblock modes (intra / forward / etc.).
///   4. Motion vectors.
///   5. Per-block quantization-index selection.
///   6. DCT coefficients (all blocks, all 64 frequencies).
///   7. Undo DC prediction (DC coefficients are differential).
///   8. Reconstruct the frame (IDCT, motion-compensate, loop filter).
///   9. Update reference frames (and golden frame on intra).
/// `out_y`/`out_cb`/`out_cr` receive pointers to the decoded planes
/// (owned by the decoder). Returns 0 on success, -1 on any error.
/// @param dec Decoder with completed headers and allocated frame state.
/// @param data Borrowed compressed video packet.
/// @param len Accessible packet length.
/// @param out_y Optional destination for the decoder-owned luma plane.
/// @param out_cb Optional destination for the decoder-owned Cb plane.
/// @param out_cr Optional destination for the decoder-owned Cr plane.
/// @return 0 on success, or -1 for invalid state, malformed/truncated data, or snapshot allocation
///         failure.
int theora_decode_frame(theora_decoder_t *dec,
                        const uint8_t *data,
                        size_t len,
                        const uint8_t **out_y,
                        const uint8_t **out_cb,
                        const uint8_t **out_cr) {
    theora_priv_t *priv;
    bitreader_t br;
    theora_frame_header_t fh;
    theora_decode_snapshot_t snapshot;
    int ok = 0;
    size_t y_size, c_size;

    if (!dec || !data || len < 1 || !dec->headers_complete || !dec->priv)
        return -1;

    priv = (theora_priv_t *)dec->priv;
    br_init(&br, data, len);
    memset(&fh, 0, sizeof(fh));
    memset(&snapshot, 0, sizeof(snapshot));

    if (decode_frame_header(dec, &br, &fh) != 0)
        return -1;
    if (!theora_decode_snapshot_take(priv, &snapshot)) {
        theora_decode_snapshot_free(&snapshot);
        return -1;
    }
    if (decode_block_flags(dec, priv, &br, fh.frame_type) == 0 &&
        decode_mb_modes(dec, priv, &br, fh.frame_type) == 0 &&
        decode_motion_vectors(dec, priv, &br, fh.frame_type) == 0 &&
        decode_qiis(priv, &br, fh.nqi) == 0 && decode_coefficients(priv, &br) == 0) {
        ok = 1;
    }
    if (!ok) {
        theora_decode_snapshot_restore(priv, &snapshot);
        theora_decode_snapshot_free(&snapshot);
        return -1;
    }
    theora_decode_snapshot_free(&snapshot);
    undo_dc_prediction(priv);
    reconstruct_frame(dec, priv, &fh);

    y_size = (size_t)dec->y_stride * (size_t)dec->y_height;
    c_size = (size_t)dec->c_stride * (size_t)dec->c_height;
    memcpy(dec->ref_y, dec->cur_y, y_size);
    memcpy(dec->ref_cb, dec->cur_cb, c_size);
    memcpy(dec->ref_cr, dec->cur_cr, c_size);
    if (fh.frame_type == 0) {
        memcpy(dec->gold_y, dec->cur_y, y_size);
        memcpy(dec->gold_cb, dec->cur_cb, c_size);
        memcpy(dec->gold_cr, dec->cur_cr, c_size);
    }
    priv->first_frame_decoded = 1;

    if (out_y)
        *out_y = dec->cur_y;
    if (out_cb)
        *out_cb = dec->cur_cb;
    if (out_cr)
        *out_cr = dec->cur_cr;
    return 0;
}
